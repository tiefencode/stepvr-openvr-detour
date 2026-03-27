#include <windows.h>
#include <psapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hooking_library/forward_ingress.h"

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(8);
constexpr uint64_t kRescanIntervalMs = 1000;
constexpr float kPrintEpsilon = 0.01f;
constexpr float kCenterDeadzone = 0.04f;

constexpr wchar_t kDefaultTargetProcessName[] = L"Monster Titans Playground";
constexpr wchar_t kPreferredDeviceName[] = L"StepVR ATOM Stepper";
constexpr wchar_t kPreferredToken1[] = L"stepvr";
constexpr wchar_t kPreferredToken2[] = L"stepper";

std::atomic<bool> g_running{ true };

struct JoystickDevice {
    UINT id = std::numeric_limits<UINT>::max();
    JOYCAPSW caps{};
    bool valid = false;
};

struct ProcessMatch {
    DWORD pid = 0;
    std::wstring name;
    bool valid = false;
};

struct InjectedProcess {
    DWORD pid = 0;
    HANDLE handle = nullptr;
};

class Logger {
public:
    explicit Logger(const std::filesystem::path& logPath) {
        file_.open(logPath.string(), std::ios::out | std::ios::app);
    }

    void line(const std::string& message) {
        const auto formatted = timestamp_now() + " " + message;

        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << formatted << std::endl;

        if (file_.is_open()) {
            file_ << formatted << std::endl;
            file_.flush();
        }
    }

private:
    static std::string timestamp_now() {
        SYSTEMTIME now{};
        GetLocalTime(&now);

        char buffer[64]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%04u-%02u-%02u %02u:%02u:%02u.%03u",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds);

        return buffer;
    }

    std::ofstream file_;
    std::mutex mutex_;
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

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (required <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);

