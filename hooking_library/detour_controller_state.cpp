#include "detour_controller_state.h"
#include <MinHook.h>

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
	static bool __fastcall hook_get_controller_state(void* self, void* /*edx*/, vr::TrackedDeviceIndex_t controllerIndex, vr::VRControllerState_t * state, uint32_t stateSize) {
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

			/*
			if (g_config.enableAxisOverride &&
				g_config.overrideAxisIndex >= 0 &&
				g_config.overrideAxisIndex < 5) {
				state->rAxis[g_config.overrideAxisIndex].x = g_config.overrideX;
				state->rAxis[g_config.overrideAxisIndex].y = g_config.overrideY;
				log_line("[GetControllerState] axis override applied");
			}
			*/
		}
		else {
			log_line("[GetControllerState] invalid state buffer");
		}

		return result;
	}

	void Install_GetControllerState_Hook(VR_IVRSystem_FnTable * vfntable)
	{
		if (MH_CreateHook(vfntable->GetControllerState, hook_get_controller_state, (LPVOID*)&g_originalGetControllerState) == MH_OK)
		{
			MH_EnableHook(vfntable->GetControllerState);
		}
	}
}