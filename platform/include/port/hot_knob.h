// Switches read once without a barrier on the game thread's, the audio mixer's and the GX writer's hot paths (GXV_HOT_KNOBS).
//
// A switch kept as a function-local `static const T x = <read of the environment>;` is a guarded static: every call loads the
// guard byte and, on the console (clang, arm-vita-eabi, ARMv7), follows it with `dmb ish`, the acquire the guard needs because
// the first call may run on any thread. A barrier waits until every earlier load and store has completed, so it also waits for
// the caller's store misses. The front end lost ~40,000 of them a picture this way (fe17, GXV_FE_KNOBS, vgx_common.h VGX_KNOB);
// the game thread pays them per actor, per model, per material, per matrix load (GXLoadPosMtxImm) and per allocation.
//
// PORT_HOT_KNOB(T, expr) is the value of `expr` (T: an integer, enum, bool, float or pointer type of at most 4 bytes on the
// console), worked out at the first call and kept in a constant-initialised atomic word (no guard) read with a relaxed load: one
// load and one compare, no barrier. Two threads that both find the word unset both work `expr` out and store the same value (the
// switches are read from the environment, which nothing changes once the game runs): a whole word, read whole. `expr` must
// therefore be free of side effects (or be the call of a function that keeps its own guarded static, which then runs once).
// A value whose bits are the "unset" pattern (0x7FBADBAD, a NaN as a float, an odd pointer) is not kept: it is worked out again
// at every call, which is still the right value.
//
//   GXV_HOT_KNOBS=1 (default): as above.
//   GXV_HOT_KNOBS=0: the old reads, for the console A/B: each read is followed by an acquire fence (`dmb ish`), the barrier the
//                    guarded static had. The values are the same either way. Asked at each knob's first read.
// (C++ only. The vgx front end has its own copy of this, VGX_KNOB under GXV_FE_KNOBS.)
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace port {
namespace hotknob {

template <class T> using Bits = typename std::conditional<(sizeof(T) <= 4), uint32_t, uint64_t>::type;
template <class T> constexpr Bits<T> kUnset = static_cast<Bits<T>>(sizeof(T) <= 4 ? 0x7FBADBADull : 0x7FBADBAD7FBADBADull);

// GXV_HOT_KNOBS: 1 (unset, empty or not 0) = without barriers; 0 = with them. Asked once per knob, at its first read (not kept:
// a knob read during static initialisation, a global Color8 for one, must not decide it for the knobs read after env.txt is loaded).
inline __attribute__((noinline)) bool Fenced() {
    const char* e = std::getenv("GXV_HOT_KNOBS");
    return e && *e && std::strtol(e, nullptr, 0) == 0;
}

template <class T> inline Bits<T> ToBits(T v) {
    static_assert(sizeof(T) <= 8 && std::is_trivially_copyable<T>::value, "PORT_HOT_KNOB: a word-sized value");
    if constexpr (std::is_pointer<T>::value) {
        return static_cast<Bits<T>>(reinterpret_cast<uintptr_t>(v));
    } else if constexpr (std::is_integral<T>::value || std::is_enum<T>::value) {
        return static_cast<Bits<T>>(v);
    } else {
        Bits<T> b = 0;
        std::memcpy(&b, &v, sizeof v);
        return b;
    }
}
template <class T> inline T FromBits(Bits<T> b) {
    if constexpr (std::is_pointer<T>::value) {
        return reinterpret_cast<T>(static_cast<uintptr_t>(b));
    } else if constexpr (std::is_same<T, bool>::value) {
        return b != 0;
    } else if constexpr (std::is_integral<T>::value || std::is_enum<T>::value) {
        return static_cast<T>(b);
    } else {
        T v;
        std::memcpy(&v, &b, sizeof v);
        return v;
    }
}
// (the value goes into `k`, read without a barrier, or with GXV_HOT_KNOBS=0 into `kf`, read with one)
template <class T, class F> __attribute__((noinline)) T Init(std::atomic<Bits<T>>& k, std::atomic<Bits<T>>& kf, F f) {
    const T v = f();
    const Bits<T> b = ToBits<T>(v);
    if (b != kUnset<T>) (Fenced() ? kf : k).store(b, std::memory_order_relaxed);
    return v;
}
template <class T, class F> inline __attribute__((always_inline)) T Read(std::atomic<Bits<T>>& k, std::atomic<Bits<T>>& kf, F f) {
    const Bits<T> v = k.load(std::memory_order_relaxed);
    if (__builtin_expect(v != kUnset<T>, 1)) return FromBits<T>(v);
    const Bits<T> w = kf.load(std::memory_order_relaxed);
    if (w != kUnset<T>) {
        std::atomic_thread_fence(std::memory_order_acquire);  // (GXV_HOT_KNOBS=0: the guarded static's barrier)
        return FromBits<T>(w);
    }
    return Init<T>(k, kf, f);
}

} // namespace hotknob
} // namespace port

#define PORT_HOT_KNOB(T, ...) \
    ([]() -> T { \
        static std::atomic<::port::hotknob::Bits<T>> port_knob_{::port::hotknob::kUnset<T>}, port_knob_fenced_{::port::hotknob::kUnset<T>}; \
        return ::port::hotknob::Read<T>(port_knob_, port_knob_fenced_, []() -> T { return static_cast<T>(__VA_ARGS__); }); \
    }())
