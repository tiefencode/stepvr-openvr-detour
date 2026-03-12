#pragma once

#include <openvr_capi.h>

namespace stepvr {
void Install_IVRInput_Hooks(struct VR_IVRInput_FnTable* ivrinput_fntable);
}