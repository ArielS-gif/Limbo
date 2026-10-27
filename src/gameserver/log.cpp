#include "log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace logx {

static FILE* g_file = nullptr;
static std::mutex g_write;  // keeps lines whole when several threads log

void Open(const std::wstring& path) {
    g_file = _wfopen(path.c_str(), L"w");
}

void Write(const char* fmt, ...) {
    if (!g_file) return;
    std::lock_guard<std::mutex> guard(g_write);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
}

}  // namespace logx
