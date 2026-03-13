#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "hooking_library/forward_ingress.h"

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(8);
constexpr uint64_t kRescanIntervalMs = 1000;
constexpr float kPrintEpsilon = 0.01f;
constexpr float kCenterDeadzone = 0.04f;

constexpr wchar_t kPreferredDeviceName[] = L"StepVR ATOM Stepper";
constexpr wchar_t kPreferredToken1[] = L"stepvr";
constexpr wchar_t kPreferredToken2[] = L"stepper";

std::atomic<bool> g_running{ true };

struct JoystickDevice {
    UINT id = std::numeric_limits<UINT>::max();
    JOYCAPSW caps{};
    bool valid = false;
};

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

std::wstring to_lower_copy(std::wstring value) {
    for (auto& ch : value) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

bool contains_icase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }

    const auto haystackLower = to_lower_copy(haystack);
    const auto needleLower = to_lower_copy(needle);
    return haystackLower.find(needleLower) != std::wstring::npos;
}

std::wstring device_name_from_caps(const JOYCAPSW& caps) {
    return std::wstring(caps.szPname);
}

bool query_caps(UINT id, JOYCAPSW& caps) {
    std::memset(&caps, 0, sizeof(caps));
    return joyGetDevCapsW(id, &caps, sizeof(caps)) == JOYERR_NOERROR;
}

MMRESULT query_state(UINT id, JOYINFOEX& info) {
    std::memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);
    info.dwFlags = JOY_RETURNY;
    return joyGetPosEx(id, &info);
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

float normalize_forward_from_y(const JOYCAPSW& caps, DWORD rawY) {
    const double minY = static_cast<double>(caps.wYmin);
    const double maxY = static_cast<double>(caps.wYmax);

    if (maxY <= minY) {
        return 0.0f;
    }

    const double center = (minY + maxY) * 0.5;
    const double halfRange = (maxY - minY) * 0.5;

    if (halfRange <= 0.0) {
        return 0.0f;
    }

    // ESP32 sends forward by moving Y upward toward its minimum.
    // So:
    //   minY    -> forward = 1.0
    //   center  -> forward = 0.0
    //   maxY    -> backward / ignored here
    double signedValue = (center - static_cast<double>(rawY)) / halfRange;
    double forward = std::clamp(signedValue, 0.0, 1.0);

    if (forward <= kCenterDeadzone) {
        return 0.0f;
    }

    forward = (forward - kCenterDeadzone) / (1.0 - kCenterDeadzone);
    return static_cast<float>(std::clamp(forward, 0.0, 1.0));
}

JoystickDevice find_best_stepper_device(std::vector<JoystickDevice>& connectedDevices) {
    connectedDevices.clear();

    const UINT numDevices = joyGetNumDevs();

    for (UINT id = 0; id < numDevices; ++id) {
        JOYCAPSW caps{};
        if (!query_caps(id, caps)) {
            continue;
        }

        JOYINFOEX info{};
        if (query_state(id, info) != JOYERR_NOERROR) {
            continue;
        }

        JoystickDevice device{};
        device.id = id;
        device.caps = caps;
        device.valid = true;
        connectedDevices.push_back(device);
    }

    for (const auto& device : connectedDevices) {
        const auto name = device_name_from_caps(device.caps);

        if (contains_icase(name, kPreferredDeviceName)) {
            return device;
        }
    }

    for (const auto& device : connectedDevices) {
        const auto name = device_name_from_caps(device.caps);

        if (contains_icase(name, kPreferredToken1) &&
            contains_icase(name, kPreferredToken2)) {
            return device;
        }
    }

    // Fallback: if exactly one joystick/gamepad is connected, use it.
    if (connectedDevices.size() == 1) {
        return connectedDevices.front();
    }

    return {};
}

