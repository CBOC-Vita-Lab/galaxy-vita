// Galaxy 3.0 multiplayer (port side, owner: core): mp_log -> "[mp] ..." lines in the port log (ux0:data/smg/log.txt / desktop
// game.log). Player builds without runtime diagnostics still get the lines in the basic log (port_log_summary). Host tests (no
// port layer linked) get stderr, and can capture lines with mp_log_set_sink.
#include "port/mp_bridge.h"
#include "port/diagnostics.h"

#include <stdarg.h>
#include <stdio.h>

extern "C" void port_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));

extern "C" void port_host_heap_log(const char* when) __attribute__((weak));

static void (*s_sink)(const char* line) = 0;
static bool s_quiet = false;

// Internal (mp_core.h): tests capture every line; quiet = no stderr copy in host tests.
void mp_log_set_sink(void (*sink)(const char* line), bool quiet) {
    s_sink = sink;
    s_quiet = quiet;
}

extern "C" void mp_log(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    const int prefix = snprintf(line, sizeof line, "[mp] ");
    vsnprintf(line + prefix, sizeof line - (size_t)prefix, fmt, ap);
    va_end(ap);
    if (s_sink) s_sink(line);
#if PORT_RUNTIME_DIAGNOSTICS
    if (port_log) { port_log("%s", line); return; }
#else
    if (port_log_summary) { port_log_summary("%s", line); return; }
#endif
    if (!s_quiet) {
        fputs(line, stderr);
        fputc('\n', stderr);
    }
}

// memfix: the C heap's use at the multiplayer milestones ("host heap <when>: arena .. KiB, in use .. KiB ..": os_thread.cpp). `when` is
// "mp session start" / "mp session end" / "mp first tls". Nothing in host tests (the port layer is not linked).
extern "C" void mp_aids_memory_line(const char* when) __attribute__((weak));   // MP3 hwaids (mp_hwaids.cpp)
extern "C" void mp_heap_note(const char* when) {
    if (port_host_heap_log) port_host_heap_log(when);
    if (mp_aids_memory_line) mp_aids_memory_line(when);   // MP3 hwaids: the "[mp] mem ..." line (Vita free memory, heap, resident size)
}
