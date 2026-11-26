#include "hooks/dxgi_hook.h"
#include "log.h"
#include "proxy/version_proxy.h"

#include <Windows.h>

namespace {

HMODULE g_self_module = nullptr;
HANDLE g_init_thread = nullptr;

#ifndef GOTHAMDUO_VERSION_STR
#define GOTHAMDUO_VERSION_STR "0.0.0-dev"
#endif

DWORD WINAPI InitThreadProc(LPVOID) {
    gotham::InitLog(g_self_module);
    gotham::Log("Gotham Duo v%s loaded into PID %lu", GOTHAMDUO_VERSION_STR, GetCurrentProcessId());
    gotham::InitDxgiHook(g_self_module);
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpvReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_self_module = hModule;

        if (!gotham::InitVersionProxy()) {
            return FALSE;
        }

        g_init_thread = CreateThread(nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_init_thread) {
            CloseHandle(g_init_thread);
            g_init_thread = nullptr;
        }
        // Only tear down hooks/handles on explicit FreeLibrary; during process exit
        // other threads are already terminated and touching locks/MinHook can deadlock.
        if (lpvReserved == nullptr) {
            gotham::ShutdownDxgiHook();
            gotham::ShutdownLog();
            gotham::ShutdownVersionProxy();
        }
    }
    return TRUE;
}
