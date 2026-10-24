#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port/port.h"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "JSystem/JKernel/JKRExpHeap.hpp"
#include "JSystem/JKernel/JKRSolidHeap.hpp"
#include "JSystem/JUtility/JUTException.hpp"

namespace {
unsigned checks;
thread_local unsigned hostDepth;
thread_local bool gameThread = true;
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "Vita guest heap FAILED: %s\n", message); std::exit(1); }
}
}

extern "C" int port_use_game_heap() { return gameThread && hostDepth == 0; }
extern "C" int port_is_game_thread() { return gameThread; }
void test_set_game_thread(bool value) { gameThread = value; }
extern "C" int port_is_game_heap_ptr(const void* p) { return port_is_game_memory(p); }
extern "C" void port_host_alloc_scope_enter() { ++hostDepth; }
extern "C" void port_host_alloc_scope_leave() { --hostDepth; }
extern "C" void* port_host_alloc(size_t size, int align) {
    void* p = nullptr;
    if (align < static_cast<int>(sizeof(void*))) align = sizeof(void*);
    return posix_memalign(&p, align, size) ? nullptr : p;
}
extern "C" void port_host_alloc_failed(size_t, int) { port_fatal("host heap exhausted"); std::abort(); }
void test_note_host_free(const void*);
extern "C" void port_host_free(void* p) { test_note_host_free(p); std::free(p); }

// Substitute OS locking, but reject the host-only heap access seen in native boot.
extern "C" void OSInitMutex(OSMutex* mutex) { std::memset(mutex, 0, sizeof(*mutex)); }
extern "C" void OSLockMutex(OSMutex*) { if (!gameThread) port_fatal("host worker touched original game heap mutex"); }
extern "C" void OSUnlockMutex(OSMutex*) { if (!gameThread) port_fatal("host worker touched original game heap mutex"); }
extern "C" void OSReport(const char*, ...) {}
extern "C" void OSPanic(const char*, int, const char*, ...) { port_fatal("guest heap allocation failed"); }
extern "C" u32 WPADGetWorkMemorySize() { return 0x50000; }
extern "C" void JUTReportConsole(const char*) {}
extern "C" void JUTWarningConsole_f_va(const char*, va_list) {}
extern "C" void JUTReportConsole_f(const char*, ...) {}
void JUTException::panic_f(const char*, int, const char*, ...) { port_fatal("guest heap integrity failed"); }

int test_dvd_heap_routing();

int test_vita_game_heaps() {
    HeapMemoryWatcher::createRootHeap();
    HeapMemoryWatcher watcher;
    require(watcher.mGameHeapNapa && watcher.mGameHeapGDDR, "original native guest heap hierarchy is constructed");
    require(watcher.mGameHeapGDDR->getMaxAllocatableSize(32) >= 17040384,
            "initial Logo scene file-cache request fits before stationed-heap adjustment");
    watcher.createFileCacheHeapOnGameHeap(0x1040400);
    require(watcher.mFileCacheHeap != nullptr, "real solid-heap file cache is created");
    require(watcher.mFileCacheHeap->getFreeSize() >= 17039360, "file cache retains its original payload capacity");
    watcher.createSceneHeapOnGameHeap();
    require(watcher.mSceneHeapNapa && watcher.mSceneHeapGDDR, "initial scene heaps fit beside the complete file cache");
    require(watcher.mSceneHeapNapa->getFreeSize() > 0 && watcher.mSceneHeapGDDR->getFreeSize() > 0,
            "both scene banks retain usable original heap memory");
    require(JKRHeap::sRootHeap->check() && HeapMemoryWatcher::sRootHeapGDDR3->check() &&
            watcher.mGameHeapNapa->check() && watcher.mGameHeapGDDR->check(), "real parent and child block lists remain valid");
    std::fprintf(stderr, "Vita guest heaps: %u real heap checks passed; host layout/OS substitutes, not full startup/device proof\n", checks);
    watcher.mSceneHeapNapa->becomeCurrentHeap();
    return test_dvd_heap_routing();
}
