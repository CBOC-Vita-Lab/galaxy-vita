#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

#include "port/port.h"
#include "revolution/os.h"
#include "memory.h"

namespace {
unsigned checks;
uint32_t allocated;
constexpr uint32_t userBudget = 128u << 20;
alignas(32) unsigned char image[4096];

void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "Vita arena FAILED: %s\n", message); std::exit(1); }
}
}

extern "C" void port_log(const char*, ...) {}
extern "C" void port_fatal(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
    std::fputc('\n', stderr); std::exit(2);
}
extern "C" BOOL OSDisableInterrupts() { return TRUE; }
extern "C" BOOL OSRestoreInterrupts(BOOL value) { return value; }

// Substitute only kernel allocation: the real port owns sizes, bounds and maps.
extern "C" int port_vita_memory_alloc(const char* name, uint32_t bytes, PortVitaMemoryBlock* out) {
    *out = {nullptr, 0, -1};
    if (bytes > userBudget - allocated) return -12;
    uintptr_t base = 0;
    if (!std::strcmp(name, "galaxy-mem1")) base = 0x60000000;
    else if (!std::strcmp(name, "galaxy-mem2")) base = 0x70000000;
    else if (!std::strcmp(name, "galaxy-lc")) base = 0x50000000;
    else return -22;
    void* mapped = mmap(reinterpret_cast<void*>(base), bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped == MAP_FAILED) return -12;
    allocated += bytes;
    *out = {mapped, bytes, static_cast<int32_t>(base >> 16)};
    return 0;
}
extern "C" int port_vita_memory_free(PortVitaMemoryBlock* block) {
    if (munmap(block->base, block->size)) return -22;
    allocated -= block->size; *block = {nullptr, 0, -1}; return 0;
}
extern "C" int port_vita_memory_image(PortVitaImageSegment* segments, uint32_t capacity, uint32_t* count) {
    if (!capacity) return -22;
    segments[0] = {image, sizeof(image)}; *count = 1; return 0;
}
extern "C" int port_vita_memory_free_sizes(uint32_t* user, uint32_t* cdram, uint32_t* phycont) {
    *user = userBudget - allocated; *cdram = 112u << 20; *phycont = 26u << 20; return 0;
}
extern "C" int port_vita_memory_valid_range(const void*, uint32_t) { return 0; }

int test_vita_game_heaps();

int main(int argc, char**) {
    require(port_mem_init() == 1, "original banks fit alongside a native-runtime reservation");
    if (argc == 2) return test_vita_game_heaps();
    require(allocated == 92291072, "only 24 MiB MEM1, 64 MiB MEM2 and 16 KiB locked cache are allocated");
    require(userBudget - allocated == 41926656, "native heap and stack headroom is not consumed by desktop bank expansion");
    auto* mem1 = static_cast<unsigned char*>(port_phys_to_host(0));
    auto* mem2 = static_cast<unsigned char*>(port_phys_to_host(0x10000000));
    void* window = nullptr; uint32_t base = 0; size_t bytes = 0;
    require(port_physical_window(0x017fffff, &window, &base, &bytes) &&
            window == mem1 && base == 0 && bytes == 25165824, "complete original MEM1 is available");
    require(!port_physical_window(0x01800000, &window, &base, &bytes) && !window && !bytes,
            "MEM1 cannot grow into a desktop-only physical extension");
    require(port_physical_window(0x13ffffff, &window, &base, &bytes) &&
            window == mem2 && base == 0x10000000 && bytes == 67108864, "complete original MEM2 is available");
    require(!port_physical_window(0x14000000, &window, &base, &bytes) && !window && !bytes,
            "MEM2 cannot grow beyond the original physical bank");
    mem1[25165823] = 0xA6; mem2[67108863] = 0x5B;
    require(*static_cast<unsigned char*>(port_phys_to_uncached(0x017fffff)) == 0xA6 &&
            *static_cast<unsigned char*>(port_phys_to_uncached(0x13ffffff)) == 0x5B,
            "original last bytes retain cached/uncached identity");
    require(port_is_native_range(mem1 + 25165823, 1) && !port_is_native_range(mem1 + 25165823, 2),
            "MEM1 full-range checks respect its real allocation");
    require(port_is_native_range(mem2 + 67108863, 1) && !port_is_native_range(mem2 + 67108863, 2),
            "MEM2 full-range checks respect its real allocation");
    require(OSGetArenaHi() == mem1 + 25165824 && OSGetMEM2ArenaHi() == mem2 + 67108864,
            "guest arena ends use real native bank sizes");
    const size_t mem1Free = static_cast<unsigned char*>(OSGetArenaHi()) - static_cast<unsigned char*>(OSGetArenaLo());
    const size_t mem2Free = static_cast<unsigned char*>(OSGetMEM2ArenaHi()) - static_cast<unsigned char*>(OSGetMEM2ArenaLo());
    require(OSAllocFromArenaLo(mem1Free, 32) == mem1 + 0x4000 && OSGetArenaLo() == OSGetArenaHi(),
            "the entire original MEM1 arena remains allocatable");
    require(mem2Free == 67106816, "the entire original MEM2 arena is advertised to the guest heap");
    OSSetMEM2ArenaHi(mem2 + 0x800);
    require(OSGetMEM2ArenaLo() == OSGetMEM2ArenaHi(), "MEM2 bounds permit consuming its entire arena");
    require(port_mem_init() == 1 && allocated == 92291072, "reinitialization does not reserve another bank");
    require(!munmap(mem1, 25165824) && !munmap(mem2, 67108864) &&
            !munmap(port_phys_to_host(0x30000000), 16384), "test kernel allocations are retired");
    std::fprintf(stderr, "Vita arenas: %u real memory checks passed; host kernel substitute, not device evidence\n", checks);
}
