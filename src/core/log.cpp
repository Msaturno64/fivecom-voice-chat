#include "core/log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

namespace fivecom {

static FILE* g_log = nullptr;
static std::mutex g_mu;

void log_init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_log) return;
    g_log = fopen("fivecom.log", "w");
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_log, "=== FiveCom log iniciado %02d:%02d:%02d ===\n",
                st.wHour, st.wMinute, st.wSecond);
        fflush(g_log);
    }
}

void log_msg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\n");
    fflush(g_log);
}

} // namespace fivecom