    return result;
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

std::string format_win32_error(DWORD error) {
    LPSTR messageBuffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;

    const DWORD length = FormatMessageA(
        flags,
        nullptr,
        error,
        0,
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    std::string message;
    if (length > 0 && messageBuffer) {
        message.assign(messageBuffer, messageBuffer + length);
        while (!message.empty() &&
               (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
    }

    if (messageBuffer) {
        LocalFree(messageBuffer);
    }

    if (message.empty()) {
        std::ostringstream oss;
        oss << "error " << error;
        return oss.str();
    }

    std::ostringstream oss;
    oss << "error " << error << ": " << message;
    return oss.str();
}

std::filesystem::path executable_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
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
        if (contains_icase(device_name_from_caps(device.caps), kPreferredDeviceName)) {
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

    if (connectedDevices.size() == 1) {
        return connectedDevices.front();
    }

    return {};
}

void log_connected_devices(Logger& logger, const std::vector<JoystickDevice>& devices) {
    if (devices.empty()) {
        logger.line("[writer] no WinMM joystick devices currently connected");
        return;
    }

    logger.line("[writer] connected WinMM joystick devices:");
    for (const auto& device : devices) {
        std::ostringstream oss;
        oss << "[writer]   id=" << device.id
            << " name=\"" << to_utf8(device_name_from_caps(device.caps)) << "\""
            << " axes=" << device.caps.wNumAxes
            << " buttons=" << device.caps.wNumButtons
            << " Y=[" << device.caps.wYmin << "," << device.caps.wYmax << "]";
        logger.line(oss.str());
    }
}

ProcessMatch find_process_by_name(const std::wstring& targetProcess) {
    DWORD processes[2048]{};
    DWORD bytesReturned = 0;

    if (!EnumProcesses(processes, sizeof(processes), &bytesReturned)) {
        return {};
    }

    const size_t processCount = bytesReturned / sizeof(DWORD);
    for (size_t i = 0; i < processCount; ++i) {
        const DWORD pid = processes[i];
        if (pid == 0) {
            continue;
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!process) {
            continue;
        }

        wchar_t name[MAX_PATH]{};
        const DWORD copied = GetModuleBaseNameW(process, nullptr, name, MAX_PATH);
        CloseHandle(process);

        if (copied == 0 || name[0] == L'\0') {
            continue;
        }

        if (contains_icase(name, targetProcess)) {
            ProcessMatch match{};
            match.pid = pid;
            match.name = name;
            match.valid = true;
            return match;
        }
    }

    return {};
}

void clear_injected_process(InjectedProcess& injected) {
    if (injected.handle) {
        CloseHandle(injected.handle);
        injected.handle = nullptr;
    }

    injected.pid = 0;
}

bool is_injected_process_alive(const InjectedProcess& injected) {
    if (!injected.handle) {
        return false;
    }

    return WaitForSingleObject(injected.handle, 0) == WAIT_TIMEOUT;
}

bool monitor_injected_process(DWORD pid, InjectedProcess& injected, Logger& logger) {
    clear_injected_process(injected);

    injected.handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!injected.handle) {
        std::ostringstream oss;
        oss << "[inject] injected into pid=" << pid
            << " but failed to open monitor handle: "
            << format_win32_error(GetLastError());
        logger.line(oss.str());
        return false;
    }

    injected.pid = pid;
    return true;
}

bool inject_dll_into_process(DWORD pid,
                             const std::filesystem::path& dllPath,
                             InjectedProcess& injected,
                             Logger& logger) {
    const auto dllPathWide = dllPath.wstring();

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        logger.line("[inject] kernel32.dll not found");
        return false;
    }

    auto* loadLibraryWFn = GetProcAddress(kernel32, "LoadLibraryW");
    if (!loadLibraryWFn) {
        logger.line("[inject] LoadLibraryW not found");
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
            PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD,
        FALSE,
        pid);

    if (!process) {
        std::ostringstream oss;
        oss << "[inject] OpenProcess failed for pid=" << pid
            << ": " << format_win32_error(GetLastError());
        logger.line(oss.str());
        return false;
    }

    const SIZE_T bytes = (dllPathWide.size() + 1) * sizeof(wchar_t);
    void* remoteBuffer = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remoteBuffer) {
        std::ostringstream oss;
        oss << "[inject] VirtualAllocEx failed for pid=" << pid
            << ": " << format_win32_error(GetLastError());
        logger.line(oss.str());
        CloseHandle(process);
        return false;
    }

    SIZE_T writtenBytes = 0;
    if (!WriteProcessMemory(process, remoteBuffer, dllPathWide.c_str(), bytes, &writtenBytes) ||
        writtenBytes != bytes) {
        std::ostringstream oss;
        oss << "[inject] WriteProcessMemory failed for pid=" << pid
            << ": " << format_win32_error(GetLastError());
        logger.line(oss.str());
        VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE remoteThread = CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryWFn),
        remoteBuffer,
        0,
        nullptr);

    if (!remoteThread) {
        std::ostringstream oss;
        oss << "[inject] CreateRemoteThread failed for pid=" << pid
            << ": " << format_win32_error(GetLastError());
        logger.line(oss.str());
        VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(remoteThread, 10000);
    DWORD remoteExitCode = 0;
    GetExitCodeThread(remoteThread, &remoteExitCode);

    CloseHandle(remoteThread);
    VirtualFreeEx(process, remoteBuffer, 0, MEM_RELEASE);
    CloseHandle(process);

    if (waitResult != WAIT_OBJECT_0 || remoteExitCode == 0) {
        std::ostringstream oss;
        oss << "[inject] remote LoadLibraryW failed for pid=" << pid
            << " waitResult=" << waitResult
            << " exitCode=0x" << std::hex << remoteExitCode << std::dec;
        logger.line(oss.str());
        return false;
    }

    if (!monitor_injected_process(pid, injected, logger)) {
        return false;
    }

    std::ostringstream oss;
    oss << "[inject] successfully injected pid=" << pid
        << " dll=\"" << to_utf8(dllPathWide) << "\"";
    logger.line(oss.str());
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        std::cerr << "failed to install console control handler" << std::endl;
        return 1;
    }

    const auto baseDir = executable_dir();
    Logger logger(baseDir / L"stepvr_launcher.log");

    const std::wstring targetProcess =
        argc > 1 && argv[1] && argv[1][0] != L'\0' ? argv[1] : kDefaultTargetProcessName;
    const auto dllPath = baseDir / L"stepvr_detour.dll";

