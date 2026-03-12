#include "detour_input.h"

#include "shared.h"

#include "MinHook.h"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace stepvr {

#if defined(_M_X64)
using GetActionSetHandleFn =
    vr::EVRInputError (*)(void* self, const char* actionSetName, vr::VRActionSetHandle_t* handle);
using GetActionHandleFn =
    vr::EVRInputError (*)(void* self, const char* actionName, vr::VRActionHandle_t* handle);
using GetInputSourceHandleFn =
    vr::EVRInputError (*)(void* self, const char* inputSourcePath, vr::VRInputValueHandle_t* handle);
using UpdateActionStateFn =
    vr::EVRInputError (*)(void* self, vr::VRActiveActionSet_t* sets, uint32_t setSize, uint32_t setCount);
using GetAnalogActionDataFn =
    vr::EVRInputError (*)(void* self,
                          vr::VRActionHandle_t action,
                          vr::InputAnalogActionData_t* actionData,
                          uint32_t actionDataSize,
                          vr::VRInputValueHandle_t restrictToDevice);
using GetDigitalActionDataFn =
    vr::EVRInputError (*)(void* self,
                          vr::VRActionHandle_t action,
                          vr::InputDigitalActionData_t* actionData,
                          uint32_t actionDataSize,
                          vr::VRInputValueHandle_t restrictToDevice);
using GetOriginTrackedDeviceInfoFn =
    vr::EVRInputError (*)(void* self,
                          vr::VRInputValueHandle_t origin,
                          vr::InputOriginInfo_t* originInfo,
                          uint32_t originInfoSize);
#else
using GetActionSetHandleFn =
    vr::EVRInputError (__thiscall*)(void* self, const char* actionSetName, vr::VRActionSetHandle_t* handle);
using HookGetActionSetHandleFn =
    vr::EVRInputError (__fastcall*)(void* self, void* edx, const char* actionSetName, vr::VRActionSetHandle_t* handle);

using GetActionHandleFn =
    vr::EVRInputError (__thiscall*)(void* self, const char* actionName, vr::VRActionHandle_t* handle);
using HookGetActionHandleFn =
    vr::EVRInputError (__fastcall*)(void* self, void* edx, const char* actionName, vr::VRActionHandle_t* handle);

using GetInputSourceHandleFn =
    vr::EVRInputError (__thiscall*)(void* self, const char* inputSourcePath, vr::VRInputValueHandle_t* handle);
using HookGetInputSourceHandleFn =
    vr::EVRInputError (__fastcall*)(void* self, void* edx, const char* inputSourcePath, vr::VRInputValueHandle_t* handle);

using UpdateActionStateFn =
    vr::EVRInputError (__thiscall*)(void* self, vr::VRActiveActionSet_t* sets, uint32_t setSize, uint32_t setCount);
using HookUpdateActionStateFn =
    vr::EVRInputError (__fastcall*)(void* self, void* edx, vr::VRActiveActionSet_t* sets, uint32_t setSize, uint32_t setCount);

using GetAnalogActionDataFn =
    vr::EVRInputError (__thiscall*)(void* self,
                                    vr::VRActionHandle_t action,
                                    vr::InputAnalogActionData_t* actionData,
                                    uint32_t actionDataSize,
                                    vr::VRInputValueHandle_t restrictToDevice);
using HookGetAnalogActionDataFn =
    vr::EVRInputError (__fastcall*)(void* self,
                                    void* edx,
                                    vr::VRActionHandle_t action,
                                    vr::InputAnalogActionData_t* actionData,
                                    uint32_t actionDataSize,
                                    vr::VRInputValueHandle_t restrictToDevice);

using GetDigitalActionDataFn =
    vr::EVRInputError (__thiscall*)(void* self,
                                    vr::VRActionHandle_t action,
                                    vr::InputDigitalActionData_t* actionData,
                                    uint32_t actionDataSize,
                                    vr::VRInputValueHandle_t restrictToDevice);
using HookGetDigitalActionDataFn =
    vr::EVRInputError (__fastcall*)(void* self,
                                    void* edx,
                                    vr::VRActionHandle_t action,
                                    vr::InputDigitalActionData_t* actionData,
                                    uint32_t actionDataSize,
                                    vr::VRInputValueHandle_t restrictToDevice);

using GetOriginTrackedDeviceInfoFn =
    vr::EVRInputError (__thiscall*)(void* self,
                                    vr::VRInputValueHandle_t origin,
                                    vr::InputOriginInfo_t* originInfo,
                                    uint32_t originInfoSize);
#endif

