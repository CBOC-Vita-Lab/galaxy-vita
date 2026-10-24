// What the (excluded) legacy GLES renderer files used to define and the vgx-only armhf build still references.
#include <stdint.h>

extern "C" void port_headless_write_png(const char*, const unsigned char*, int, int) {}
extern "C" unsigned long long port_headless_render(const char*, int) { return 0; }

// Integer divide-by-zero: the Vita's libgcc (arm-vita-eabi, no OS) lets __aeabi_idiv/__aeabi_uidiv return 0 ("bl __aeabi_idiv0; mov r0,#0"),
// the GNU/Linux libgcc raises SIGFPE.  The game divides by zero here and there (e.g. JPAVolumeSphere with a division number of 1, whose
// PowerPC divw does not trap), so the harness takes the Vita's behaviour.  (Needs the static libgcc, see CMakeLists.txt.)
extern "C" int __aeabi_idiv0(int value) { return value; }
extern "C" long long __aeabi_ldiv0(long long value) { return value; }
