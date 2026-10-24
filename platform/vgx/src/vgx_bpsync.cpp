// Galaxy: textures, palettes, EFB copies and the Z texture as the game's own GX library
// programs them, through the pixel-engine / texture-unit registers.
//
// vgx was written under a GX layer that called it (GXLoadTexObj with a host pointer,
// GXCopyTex with a destination pointer). Here the caller is the console's GX library itself,
// compiled from the decompilation, and everything arrives as register writes with console
// addresses: this file turns those back into what the texture cache and the copy code want.
#include "vgx_state.h"
#include "vgx_texdecode.h"

namespace vgx {

namespace {

// Palette memory: the 512 KiB of texture memory above 0x80000 that GXLoadTlut copies palettes
// into, in units of 512 bytes (BP 0x65 bits 0-9). A palette is copied when the load command is
// read, so what a texture is decoded with is what the console would have had loaded, whatever
// the game does to the source afterwards. Entries stay big-endian, as in the game's data.
uint8_t g_tlutMem[kTlutSlots * 512 + 64];
} // namespace
uint32_t g_tlutLoadGen = 1;
namespace {
uint32_t g_tlutVersion[kTlutSlots];

// Texture-unit minification filter (BP bits 5-7: mip filter in bits 0-1, linear in bit 2)
// -> GXTexFilter numbering, which is what TexObj carries.
const uint8_t kMinFilter[8] = {0 /*near*/, 2 /*near mip near*/, 4 /*near mip lin*/, 4, 1 /*linear*/, 3 /*lin mip near*/, 5 /*lin mip lin*/, 5};

inline int TexBase(int m) { return m < 4 ? 0x80 + m : 0xA0 + (m - 4); }

} // namespace

void BpTexSlotSync(int m) {
    m &= 7;
    g.bpTexStale &= static_cast<uint8_t>(~(1u << m));
    const int b = TexBase(m);
    const uint32_t mode0 = g.bp[b], mode1 = g.bp[b + 4], image0 = g.bp[b + 8], image3 = g.bp[b + 0x14], tl = g.bp[b + 0x18];
    TexSlot& slot = g.tex[m];
    TexObj& o = slot.obj;
    uint32_t avail = 0;
    const uint8_t* data = Hooks().resolve ? Hooks().resolve(image3 << 5, &avail) : nullptr;
    o.data = data;
    o.width = static_cast<uint16_t>((image0 & 0x3FF) + 1);
    o.height = static_cast<uint16_t>(((image0 >> 10) & 0x3FF) + 1);
    o.format = (image0 >> 20) & 0xF;
    o.wrapS = static_cast<uint8_t>(mode0 & 3);
    o.wrapT = static_cast<uint8_t>((mode0 >> 2) & 3);
    o.magFilter = static_cast<uint8_t>((mode0 >> 4) & 1);
    const uint32_t hwMin = (mode0 >> 5) & 7;
    o.minFilter = kMinFilter[hwMin];
    o.lodBias = static_cast<float>(static_cast<int8_t>((mode0 >> 9) & 0xFF)) / 32.0f;
    o.minLod = static_cast<float>(mode1 & 0xFF) / 16.0f;
    o.maxLod = static_cast<float>((mode1 >> 8) & 0xFF) / 16.0f;
    o.mipmap = static_cast<uint8_t>((hwMin & 3) != 0 && o.maxLod >= 1.0f);
    o.tlut = 0;
    if (TexFormatIsCI(o.format)) {
        const uint32_t ts = tl & 0x3FF;
        const uint32_t entries = o.format == 8 ? 16u : (o.format == 9 ? 256u : 16384u);
        const uint32_t span = (entries * 2 + 511) >> 9; // palette slots the texture can index
        TlutSlot& t = g.tlut[ts];
        t.obj.data = g_tlutMem + (ts << 9);
        t.obj.format = (tl >> 10) & 3;
        t.obj.entries = static_cast<uint16_t>(entries > 0xFFFF ? 0xFFFF : entries);
        uint32_t ver = 0;
        for (uint32_t i = 0; i < span && ts + i < static_cast<uint32_t>(kTlutSlots); ++i) ver += g_tlutVersion[ts + i];
        // (entries and format are part of what the cached palette hash covers: fold them in)
        t.obj.version = ver * 4099u + entries * 31u + t.obj.format + 1u;
        t.valid = true;
        o.tlut = ts;
    }
    o.userData = nullptr;
    ++o.dataVersion;
    o.magic = 0x76677854u;
    slot.avail = avail;
    slot.valid = data != nullptr && avail >= TexLevelSize(o.width, o.height, o.format);
    ++slot.loadSerial;
}

void BpTlutLoad() {
    const uint32_t r = g.bp[0x65];
    const uint32_t ts = r & 0x3FF, bytes = ((r >> 10) & 0x7FF) * 32u; // count: units of 16 entries
    uint32_t avail = 0;
    const uint8_t* src = Hooks().resolve ? Hooks().resolve(g.bp[0x64] << 5, &avail) : nullptr;
    uint32_t n = bytes;
    const uint32_t room = static_cast<uint32_t>(kTlutSlots * 512) - (ts << 9);
    if (n > room) n = room;
    if (!src || avail < n) {
        VGX_LOG_N(8, "palette load from %08X (%u bytes): not mapped\n", g.bp[0x64] << 5, bytes);
        return;
    }
    uint8_t* dst = g_tlutMem + (ts << 9);
    if (std::memcmp(dst, src, n) == 0) return; // the same palette again (J3D reloads them with every material)
    std::memcpy(dst, src, n);
    ++g_tlutLoadGen;
    for (uint32_t s = ts; s < ts + ((n + 511) >> 9) && s < static_cast<uint32_t>(kTlutSlots); ++s) ++g_tlutVersion[s];
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
    g.bpTexStale = 0xFF;
}

// The copy format the game asked for, in the GXTexFmt / GXCopyTexFmt numbering QueueCopyTex
// switches on, from the copy command (BP 0x52) and the EFB pixel format (BP 0x43):
// GXSetTexCopyDst keeps four bits of the format in the command and tells colour from intensity
// by the "colour conversion" field and colour from depth by the pixel format.
static uint32_t CopyFormat(uint32_t cmd) {
    const uint32_t f = ((cmd >> 4) & 7) | (cmd & 8);
    const bool isZ = (g.bp[0x43] & 7) == 3;
    const bool intensity = ((cmd >> 15) & 3) == 3;
    if (isZ) {
        switch (f) {
        case 0x0: return 0x30; // Z4
        case 0x1: return 0x11; // Z8
        case 0x6: return 0x16; // Z24X8
        case 0x9: return 0x39; // Z8M
        case 0xA: return 0x3A; // Z8L
        case 0xB: return 0x13; // Z16
        case 0xC: return 0x3C; // Z16L
        default: return 0x16;
        }
    }
    switch (f) {
    case 0x0: return intensity ? 0x00 : 0x20; // I4 / R4
    case 0x1: return 0x01;                    // I8
    case 0x2: return intensity ? 0x02 : 0x22; // IA4 / RA4
    case 0x3: return intensity ? 0x03 : 0x23; // IA8 / RA8
    case 0x4: return 0x04;                    // RGB565
    case 0x5: return 0x05;                    // RGB5A3
    case 0x6: return intensity ? 0x26 : 0x06; // YUVA8 / RGBA8
    case 0x7: return 0x27;                    // A8
    case 0x8: return 0x28;                    // R8
    case 0x9: return 0x29;                    // G8
    case 0xA: return 0x2A;                    // B8
    case 0xB: return 0x2B;                    // RG8
    case 0xC: return 0x2C;                    // GB8
    default: return 0x06;
    }
}

void BpCopyTrigger(uint32_t cmd) {
    const bool toXfb = (cmd >> 14) & 1, clear = (cmd >> 11) & 1, half = (cmd >> 9) & 1;
    const uint32_t tl = g.bp[0x49], wh = g.bp[0x4A];
    EfbRect src;
    src.x = static_cast<int16_t>(tl & 0x3FF);
    src.y = static_cast<int16_t>((tl >> 10) & 0x3FF);
    src.w = static_cast<int16_t>((wh & 0x3FF) + 1);
    src.h = static_cast<int16_t>(((wh >> 10) & 0x3FF) + 1);
    if (toXfb) {
        g.dispCopySrc = src;
        QueueCopyDisp(clear);
        if (Hooks().displayCopy) Hooks().displayCopy();
        return;
    }
    uint32_t avail = 0;
    const uint8_t* dst = Hooks().resolve ? Hooks().resolve(g.bp[0x4B] << 5, &avail) : nullptr;
    if (!dst) {
        VGX_LOG_N(8, "EFB copy to %08X: not mapped\n", g.bp[0x4B] << 5);
        // The clear still has to happen: a copy to nowhere with "clear" is how some code clears.
        dst = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(g.bp[0x4B] << 5) | 1u);
    }
    g.texCopySrc = src;
    g.texCopyHalf = half;
    g.texCopyDstW = static_cast<uint16_t>(half ? (src.w + 1) / 2 : src.w);
    g.texCopyDstH = static_cast<uint16_t>(half ? (src.h + 1) / 2 : src.h);
    g.texCopyFmt = CopyFormat(cmd);
    QueueCopyTex(dst, clear);
}

