/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
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

#include "native_module_ohos_effect.h"
#include "hilog/log.h"
#include "effect_utils.h"
#include "color_picker.h"

using OHOS::HiviewDFX::HiLog;
namespace OHOS {
namespace Rosen {
/*
 * Function registering all props and functions of ohos.effectlibrary module
 */
static napi_status RegisterPictureLightDegree(napi_env env, napi_value exports)
{
    struct Member {
        const char *name;
        int32_t value;
    };
    const Member members[] = {
        {"UNKNOWN_LIGHT_COLOR_DEGREE_PICTURE", static_cast<int32_t>(UNKOWN_LIGHT_COLOR_DEGREE_PICTURE)},
        {"EXTREMELY_LIGHT_COLOR_PICTURE", static_cast<int32_t>(EXTREMELY_LIGHT_COLOR_PICTURE)},
        {"LIGHT_COLOR_PICTURE", static_cast<int32_t>(LIGHT_COLOR_PICTURE)},
        {"DARK_COLOR_PICTURE", static_cast<int32_t>(DARK_COLOR_PICTURE)},
        {"EXTREMELY_DARK_COLOR_PICTURE", static_cast<int32_t>(EXTREMELY_DARK_COLOR_PICTURE)},
        {"FLOWERY_PICTURE", static_cast<int32_t>(FLOWERY_PICTURE)},
        {"EXTREMELY_FLOWERY_PICTURE", static_cast<int32_t>(EXTREMELY_FLOWERY_PICTURE)},
    };
    napi_value object = nullptr;
    napi_status status = napi_create_object(env, &object);
    if (status != napi_ok) {
        return status;
    }
    for (const auto &member : members) {
        napi_value value = nullptr;
        status = napi_create_int32(env, member.value, &value);
        if (status != napi_ok) {
            return status;
        }
        status = napi_set_named_property(env, object, member.name, value);
        if (status != napi_ok) {
            return status;
        }
    }
    return napi_set_named_property(env, exports, "PictureLightDegree", object);
}

static napi_value Export(napi_env env, napi_value exports)
{
    EFFECT_LOG_I("ColorPicker Napi CALL");
    if (RegisterPictureLightDegree(env, exports) != napi_ok) {
        return nullptr;
    }
    ColorPickerNapi::Init(env, exports);
    FilterNapi::Init(env, exports);
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
    .nm_modname = "effectKit",
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