namespace {

constexpr float kAnalogEpsilon = 0.01f;

GetActionSetHandleFn g_originalGetActionSetHandle = nullptr;
GetActionHandleFn g_originalGetActionHandle = nullptr;
GetInputSourceHandleFn g_originalGetInputSourceHandle = nullptr;
UpdateActionStateFn g_originalUpdateActionState = nullptr;
GetAnalogActionDataFn g_originalGetAnalogActionData = nullptr;
GetDigitalActionDataFn g_originalGetDigitalActionData = nullptr;

// Nur zum Auflösen zusätzlicher Infos beim Logging.
// Diese Funktion wird NICHT selbst gehookt.
GetOriginTrackedDeviceInfoFn g_originTrackedDeviceInfo = nullptr;

std::mutex g_inputMutex;
std::unordered_map<vr::VRActionHandle_t, std::string> g_actionNames;
std::unordered_map<vr::VRActionSetHandle_t, std::string> g_actionSetNames;
std::unordered_map<vr::VRInputValueHandle_t, std::string> g_inputSourceNames;

struct ActiveActionSetSnapshot {
    vr::VRActionSetHandle_t actionSet = vr::k_ulInvalidActionSetHandle;
    vr::VRInputValueHandle_t restrictedToDevice = vr::k_ulInvalidInputValueHandle;
    vr::VRActionSetHandle_t secondaryActionSet = vr::k_ulInvalidActionSetHandle;
    int32_t priority = 0;

    bool operator==(const ActiveActionSetSnapshot& other) const {
        return actionSet == other.actionSet &&
               restrictedToDevice == other.restrictedToDevice &&
               secondaryActionSet == other.secondaryActionSet &&
               priority == other.priority;
    }
};

std::vector<ActiveActionSetSnapshot> g_lastActionSets;

struct ActionDeviceKey {
    vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
    vr::VRInputValueHandle_t restrictToDevice = vr::k_ulInvalidInputValueHandle;

