/*
 * Copyright (C) 2024 Huawei Device Co., Ltd.
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

#include "native_module_ohos_ui_effect.h"
#include "hilog/log.h"

using OHOS::HiviewDFX::HiLog;
namespace OHOS {
namespace Rosen {
static napi_status RegisterPublicFilterModes(napi_env env, napi_value exports)
{
    // Public protocol values consumed by the existing native Filter methods.
    // These do not fabricate an effect, scene, device or renderer capability.
    struct Member { const char *name; int32_t value; };
    static const Member fly[] = {{"BOTTOM", 0}, {"TOP", 1}};
    static const Member ripple[] = {
        {"SMALL2MEDIUM_RECV", 0}, {"SMALL2MEDIUM_SEND", 1},
        {"SMALL2SMALL", 2}, {"MINI_RECV", 3},
    };
    struct Enum { const char *name; const Member *members; size_t count; };
    static const Enum enums[] = {
        {"FlyMode", fly, sizeof(fly) / sizeof(fly[0])},
        {"WaterRippleMode", ripple, sizeof(ripple) / sizeof(ripple[0])},
    };
    for (const auto &entry : enums) {
        napi_value object = nullptr;
        napi_status status = napi_create_object(env, &object);
        if (status != napi_ok) {
            return status;
        }
        for (size_t index = 0; index < entry.count; ++index) {
            napi_value value = nullptr;
            status = napi_create_int32(env, entry.members[index].value, &value);
            if (status != napi_ok) {
                return status;
            }
            status = napi_set_named_property(env, object, entry.members[index].name, value);
            if (status != napi_ok) {
                return status;
            }
        }
        status = napi_set_named_property(env, exports, entry.name, object);
        if (status != napi_ok) {
            return status;
        }
    }
    return napi_ok;
}

/*
 * Function registering all props and functions of ohos.effectlibrary module
 */
static napi_value Export(napi_env env, napi_value exports)
{
    if (RegisterPublicFilterModes(env, exports) != napi_ok) {
        return nullptr;
    }
    FilterNapi::Init(env, exports);
    EffectNapi::Init(env, exports);
    MaskNapi::Init(env, exports);
    return exports;
}

/*
 * module define
 */
static napi_module g_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Export,
    .nm_modname = "uieffect",
    .nm_priv = nullptr,
    .reserved = {0}
};

/*
 * module register
 */
extern "C" __attribute__((constructor)) void RegisterModule(void)
{
    napi_module_register(&g_module);
}
} // namespace Rosen
} // namespace OHOS