#include "version_proxy.h"

#include <Windows.h>

extern "C" {
FARPROC fp_GetFileVersionInfoA = nullptr;
FARPROC fp_GetFileVersionInfoByHandle = nullptr;
FARPROC fp_GetFileVersionInfoExA = nullptr;
FARPROC fp_GetFileVersionInfoExW = nullptr;
FARPROC fp_GetFileVersionInfoSizeA = nullptr;
FARPROC fp_GetFileVersionInfoSizeExA = nullptr;
FARPROC fp_GetFileVersionInfoSizeExW = nullptr;
FARPROC fp_GetFileVersionInfoSizeW = nullptr;
FARPROC fp_GetFileVersionInfoW = nullptr;
FARPROC fp_VerFindFileA = nullptr;
FARPROC fp_VerFindFileW = nullptr;
FARPROC fp_VerInstallFileA = nullptr;
FARPROC fp_VerInstallFileW = nullptr;
FARPROC fp_VerLanguageNameA = nullptr;
FARPROC fp_VerLanguageNameW = nullptr;
FARPROC fp_VerQueryValueA = nullptr;
FARPROC fp_VerQueryValueW = nullptr;
}

namespace {

HMODULE g_real_version_dll = nullptr;

struct ExportBinding {
    const char* name;
    FARPROC* target;
};

constexpr ExportBinding kBindings[] = {
    {"GetFileVersionInfoA", &fp_GetFileVersionInfoA},
    {"GetFileVersionInfoByHandle", &fp_GetFileVersionInfoByHandle},
    {"GetFileVersionInfoExA", &fp_GetFileVersionInfoExA},
    {"GetFileVersionInfoExW", &fp_GetFileVersionInfoExW},
    {"GetFileVersionInfoSizeA", &fp_GetFileVersionInfoSizeA},
    {"GetFileVersionInfoSizeExA", &fp_GetFileVersionInfoSizeExA},
    {"GetFileVersionInfoSizeExW", &fp_GetFileVersionInfoSizeExW},
    {"GetFileVersionInfoSizeW", &fp_GetFileVersionInfoSizeW},
    {"GetFileVersionInfoW", &fp_GetFileVersionInfoW},
    {"VerFindFileA", &fp_VerFindFileA},
    {"VerFindFileW", &fp_VerFindFileW},
    {"VerInstallFileA", &fp_VerInstallFileA},
    {"VerInstallFileW", &fp_VerInstallFileW},
    {"VerLanguageNameA", &fp_VerLanguageNameA},
    {"VerLanguageNameW", &fp_VerLanguageNameW},
    {"VerQueryValueA", &fp_VerQueryValueA},
    {"VerQueryValueW", &fp_VerQueryValueW},
};

}  // namespace

namespace gotham {

bool InitVersionProxy() {
    if (g_real_version_dll) {
        return true;
    }

    wchar_t sys_dir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sys_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    wchar_t dll_path[MAX_PATH] = {};
    wsprintfW(dll_path, L"%s\\version.dll", sys_dir);
    g_real_version_dll = LoadLibraryW(dll_path);
    if (!g_real_version_dll) {
        return false;
    }

    for (const auto& entry : kBindings) {
        *entry.target = GetProcAddress(g_real_version_dll, entry.name);
    }
    return true;
}

void ShutdownVersionProxy() {
    if (g_real_version_dll) {
        FreeLibrary(g_real_version_dll);
        g_real_version_dll = nullptr;
    }
}

}  // namespace gotham
