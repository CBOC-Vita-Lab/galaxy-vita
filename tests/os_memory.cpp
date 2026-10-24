#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>

#include "port/port.h"
#include "port/offset32.h"
#include "revolution/os.h"
#include "revolution/aralt.h"
#include "JSystem/J3DGraphBase/J3DTexture.hpp"
#include "Game/Map/KCollision.hpp"

static unsigned checks;
static void require(bool valid, const char* message) {
    ++checks;
    if (!valid) { std::fprintf(stderr, "memory FAILED: %s\n", message); std::exit(1); }
}

extern "C" void port_log(const char*, ...) {}
extern "C" void port_fatal(const char* format, ...) {
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
    std::fputc('\n', stderr); std::exit(2);
}
extern "C" s32 OSDisableScheduler(void) { return 0; }
extern "C" s32 OSEnableScheduler(void) { return 0; }
extern "C" BOOL OSDisableInterrupts(void) { return TRUE; }
extern "C" BOOL OSRestoreInterrupts(BOOL state) { return state; }
extern "C" void OSReport(const char*, ...) {}
extern "C" void GXResetWriteGatherPipe(void);
extern "C" int port_physical_window(uint32_t,void**,uint32_t*,size_t*) __attribute__((weak));

alignas(32) static unsigned char imageData[128];
extern "C" void test_j3d_array_base(unsigned, void*);
extern "C" {
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
void __PortGXFifoOverflow(void) { require(false, "original J3D command fits bounded FIFO"); }
}

