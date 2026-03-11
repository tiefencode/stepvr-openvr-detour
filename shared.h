#pragma once

#include <windows.h>
#include <openvr.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace stepvr {

struct HookConfig {
    bool enableDetour = false;
    bool enableAxisOverride = false;
    uint32_t getControllerStateIndex = 33; // probe value for IVRSystem_019; verify before enabling detour
    int overrideAxisIndex = 0;
    float overrideX = 0.0f;
    float overrideY = 0.8f;
};

extern HookConfig g_config;

extern std::ofstream g_log;
extern std::mutex g_logMutex;
extern std::atomic<bool> g_initialized;
extern std::atomic<bool> g_vrSystemProcessed;

void log_line(const std::string& line);
std::wstring get_module_dir(HMODULE module);
void initialize_async(HMODULE module);
DWORD WINAPI init_thread_proc(LPVOID param);

bool obtain_vr_system_and_prepare();
void inspect_and_optionally_hook(vr::IVRSystem* vrSystem);

}