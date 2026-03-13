#include "forward_ingress.h"

#include "shared.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace stepvr {

namespace {

std::mutex g_ingressMutex;
HANDLE g_forwardIngressMapping = nullptr;
const void* g_forwardIngressView = nullptr;
uint64_t g_nextReconnectTick = 0;
bool g_loggedConnected = false;
bool g_loggedInvalid = false;
bool g_loggedStale = false;

constexpr uint64_t kReconnectIntervalMs = 1000;

void close_forward_ingress_locked() {
    if (g_forwardIngressView) {
        UnmapViewOfFile(g_forwardIngressView);
        g_forwardIngressView = nullptr;
    }

    if (g_forwardIngressMapping) {
        CloseHandle(g_forwardIngressMapping);
        g_forwardIngressMapping = nullptr;
    }
}

void try_open_forward_ingress_locked() {
    if (g_forwardIngressView) {
        return;
    }

    const uint64_t now = GetTickCount64();
    if (now < g_nextReconnectTick) {
        return;
    }

    g_nextReconnectTick = now + kReconnectIntervalMs;

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kForwardIngressMapName);
    if (!mapping) {
        return;
    }

    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ForwardIngressSharedState));
    if (!view) {
        CloseHandle(mapping);
        return;
    }

    g_forwardIngressMapping = mapping;
    g_forwardIngressView = view;

    if (!g_loggedConnected) {
        g_loggedConnected = true;
        log_line("[ForwardIngress] connected");
    }
}

bool read_shared_state_locked(ForwardIngressSharedState& state) {
    try_open_forward_ingress_locked();

    if (!g_forwardIngressView) {
        return false;
    }

    std::memcpy(&state, g_forwardIngressView, sizeof(state));

    if (state.magic != kForwardIngressMagic || state.version != kForwardIngressVersion) {
        if (!g_loggedInvalid) {
            g_loggedInvalid = true;
            log_line("[ForwardIngress] invalid shared state header");
        }

        close_forward_ingress_locked();
        return false;
    }

    const uint64_t now = GetTickCount64();
    if (state.writerTickMs == 0 || (now - state.writerTickMs) > kForwardIngressStaleAfterMs) {
        if (!g_loggedStale) {
            g_loggedStale = true;
            log_line("[ForwardIngress] stale writer heartbeat");
        }
        return false;
    }

    g_loggedStale = false;
    return true;
}

} // namespace

bool read_forward_ingress_snapshot(ForwardIngressSnapshot& out) {
    out = {};

    std::lock_guard<std::mutex> lock(g_ingressMutex);

    ForwardIngressSharedState shared{};
    if (!read_shared_state_locked(shared)) {
        return false;
    }

    out.available = true;
    out.enabled = (shared.enabled != 0);
    out.forwardY = std::clamp(shared.forwardY, 0.0f, 1.0f);
    out.seq = shared.seq;
    return true;
}

} // namespace stepvr