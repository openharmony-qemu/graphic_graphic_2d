/*
 * Copyright (c) 2022-2023 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "transaction/rs_ashmem_helper.h"

#include <climits>
#include <cstdlib>
#include <vector>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>

#include "ashmem.h"
#include "ipc_file_descriptor.h"
#include "ipc_types.h"
#include "platform/common/rs_log.h"
#include "rs_profiler.h"
#include "rs_trace.h"
#include "securec.h"
#include "sys_binder.h"
#include "sandbox_utils.h"
#include "platform/ohos/transaction/zidl/rs_iclient_to_render_connection.h"

namespace OHOS {
namespace Rosen {
namespace {
constexpr size_t LARGE_MALLOC = 200000000;
thread_local bool g_isUnmarshalThread = false;

bool IsValidAshmemObjectOffsets(const binder_size_t* offsets, size_t count, size_t dataSize)
{
    if (count > 0 && offsets == nullptr) {
        return false;
    }
    size_t previousEnd = 0;
    for (size_t i = 0; i < count; ++i) {
        const binder_size_t offset = offsets[i];
        if (offset < previousEnd || offset > dataSize ||
            sizeof(flat_binder_object) > dataSize - static_cast<size_t>(offset) ||
            offset % sizeof(uint32_t) != 0) {
            return false;
        }
        previousEnd = static_cast<size_t>(offset) + sizeof(flat_binder_object);
    }
    return true;
}

// Versioned private ashmem sidecar: strong Binder objects plus owning FDs.
constexpr uint32_t RS_OBJECT_SIDECAR_VERSION = 0x52534F31;
constexpr uint32_t RS_OBJECT_FD = 1;
constexpr uint32_t RS_OBJECT_REMOTE = 2;

struct RSParcelObjectSlot {
    size_t offset = 0;
    size_t end = 0;
    uint32_t kind = 0;
};

class RSParcelReadPosition final {
public:
    explicit RSParcelReadPosition(MessageParcel& parcel) : parcel_(parcel), position_(parcel.GetReadPosition()) {}
    ~RSParcelReadPosition()
    {
        // The saved position was valid before any read and the source is not modified.
        (void)parcel_.RewindRead(position_);
    }
private:
    MessageParcel& parcel_;
    const size_t position_;
};

bool BuildRSObjectLayout(const MessageParcel& parcel, const binder_size_t* offsets, size_t count,
    std::vector<RSParcelObjectSlot>& slots)
{
    const size_t size = parcel.GetDataSize();
    if (!IsValidAshmemObjectOffsets(offsets, count, size)) {
        return false;
    }
    size_t previousEnd = 0;
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = static_cast<size_t>(offsets[index]);
        if (offset < previousEnd) {
            return false;
        }
        flat_binder_object flat {};
        if (memcpy_s(&flat, sizeof(flat), reinterpret_cast<const void*>(parcel.GetData() + offset),
            sizeof(flat)) != EOK) {
            return false;
        }
        RSParcelObjectSlot slot { offset, offset + sizeof(flat), RS_OBJECT_REMOTE };
        if (flat.hdr.type == BINDER_TYPE_FD || flat.hdr.type == BINDER_TYPE_FDR) {
            slot.kind = RS_OBJECT_FD;
        } else if (flat.hdr.type == BINDER_TYPE_PTR) {
            // DBinder's PTR and its following strong object are one logical
            // object. The buffer/cookie are deliberately never dereferenced here.
            binder_buffer_object buffer {};
            if (sizeof(buffer) > size - offset ||
                memcpy_s(&buffer, sizeof(buffer), reinterpret_cast<const void*>(parcel.GetData() + offset),
                    sizeof(buffer)) != EOK ||
                buffer.flags != BINDER_BUFFER_FLAG_HAS_DBINDER ||
                buffer.length != sizeof(dbinder_negotiation_data) ||
                index + 1 >= count || offsets[index + 1] != offset + sizeof(buffer)) {
                return false;
            }
            const size_t next = static_cast<size_t>(offsets[++index]);
            if (memcpy_s(&flat, sizeof(flat), reinterpret_cast<const void*>(parcel.GetData() + next),
                sizeof(flat)) != EOK ||
                (flat.hdr.type != BINDER_TYPE_BINDER && flat.hdr.type != BINDER_TYPE_HANDLE)) {
                return false;
            }
            slot.end = next + sizeof(flat);
        } else if (flat.hdr.type != BINDER_TYPE_BINDER && flat.hdr.type != BINDER_TYPE_HANDLE) {
            // Weak references, FDA and arbitrary pointer objects are not RS
            // command arguments. Do not acquire anything described by raw bytes.
            return false;
        }
        previousEnd = slot.end;
        slots.push_back(slot);
    }
    return true;
}

bool CopyRSObject(MessageParcel& source, MessageParcel& destination, uint32_t kind)
{
    if (kind == RS_OBJECT_REMOTE) {
        const auto object = source.ReadRemoteObject();
        // WriteRemoteObject owns a real sptr (and any DBinder backing buffer).
        return object != nullptr && destination.WriteRemoteObject(object);
    }
    if (kind != RS_OBJECT_FD) {
        return false;
    }
    const int fd = source.ReadFileDescriptor();
    if (fd < 0) {
        return false;
    }
    const bool written = destination.WriteFileDescriptor(fd);
    // ReadFileDescriptor and WriteFileDescriptor each duplicate: this temporary
    // belongs to neither Parcel, including on the partial-write failure path.
    ::close(fd);
    return written;
}

std::shared_ptr<MessageParcel> RebuildRSObjectParcel(const MessageParcel& snapshot,
    const std::vector<RSParcelObjectSlot>& slots, MessageParcel& objects, bool sidecar)
{
    const size_t size = snapshot.GetDataSize();
    // Re-encoding a local strong object as DBinder can add one buffer descriptor.
    // Bound allocation by the already bounded payload; check before arithmetic.
    if (slots.size() > (SIZE_MAX - size) / sizeof(binder_buffer_object)) {
        return nullptr;
    }
    auto parcel = RS_PROFILER_COPY_PARCEL(snapshot);
    const size_t capacity = size + slots.size() * sizeof(binder_buffer_object);
    if (capacity > parcel->GetMaxCapacity() && !parcel->SetMaxCapacity(capacity)) {
        return nullptr;
    }
    const RSParcelReadPosition restore(objects);
    size_t previousEnd = 0;
    for (const auto& slot : slots) {
        if (slot.offset > previousEnd &&
            !parcel->WriteBuffer(reinterpret_cast<const void*>(snapshot.GetData() + previousEnd),
                slot.offset - previousEnd)) {
            return nullptr;
        }
        uint32_t kind = slot.kind;
        if (sidecar) {
            if (!objects.ReadUint32(kind) || kind != slot.kind) {
                return nullptr;
            }
        } else if (!objects.RewindRead(slot.offset)) {
            return nullptr;
        }
        if (!CopyRSObject(objects, *parcel, kind) || (!sidecar && objects.GetReadPosition() != slot.end)) {
            return nullptr;
        }
        previousEnd = slot.end;
    }
    if (sidecar && objects.GetReadableBytes() != 0) {
        return nullptr;
    }
    if (size > previousEnd &&
        !parcel->WriteBuffer(reinterpret_cast<const void*>(snapshot.GetData() + previousEnd), size - previousEnd)) {
        return nullptr;
    }
    return parcel;
}

bool RestoreRSObjectSidecar(std::shared_ptr<MessageParcel>& snapshot, MessageParcel& sidecar)
{
    int32_t count = 0;
    uint32_t version = 0;
    if (!sidecar.ReadInt32(count) || count < 0 ||
        !sidecar.ReadUint32(version) || version != RS_OBJECT_SIDECAR_VERSION ||
        static_cast<size_t>(count) > sidecar.GetReadableBytes() / sizeof(binder_size_t) ||
        static_cast<size_t>(count) > snapshot->GetDataSize() / sizeof(flat_binder_object)) {
        return false;
    }
    std::vector<binder_size_t> offsets(static_cast<size_t>(count));
    if (count > 0) {
        const size_t bytes = offsets.size() * sizeof(binder_size_t);
        const auto* raw = sidecar.ReadBuffer(bytes);
        if (raw == nullptr || memcpy_s(offsets.data(), bytes, raw, bytes) != EOK) {
            return false;
        }
    }
    std::vector<RSParcelObjectSlot> slots;
    if (!BuildRSObjectLayout(*snapshot, offsets.data(), offsets.size(), slots)) {
        return false;
    }
    auto parcel = RebuildRSObjectParcel(*snapshot, slots, sidecar, true);
    if (parcel == nullptr) {
        return false;
    }
    // The discarded snapshot never had object offsets: no raw cookie or FD is
    // published even during failure cleanup. Only standard writers add offsets.
    snapshot = std::move(parcel);
    return true;
}

bool SanitizeAshmemFileDescriptors(MessageParcel& parcel, const binder_size_t* offsets, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        flat_binder_object flat {};
        auto* bytes = reinterpret_cast<void*>(parcel.GetData() + offsets[i]);
        if (memcpy_s(&flat, sizeof(flat), bytes, sizeof(flat)) != EOK) {
            return false;
        }
        if (flat.hdr.type == BINDER_TYPE_FD || flat.hdr.type == BINDER_TYPE_FDR) {
            // These numbers belong to the sender, not to this process. Never
            // publish them in the receiver's object table, even on failure.
            flat.hdr.type = BINDER_TYPE_FD;
            flat.handle = static_cast<uint32_t>(-1);
            flat.cookie = 1;
            if (memcpy_s(bytes, sizeof(flat), &flat, sizeof(flat)) != EOK) {
                return false;
            }
        }
    }
    return true;
}

bool RestoreAshmemFileDescriptors(std::shared_ptr<MessageParcel>& parcel, MessageParcel* ashmem,
    std::unique_ptr<AshmemFdWorker>& worker, pid_t callingPid)
{
    worker = std::make_unique<AshmemFdWorker>(callingPid);
    const auto* offsets = reinterpret_cast<const binder_size_t*>(parcel->GetObjectOffsets());
    for (size_t i = 0; i < parcel->GetOffsetsSize(); ++i) {
        const binder_size_t offset = offsets[i];
        if (offset > parcel->GetDataSize() || sizeof(flat_binder_object) > parcel->GetDataSize() - offset) {
            return false;
        }
        flat_binder_object flat {};
        auto* bytes = reinterpret_cast<void*>(parcel->GetData() + offset);
        if (memcpy_s(&flat, sizeof(flat), bytes, sizeof(flat)) != EOK) {
            return false;
        }
        if (flat.hdr.type != BINDER_TYPE_FD) {
            continue;
        }
        const int fd = ashmem->ReadFileDescriptor();
        if (fd < 0) {
            return false;
        }
        flat.handle = static_cast<uint32_t>(fd);
        if (memcpy_s(bytes, sizeof(flat), &flat, sizeof(flat)) != EOK) {
            ::close(fd);
            return false;
        }
        // The Parcel owns every duplicate until the async path FlushBuffer()s
        // and explicitly transfers that responsibility to this worker.
        worker->InsertFdWithOffset(fd, offset, true);
    }
    return true;
}

}
std::unique_ptr<AshmemAllocator> AshmemAllocator::CreateAshmemAllocator(size_t size, int mapType)
{
    static pid_t pid_ = GetRealPid();
    static std::atomic<uint32_t> shmemCount = 0;
    uint64_t id = ((uint64_t)pid_ << 32) | shmemCount++;
    std::string name = "RSAshmem" + std::to_string(id);

    int fd = AshmemCreate(name.c_str(), size);
    if (fd < 0) {
        ROSEN_LOGE("CreateAshmemAllocator: AshmemCreate failed, fd:%{public}d", fd);
        return nullptr;
    }
    auto allocator = std::make_unique<AshmemAllocator>(fd, size);

    int result = AshmemSetProt(fd, PROT_READ | PROT_WRITE);
    if (result < 0) {
        ROSEN_LOGE("CreateAshmemAllocator: AshmemSetProt failed, result:%{public}d", result);
        return nullptr;
    }

    if (!allocator->MapAshmem(mapType)) {
        ROSEN_LOGE("CreateAshmemAllocator: MapAshmem failed");
        return nullptr;
    }

    return allocator;
}

std::unique_ptr<AshmemAllocator> AshmemAllocator::CreateAshmemAllocatorWithFd(int fd, size_t size, int mapType)
{
    if (fd < 0) {
        ROSEN_LOGE("CreateAshmemAllocatorWithFd: fd < 0");
        return nullptr;
    }
    auto allocator = std::make_unique<AshmemAllocator>(fd, size);

    int ashmemSize = AshmemGetSize(fd);
    if (ashmemSize < 0 || size_t(ashmemSize) < size) {
        ROSEN_LOGE("CreateAshmemAllocatorWithFd: ashmemSize < size");
        return nullptr;
    }

    if (!allocator->MapAshmem(mapType)) {
        ROSEN_LOGE("CreateAshmemAllocatorWithFd: MapAshmem failed");
        return nullptr;
    }

    return allocator;
}

AshmemAllocator::AshmemAllocator(int fd, size_t size) : fd_(fd), size_(size) {}

AshmemAllocator::~AshmemAllocator()
{
    Dealloc(data_);
}

bool AshmemAllocator::MapAshmem(int mapType)
{
    void *startAddr = ::mmap(nullptr, size_, mapType, MAP_SHARED, fd_, 0);
    if (startAddr == MAP_FAILED) {
        ROSEN_LOGE("AshmemAllocator::MapAshmem MAP_FAILED");
        return false;
    }
    data_ = startAddr;
    return true;
}

bool AshmemAllocator::WriteToAshmem(const void *data, size_t size)
{
    if (data == nullptr || size_ < size) {
        return false;
    }
    if (!data_) {
        ROSEN_LOGE("AshmemAllocator::WriteToAshmem data_ is nullptr");
        return false;
    }
    errno_t err = memcpy_s(data_, size, data, size);
    if (err != EOK) {
        ROSEN_LOGE("AshmemAllocator::WriteToAshmem memcpy_s failed, err:%{public}d", err);
        return false;
    }
    return true;
}

void* AshmemAllocator::CopyFromAshmem(size_t size)
{
    if (size_ < size) {
        return nullptr;
    }
    if (size > LARGE_MALLOC) {
        ROSEN_LOGW("AshmemAllocator::CopyFromAshmem this time malloc large memory, size:%{public}zu", size);
    }
    if (!data_) {
        ROSEN_LOGE("AshmemAllocator::CopyFromAshmem data_ is nullptr");
        return nullptr;
    }
    void* base = malloc(size);
    if (base == nullptr) {
        ROSEN_LOGE("AshmemAllocator::CopyFromAshmem malloc failed, size:%{public}zu", size);
        return nullptr;
    }
    errno_t err = memcpy_s(base, size, data_, size);
    if (err != EOK) {
        free(base);
        base = nullptr;
        ROSEN_LOGE("AshmemAllocator::CopyFromAshmem memcpy_s failed, err:%{public}d", err);
        return nullptr;
    }
    return base;
}

// the fd is only valid during the object lifetime
int AshmemAllocator::GetFd() const
{
    return fd_;
}

size_t AshmemAllocator::GetSize() const
{
    return size_;
}

void* AshmemAllocator::GetData() const
{
    return data_;
}

void AshmemAllocator::Dealloc(void* data)
{
    if (data != data_) {
        ROSEN_LOGW("AshmemAllocator::Dealloc data addr not match!");
    }
    if (data_ != nullptr) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void* AshmemAllocator::Alloc(size_t size)
{
    ROSEN_LOGW("AshmemAllocator::Alloc");
    return nullptr;
}

void* AshmemAllocator::Realloc(void* data, size_t newSize)
{
    ROSEN_LOGW("AshmemAllocator::Realloc");
    return data_;
}

AshmemFdContainer& AshmemFdContainer::Instance()
{
    thread_local AshmemFdContainer instance;
    return instance;
}

void AshmemFdContainer::SetIsUnmarshalThread(bool isUnmarshalThread)
{
    g_isUnmarshalThread = isUnmarshalThread;
}

int AshmemFdContainer::ReadSafeFd(Parcel &parcel, std::function<int(Parcel&)> readFdDefaultFunc)
{
    if (!g_isUnmarshalThread || !isUseFdContainer_) {
        if (readFdDefaultFunc == nullptr) {
            return static_cast<MessageParcel*>(&parcel)->ReadFileDescriptor();
        }
        return readFdDefaultFunc(parcel);
    }

    size_t offset = parcel.GetReadPosition();
    sptr<IPCFileDescriptor> descriptor = parcel.ReadObject<IPCFileDescriptor>();

    int parcelFd = INVALID_FD;
    if (descriptor == nullptr) {
        ROSEN_LOGE("AshmemFdContainer::ReadSafeFd ReadObject failed");
    } else {
        parcelFd = descriptor->GetFd();
    }
    if (parcelFd < 0) {
        ROSEN_LOGE("AshmemFdContainer::ReadSafeFd failed: invalid parcelFd = %{public}d", parcelFd);
    }

    int containerFd = INVALID_FD;
    auto it = fds_.find(offset);
    if (it != fds_.end()) {
        containerFd = it->second;
        fds_.erase(it);
    } else {
        ROSEN_LOGE("AshmemFdContainer::ReadSafeFd failed: offset %{public}zu not found", offset);
    }
    if (containerFd < 0) {
        ROSEN_LOGE("AshmemFdContainer::ReadSafeFd failed: invalid containerFd = %{public}d", containerFd);
    }

    if (parcelFd != containerFd) {
        ROSEN_LOGW("AshmemFdContainer::ReadSafeFd inconsistent parcelFd = %{public}d, containerFd = %{public}d",
            parcelFd, containerFd);
    }

    if (parcelFd >= 0 && containerFd < 0) {
        int fd = dup(parcelFd);
        if (fd < 0) {
            ROSEN_LOGE("AshmemFdContainer::ReadSafeFd dup failed: parcelFd = %{public}d, errno = %{public}d",
                parcelFd, errno);
        }
        return fd;
    }

    if (containerFd < 0) {
        return INVALID_FD;
    }

    int safeFd = dup(containerFd);
    if (safeFd < 0) {
        ROSEN_LOGE("AshmemFdContainer::ReadSafeFd dup failed: containerFd = %{public}d, errno = %{public}d",
            containerFd, errno);
    }
    return safeFd;
}

void AshmemFdContainer::Merge(const std::unordered_map<binder_size_t, int>& fds)
{
    if (!g_isUnmarshalThread) {
        return;
    }
    isUseFdContainer_ = true;
    fds_ = fds;
    ROSEN_LOGI_IF(DEBUG_IPC, "AshmemFdContainer::Merge fds_ = %s", PrintFds().c_str());
}

void AshmemFdContainer::Clear()
{
    if (!g_isUnmarshalThread) {
        return;
    }
    isUseFdContainer_ = false;
    if (!fds_.empty()) {
        ROSEN_LOGW_IF(DEBUG_IPC, "AshmemFdContainer::Clear not empty, fds = %{public}s", PrintFds().c_str());
        fds_.clear();
    }
}

std::string AshmemFdContainer::PrintFds() const
{
    std::string ret = "{";
    for (const auto& [offset, fd]: fds_) {
        ret += std::to_string(offset);
        ret += ":";
        ret += std::to_string(fd);
        ret += ", ";
    }
    ret += "}";
    return ret;
}

AshmemFdContainerScope::AshmemFdContainerScope(AshmemFdWorker* worker)
    : container_(AshmemFdContainer::Instance()), previousFds_(std::move(container_.fds_)),
      previousUse_(container_.isUseFdContainer_), previousUnmarshal_(g_isUnmarshalThread)
{
    AshmemFdContainer::SetIsUnmarshalThread(true);
    container_.isUseFdContainer_ = false;
    if (worker != nullptr) {
        worker->PushFdsToContainer();
    }
}

AshmemFdContainerScope::~AshmemFdContainerScope()
{
    // Never defer TLS cleanup to the final shared worker owner: that owner
    // may be the producer, or a queue cancellation thread.
    container_.fds_ = std::move(previousFds_);
    container_.isUseFdContainer_ = previousUse_;
    AshmemFdContainer::SetIsUnmarshalThread(previousUnmarshal_);
}

AshmemFdWorker::AshmemFdWorker(const pid_t callingPid) : callingPid_(callingPid) {}

AshmemFdWorker::~AshmemFdWorker()
{
    if (needManualCloseFds_) {
        for (const int fd : fdsToBeClosed_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }
}

void AshmemFdWorker::InsertFdWithOffset(int fd, binder_size_t offset, bool shouldCloseFd)
{
    if (shouldCloseFd) {
        fdsToBeClosed_.insert(fd);
    }
    if (isFdContainerUpdated_) {
        ROSEN_LOGE("AshmemFdWorker::InsertFdWithOffset fd container has been updated, skip fd %{public}d with "
            "offset %{public}" PRIu64, fd, static_cast<uint64_t>(offset));
        return;
    }
    auto [it, isNewElement] = fds_.try_emplace(offset, fd);
    if (isNewElement) {
        return;
    }
    ROSEN_LOGW("AshmemFdWorker::InsertFdWithOffset existed offset %{public}" PRIu64 "is overriden, "
        "old fd = %{public}d, new fd = %{public}d", static_cast<uint64_t>(offset), it->second, fd);
    it->second = fd;
}

void AshmemFdWorker::PushFdsToContainer()
{
    if (isFdContainerUpdated_) {
        ROSEN_LOGE("AshmemFdWorker::PushFdsToContainer fd container has been updated, skip push operation");
        return;
    }
    ROSEN_LOGI_IF(DEBUG_IPC, "AshmemFdWorker::PushFdsToContainer from callingPid %{public}d",
        static_cast<int>(callingPid_));
    AshmemFdContainer::Instance().Merge(fds_);
    isFdContainerUpdated_ = true;
}

void AshmemFdWorker::EnableManualCloseFds()
{
    needManualCloseFds_ = true;
}

void RSAshmemHelper::CopyFileDescriptor(
    MessageParcel* ashmemParcel, std::shared_ptr<MessageParcel>& dataParcel)
{
    binder_size_t* object = reinterpret_cast<binder_size_t*>(dataParcel->GetObjectOffsets());
    size_t objectNum = dataParcel->GetOffsetsSize();
    uintptr_t data = dataParcel->GetData();
    for (size_t i = 0; i < objectNum; i++) {
        flat_binder_object flat {};
        const binder_size_t offset = object[i];
        if (offset > dataParcel->GetDataSize() ||
            sizeof(flat) > dataParcel->GetDataSize() - offset ||
            memcpy_s(&flat, sizeof(flat), reinterpret_cast<void*>(data + offset), sizeof(flat)) != EOK) {
            return;
        }
        if (flat.hdr.type == BINDER_TYPE_FD || flat.hdr.type == BINDER_TYPE_FDR) {
            if (!ashmemParcel->WriteFileDescriptor(static_cast<int32_t>(flat.handle))) {
                ROSEN_LOGE("RSAshmemHelper::CopyFileDescriptor failed, fd:%{public}u", flat.handle);
            }
        }
    }
}

void RSAshmemHelper::InjectFileDescriptor(std::shared_ptr<MessageParcel>& dataParcel, MessageParcel* ashmemParcel,
    std::unique_ptr<AshmemFdWorker>& ashmemFdWorker, pid_t callingPid)
{
    const auto* offsets = reinterpret_cast<const binder_size_t*>(dataParcel->GetObjectOffsets());
    const size_t count = dataParcel->GetOffsetsSize();
    if (!IsValidAshmemObjectOffsets(offsets, count, dataParcel->GetDataSize()) ||
        !SanitizeAshmemFileDescriptors(*dataParcel, offsets, count) ||
        !RestoreAshmemFileDescriptors(dataParcel, ashmemParcel, ashmemFdWorker, callingPid)) {
        ROSEN_LOGE("RSAshmemHelper::InjectFileDescriptor failed to restore receiver-owned FDs");
    }
}

std::shared_ptr<MessageParcel> RSAshmemHelper::CopyTransactionParcel(MessageParcel& source)
{
    const auto* offsets = reinterpret_cast<const binder_size_t*>(source.GetObjectOffsets());
    std::vector<RSParcelObjectSlot> slots;
    if (!BuildRSObjectLayout(source, offsets, source.GetOffsetsSize(), slots)) {
        return nullptr;
    }
    return RebuildRSObjectParcel(source, slots, source, false);
}

std::shared_ptr<MessageParcel> RSAshmemHelper::CreateAshmemParcel(std::shared_ptr<MessageParcel>& dataParcel)
{
    size_t dataSize = dataParcel->GetDataSize();
    RS_TRACE_NAME("CreateAshmemParcel data size:" + std::to_string(dataSize));

    // if want a full copy of parcel, need to save its data and fds both:
    // 1. save origin parcel data to ashmeme and record the fd to new parcel
    // 2. save all fds and their offsets in new parcel
    auto ashmemAllocator = AshmemAllocator::CreateAshmemAllocator(dataSize, PROT_READ | PROT_WRITE);
    if (!ashmemAllocator) {
        ROSEN_LOGE("CreateAshmemParcel failed, ashmemAllocator is nullptr");
        return nullptr;
    }
    if (!ashmemAllocator->WriteToAshmem(reinterpret_cast<void*>(dataParcel->GetData()), dataSize)) {
        ROSEN_LOGE("CreateAshmemParcel: WriteToAshmem failed");
        return nullptr;
    }
    // 1. save data
    int fd = ashmemAllocator->GetFd();
    std::shared_ptr<MessageParcel> ashmemParcel = std::make_shared<MessageParcel>();
    if (dataSize > UINT32_MAX ||
        !ashmemParcel->WriteInterfaceToken(RSIClientToRenderConnection::GetDescriptor()) ||
        !ashmemParcel->WriteInt32(1) || // 1: indicate ashmem parcel
        !ashmemParcel->WriteUint32(static_cast<uint32_t>(dataSize)) ||
        !ashmemParcel->WriteFileDescriptor(fd)) {
        return nullptr;
    }

    // The shared bytes are payload only. Every descriptor/object travels in
    // the outer Binder transaction and is decoded while that transaction lives.
    const auto* offsets = reinterpret_cast<const binder_size_t*>(dataParcel->GetObjectOffsets());
    const size_t count = dataParcel->GetOffsetsSize();
    std::vector<RSParcelObjectSlot> slots;
    if (count > INT32_MAX || !BuildRSObjectLayout(*dataParcel, offsets, count, slots) ||
        !ashmemParcel->WriteInt32(static_cast<int32_t>(count)) ||
        !ashmemParcel->WriteUint32(RS_OBJECT_SIDECAR_VERSION) ||
        (count > 0 && !ashmemParcel->WriteBuffer(offsets, count * sizeof(binder_size_t)))) {
        return nullptr;
    }
    const RSParcelReadPosition restore(*dataParcel);
    for (const auto& slot : slots) {
        if (!dataParcel->RewindRead(slot.offset) || !ashmemParcel->WriteUint32(slot.kind) ||
            !CopyRSObject(*dataParcel, *ashmemParcel, slot.kind) || dataParcel->GetReadPosition() != slot.end) {
            return nullptr;
        }
    }
    return ashmemParcel;
}

std::shared_ptr<MessageParcel> RSAshmemHelper::ParseFromAshmemParcel(MessageParcel* ashmemParcel,
    std::unique_ptr<AshmemFdWorker>& ashmemFdWorker,
    std::shared_ptr<AshmemFlowControlUnit> &ashmemFlowControlUnit, pid_t callingPid)
{
    if (!ashmemParcel) {
        ROSEN_LOGE("ParseFromAshmemParcel ashmemParcel is nullptr");
        return nullptr;
    }
    uint32_t dataSize{0};
    if (!ashmemParcel->ReadUint32(dataSize)) {
        ROSEN_LOGE("ParseFromAshmemParcel Read dataSize is failed");
        return nullptr;
    }
    RS_TRACE_NAME("ParseFromAshmemParcel data size:" + std::to_string(dataSize));
    // ashmem parcel flow control begins
    ashmemFlowControlUnit = AshmemFlowControlUnit::CheckOverflowAndCreateInstance(callingPid, dataSize);
    if (ashmemFlowControlUnit == nullptr) {
        // discard this ashmem parcel since callingPid is submitting too many data to RS simultaneously
        RS_TRACE_NAME_FMT("RSAshmemHelper::ParseFromAshmemParcel reject ashmem buffer size %" PRIu32
            " from pid %d", dataSize, static_cast<int>(callingPid));
        ROSEN_LOGE("RSAshmemHelper::ParseFromAshmemParcel reject ashmem buffer size %{public}" PRIu32
            " from pid %{public}d", dataSize, static_cast<int>(callingPid));
        return nullptr;
    }

    int fd = ashmemParcel->ReadFileDescriptor();
    auto ashmemAllocator = AshmemAllocator::CreateAshmemAllocatorWithFd(fd, dataSize, PROT_READ);
    if (!ashmemAllocator) {
        ROSEN_LOGE("ParseFromAshmemParcel failed, ashmemAllocator is nullptr");
        return nullptr;
    }
    // Only a private snapshot may hold object metadata or queued commands.
    void* data = ashmemAllocator->CopyFromAshmem(dataSize);
    if (data == nullptr) {
        return nullptr;
    }
    auto dataParcel = std::make_shared<MessageParcel>();
    if (!dataParcel->ParseFrom(reinterpret_cast<uintptr_t>(data), dataSize)) {
        free(data);
        return nullptr;
    }

    ashmemFdWorker.reset(); // Rebuilt MessageParcel owns all object/FD lifetimes.
    if (!RestoreRSObjectSidecar(dataParcel, *ashmemParcel)) {
        ROSEN_LOGE("ParseFromAshmemParcel: invalid or incomplete Binder object sidecar");
        return nullptr;
    }

    auto token = dataParcel->ReadInterfaceToken();
    if (token != RSIClientToRenderConnection::GetDescriptor()) {
        return nullptr;
    }

    if (dataParcel->ReadInt32() != 0) { // identify normal parcel
        ROSEN_LOGE("RSAshmemHelper::ParseFromAshmemParcel failed");
        return nullptr;
    }

    return dataParcel;
}
} // namespace Rosen
} // namespace OHOS