    bool operator==(const ActionDeviceKey& other) const {
        return action == other.action && restrictToDevice == other.restrictToDevice;
    }
};

struct ActionDeviceKeyHash {
    size_t operator()(const ActionDeviceKey& key) const noexcept {
        const auto h1 = std::hash<uint64_t>{}(static_cast<uint64_t>(key.action));
        const auto h2 = std::hash<uint64_t>{}(static_cast<uint64_t>(key.restrictToDevice));
        return h1 ^ (h2 << 1);
    }
};

struct AnalogSnapshot {
    bool valid = false;
    vr::EVRInputError error = vr::VRInputError_None;
    vr::InputAnalogActionData_t data{};
};

struct DigitalSnapshot {
    bool valid = false;
    vr::EVRInputError error = vr::VRInputError_None;
    vr::InputDigitalActionData_t data{};
};

std::unordered_map<ActionDeviceKey, AnalogSnapshot, ActionDeviceKeyHash> g_lastAnalog;
std::unordered_map<ActionDeviceKey, DigitalSnapshot, ActionDeviceKeyHash> g_lastDigital;

std::string hex_u64(uint64_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << value << std::dec;
    return oss.str();
}

std::string action_name(vr::VRActionHandle_t handle) {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    auto it = g_actionNames.find(handle);
    if (it != g_actionNames.end()) {
        return it->second;
    }
    return "<action:" + hex_u64(handle) + ">";
}

std::string action_set_name(vr::VRActionSetHandle_t handle) {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    auto it = g_actionSetNames.find(handle);
    if (it != g_actionSetNames.end()) {
        return it->second;
    }
    return "<action_set:" + hex_u64(handle) + ">";
}

std::string input_source_name(vr::VRInputValueHandle_t handle) {
    if (handle == vr::k_ulInvalidInputValueHandle) {
        return "<any>";
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    auto it = g_inputSourceNames.find(handle);
    if (it != g_inputSourceNames.end()) {
        return it->second;
    }
    return "<source:" + hex_u64(handle) + ">";
}

std::string origin_details(void* self, vr::VRInputValueHandle_t origin) {
    std::ostringstream oss;
    oss << input_source_name(origin);

    if (!g_originTrackedDeviceInfo || origin == vr::k_ulInvalidInputValueHandle) {
        return oss.str();
    }

    vr::InputOriginInfo_t info{};
    const auto err = g_originTrackedDeviceInfo(
        self,
        origin,
        &info,
        static_cast<uint32_t>(sizeof(info)));

    if (err == vr::VRInputError_None) {
        oss << " idx=" << info.trackedDeviceIndex;
        if (info.rchRenderModelComponentName[0] != '\0') {
            oss << " comp=" << info.rchRenderModelComponentName;
        }
    }

    return oss.str();
}

bool float_changed(float lhs, float rhs) {
    return std::fabs(lhs - rhs) > kAnalogEpsilon;
}

bool should_log_analog(const ActionDeviceKey& key,
                       vr::EVRInputError error,
                       const vr::InputAnalogActionData_t& data) {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    auto& cached = g_lastAnalog[key];

    bool changed = !cached.valid ||
                   cached.error != error ||
                   cached.data.bActive != data.bActive ||
                   cached.data.activeOrigin != data.activeOrigin ||
                   float_changed(cached.data.x, data.x) ||
                   float_changed(cached.data.y, data.y) ||
                   float_changed(cached.data.z, data.z) ||
                   float_changed(cached.data.deltaX, data.deltaX) ||
                   float_changed(cached.data.deltaY, data.deltaY) ||
                   float_changed(cached.data.deltaZ, data.deltaZ);

    if (changed) {
        cached.valid = true;
        cached.error = error;
        cached.data = data;
    }

    return changed;
}

bool should_log_digital(const ActionDeviceKey& key,
                        vr::EVRInputError error,
                        const vr::InputDigitalActionData_t& data) {
    std::lock_guard<std::mutex> lock(g_inputMutex);
    auto& cached = g_lastDigital[key];

    bool changed = !cached.valid ||
                   cached.error != error ||
                   cached.data.bActive != data.bActive ||
                   cached.data.activeOrigin != data.activeOrigin ||
                   cached.data.bState != data.bState ||
                   cached.data.bChanged != data.bChanged;

    if (changed) {
        cached.valid = true;
        cached.error = error;
        cached.data = data;
    }

    return changed;
}

bool should_log_action_sets(vr::VRActiveActionSet_t* sets, uint32_t setCount) {
    std::vector<ActiveActionSetSnapshot> current;
    current.reserve(setCount);

    for (uint32_t i = 0; i < setCount; ++i) {
        ActiveActionSetSnapshot snap{};
        snap.actionSet = sets[i].ulActionSet;
        snap.restrictedToDevice = sets[i].ulRestrictedToDevice;
        snap.secondaryActionSet = sets[i].ulSecondaryActionSet;
        snap.priority = sets[i].nPriority;
        current.push_back(snap);
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    if (current == g_lastActionSets) {
        return false;
    }

    g_lastActionSets = std::move(current);
    return true;
}

std::string format_action_sets(vr::VRActiveActionSet_t* sets, uint32_t setCount) {
    std::ostringstream oss;
    oss << " sets=" << setCount;

    for (uint32_t i = 0; i < setCount; ++i) {
        oss << " ["
            << i
            << "] set=" << action_set_name(sets[i].ulActionSet)
            << " restricted=" << input_source_name(sets[i].ulRestrictedToDevice)
            << " secondary=" << action_set_name(sets[i].ulSecondaryActionSet)
            << " priority=" << sets[i].nPriority;
    }

    return oss.str();
}

void remember_action_name(vr::VRActionHandle_t handle, const char* actionName) {
    if (!actionName || handle == vr::k_ulInvalidActionHandle) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_actionNames[handle] = actionName;
}

void remember_action_set_name(vr::VRActionSetHandle_t handle, const char* actionSetName) {
    if (!actionSetName || handle == vr::k_ulInvalidActionSetHandle) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_actionSetNames[handle] = actionSetName;
}

void remember_input_source_name(vr::VRInputValueHandle_t handle, const char* inputSourcePath) {
    if (!inputSourcePath || handle == vr::k_ulInvalidInputValueHandle) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_inputMutex);
    g_inputSourceNames[handle] = inputSourcePath;
}

void install_one_hook(void* target, void* detour, void** original, const char* name) {
    const auto createRes = MH_CreateHook(target, detour, original);
    if (createRes != MH_OK) {
        std::ostringstream oss;
        oss << "[IVRInput] MH_CreateHook failed for " << name << " code=" << createRes;
        log_line(oss.str());
        return;
    }

    const auto enableRes = MH_EnableHook(target);
    if (enableRes != MH_OK) {
        std::ostringstream oss;
        oss << "[IVRInput] MH_EnableHook failed for " << name << " code=" << enableRes;
        log_line(oss.str());
        return;
    }

    std::ostringstream oss;
    oss << "[IVRInput] hooked " << name;
    log_line(oss.str());
}

#if defined(_M_X64)
vr::EVRInputError hook_get_action_set_handle(void* self,
                                             const char* actionSetName,
                                             vr::VRActionSetHandle_t* handle) {
#else
vr::EVRInputError __fastcall hook_get_action_set_handle(void* self,
                                                        void* /*edx*/,
                                                        const char* actionSetName,
                                                        vr::VRActionSetHandle_t* handle) {
#endif
    auto err = vr::VRInputError_InvalidHandle;
    if (g_originalGetActionSetHandle) {
        err = g_originalGetActionSetHandle(self, actionSetName, handle);
    }

    if (err == vr::VRInputError_None && handle) {
        remember_action_set_name(*handle, actionSetName);

        std::ostringstream oss;
        oss << "[IVRInput::GetActionSetHandle] "
            << (actionSetName ? actionSetName : "<null>")
            << " -> " << hex_u64(*handle);
        log_line(oss.str());
    }

    return err;
}

#if defined(_M_X64)
vr::EVRInputError hook_get_action_handle(void* self,
                                         const char* actionName,
                                         vr::VRActionHandle_t* handle) {
#else
vr::EVRInputError __fastcall hook_get_action_handle(void* self,
                                                    void* /*edx*/,
                                                    const char* actionName,
                                                    vr::VRActionHandle_t* handle) {
#endif
    auto err = vr::VRInputError_InvalidHandle;
    if (g_originalGetActionHandle) {
        err = g_originalGetActionHandle(self, actionName, handle);
    }

    if (err == vr::VRInputError_None && handle) {
        remember_action_name(*handle, actionName);

        std::ostringstream oss;
        oss << "[IVRInput::GetActionHandle] "
            << (actionName ? actionName : "<null>")
            << " -> " << hex_u64(*handle);
        log_line(oss.str());
    }

    return err;
}

#if defined(_M_X64)
vr::EVRInputError hook_get_input_source_handle(void* self,
                                               const char* inputSourcePath,
                                               vr::VRInputValueHandle_t* handle) {
#else
vr::EVRInputError __fastcall hook_get_input_source_handle(void* self,
                                                          void* /*edx*/,
                                                          const char* inputSourcePath,
                                                          vr::VRInputValueHandle_t* handle) {
#endif
    auto err = vr::VRInputError_InvalidHandle;
    if (g_originalGetInputSourceHandle) {
        err = g_originalGetInputSourceHandle(self, inputSourcePath, handle);
    }

    if (err == vr::VRInputError_None && handle) {
        remember_input_source_name(*handle, inputSourcePath);

        std::ostringstream oss;
        oss << "[IVRInput::GetInputSourceHandle] "
            << (inputSourcePath ? inputSourcePath : "<null>")
            << " -> " << hex_u64(*handle);
        log_line(oss.str());
    }

    return err;
}

#if defined(_M_X64)
vr::EVRInputError hook_update_action_state(void* self,
                                           vr::VRActiveActionSet_t* sets,
                                           uint32_t setSize,
                                           uint32_t setCount) {
#else
vr::EVRInputError __fastcall hook_update_action_state(void* self,
                                                      void* /*edx*/,
                                                      vr::VRActiveActionSet_t* sets,
                                                      uint32_t setSize,
                                                      uint32_t setCount) {
#endif
    auto err = vr::VRInputError_InvalidHandle;
    if (g_originalUpdateActionState) {
        err = g_originalUpdateActionState(self, sets, setSize, setCount);
    }

    if (err == vr::VRInputError_None && sets && setSize >= sizeof(vr::VRActiveActionSet_t)) {
        if (should_log_action_sets(sets, setCount)) {
            std::ostringstream oss;
            oss << "[IVRInput::UpdateActionState]"
                << format_action_sets(sets, setCount);
            log_line(oss.str());
        }
    }

    return err;
}

#if defined(_M_X64)
vr::EVRInputError hook_get_analog_action_data(void* self,
                                              vr::VRActionHandle_t action,
                                              vr::InputAnalogActionData_t* actionData,
                                              uint32_t actionDataSize,
                                              vr::VRInputValueHandle_t restrictToDevice) {
#else
vr::EVRInputError __fastcall hook_get_analog_action_data(void* self,
                                                         void* /*edx*/,
                                                         vr::VRActionHandle_t action,
                                                         vr::InputAnalogActionData_t* actionData,
                                                         uint32_t actionDataSize,
                                                         vr::VRInputValueHandle_t restrictToDevice) {
#endif
    auto err = vr::VRInputError_InvalidHandle;
    if (g_originalGetAnalogActionData) {
        err = g_originalGetAnalogActionData(self, action, actionData, actionDataSize, restrictToDevice);
    }

    if (!actionData || actionDataSize < sizeof(vr::InputAnalogActionData_t)) {
        return err;
    }

    const ActionDeviceKey key{ action, restrictToDevice };
    if (!should_log_analog(key, err, *actionData)) {
        return err;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "[IVRInput::GetAnalogActionData] "
        << action_name(action)
        << " restrict=" << input_source_name(restrictToDevice)
        << " err=" << static_cast<int>(err)
        << " active=" << static_cast<int>(actionData->bActive)
        << " origin=" << origin_details(self, actionData->activeOrigin)
        << " x=" << actionData->x
        << " y=" << actionData->y
        << " z=" << actionData->z
        << " dx=" << actionData->deltaX
        << " dy=" << actionData->deltaY
        << " dz=" << actionData->deltaZ
        << " t=" << actionData->fUpdateTime;

    log_line(oss.str());
    return err;
}

#if defined(_M_X64)
vr::EVRInputError hook_get_digital_action_data(void* self,
                                               vr::VRActionHandle_t action,
                                               vr::InputDigitalActionData_t* actionData,
                                               uint32_t actionDataSize,
                                               vr::VRInputValueHandle_t restrictToDevice) {
#else
vr::EVRInputError __fastcall hook_get_digital_action_data(void* self,
                                                          void* /*edx*/,
                                                          vr::VRActionHandle_t action,
                                                          vr::InputDigitalActionData_t* actionData,
                                                          uint32_t actionDataSize,
                                                          vr::VRInputValueHandle_t restrictToDevice) {
#endif
    auto err = vr::VRInputError_InvalidHandle;
    if (g_originalGetDigitalActionData) {
        err = g_originalGetDigitalActionData(self, action, actionData, actionDataSize, restrictToDevice);
    }

    if (!actionData || actionDataSize < sizeof(vr::InputDigitalActionData_t)) {
        return err;
    }

    const ActionDeviceKey key{ action, restrictToDevice };
    if (!should_log_digital(key, err, *actionData)) {
        return err;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "[IVRInput::GetDigitalActionData] "
        << action_name(action)
        << " restrict=" << input_source_name(restrictToDevice)
        << " err=" << static_cast<int>(err)
        << " active=" << static_cast<int>(actionData->bActive)
        << " origin=" << origin_details(self, actionData->activeOrigin)
        << " state=" << static_cast<int>(actionData->bState)
        << " changed=" << static_cast<int>(actionData->bChanged)
        << " t=" << actionData->fUpdateTime;

    log_line(oss.str());
    return err;
}

} // namespace

void Install_IVRInput_Hooks(VR_IVRInput_FnTable* ivrinput_fntable) {
    if (!ivrinput_fntable) {
        log_line("[IVRInput] invalid fn table");
        return;
    }

    g_originTrackedDeviceInfo =
        reinterpret_cast<GetOriginTrackedDeviceInfoFn>(ivrinput_fntable->GetOriginTrackedDeviceInfo);

    install_one_hook(
        reinterpret_cast<void*>(ivrinput_fntable->GetActionSetHandle),
        reinterpret_cast<void*>(hook_get_action_set_handle),
        reinterpret_cast<void**>(&g_originalGetActionSetHandle),
        "GetActionSetHandle");

    install_one_hook(
        reinterpret_cast<void*>(ivrinput_fntable->GetActionHandle),
        reinterpret_cast<void*>(hook_get_action_handle),
        reinterpret_cast<void**>(&g_originalGetActionHandle),
        "GetActionHandle");

    install_one_hook(
        reinterpret_cast<void*>(ivrinput_fntable->GetInputSourceHandle),
        reinterpret_cast<void*>(hook_get_input_source_handle),
        reinterpret_cast<void**>(&g_originalGetInputSourceHandle),
        "GetInputSourceHandle");

    install_one_hook(
        reinterpret_cast<void*>(ivrinput_fntable->UpdateActionState),
        reinterpret_cast<void*>(hook_update_action_state),
        reinterpret_cast<void**>(&g_originalUpdateActionState),
        "UpdateActionState");

    install_one_hook(
        reinterpret_cast<void*>(ivrinput_fntable->GetAnalogActionData),
        reinterpret_cast<void*>(hook_get_analog_action_data),
        reinterpret_cast<void**>(&g_originalGetAnalogActionData),
        "GetAnalogActionData");

    install_one_hook(
        reinterpret_cast<void*>(ivrinput_fntable->GetDigitalActionData),
        reinterpret_cast<void*>(hook_get_digital_action_data),
        reinterpret_cast<void**>(&g_originalGetDigitalActionData),
        "GetDigitalActionData");
}

} // namespace stepvr