    logger.line("=== stepvr launcher init ===");
    logger.line("[launcher] target process filter: \"" + to_utf8(targetProcess) + "\"");
    logger.line("[launcher] dll path: \"" + to_utf8(dllPath.wstring()) + "\"");

    if (!std::filesystem::exists(dllPath)) {
        logger.line("[launcher] stepvr_detour.dll was not found next to the launcher");
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
        logger.line("[launcher] CreateFileMappingW failed: " + format_win32_error(GetLastError()));
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
        logger.line("[launcher] MapViewOfFile failed: " + format_win32_error(GetLastError()));
        CloseHandle(mapping);
        return 1;
    }

    std::memset(shared, 0, sizeof(*shared));

    uint64_t seq = 1;
    write_shared_state(*shared, false, 0.0f, seq);

    logger.line("[launcher] shared mapping: Local\\StepVRForwardState");
    logger.line("[launcher] input path: WinMM joystick Y axis -> forwardY");
    logger.line("[launcher] payload logs are written by the DLL to stepvr_detour.log");
    logger.line("[launcher] press Ctrl+C to quit");

    JoystickDevice selected{};
    uint64_t nextDeviceRescanTick = 0;
    bool loggedWaitingForDevice = false;

    bool lastEnabled = false;
    float lastForwardY = -1.0f;
    DWORD lastRawY = std::numeric_limits<DWORD>::max();

    InjectedProcess injected{};
    uint64_t nextProcessRescanTick = 0;
    bool loggedWaitingForProcess = false;

    while (g_running.load()) {
        const uint64_t now = GetTickCount64();

        if (injected.handle && !is_injected_process_alive(injected)) {
            std::ostringstream oss;
            oss << "[inject] monitored process exited: pid=" << injected.pid;
            logger.line(oss.str());
            clear_injected_process(injected);
        }

        if (!injected.handle && now >= nextProcessRescanTick) {
            const auto match = find_process_by_name(targetProcess);
            if (match.valid) {
                loggedWaitingForProcess = false;

                std::ostringstream oss;
                oss << "[inject] found target process pid=" << match.pid
                    << " name=\"" << to_utf8(match.name) << "\"";
                logger.line(oss.str());

                inject_dll_into_process(match.pid, dllPath, injected, logger);
            } else if (!loggedWaitingForProcess) {
                logger.line("[inject] waiting for target process");
                loggedWaitingForProcess = true;
            }

            nextProcessRescanTick = now + kRescanIntervalMs;
        }

        if (!selected.valid && now >= nextDeviceRescanTick) {
            std::vector<JoystickDevice> connectedDevices;
            const auto candidate = find_best_stepper_device(connectedDevices);

            if (candidate.valid) {
                selected = candidate;
                loggedWaitingForDevice = false;

                std::ostringstream oss;
                oss << "[writer] selected joystick id=" << selected.id
                    << " name=\"" << to_utf8(device_name_from_caps(selected.caps)) << "\""
                    << " axes=" << selected.caps.wNumAxes
                    << " buttons=" << selected.caps.wNumButtons
                    << " Y=[" << selected.caps.wYmin << "," << selected.caps.wYmax << "]";
                logger.line(oss.str());
            } else if (!loggedWaitingForDevice) {
                logger.line("[writer] preferred stepper device not found");
                log_connected_devices(logger, connectedDevices);
                loggedWaitingForDevice = true;
            }

            nextDeviceRescanTick = now + kRescanIntervalMs;
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

                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(3);
                    oss << "[writer] id=" << selected.id
                        << " name=\"" << to_utf8(device_name_from_caps(selected.caps)) << "\""
                        << " rawY=" << rawY
                        << " forwardY=" << forwardY
                        << " enabled=" << (enabled ? 1 : 0);
                    logger.line(oss.str());

                    lastRawY = rawY;
                    lastForwardY = forwardY;
                    lastEnabled = enabled;
                }
            } else {
                std::ostringstream oss;
                oss << "[writer] selected joystick disconnected or unavailable: id=" << selected.id
                    << " name=\"" << to_utf8(device_name_from_caps(selected.caps)) << "\""
                    << " mmr=" << mmr;
                logger.line(oss.str());

                selected = {};
                nextDeviceRescanTick = 0;

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

    clear_injected_process(injected);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);

    logger.line("[launcher] stopped");
    return 0;
}
