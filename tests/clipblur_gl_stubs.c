// tests/clipblur_gl_game.cpp's link: the services the GX library's capture layer (platform/src/gx/gx_capture.cpp, never armed here) and the front end use.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int64_t port_host_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}
void port_log(const char* fmt, ...) {
    (void)fmt;
}
void port_fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    abort();
}
int port_vsnprintf(char* dst, size_t n, const char* fmt, va_list args) { return vsnprintf(dst, n, fmt, args); }
void port_stream_census_copy(void) {}
