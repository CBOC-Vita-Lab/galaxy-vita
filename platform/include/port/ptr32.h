// 32-bit pointer fields for structures that map game data files.
//
// Many file formats reserve 4 bytes for a pointer (either an offset the loader
// resolves, or a field the loader overwrites with a real pointer).  On the
// 64-bit host those fields must stay 4 bytes wide; the port keeps every game
// address below 4 GiB (see port.h), so a 32-bit value holds any of them.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C++" {

template < typename T >
struct Ptr32 {
    uint32_t v;

    Ptr32() = default;
    Ptr32(T* p) : v((uint32_t)(uintptr_t)p) {}
    Ptr32& operator=(T* p) {
        v = (uint32_t)(uintptr_t)p;
        return *this;
    }
    operator T*() const { return (T*)(uintptr_t)v; }
    T* operator->() const { return (T*)(uintptr_t)v; }
    T* get() const { return (T*)(uintptr_t)v; }
#if UINTPTR_MAX != UINT32_MAX
    explicit operator uintptr_t() const { return v; }
#endif
    explicit operator uint32_t() const { return v; }
    explicit operator int32_t() const { return (int32_t)v; }
};

template <>
struct Ptr32< void > {
    uint32_t v;

    Ptr32() = default;
    Ptr32(void* p) : v((uint32_t)(uintptr_t)p) {}
    Ptr32& operator=(void* p) {
        v = (uint32_t)(uintptr_t)p;
        return *this;
    }
    operator void*() const { return (void*)(uintptr_t)v; }
    void* get() const { return (void*)(uintptr_t)v; }
#if UINTPTR_MAX != UINT32_MAX
    explicit operator uintptr_t() const { return v; }
#endif
    explicit operator uint32_t() const { return v; }
    explicit operator int32_t() const { return (int32_t)v; }
};

static_assert(sizeof(Ptr32< void >) == 4, "Ptr32 must be 4 bytes");
}  // extern "C++"

#define PTR32(T) Ptr32< T >
#else
#define PTR32(T) uint32_t
#endif
