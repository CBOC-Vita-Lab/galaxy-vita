// GXV_LC_NATIVE host proof (logiccut, 6 Nov): platform/src/os/os_mem.cpp's region answers (port_is_native_range, port_is_mem1/2,
// port_is_game_memory, port_native_window, port_host_to_phys) printed for 400000 pointers and sizes around every region and bank
// edge (MEM1, MEM2, the locked cache, a low-pool block, the image, unmapped space; sizes 0, small and huge). tools/test_lc_native.sh
// runs it with the switch unset and set and compares the outputs byte for byte. Host addresses: PETARI_NATIVE_ADDRESSES=1 (the rebased
// windows), the driver linked at 0x58000000 so that its image registers as a region too.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
extern "C" {
int port_mem_init(void);
int port_is_native_range(const void* p, size_t size);
uint32_t port_host_to_phys(const void* p);
int port_is_mem1(const void* p);
int port_is_mem2(const void* p);
int port_is_game_memory(const void* p);
int port_native_window(const void* p, void** base, size_t* bytes);
void* port_low_alloc(size_t size);
void port_fatal(const char* f, ...) {
    va_list a;
    va_start(a, f);
    vfprintf(stderr, f, a);
    va_end(a);
    fputc('\n', stderr);
    exit(3);
}
void port_log(const char*, ...) {}
void port_vgx_notify_write(const void*, size_t) {}
}
int main() {
    if (!port_mem_init()) return 2;
    void* lp = port_low_alloc(0x8000);
    std::mt19937 r(7);
    const uintptr_t bases[] = {0x60000000u, 0x70000000u, 0x50000000u, 0x58000000u, 0x59000000u, (uintptr_t)lp,
                               0x61800000u, 0x74000000u, 0x1000u, 0x40000000u};
    for (int i = 0; i < 400000; i++) {
        const uintptr_t b = bases[r() % 10];
        uintptr_t a = b + (uintptr_t)((int)(r() % 0x3000000) - 0x800000);
        if (r() % 4 == 0) a = b + (r() % 64) - 32;
        if (r() % 8 == 0) a = b + 0x1800000 + (r() % 64) - 32;  // around MEM1's end
        if (r() % 8 == 0) a = b + 0x4000000 + (r() % 64) - 32;  // around MEM2's end
        a &= 0xFFFFFFFFu;
        const size_t sz = (r() % 3 == 0) ? 0 : (r() % 2 ? 1 + r() % 64 : r() % 0x5000000);
        void* p = (void*)a;
        void* wb = nullptr;
        size_t wn = 0;
        const int nr = port_is_native_range(p, sz);
        const int w = port_native_window(p, &wb, &wn);
        const uint32_t ph = (a && port_is_native_range(p, 1)) ? port_host_to_phys(p) : 0;
        printf("%lx %zx %d %d %d %d %d %p %zx %x\n", (unsigned long)a, sz, nr, port_is_mem1(p), port_is_mem2(p), port_is_game_memory(p), w,
               wb, wn, ph);
    }
    return 0;
}
