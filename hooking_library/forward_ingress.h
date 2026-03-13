#pragma once

#include <cstdint>

namespace stepvr {

constexpr wchar_t kForwardIngressMapName[] = L"Local\\StepVRForwardState";

constexpr uint32_t kForwardIngressMagic = 0x53545652u; // "STVR"
constexpr uint32_t kForwardIngressVersion = 1;
constexpr uint64_t kForwardIngressStaleAfterMs = 250;

struct ForwardIngressSharedState {
    uint32_t magic = kForwardIngressMagic;
    uint32_t version = kForwardIngressVersion;
    uint32_t enabled = 0;       // 0 = aus, !=0 = an
    float forwardY = 0.0f;      // 0.0 .. 1.0
    uint64_t writerTickMs = 0;  // heartbeat
    uint64_t seq = 0;           // optional für Debug
};

static_assert(sizeof(ForwardIngressSharedState) == 32, "Unexpected shared ingress layout");

struct ForwardIngressSnapshot {
    bool available = false;
    bool enabled = false;
    float forwardY = 0.0f;
    uint64_t seq = 0;
};

bool read_forward_ingress_snapshot(ForwardIngressSnapshot& out);

} // namespace stepvr