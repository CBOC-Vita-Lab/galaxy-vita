// (firstuse24; GXV_FIRSTUSE_LOG, default 1) The game thread's side of the first-use ledger: a scope that times one thing the game does the first
// time (a sound id started, an effect's emitter made) and tells platform/src/port/perf.cpp, which logs it ("first-use: game thread: ...") when it
// took 1.5 ms or more (GXV_FIRSTUSE_GAME_US) and it was the first time. Weak: host tests that build a few game files without perf.cpp have none.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int64_t port_host_time_ns(void);
void port_firstuse_game(const char* kind, unsigned id, const char* name, int64_t ns) __attribute__((weak));
#ifdef __cplusplus
}
struct PortFirstUseScope {
    const char* kind;
    unsigned id;
    const char* name;
    int64_t t0;
    PortFirstUseScope(const char* k, unsigned i, const char* n) : kind(k), id(i), name(n), t0(port_firstuse_game ? port_host_time_ns() : 0) {}
    ~PortFirstUseScope() {
        if (port_firstuse_game) port_firstuse_game(kind, id, name, port_host_time_ns() - t0);
    }
};
#endif