int main(int argc, char** argv) {
    require(port_mem_init() == 1, "memory initialization");
    GXResetWriteGatherPipe();
    require(true, "display-list write-gather reset must not translate PPC MMIO on native builds");
    if (argc == 2) {
        if (!std::strcmp(argv[1], "physical-hole")) port_phys_to_host(0x08000000u);
        else if (!std::strcmp(argv[1], "masked-physical")) port_phys_to_host(0x80000000u);
        else if (!std::strcmp(argv[1], "native-hole")) port_resolve_gpu_address(0x7F000000u);
        else if (!std::strcmp(argv[1], "bad-alignment")) OSAllocFromArenaLo(32, 3);
        else if (!std::strcmp(argv[1], "arena-exhausted")) OSAllocFromArenaHi(UINT32_MAX, 32);
        else if (!std::strcmp(argv[1], "arena-outside")) OSSetArenaLo((void*)0x12345);
        else if (!std::strcmp(argv[1], "low-overflow")) port_low_alloc(SIZE_MAX);
        else if (!std::strcmp(argv[1], "low-guard")) { port_low_alloc(32); port_phys_to_host(0x20004000u); }
        else if (!std::strcmp(argv[1], "resource-high-base")) port_offset32(reinterpret_cast<void*>(UINT64_C(0x100001000)), 0);
        else return 3;
        std::fprintf(stderr, "memory FAILED: invalid request did not fail closed\n"); return 1;
    }
    const bool rebased = std::getenv("PETARI_NATIVE_ADDRESSES") != nullptr;
    auto* mem1 = static_cast<unsigned char*>(OSPhysicalToCached(0));
    auto* mem2 = static_cast<unsigned char*>(OSPhysicalToCached(0x10000000));
    require(port_physical_window,"GPU worker can query actual disjoint physical RAM windows");
    void* window=nullptr; uint32_t windowBase=0; size_t windowBytes=0;
    require(port_physical_window(32,&window,&windowBase,&windowBytes) && window==mem1 &&
            windowBase==0 && windowBytes==PORT_MEM1_SIZE,"MEM1 lookup publishes the actual registered region");
    require(port_physical_window(0x10000020u,&window,&windowBase,&windowBytes) && window==mem2 &&
            windowBase==0x10000000u && windowBytes==PORT_MEM2_SIZE,"MEM2 is not guessed contiguous with MEM1");
    require(!port_physical_window(0x08000000u,&window,&windowBase,&windowBytes) && !window &&
            !windowBase && !windowBytes,"physical hole rejects and clears stale window outputs");
    require(!port_physical_window(0,nullptr,&windowBase,&windowBytes),"missing output pointer rejects safely");
    require((reinterpret_cast<uintptr_t>(mem1) < 0x80000000u) == rebased,
            "native-address mode must actually move MEM1 below the console prefix");
    port_mem_set_disc_id("RMGE01");
    require(std::memcmp(mem1, "RMGE01", 6) == 0, "disc ID at translated physical zero");
    require(__OSBusClock == 243000000u && OS_BUS_CLOCK_SPEED == 243000000u, "translated clock globals");
    require(__MEM2End == reinterpret_cast<uintptr_t>(mem2) + PORT_MEM2_SIZE, "native MEM2 end global");
    require(OSIsMEM1Region(mem1) && !OSIsMEM2Region(mem1), "native MEM1 classification");
    require(OSIsMEM2Region(mem2) && !OSIsMEM1Region(mem2), "native MEM2 classification");
    for (uint32_t i = 0; i < 0x10000; i += 32) {
        mem1[i] = static_cast<unsigned char>(i >> 5);
        mem2[i] = static_cast<unsigned char>((i >> 5) ^ 0xA5);
        auto* alias1 = static_cast<unsigned char*>(OSPhysicalToUncached(i));
        auto* alias2 = static_cast<unsigned char*>(OSPhysicalToUncached(0x10000000u + i));
        require(alias1[0] == mem1[i] && alias2[0] == mem2[i], "cached/uncached data identity");
        require(OSCachedToPhysical(mem1 + i) == i && OSUncachedToPhysical(alias1) == i, "MEM1 round trip");
        require(GX_PHY_ADDR(mem1 + i) == i, "SDK GX address encoding must use the native map, not a bit mask");
        require(OSCachedToPhysical(mem2 + i) == 0x10000000u + i, "MEM2 round trip");
        require(port_phys_to_host(OSCachedToPhysical(mem2 + i)) == mem2 + i, "physical/native round trip");
        require(port_resolve_gpu_address(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem1 + i))) == mem1 + i,
                "CP native array address works without a high-bit prefix");
        require(port_resolve_gpu_address(0x10000000u + i) == mem2 + i, "CP physical array address");
        require(OSPhysicalToCached(OSCachedToPhysical(mem1 + i) >> 5 << 5) == mem1 + i,
                "GX word-address encoding preserves aligned pointers");
    }
    const uint32_t imageAddress = port_host_to_phys(imageData);
    require(port_phys_to_host(imageAddress) == imageData, "static DSP/GX image address round trip");
    require((imageAddress & 31u) == 0, "static image word-address alignment");
    void* textureImages[] = {mem1 + 0x20000, mem2 + 0x20000, imageData, nullptr};
    for (void* textureImage : textureImages) {
        GXTexObj texture;
        GXInitTexObj(&texture, textureImage, 16, 8, GX_TF_RGBA8, GX_REPEAT, GX_CLAMP, GX_TRUE);
        void* readImage = nullptr; u16 width = 0, height = 0;
        GXTexFmt format; GXTexWrapMode wrapS, wrapT; GXBool mipmap;
        GXGetTexObjAll(&texture, &readImage, &width, &height, &format, &wrapS, &wrapT, &mipmap);
        require(readImage == textureImage, "SDK texture object native image round trip, including empty binding");
        require(width == 16 && height == 8 && format == GX_TF_RGBA8 && wrapS == GX_REPEAT && wrapT == GX_CLAMP && mipmap,
                "SDK texture state preserved");
    }
    static_assert(sizeof(ResTIMG) == 32, "cooked texture header layout");
    auto* sourceTimg = ::new (mem1 + 0x22000) ResTIMG{};
    sourceTimg->mWidth = 16; sourceTimg->mHeight = 8;
    sourceTimg->mFormat = GX_TF_RGBA8;
    sourceTimg->mImageDataOffset = 0x2000;
    sourceTimg->mPaletteDataOffset = 0x4000;
    auto* relocatedTimg = ::new (mem2 + 0x28000) ResTIMG{};
    J3DTexture table(1, relocatedTimg);
    table.setResTIMG(0, *sourceTimg);
    require(relocatedTimg->mImageDataOffset > INT32_MAX, "texture relocation encodes backward offsets modulo 32 bits");
    require(port_offset32(relocatedTimg, relocatedTimg->mImageDataOffset) == reinterpret_cast<ResTIMG*>(mem1 + 0x24000),
            "relocated image byte offset preserves the original resource address");
    require(port_offset32(relocatedTimg, relocatedTimg->mPaletteDataOffset) == reinterpret_cast<ResTIMG*>(mem1 + 0x26000),
            "relocated palette byte offset preserves the original resource address");
    require(port_offset32(sourceTimg, 0) == sourceTimg && port_offset32(sourceTimg, 32) == sourceTimg + 1,
            "zero and positive offsets remain byte offsets");
    void* textureStorage = port_low_alloc(sizeof(JUTTexture));
    std::memset(textureStorage, 0, sizeof(JUTTexture));
    auto* relocatedTexture = ::new (textureStorage) JUTTexture;
    relocatedTexture->mTIMG = relocatedTimg;
    for (bool indexed : {false, true}) {
        if (indexed) relocatedTexture->initTexObj(GX_TLUT0);
        else relocatedTexture->initTexObj();
        void* readImage = nullptr; u16 width, height; GXTexFmt format;
        GXTexWrapMode wrapS, wrapT; GXBool mipmap;
        GXGetTexObjAll(&relocatedTexture->mObj, &readImage, &width, &height, &format, &wrapS, &wrapT, &mipmap);
        require(readImage == mem1 + 0x24000, "real JUT texture initialization resolves relocated backward image offsets");
    }
    relocatedTimg->mImageDataOffset = 0;
    relocatedTexture->initTexObj();
    void* fallbackImage = nullptr; u16 fallbackWidth, fallbackHeight; GXTexFmt fallbackFormat;
    GXTexWrapMode fallbackWrapS, fallbackWrapT; GXBool fallbackMipmap;
    GXGetTexObjAll(&relocatedTexture->mObj, &fallbackImage, &fallbackWidth, &fallbackHeight,
                  &fallbackFormat, &fallbackWrapS, &fallbackWrapT, &fallbackMipmap);
    require(fallbackImage == mem2 + 0x28020,
            "zero texture offset retains the existing cooked-header fallback");
    static_assert(sizeof(KCLFile) == 0x38 && sizeof(KC_PrismData) == 0x10, "cooked collision layouts");
    for (auto* collisionMemory : {mem1, mem2}) {
        auto* collisionFile = ::new (collisionMemory + 0x30000) KCLFile{};
        collisionFile->mPosOffset = 0x38; collisionFile->mNormOffset = 0x50;
        collisionFile->mPrismOffset = 0x70; collisionFile->mOctreeOffset = 0x90;
        ::new (collisionMemory + 0x30038) TVec3f(12.0f, 34.0f, 56.0f);
        auto* prism = ::new (collisionMemory + 0x30080) KC_PrismData{};
        require(!KCollisionServer::isBinaryInitialized(collisionFile), "cooked collision offsets are not native pointers");
        KCollisionServer collision;
        collision.setData(collisionFile);
        require(collisionFile->mPos.get() == reinterpret_cast<TVec3f*>(collisionMemory + 0x30038), "initial collision position relocation");
        require(KCollisionServer::isBinaryInitialized(collisionFile), "relocated collision recognized without assuming a native sign bit");
        require(collision.getTriangleNum() == 1, "relocated collision prism count");
        require(collisionFile->mNorms.get() == reinterpret_cast<TVec3f*>(collisionMemory + 0x30050) &&
                collisionFile->mPrisms.get() == reinterpret_cast<KC_PrismData*>(collisionMemory + 0x30070) &&
                collisionFile->mOctree.get() == collisionMemory + 0x30090, "all collision table pointers relocate exactly");
        uint32_t collisionPointers[4];
        std::memcpy(collisionPointers, collisionFile, sizeof(collisionPointers));
        collision.setData(collisionFile);
        require(std::memcmp(collisionPointers, collisionFile, sizeof(collisionPointers)) == 0,
                "reusing a collision resource must not relocate its native pointers twice");
        const TVec3f position = collision.getPos(prism, 0);
        require(position.x == 12.0f && position.y == 34.0f && position.z == 56.0f, "reused collision preserves actual prism vertex access");
        delete collision.mapInfo;
    }
    require(port_is_native_range(mem1 + PORT_MEM1_SIZE, 0) && !port_is_native_range(mem1 + PORT_MEM1_SIZE, 1), "half-open native ranges");
    require(!port_is_native_range(nullptr, 0) && !port_is_native_range(mem1, SIZE_MAX), "null and range overflow rejection");
    require(ARInit(nullptr, 0) == 0x4000, "native ARAM initialization");
    auto* source = mem1 + 0x20000;
    auto* destination = mem1 + 0x20100;
    for (unsigned i = 0; i < 64; ++i) source[i] = static_cast<unsigned char>(i * 3);
    std::memset(destination, 0, 64);
    ARStartDMA(0, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(source)), 0x4000, 64);
    ARStartDMA(1, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destination)), 0x4000, 64);
    require(std::memcmp(source, destination, 64) == 0, "ARAM DMA preserves both transfer directions");
    void* lo = OSGetArenaLo();
    void* hi = OSGetArenaHi();
    auto* a = static_cast<unsigned char*>(OSAllocFromArenaLo(65, 64));
    require((reinterpret_cast<uintptr_t>(a) & 63u) == 0 && a >= lo, "low arena alignment");
    auto* b = static_cast<unsigned char*>(OSAllocFromArenaHi(65, 128));
    require((reinterpret_cast<uintptr_t>(b) & 127u) == 0 && b + 65 <= hi, "high arena alignment");
    require(OSGetArenaLo() <= OSGetArenaHi(), "arena bounds remain ordered");
    alignas(32) unsigned char cache[96];
    std::memset(cache, 0xA5, sizeof(cache));
    DCZeroRange(cache + 33, 1);
    require(cache[31] == 0xA5 && cache[32] == 0 && cache[63] == 0 && cache[64] == 0xA5,
            "cache zero keeps console cache-line coverage");
    std::memset(cache, 0xA5, sizeof(cache));
    DCZeroRange(cache + 33, 0);
    require(cache[32] == 0xA5, "empty cache zero must not write a cache line");
    auto* low = static_cast<unsigned char*>(port_low_alloc(123));
    require(reinterpret_cast<uintptr_t>(low) <= UINT32_MAX && !port_is_game_memory(low), "low pool native pointer");
    require(port_phys_to_host(port_host_to_phys(low)) == low, "low pool reversible address");
    void* arrays[] = {mem1 + 0x24000, mem2 + 0x24000, imageData, low,
        OSPhysicalToUncached(0x24000), OSPhysicalToUncached(0x10024000), nullptr};
    for (void* array : arrays) {
        for (const GXAttr attr : {GX_VA_POS, GX_VA_NRM, GX_VA_NBT, GX_VA_CLR0}) {
            unsigned char command[16]{};
            __PortGXFifoPtr = command;
            __PortGXFifoLimit = command + sizeof(command);
            test_j3d_array_base(static_cast<unsigned>(attr), array);
            const unsigned index = attr == GX_VA_NBT ? 1u : attr - GX_VA_POS;
            require(__PortGXFifoPtr == command + 6 && command[0] == 8 && command[1] == 0xA0 + index,
                    "original J3D shape emits exact CP array command and NBT remap");
            uint32_t arrayCommandAddress;
            std::memcpy(&arrayCommandAddress, command + 2, sizeof(arrayCommandAddress));
            arrayCommandAddress = __builtin_bswap32(arrayCommandAddress);
            require(arrayCommandAddress == port_host_to_phys(array),
                    "original J3D shape uses registered physical mapping, not Wii pointer mask");
            if (array) require(port_resolve_gpu_address(arrayCommandAddress) ==
                    port_phys_to_host(port_host_to_phys(array)),
                    "original J3D indexed-array command returns exact owned source");
        }
    }
    std::printf("memory passed: %u checks, %s arenas, image/native/GX/DSP round trips\n", checks, rebased ? "rebased" : "console-address");
}
