#pragma once

#include <Windows.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace gotham {

inline std::mutex& LogMutex() {
    static std::mutex mtx;
    return mtx;
}

inline FILE*& LogFile() {
    static FILE* file = nullptr;
    return file;
}

inline void InitLog(HMODULE self_module) {
    std::lock_guard<std::mutex> lock(LogMutex());
    if (LogFile()) {
        return;
    }

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self_module, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"gothamduo.log");
    } else {
        wcscpy_s(path, L"gothamduo.log");
    }

    LogFile() = _wfsopen(path, L"w", _SH_DENYNO);
}

inline void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(LogMutex());
    FILE* f = LogFile();
    if (!f) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(f, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);

    std::fputc('\n', f);
    std::fflush(f);
}

inline void ShutdownLog() {
    std::lock_guard<std::mutex> lock(LogMutex());
    if (LogFile()) {
        std::fclose(LogFile());
        LogFile() = nullptr;
    }
}

}  // namespace gotham
