#include "shared.h"

#include <iomanip>
#include <sstream>
#include <cstring>

namespace stepvr {

#if defined(_M_X64)
using GetControllerStateFn = bool(*)(void* self, vr::TrackedDeviceIndex_t controllerIndex, vr::VRControllerState_t* state, uint32_t stateSize);
using HookGetControllerStateFn = bool(*)(void* self, vr::TrackedDeviceIndex_t controllerIndex, vr::VRControllerState_t* state, uint32_t stateSize);
#else
using GetControllerStateFn = bool(__thiscall*)(void* self, vr::TrackedDeviceIndex_t controllerIndex, vr::VRControllerState_t* state, uint32_t stateSize);
using HookGetControllerStateFn = bool(__fastcall*)(void* self, void* edx, vr::TrackedDeviceIndex_t controllerIndex, vr::VRControllerState_t* state, uint32_t stateSize);
#endif

static GetControllerStateFn g_originalGetControllerState = nullptr;
static void** g_vtable = nullptr;
static bool g_hookInstalled = false;

static std::string axis_to_string(const vr::VRControllerState_t* state) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    for (int i = 0; i < 5; ++i) {
        oss << " a" << i << "(" << state->rAxis[i].x << "," << state->rAxis[i].y << ")";
    }
    return oss.str();
}

#if defined(_M_X64)
static bool hook_get_controller_state(void* self, vr::TrackedDeviceIndex_t controllerIndex, vr::VRControllerState_t* state, uint32_t stateSize) {
#else
static bool __fastcall hook_get_controller_state(void* self, void* /*edx*/, vr::TrackedDeviceIndex_t controllerIndex, vr::VRControllerState_t* state, uint32_t stateSize) {
#endif
    bool result = false;

    if (g_originalGetControllerState) {
        result = g_originalGetControllerState(self, controllerIndex, state, stateSize);
    }

    if (state && stateSize >= sizeof(vr::VRControllerState_t)) {
        std::ostringstream oss;
        oss << "[GetControllerState] idx=" << controllerIndex
            << " result=" << result
            << " packet=" << state->unPacketNum
            << " pressed=0x" << std::hex << state->ulButtonPressed
            << " touched=0x" << std::hex << state->ulButtonTouched
            << std::dec
            << axis_to_string(state);
        log_line(oss.str());

        if (g_config.enableAxisOverride &&
            g_config.overrideAxisIndex >= 0 &&
            g_config.overrideAxisIndex < 5) {
            state->rAxis[g_config.overrideAxisIndex].x = g_config.overrideX;
            state->rAxis[g_config.overrideAxisIndex].y = g_config.overrideY;
            log_line("[GetControllerState] axis override applied");
        }
    } else {
        log_line("[GetControllerState] invalid state buffer");
    }

    return result;
}

static bool patch_vtable_entry(void** vtable, size_t index, void* replacement, void** originalOut) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    *originalOut = vtable[index];
    vtable[index] = replacement;

    DWORD dummy = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &dummy);
    return true;
}

void inspect_and_optionally_hook(vr::IVRSystem* vrSystem) {
    if (!vrSystem) {
        log_line("inspect_and_optionally_hook: vrSystem is null");
        return;
    }

    if (g_vrSystemProcessed.exchange(true)) {
        log_line("inspect_and_optionally_hook: already processed");
        return;
    }

    g_vtable = *reinterpret_cast<void***>(vrSystem);
    if (!g_vtable) {
        log_line("inspect_and_optionally_hook: vtable missing");
        return;
    }

    log_line("inspect_and_optionally_hook: IVRSystem vtable acquired");

    for (size_t i = 0; i < 50; ++i) {
        std::ostringstream oss;
        oss << "vtable[" << i << "] = 0x" << std::hex << reinterpret_cast<uintptr_t>(g_vtable[i]);
        log_line(oss.str());
    }

    if (!g_config.enableDetour) {
        log_line("GetControllerState detour disabled by config");
        return;
    }

    void* original = nullptr;
    const bool ok = patch_vtable_entry(
        g_vtable,
        g_config.getControllerStateIndex,
        reinterpret_cast<void*>(&hook_get_controller_state),
        &original
    );

    if (!ok) {
        log_line("failed to patch GetControllerState vtable entry");
        return;
    }

    g_originalGetControllerState = reinterpret_cast<GetControllerStateFn>(original);
    g_hookInstalled = true;

    {
        std::ostringstream oss;
        oss << "GetControllerState detour installed at vtable index "
            << g_config.getControllerStateIndex
            << ", original=0x" << std::hex << reinterpret_cast<uintptr_t>(original);
        log_line(oss.str());
    }
}

}