// GXSetZTexture(GX_ZT_REPLACE): the depth a fragment is tested with and writes is a texel of
// the last TEV stage's texture instead of the triangle's own depth. The game uses it with a
// 4x4 texture of one value to clear the depth buffer with a screen-sized rectangle (every
// frame, MainLoopFramework::clearEfb, and DrawUtil::clearZBuffer); GLES2 has no fragment
// depth output, but a constant needs none: the draw's depth range collapses to that value.
bool BpZTexConstant(float& z) {
    const uint32_t gm = g.bp[0x00];
    const int last = static_cast<int>(bits(gm, 10, 4));
    const uint32_t tref = g.bp[0x28 + (last >> 1)] >> ((last & 1) * 12);
    if (!bits(tref, 6, 1)) return false;
    const int m = static_cast<int>(bits(tref, 0, 3));
    if (g.bpTexStale & (1u << m)) BpTexSlotSync(m);
    const TexSlot& slot = g.tex[m];
    if (!slot.valid) return false;
    const TexObj& o = slot.obj;
    const uint8_t* d = static_cast<const uint8_t*>(o.data);
    const uint32_t texels = static_cast<uint32_t>(o.width) * o.height;
    uint32_t v = 0;
    bool constant = true;
    switch (o.format) {
    // (the texture unit's format field has four bits: a Z format arrives as the colour format
    // with the same layout, Z24X8 as RGBA8, Z16 as IA8, Z8 as I8)
    case 0x6: // Z24X8: 4x4 tiles, 16 (X, Z high) pairs then 16 (Z mid, Z low) pairs
        if (slot.avail < TexLevelSize(o.width, o.height, o.format)) return false;
        v = (static_cast<uint32_t>(d[1]) << 16) | (static_cast<uint32_t>(d[32]) << 8) | d[33];
        if (texels <= 256) {
            const uint32_t tiles = ((o.width + 3u) / 4u) * ((o.height + 3u) / 4u);
            for (uint32_t t = 0; t < tiles && constant; ++t)
                for (int i = 0; i < 16; ++i) {
                    const uint8_t* q = d + t * 64u + i * 2;
                    if (q[1] != d[1] || q[32] != d[32] || q[33] != d[33]) { constant = false; break; }
                }
        }
        break;
    case 0x3: v = be16(d); break; // Z16 (one texel looked at)
    case 0x1: v = d[0]; break;    // Z8
    default: return false;
    }
    if (!constant) return false;
    // BP 0xF5 bits 0-1: how wide the texel is (0: 8 bit, 1: 16, 2: 24); 0xF4: the bias added to it.
    const uint32_t type = bits(g.bp[0xF5], 0, 2);
    if (type == 0) v &= 0xFF;
    else if (type == 1) v &= 0xFFFF;
    uint64_t sum = static_cast<uint64_t>(v) + (g.bp[0xF4] & 0xFFFFFFu);
    if (sum > 0xFFFFFFu) sum &= 0xFFFFFFu; // (the hardware wraps)
    z = static_cast<float>(sum) / 16777215.0f;
    return true;
}

} // namespace vgx
