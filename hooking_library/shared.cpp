#include "shared.h"
#include "detour_controller_state.h"
#include "detour_input.h"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

#include "MinHook.h"

namespace stepvr {

HANDLE StdOut = INVALID_HANDLE_VALUE;
HookConfig g_config{};

std::ofstream g_log;
std::mutex g_logMutex;
std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_vrSystemProcessed{ false };

void log_line(const std::string& line) {
    if (StdOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(
            StdOut,
            line.c_str(),
            static_cast<DWORD>(line.size()),
            &written,
            nullptr);

        WriteFile(
            StdOut,
            "\n",
            1,
            &written,
            nullptr);
    }

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

void initialize(HMODULE module) {
    bool hasConsole = AllocConsole();
    if (hasConsole) {
        StdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    }

    if (MH_Initialize() != MH_OK) {
        log_line("=== MINHOOK was not initialized correctly ===");
    }

    const auto logPath = std::filesystem::path(get_module_dir(module)) / L"stepvr_detour.log";
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.open(logPath.string(), std::ios::out | std::ios::app);
    }

    log_line("=== stepvr detour init ===");

    if (!obtain_vr_system_and_prepare()) {
        log_line("failed to obtain OpenVR interfaces");
    }

    g_initialized.store(true);
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

    bool installedAnyHook = false;

    {
        vr::EVRInitError err = vr::VRInitError_None;
        auto* vrSystem = reinterpret_cast<void*>(getGenericInterface(vr::IVRSystem_Version, &err));

        std::ostringstream oss;
        oss << "VR_GetGenericInterface(" << vr::IVRSystem_Version << ") -> " << vrSystem
            << " err=" << static_cast<int>(err);
        log_line(oss.str());

        if (vrSystem && err == vr::VRInitError_None) {
            auto* system_vtable = reinterpret_cast<VR_IVRSystem_FnTable*>(*(void***)vrSystem);
            Install_GetControllerState_Hook(system_vtable);
            installedAnyHook = true;
        } else {
            log_line("failed to obtain IVRSystem");
        }
    }

    {
        vr::EVRInitError err = vr::VRInitError_None;
        auto* vrInput = reinterpret_cast<void*>(getGenericInterface(vr::IVRInput_Version, &err));

        std::ostringstream oss;
        oss << "VR_GetGenericInterface(" << vr::IVRInput_Version << ") -> " << vrInput
            << " err=" << static_cast<int>(err);
        log_line(oss.str());

        if (vrInput && err == vr::VRInitError_None) {
            auto* input_vtable = reinterpret_cast<VR_IVRInput_FnTable*>(*(void***)vrInput);
            Install_IVRInput_Hooks(input_vtable);
            installedAnyHook = true;
        } else {
            log_line("failed to obtain IVRInput");
        }
    }

    return installedAnyHook;
}

} // namespace stepvr