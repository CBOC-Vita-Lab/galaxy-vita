// Virtual clock for the armhf/qemu harness (PETARI_VCLOCK=1).  Under qemu the game runs 20-50x slower than real
// time, so the wall clock would make the VI retrace, audio blocks and OS alarms fire at the wrong moments per game
// frame.  With the virtual clock, time only advances when the emulated CPU is idle (every OSThread blocked):
// it then jumps to the next sleeper's wake-up time (retrace, audio block, alarm), so the game gets exactly one
// retrace per frame it finishes, deterministically.  Compiled to nothing outside PORT_ARMHF_HARNESS.
#pragma once
#include <stdint.h>

#if defined(PORT_ARMHF_HARNESS)
#ifdef __cplusplus
extern "C" {
#endif
int port_vclock_enabled(void);
int64_t port_vclock_now(void);                                   // ns, monotonic, starts at 1 s
// Blocks the calling host thread until virtual time reaches absNs (INT64_MAX: until poked).  pokeSeq: the value of
// port_vclock_poke_seq() read before the caller sampled the state it sleeps on (returns at once if poked since); ~0: ignore.
void port_vclock_sleep_until(int64_t absNs, uint64_t pokeSeq);
void port_vclock_sleep_ns(int64_t ns);
uint64_t port_vclock_poke_seq(void);
void port_vclock_name_thread(const char* name);                   // diagnostics only
void port_vclock_release(void);                                  // stop counting this thread as outstanding work (before a join)
void* port_vclock_self(void);                                    // this thread's sleeper (for port_vclock_poke)
void port_vclock_poke(void* sleeper);                            // wakes it (once) and bumps the poke sequence
// Asynchronous host work the emulated CPU is waiting on (DVD reads): time does not advance while any is outstanding.
void port_vclock_external_begin(void);
void port_vclock_external_end(void);
// The emulated CPU is idle: wait for woken sleepers and external work to settle, then advance virtual time to the
// next wake-up.  Returns at once if an interrupt is pending (irqPending() != 0).
void port_vclock_idle_step(int (*irqPending)(void));
#ifdef __cplusplus
}
#endif
#else
// Not the harness: there is no virtual clock, and the calls behind "if (port_vclock_enabled())"
// compile to nothing.
static inline int port_vclock_enabled(void) { return 0; }
static inline int64_t port_vclock_now(void) { return 0; }
static inline void port_vclock_sleep_until(int64_t, uint64_t) {}
static inline void port_vclock_sleep_ns(int64_t) {}
static inline uint64_t port_vclock_poke_seq(void) { return 0; }
static inline void port_vclock_name_thread(const char*) {}
static inline void port_vclock_release(void) {}
static inline void* port_vclock_self(void) { return 0; }
static inline void port_vclock_poke(void*) {}
static inline void port_vclock_external_begin(void) {}
static inline void port_vclock_external_end(void) {}
static inline void port_vclock_idle_step(int (*)(void)) {}
#endif
