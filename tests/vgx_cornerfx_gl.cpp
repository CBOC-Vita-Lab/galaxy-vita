// The game's image effects that work in a corner of the EFB (decomp Game/Screen/BloomEffectSimple.cpp, DepthOfFieldBlur.cpp,
// ImageEffectLocalUtil.cpp): the corner (a tile of 1/4 or 1/8 of the EFB) is saved by a copy, used as scratch for a downsampled
// picture and a blur, and put back by drawing the saved copy over the same tile. Through the production front and back end on
// Mesa (no game data); at a render size whose scale is not 1 every logical edge must land on the same render row for the save
// copy, the scratch draws and the restore draw, or a row of scratch shows in the finished picture.
//   vgx_cornerfx_gl <render width> <render height> [mode: 0 the effect, 1 a plain scratch corner: saved, drawn over, copied, put back]
// Prints the picture's hash, the rows whose first pixel is near black where the scene's is not ("black rows": scratch left in
// the picture) and the rows that differ from the scene at all ("bad rows"; at a size where the tile's edge is not on a whole
// render pixel, 608x346, the corner comes back resampled), and exits 0 when there are no black rows.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
const int EW = 640, EH = 456;
alignas(32) unsigned char tex20[32], tex18[32], tex1C[32], tex2C[32], tex24[32], tex28[32], texRead[32], texScene[32];
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport() {
    const uint32_t w[] = {f2u(EW / 2.0f), f2u(-EH / 2.0f), f2u(16777215.0f + 1), f2u(342 + EW / 2.0f), f2u(342 + EH / 2.0f), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, w);
}
// C_MTXOrtho(top 0, bottom EH, left 0, right EW): vertices in EFB pixels, as the effects send them.
void PixelProjection() {
    const uint32_t proj[] = {f2u(2.0f / EW), f2u(-1.0f), f2u(-2.0f / EH), f2u(1.0f), f2u(1.0f), 0, 1};
    WriteXF(0x1020, 7, proj);
}
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
void Blend(bool additive) { BP(0x41, (additive ? (1u | (1u << 5) | (1u << 8)) : 0u) | (1u << 3)); } // (colour update, no alpha update)
void Plain() { // TEV: the register colour, no texture
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
}
void Rect(float l, float t, float r, float b) {
    const float z = -0.5f;
    const float v[16] = {l, b, z, 0, r, b, z, 0, r, t, z, 0, l, t, z, 0};
    DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
}
void Bind(void* data, unsigned format, int w, int h) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = data;
    t.obj.width = static_cast<uint16_t>(w);
    t.obj.height = static_cast<uint16_t>(h);
    t.obj.format = format;
    t.obj.magFilter = 1; t.obj.minFilter = 1;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
}
void TexRect(float l, float t, float r, float b) {
    const float z = -0.5f;
    const float v[] = {l, b, z, 0, 0, 1, r, b, z, 0, 1, 1, r, t, z, 0, 1, 0, l, t, z, 0, 0, 0};
    DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
}
// GXCopyTex as JUTTexture::capture makes it: the source is the texture's size (twice it for a half-size copy).
void Copy(void* dest, unsigned format, int x, int y, int w, int h, bool half, bool clear = false) {
    g.texCopySrc = EfbRect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(half ? 2 * w : w), static_cast<int16_t>(half ? 2 * h : h)};
    g.texCopyHalf = half;
    g.texCopyDstW = static_cast<uint16_t>(w);
    g.texCopyDstH = static_cast<uint16_t>(h);
    g.texCopyFmt = format;
    QueueCopyTex(dest, clear);
}
// The scene: rows of colours exact in RGB565 (the corner's save copy is RGB565), different from one row to the next.
void Scene() {
    Plain();
    Blend(false);
    for (int r = 0; r < EH; ++r) {
        const unsigned v = 64 + 8 * ((r * 7) % 24), gr = 64 + 4 * ((r * 5) % 40);
        Color(v, gr, 248 - (v - 64), 255);
        Rect(0, static_cast<float>(r), EW, static_cast<float>(r + 1));
    }
}
std::vector<uint8_t> ReadAndFlush(void* dest, int& w, int& h) {
    g.texCopySrc = EfbRect{0, 0, EW, EH};
    g.texCopyHalf = false;
    g.texCopyDstW = EW;
    g.texCopyDstH = EH;
    g.texCopyFmt = 6;
    QueueCopyTex(dest, false);
    const auto id = Pending().copyTex.back().texId;
    FrontPresent(false, nullptr);
    std::vector<uint8_t> pixels;
    if (!backend::DebugReadTexture(id, pixels, w, h)) { w = h = 0; pixels.clear(); }
    return pixels;
}
void Begin() {
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12));
    BP(0x21, (342 + EW - 1) | ((342 + EH - 1) << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0, 12, m);
    PixelProjection();
    Viewport();
    BP(0x40, 0); // (no depth test)
    BP(0x42, 1u << 8);
    for (unsigned t = 0; t < 4; ++t) { BP(0xF6 + 2 * t, 0u | (1u << 2)); BP(0xF7 + 2 * t, 2u | (3u << 2)); } // (swap tables: RGBA as is)
}
// BloomEffectSimple::draw: tile 0 of 4 saved, used for the bloom's downsampled picture and threshold, tile 0 of 8 for its blur,
// tile 0 of 4 put back from the saved copy, the bloom added over the whole picture (black here: the picture must be the scene's).
void BloomSimple() {
    const int qw = EW / 4, qh = EH / 4, ew = EW / 8, eh = EH / 8;
    Copy(tex20, 4, 0, 0, qw, qh, false);           // the corner saved
    Copy(tex18, 4, 0, 0, EW / 2, EH / 2, true);    // the whole picture at half size
    Bind(tex18, 4, EW / 2, EH / 2);
    Blend(false);
    TexRect(0, 0, qw, qh);                          // ... drawn into tile 0 (a quarter)
    Copy(tex1C, 4, 0, 0, qw, qh, false);
    Copy(tex2C, 1, 0, 0, qw, qh, false);
    Plain();                                        // the threshold: near black wherever the picture is not bright
    Color(8, 8, 8, 255);
    TexRect(0, 0, qw, qh);
    Copy(tex24, 4, 0, 0, ew, eh, true);             // tile 0 at half size
    Bind(tex24, 4, ew, eh);
    TexRect(0, 0, ew, eh);                          // the blur into tile 0 of 8
    Copy(tex28, 4, 0, 0, ew, eh, false);
    Bind(tex20, 4, qw, qh);
    TexRect(0, 0, qw, qh);                          // the corner put back
    Plain();
    Color(0, 0, 0, 255);
    Blend(true);
    Rect(0, 0, EW, EH);                             // the bloom added (black)
    Blend(false);
}
// A plain scratch corner: a small picture drawn into the top left, copied, the corner put back from a saved copy.
void ScratchCorner() {
    const int qw = 160, qh = 114;
    Copy(tex20, 4, 0, 0, qw, qh, false);
    Plain();
    Color(8, 8, 8, 255);
    Rect(0, 0, qw, qh);
    Copy(tex1C, 4, 0, 0, qw, qh, false);
    Bind(tex20, 4, qw, qh);
    TexRect(0, 0, qw, qh);
}
}
int main(int argc, char** argv) {
    const int rw = argc > 1 ? atoi(argv[1]) : EW, rh = argc > 2 ? atoi(argv[2]) : EH;
    const int mode = argc > 3 ? atoi(argv[3]) : 0;
    if (!host::CreateContext() || !backend::Init(rw, rh)) { puts("init failed"); return 1; }
    ResetState();
    g.efbW = EW; g.efbH = EH;
    // frame 1: the scene alone (the reference)
    Begin();
    Scene();
    int w1 = 0, h1 = 0;
    const auto ref = ReadAndFlush(texScene, w1, h1);
    // frame 2: the scene with the corner effect
    Begin();
    Scene();
    if (mode == 0) BloomSimple();
    else ScratchCorner();
    int w2 = 0, h2 = 0;
    const auto pic = ReadAndFlush(texRead, w2, h2);
    if (ref.empty() || pic.empty() || w1 != w2 || h1 != h2) { printf("render %dx%d: read failed\n", rw, rh); return 2; }
    if (const char* dump = std::getenv("CORNERFX_DUMP")) { // (a PPM of the picture, then of the reference, for a look)
        for (int k = 0; k < 2; ++k) {
            const auto& p = k ? ref : pic;
            const std::string name = std::string(dump) + (k ? "-ref.ppm" : ".ppm");
            if (FILE* f = std::fopen(name.c_str(), "wb")) {
                std::fprintf(f, "P6 %d %d 255\n", w2, h2);
                for (int i = 0; i < w2 * h2; ++i) std::fwrite(&p[i * 4], 1, 3, f);
                std::fclose(f);
            }
        }
    }
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t b : pic) hash = (hash ^ b) * 1099511628211ull;
    int bad = 0, black = 0;
    std::string rows;
    for (int y = 0; y < h2; ++y) {
        int worst = 0, x0 = -1, x1 = -1;
        bool dark = true; // (the corner's first pixel of the row near black where the scene is not: a row of scratch)
        for (int c = 0; c < 3; ++c) dark = dark && pic[(y * w2) * 4 + c] < 24;
        if (dark && ref[(y * w2) * 4] + ref[(y * w2) * 4 + 1] + ref[(y * w2) * 4 + 2] >= 3 * 24) ++black;
        for (int x = 0; x < w2; ++x)
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(int(pic[(y * w2 + x) * 4 + c]) - int(ref[(y * w2 + x) * 4 + c]));
                if (d > 4) { if (x0 < 0) x0 = x; x1 = x; }
                if (d > worst) worst = d;
            }
        if (x0 >= 0) {
            ++bad;
            char b[64];
            std::snprintf(b, sizeof b, " %d(x%d..%d,d%d)", y, x0, x1, worst);
            if (bad <= 8) rows += b;
        }
    }
    printf("render %dx%d mode %d: hash %016llx black rows %d bad rows %d%s\n", rw, rh, mode, static_cast<unsigned long long>(hash), black, bad, rows.c_str());
    return black ? 1 : 0;
}