void print_connected_devices(const std::vector<JoystickDevice>& devices) {
    if (devices.empty()) {
        std::wcout << L"no WinMM joystick devices currently connected" << std::endl;
        return;
    }

    std::wcout << L"connected WinMM joystick devices:" << std::endl;
    for (const auto& device : devices) {
        std::wcout
            << L"  id=" << device.id
            << L" name=\"" << device_name_from_caps(device.caps) << L"\""
            << L" axes=" << device.caps.wNumAxes
            << L" buttons=" << device.caps.wNumButtons
            << L" Y=[" << device.caps.wYmin << L"," << device.caps.wYmax << L"]"
            << std::endl;
    }
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

    std::wcout << L"stepvr_stepper_writer running" << std::endl;
    std::wcout << L"shared mapping: " << stepvr::kForwardIngressMapName << std::endl;
    std::wcout << L"preferred device name: \"" << kPreferredDeviceName << L"\"" << std::endl;
    std::wcout << L"input path: WinMM joystick Y axis -> forwardY" << std::endl;
    std::wcout << L"press Ctrl+C to quit" << std::endl;

    JoystickDevice selected{};
    uint64_t nextRescanTick = 0;
    bool printedWaiting = false;

    bool lastEnabled = false;
    float lastForwardY = -1.0f;
    DWORD lastRawY = std::numeric_limits<DWORD>::max();

    while (g_running.load()) {
        const uint64_t now = GetTickCount64();

        if (!selected.valid && now >= nextRescanTick) {
            std::vector<JoystickDevice> connectedDevices;
            const auto candidate = find_best_stepper_device(connectedDevices);

            if (candidate.valid) {
                selected = candidate;
                printedWaiting = false;

                std::wcout
                    << L"selected joystick id=" << selected.id
                    << L" name=\"" << device_name_from_caps(selected.caps) << L"\""
                    << L" axes=" << selected.caps.wNumAxes
                    << L" buttons=" << selected.caps.wNumButtons
                    << L" Y=[" << selected.caps.wYmin << L"," << selected.caps.wYmax << L"]"
                    << std::endl;
            } else {
                if (!printedWaiting) {
                    std::wcout << L"preferred stepper device not found" << std::endl;
                    print_connected_devices(connectedDevices);
                    printedWaiting = true;
                }
            }

            nextRescanTick = now + kRescanIntervalMs;
        }

        bool enabled = false;
        float forwardY = 0.0f;

        if (selected.valid) {
            JOYINFOEX info{};
            const auto mmr = query_state(selected.id, info);

            if (mmr == JOYERR_NOERROR) {
                const DWORD rawY = info.dwYpos;
                forwardY = normalize_forward_from_y(selected.caps, rawY);
                enabled = (forwardY > 0.0f);

                const bool rawChanged = rawY != lastRawY;
                const bool valueChanged = std::fabs(forwardY - lastForwardY) > kPrintEpsilon;
                const bool enabledChanged = enabled != lastEnabled;

                if (rawChanged || valueChanged || enabledChanged) {
                    ++seq;

                    std::wcout
                        << L"id=" << selected.id
                        << L" name=\"" << device_name_from_caps(selected.caps) << L"\""
                        << L" rawY=" << rawY
                        << L" forwardY=" << std::fixed << std::setprecision(3) << forwardY
                        << L" enabled=" << (enabled ? 1 : 0)
                        << std::endl;

                    lastRawY = rawY;
                    lastForwardY = forwardY;
                    lastEnabled = enabled;
                }
            } else {
                std::wcout
                    << L"selected joystick disconnected or unavailable: id=" << selected.id
                    << L" name=\"" << device_name_from_caps(selected.caps) << L"\""
                    << std::endl;

                selected = {};
                nextRescanTick = 0;

                if (lastEnabled || std::fabs(lastForwardY) > kPrintEpsilon) {
                    ++seq;
                }

                lastEnabled = false;
                lastForwardY = 0.0f;
                lastRawY = std::numeric_limits<DWORD>::max();

                enabled = false;
                forwardY = 0.0f;
            }
        }

        write_shared_state(*shared, enabled, forwardY, seq);
        std::this_thread::sleep_for(kPollInterval);
    }

    ++seq;
    write_shared_state(*shared, false, 0.0f, seq);

    UnmapViewOfFile(shared);
    CloseHandle(mapping);

    std::wcout << L"writer stopped" << std::endl;
    return 0;
}