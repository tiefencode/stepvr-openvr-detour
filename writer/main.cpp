#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>

#include "hooking_library/forward_ingress.h"

namespace {

constexpr DWORD kInvalidControllerIndex = 0xFFFFFFFFu;
constexpr auto kPollInterval = std::chrono::milliseconds(8);
constexpr float kPrintEpsilon = 0.01f;

std::atomic<bool> g_running{ true };

BOOL WINAPI console_ctrl_handler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_running.store(false);
        return TRUE;
    default:
        return FALSE;
    }
}

float normalize_forward_from_left_stick_y(SHORT rawY) {
    if (rawY <= XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        return 0.0f;
    }

    constexpr float kMaxPositive = 32767.0f;
    const float numerator = static_cast<float>(rawY) - static_cast<float>(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    const float denominator = kMaxPositive - static_cast<float>(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);

    if (denominator <= 0.0f) {
        return 0.0f;
    }

    return std::clamp(numerator / denominator, 0.0f, 1.0f);
}

bool try_get_state(DWORD userIndex, XINPUT_STATE& state) {
    std::memset(&state, 0, sizeof(state));
    return XInputGetState(userIndex, &state) == ERROR_SUCCESS;
}

bool find_connected_controller(DWORD& activeIndex, XINPUT_STATE& state) {
    if (activeIndex != kInvalidControllerIndex) {
        if (try_get_state(activeIndex, state)) {
            return true;
        }
    }

    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (try_get_state(i, state)) {
            activeIndex = i;
            return true;
        }
    }

    activeIndex = kInvalidControllerIndex;
    return false;
}

void write_shared_state(stepvr::ForwardIngressSharedState& shared,
                        bool enabled,
                        float forwardY,
                        uint64_t seq) {
    shared.magic = stepvr::kForwardIngressMagic;
    shared.version = stepvr::kForwardIngressVersion;
    shared.enabled = enabled ? 1u : 0u;
    shared.forwardY = std::clamp(forwardY, 0.0f, 1.0f);
    shared.writerTickMs = GetTickCount64();
    shared.seq = seq;
}

} // namespace

int main() {
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        std::cerr << "failed to install console control handler" << std::endl;
        return 1;
    }

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(stepvr::ForwardIngressSharedState)),
        stepvr::kForwardIngressMapName);

    if (!mapping) {
        std::cerr << "CreateFileMappingW failed: " << GetLastError() << std::endl;
        return 1;
    }

    auto* shared = reinterpret_cast<stepvr::ForwardIngressSharedState*>(
        MapViewOfFile(
            mapping,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            sizeof(stepvr::ForwardIngressSharedState)));

    if (!shared) {
        std::cerr << "MapViewOfFile failed: " << GetLastError() << std::endl;
        CloseHandle(mapping);
        return 1;
    }

    std::memset(shared, 0, sizeof(*shared));

    uint64_t seq = 1;
    write_shared_state(*shared, false, 0.0f, seq);

    std::cout << "stepvr_xinput_writer running" << std::endl;
    std::cout << "shared mapping: Local\\StepVRForwardState" << std::endl;
    std::cout << "input: Xbox left stick Y+ -> forwardY" << std::endl;
    std::cout << "press Ctrl+C to quit" << std::endl;

    DWORD activeController = kInvalidControllerIndex;
    DWORD lastPrintedController = kInvalidControllerIndex;
    DWORD lastPacketNumber = 0;
    bool lastEnabled = false;
    float lastForwardY = -1.0f;
    bool hadController = false;
    bool printedWaiting = false;

    while (g_running.load()) {
        XINPUT_STATE state{};
        const bool connected = find_connected_controller(activeController, state);

        bool enabled = false;
        float forwardY = 0.0f;

        if (connected) {
            hadController = true;
            printedWaiting = false;

            forwardY = normalize_forward_from_left_stick_y(state.Gamepad.sThumbLY);
            enabled = (forwardY > 0.0f);

            const bool packetChanged = state.dwPacketNumber != lastPacketNumber;
            const bool controllerChanged = activeController != lastPrintedController;
            const bool valueChanged = std::fabs(forwardY - lastForwardY) > kPrintEpsilon;
            const bool enabledChanged = enabled != lastEnabled;

            if (controllerChanged || packetChanged || valueChanged || enabledChanged) {
                ++seq;

                std::cout
                    << "controller=" << activeController
                    << " packet=" << state.dwPacketNumber
                    << " rawLY=" << state.Gamepad.sThumbLY
                    << " forwardY=" << std::fixed << std::setprecision(3) << forwardY
                    << " enabled=" << (enabled ? 1 : 0)
                    << std::endl;

                lastPrintedController = activeController;
                lastPacketNumber = state.dwPacketNumber;
                lastForwardY = forwardY;
                lastEnabled = enabled;
            }
        } else {
            if (hadController && !printedWaiting) {
                std::cout << "no XInput controller connected" << std::endl;
                printedWaiting = true;
            }

            hadController = false;
            activeController = kInvalidControllerIndex;

            if (lastEnabled || std::fabs(lastForwardY) > kPrintEpsilon) {
                ++seq;
                lastEnabled = false;
                lastForwardY = 0.0f;
                lastPacketNumber = 0;
                lastPrintedController = kInvalidControllerIndex;
            }

            enabled = false;
            forwardY = 0.0f;
        }

        write_shared_state(*shared, enabled, forwardY, seq);
        std::this_thread::sleep_for(kPollInterval);
    }

    ++seq;
    write_shared_state(*shared, false, 0.0f, seq);

    UnmapViewOfFile(shared);
    CloseHandle(mapping);

    std::cout << "writer stopped" << std::endl;
    return 0;
}