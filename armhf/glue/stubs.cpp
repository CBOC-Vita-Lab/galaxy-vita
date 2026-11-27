// What the (excluded) legacy GLES renderer files used to define and the vgx-only armhf build still references.
#include <stdint.h>
#include <stdio.h>
#include "port/vclock.h"

extern "C" void port_headless_write_png(const char*, const unsigned char*, int, int) {}
extern "C" unsigned long long port_vgx_capture(const char*, int);
extern "C" int port_vgx_active(void);
// Wait for actual VGX frame completion; the no-GPU harness still writes no PNG.
extern "C" unsigned long long port_headless_render(const char* path, int scale) {
    if (!port_vgx_active()) {
        fprintf(stderr, "ARMHF_CAPTURE active=0 frame=0\n");
        return 0;
    }
    if (port_vclock_enabled()) port_vclock_release();
    const auto frame = port_vgx_capture(path, scale);
    fprintf(stderr, "ARMHF_CAPTURE active=1 frame=%llu\n", frame);
    return frame;
}

// Integer divide-by-zero: the Vita's libgcc (arm-vita-eabi, no OS) lets __aeabi_idiv/__aeabi_uidiv return 0 ("bl __aeabi_idiv0; mov r0,#0"),
// the GNU/Linux libgcc raises SIGFPE.  The game divides by zero here and there (e.g. JPAVolumeSphere with a division number of 1, whose
// PowerPC divw does not trap), so the harness takes the Vita's behaviour.  (Needs the static libgcc, see CMakeLists.txt.)
extern "C" int __aeabi_idiv0(int value) { return value; }
extern "C" long long __aeabi_ldiv0(long long value) { return value; }

// The in-game settings screen lives in the Vita package (vita/vgx-game/settings_runtime.cpp); the game loop calls this hook.
extern "C" void port_settings_game_boundary(int) {}
