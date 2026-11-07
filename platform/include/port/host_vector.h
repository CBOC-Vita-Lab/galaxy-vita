// A std::vector whose storage always comes from the host (C) heap, never from the game's JKRHeap tree.
//
// Why: the global operator new (decomp/src/JSystem/JKernel/JKRHeap.cpp) allocates from the game's *current* heap whenever it is
// called on a game thread. A port-side container that lives longer than a scene (a static list, a cache) therefore gets its
// storage from that scene's heap the first time it grows during the scene. When the scene's heap is destroyed the container keeps
// pointing into it; the next scene allocates its own objects there and the container's next write lands on top of them.
// Found on the console (6 Nov 2026): sStaleDL (decomp/src/Game/LiveActor/ModelManager.cpp) kept its buffer across the file select
// -> Comet Observatory change and wrote its (actor, model) pairs over a SwitchWatcher, which then crashed in
// SwitchWatcherHolder::movement (crash "A", r58-r66).
//
// Use PortHostVector<T> for any port-side container that is not owned by a scene object.
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <vector>

#include "port/heap_routing.h"

template <class T>
struct PortHostAllocator {
    typedef T value_type;
    PortHostAllocator() noexcept {}
    template <class U>
    PortHostAllocator(const PortHostAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        const int align = alignof(T) < 16 ? 16 : static_cast<int>(alignof(T));
        void* p = port_host_alloc(bytes, align);
        if (p == nullptr) {
            port_host_alloc_failed(bytes, align);  // (never returns)
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { port_host_free(p); }
};
template <class T, class U>
inline bool operator==(const PortHostAllocator<T>&, const PortHostAllocator<U>&) noexcept {
    return true;
}
template <class T, class U>
inline bool operator!=(const PortHostAllocator<T>&, const PortHostAllocator<U>&) noexcept {
    return false;
}

template <class T>
using PortHostVector = std::vector<T, PortHostAllocator<T> >;

// GXV_HEAP_GUARD (platform/include/port/heap_routing.h): register a long-lived port-side container, or the pointer variable of a
// buffer, at namespace or file scope, next to its definition:
//     PORT_HEAP_GUARD(sMyList);        // anything with .data(): std::vector, std::string, PortHostVector ...
//     PORT_HEAP_GUARD_PTR(sMyBuffer);  // a plain pointer variable
// The check at a scene's destruction then logs any such storage that lies in a game heap.
struct PortHeapGuardRegistration {
    PortHeapGuardRegistration(const char* name, PortHeapGuardFn fn, void* context) { port_heap_guard_register(name, fn, context); }
};
template <class C>
inline const void* port_heap_guard_data(void* context) {
    return static_cast<C*>(context)->data();
}
template <class T>
inline const void* port_heap_guard_pointer(void* context) {
    return *static_cast<T* const*>(context);
}
#define PORT_HEAP_GUARD_JOIN2(a, b) a##b
#define PORT_HEAP_GUARD_JOIN(a, b) PORT_HEAP_GUARD_JOIN2(a, b)
#define PORT_HEAP_GUARD(var) \
    static const PortHeapGuardRegistration PORT_HEAP_GUARD_JOIN(sHeapGuard_, __LINE__)(#var, &port_heap_guard_data<decltype(var)>, &(var))
#define PORT_HEAP_GUARD_PTR(var) \
    static const PortHeapGuardRegistration PORT_HEAP_GUARD_JOIN(sHeapGuard_, __LINE__)( \
        #var, &port_heap_guard_pointer<std::remove_pointer<decltype(var)>::type>, &(var))
