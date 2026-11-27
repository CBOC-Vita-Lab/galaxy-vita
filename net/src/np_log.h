// Galaxy 3.0 (net/GALAXY-CHANGES.txt, "Logging"): one line per event, to stderr as in Strikers and, on the Vita, also to the port's basic log
// (port_log_summary: ux0:data/smg/log.txt) so the hardware facts (sceNetInit results, ad hoc id accepted / refused, NetCheck dialog, signaling
// failures, peer paths) reach a player's log without GXV_STDERR_FILE (the Vita build discards stderr, vita/vgx-game/silent_stdio.cpp).
// Never use it per packet or per frame. -DNP_LOG_STDERR_ONLY restores the Strikers behaviour (stderr only).
#ifndef NP_LOG_H
#define NP_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(NP_TARGET_VITA) && !defined(NP_LOG_STDERR_ONLY)
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
#endif

// `format` ends with "\n" like the old fprintf lines (the mirrored line has no newline). `tag` is the bracketed prefix, without brackets.
static inline void NpLogf(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
static inline void NpLogf(const char* tag, const char* format, ...) {
    char line[384];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s", tag, line);
#if defined(NP_TARGET_VITA) && !defined(NP_LOG_STDERR_ONLY)
    if (port_log_summary) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        port_log_summary("[%s] %s", tag, line);
    }
#endif
}

#endif  // NP_LOG_H
