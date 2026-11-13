/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
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

#ifndef ROSEN_RENDER_SERVICE_BASE_TRANSACTION_RS_CLIENT_TO_RENDER_CONNECTION_STUB_H
#define ROSEN_RENDER_SERVICE_BASE_TRANSACTION_RS_CLIENT_TO_RENDER_CONNECTION_STUB_H

#include <cstddef>
#include <unordered_set>
#include <iremote_stub.h>
#include <message_option.h>
#include <message_parcel.h>

#include "platform/ohos/transaction/zidl/rs_iclient_to_render_connection.h"
#include "platform/ohos/transaction/rs_iclient_to_render_connection_ipc_interface_code_access_verifier.h"
#include "ipc_security/rs_ipc_interface_code_security_manager.h"
#include "transaction/rs_render_service_security_utils.h"

namespace OHOS {
namespace Rosen {
// Internal request-scoped identity; not a Parcel field or public IPC API.
class RSScopedAsyncCallerPid final {
public:
    explicit RSScopedAsyncCallerPid(pid_t callingPid) : previous_(Current())
    {
        Current() = callingPid;
    }

    ~RSScopedAsyncCallerPid()
    {
        Current() = previous_;
    }

    RSScopedAsyncCallerPid(const RSScopedAsyncCallerPid&) = delete;
    RSScopedAsyncCallerPid& operator=(const RSScopedAsyncCallerPid&) = delete;

    static pid_t Get()
    {
        return Current();
    }

private:
    static pid_t& Current()
    {
        static thread_local pid_t callingPid = 0;
        return callingPid;
    }

    const pid_t previous_;
};

// Receiver-observed size, not an application-supplied accounting field.
class RSScopedCommitPayloadSize final {
public:
    explicit RSScopedCommitPayloadSize(size_t bytes) : previous_(Current())
    {
        Current() = bytes;
    }
    ~RSScopedCommitPayloadSize()
    {
        Current() = previous_;
    }
    RSScopedCommitPayloadSize(const RSScopedCommitPayloadSize&) = delete;
    RSScopedCommitPayloadSize& operator=(const RSScopedCommitPayloadSize&) = delete;
    static size_t Get()
    {
        return Current();
    }

private:
    static size_t& Current()
    {
        static thread_local size_t bytes = 0;
        return bytes;
    }
    const size_t previous_;
};

class RSClientToRenderConnectionStub : public IRemoteStub<RSIClientToRenderConnection> {
public:
    RSClientToRenderConnectionStub() = default;
    ~RSClientToRenderConnectionStub() noexcept = default;

    int OnRemoteRequest(uint32_t code, MessageParcel& data, MessageParcel& reply, MessageOption& option) override;

private:
    static const RSInterfaceCodeSecurityManager securityManager_;
    virtual std::string GetBundleName(pid_t pid) { return {}; }
    virtual bool RegisterSurfaceTransactionListenerNew(sptr<RSISurfaceTransactionListener> listener,
    uint64_t listenerId, uint32_t pid, uint32_t tid) { return 0; }
    bool ReadDataBaseRs(DataBaseRs& info, MessageParcel& data);
    bool ReadSurfaceCaptureConfig(RSSurfaceCaptureConfig& captureConfig, MessageParcel& data);
    bool ReadSurfaceCaptureBlurParam(RSSurfaceCaptureBlurParam& blurParam, MessageParcel& data);
    bool ReadSurfaceCaptureAreaRect(Drawing::Rect& specifiedAreaRect, MessageParcel& data);
    void SetQos();
    RSRenderServiceSecurityUtils securityUtils_;
    std::unordered_set<int> tids_;
    std::mutex mutex_;
};
} // namespace Rosen
} // namespace OHOS

#endif // ROSEN_RENDER_SERVICE_BASE_TRANSACTION_RS_CLIENT_TO_RENDER_CONNECTION_STUB_H