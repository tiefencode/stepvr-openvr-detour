#pragma once

#include <openvr_capi.h>


namespace stepvr {
	void Install_GetControllerState_Hook(VR_IVRSystem_FnTable* ivrsytem_fntable);
}