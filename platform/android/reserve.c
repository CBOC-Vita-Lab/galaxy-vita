// libreserve.so: preloaded into the app process by wrap.sh.
//
// A zygote-forked app process already has ART's large object space and JIT
// cache at ~0x77000000-0x9F000000, right across MEM1, MEM2 and the libgame.so
// slot, so the launcher cannot reserve the game's address window there.
// wrap.sh makes Android exec the app process instead of forking it from the
// zygote, and this constructor runs before app_process starts ART, so ART
// lays out its heap around the window.  launcher.c then finds the window
// already reserved.
#include <android/log.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif

// Same window as launcher.c.
static const uintptr_t kWindowBase = 0x80000000u;
static const size_t kWindowSize = 0x60010000u;

__attribute__((constructor)) static void reserveGameWindow(void) {
    void* win = mmap((void*)kWindowBase, kWindowSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (win != (void*)kWindowBase) {
        __android_log_print(ANDROID_LOG_ERROR, "PetariVR", "libreserve: cannot reserve the game address window at %p (got %p, errno %d)",
                            (void*)kWindowBase, win, errno);
        if (win != MAP_FAILED) {
            munmap(win, kWindowSize);
        }
        return;
    }
    // Shows up as [anon:petari game window] in /proc/<pid>/maps.
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, kWindowBase, kWindowSize, "petari game window");
}
