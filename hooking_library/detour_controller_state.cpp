#include "detour_controller_state.h"

#include "MinHook.h"
#include "shared.h"

#include <array>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace stepvr {

#if defined(_M_X64)
using GetControllerStateFn =
    bool (*)(void* self,
             vr::TrackedDeviceIndex_t controllerIndex,
             vr::VRControllerState_t* state,
             uint32_t stateSize);
#else
using GetControllerStateFn =
    bool(__thiscall*)(void* self,
                      vr::TrackedDeviceIndex_t controllerIndex,
                      vr::VRControllerState_t* state,
                      uint32_t stateSize);
using HookGetControllerStateFn =
    bool(__fastcall*)(void* self,
                      void* edx,
                      vr::TrackedDeviceIndex_t controllerIndex,
                      vr::VRControllerState_t* state,
                      uint32_t stateSize);
#endif

static GetControllerStateFn g_originalGetControllerState = nullptr;

namespace {

constexpr float kAxisEpsilon = 0.01f;

struct CachedControllerState {
    bool valid = false;
    bool result = false;
    uint32_t stateSize = 0;
    vr::VRControllerState_t state{};
};

std::array<CachedControllerState, vr::k_unMaxTrackedDeviceCount> g_lastStates{};
std::mutex g_lastStatesMutex;

bool axis_changed(const vr::VRControllerAxis_t& lhs, const vr::VRControllerAxis_t& rhs) {
    return std::fabs(lhs.x - rhs.x) > kAxisEpsilon ||
           std::fabs(lhs.y - rhs.y) > kAxisEpsilon;
}

bool should_log_state(vr::TrackedDeviceIndex_t controllerIndex,
                      bool result,
                      const vr::VRControllerState_t& state,
                      uint32_t stateSize) {
    if (controllerIndex >= g_lastStates.size()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_lastStatesMutex);
    auto& cached = g_lastStates[controllerIndex];

    bool changed = !cached.valid ||
                   cached.result != result ||
                   cached.stateSize != stateSize ||
                   cached.state.unPacketNum != state.unPacketNum ||
                   cached.state.ulButtonPressed != state.ulButtonPressed ||
                   cached.state.ulButtonTouched != state.ulButtonTouched;

    if (!changed) {
        for (int i = 0; i < 5; ++i) {
            if (axis_changed(cached.state.rAxis[i], state.rAxis[i])) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        cached.valid = true;
        cached.result = result;
        cached.stateSize = stateSize;
        cached.state = state;
    }

    return changed;
}

std::string axis_to_string(const vr::VRControllerState_t* state) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);

    for (int i = 0; i < 5; ++i) {
        oss << " a" << i
            << "(" << state->rAxis[i].x
            << "," << state->rAxis[i].y
            << ")";
    }

    return oss.str();
}

} // namespace

#if defined(_M_X64)
static bool hook_get_controller_state(void* self,
                                      vr::TrackedDeviceIndex_t controllerIndex,
                                      vr::VRControllerState_t* state,
                                      uint32_t stateSize) {
#else
static bool __fastcall hook_get_controller_state(void* self,
                                                 void* /*edx*/,
                                                 vr::TrackedDeviceIndex_t controllerIndex,
                                                 vr::VRControllerState_t* state,
                                                 uint32_t stateSize) {
#endif
    bool result = false;
    if (g_originalGetControllerState) {
        result = g_originalGetControllerState(self, controllerIndex, state, stateSize);
    }

    if (state && stateSize >= sizeof(vr::VRControllerState_t)) {
        if (should_log_state(controllerIndex, result, *state, stateSize)) {
            std::ostringstream oss;
            oss << "[GetControllerState] idx=" << controllerIndex
                << " result=" << result
                << " packet=" << state->unPacketNum
                << " pressed=0x" << std::hex << state->ulButtonPressed
                << " touched=0x" << std::hex << state->ulButtonTouched
                << std::dec
                << axis_to_string(state);
            log_line(oss.str());
        }

        /*
        if (g_config.enableAxisOverride &&
            g_config.overrideAxisIndex >= 0 &&
            g_config.overrideAxisIndex < 5) {
            state->rAxis[g_config.overrideAxisIndex].x = g_config.overrideX;
            state->rAxis[g_config.overrideAxisIndex].y = g_config.overrideY;
            log_line("[GetControllerState] axis override applied");
        }
        */
    } else {
        static bool s_loggedInvalidBuffer = false;
        if (!s_loggedInvalidBuffer) {
            s_loggedInvalidBuffer = true;
            log_line("[GetControllerState] invalid state buffer");
        }
    }

    return result;
}

void Install_GetControllerState_Hook(VR_IVRSystem_FnTable* vfntable) {
    if (MH_CreateHook(vfntable->GetControllerState,
                      hook_get_controller_state,
                      reinterpret_cast<LPVOID*>(&g_originalGetControllerState)) == MH_OK) {
        MH_EnableHook(vfntable->GetControllerState);
    }
}

} // namespace stepvr