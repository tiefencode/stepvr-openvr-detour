#include "shared.h"

#include <filesystem>
#include <sstream>
#include <thread>
#include <chrono>

namespace stepvr {

HookConfig g_config{};

std::ofstream g_log;
std::mutex g_logMutex;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_vrSystemProcessed{false};

void log_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log.is_open()) {
        g_log << line << std::endl;
        g_log.flush();
    }
}

std::wstring get_module_dir(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::filesystem::path p(path);
    return p.parent_path().wstring();
}

void initialize_async(HMODULE module) {
    HANDLE thread = CreateThread(nullptr, 0, init_thread_proc, module, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

DWORD WINAPI init_thread_proc(LPVOID param) {
    HMODULE module = reinterpret_cast<HMODULE>(param);

    const auto logPath = std::filesystem::path(get_module_dir(module)) / L"stepvr_detour.log";
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.open(logPath.string(), std::ios::out | std::ios::app);
    }

    log_line("=== stepvr detour init ===");

    if (!obtain_vr_system_and_prepare()) {
        log_line("failed to obtain IVRSystem");
    }

    g_initialized.store(true);
    return 0;
}

bool obtain_vr_system_and_prepare() {
    using VR_GetGenericInterface_Fn = void* (__cdecl*)(const char*, vr::EVRInitError*);

    HMODULE openvrModule = nullptr;

    for (int i = 0; i < 100; ++i) {
        openvrModule = GetModuleHandleW(L"openvr_api.dll");
        if (openvrModule) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!openvrModule) {
        log_line("openvr_api.dll not loaded in process");
        return false;
    }

    auto getGenericInterface =
        reinterpret_cast<VR_GetGenericInterface_Fn>(GetProcAddress(openvrModule, "VR_GetGenericInterface"));

    if (!getGenericInterface) {
        log_line("VR_GetGenericInterface not found");
        return false;
    }

    vr::EVRInitError err = vr::VRInitError_None;
    auto* vrSystem = reinterpret_cast<vr::IVRSystem*>(getGenericInterface(vr::IVRSystem_Version, &err));

    {
        std::ostringstream oss;
        oss << "VR_GetGenericInterface(" << vr::IVRSystem_Version << ") -> " << vrSystem
            << " err=" << static_cast<int>(err);
        log_line(oss.str());
    }

    if (!vrSystem || err != vr::VRInitError_None) {
        return false;
    }

    inspect_and_optionally_hook(vrSystem);
    return true;
}

}