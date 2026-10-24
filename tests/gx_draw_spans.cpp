#include "draw_spans.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>
#include <type_traits>

// The fixture never creates GL objects. Production's destructor is in the GL
// renderer; supplying only its lifetime hook keeps this test entirely GL-free.
namespace gpu { TexImage::~TexImage() = default; }

namespace {
using namespace gpu;
int checks = 0;
#define CHECK(test) do { ++checks; if (!(test)) { \
    fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #test); exit(1); } } while (0)

uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
float value(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
void cmd(Frame& f, std::initializer_list<uint32_t> words) {
    f.cmds.insert(f.cmds.end(), words.begin(), words.end());
}
void bp(Frame& f, uint32_t reg, uint32_t data) { cmd(f, {CMD_BP, (reg << 24) | data}); }
void xf(Frame& f, uint32_t address, uint32_t data) { cmd(f, {CMD_XF, address, 1, data}); }
void draw(Frame& f, uint32_t primitive = 0x90, uint32_t count = 3, uint32_t flags = 0) {
    uint32_t first = static_cast<uint32_t>(f.verts.size());
    for (uint32_t i = 0; i < count * vtxWords(flags); ++i) f.verts.push_back(0);
    cmd(f, {CMD_DRAW, primitive, flags, first, count});
}
std::shared_ptr<const DrawSpans> build(std::shared_ptr<Frame> f, DrawSpanBuildOptions options = {}) {
    DrawSpanError error;
    std::shared_ptr<const DrawSpans> out;
    bool ok = buildDrawSpans(f, out, error, options);
    if (!ok) fprintf(stderr, "build failed: word=%zu opcode=%u: %s\n",
                     error.commandWord, error.opcode, error.message.c_str());
    CHECK(ok);
    CHECK(out != nullptr);
    return out;
}

void independentStates() {
    auto f = std::make_shared<Frame>();
    f->startXfRegs[0x26] = 1;
    f->startXfRegs[0x20] = bits(2);
    f->startXfRegs[0x24] = bits(3);
    f->startXfRegs[0x1C] = bits(4);
    f->startXfRegs[0x0C] = 0x11223344;
    f->startBp[0x4F] = 0x1234;
    f->startBp[0x50] = 0x5678;
    f->startBp[0x51] = 0xABCDEF;
    draw(*f);
    draw(*f); // formerly merged, but must still be two draw spans
    xf(*f, 0, bits(9));
    xf(*f, 0x100C, 0xAABBCCDD);
    xf(*f, 0x1020, bits(7));
    bp(*f, 0xE0, 0x7FF | (0x400 << 12));
    draw(*f);
    bp(*f, 0xE0, (1u << 23) | 127 | (31 << 12));
    draw(*f);
    auto out = build(f);
    CHECK(out->draws().size() == 4);
    const auto& a = *out->draws()[0].state;
    const auto& b = *out->draws()[2].state;
    const auto& c = *out->draws()[3].state;
    CHECK(out->draws()[0].state == out->draws()[1].state);
    CHECK(out->draws()[1].state != out->draws()[2].state);
    CHECK(a.uid == b.uid); // uniforms changed without changing the shader key
    CHECK(a.xf.pos[0][0] == 0 && b.xf.pos[0][0] == 9);
    CHECK(a.xf.projection.proj[0][0] == 2 && b.xf.projection.proj[0][0] == 7);
    CHECK(a.xf.projection.proj[3][3] == 1 && a.xf.projection.depthParams[0] == 4);
    CHECK(a.xf.lights.matColor[0][0] == 17 / 255.0f);
    CHECK(b.xf.lights.matColor[0][0] == 170 / 255.0f);
    CHECK(b.pixel.tevReg[0][0] == -1 / 255.0f);
    CHECK(b.pixel.tevReg[0][3] == -1024 / 255.0f);
    CHECK(c.pixel.tevReg[0][0] == b.pixel.tevReg[0][0]);
    CHECK(c.pixel.konst[0][0] == 127 / 255.0f);
    CHECK(c.pixel.konst[0][3] == 31 / 255.0f);
    CHECK(c.registers.tevReg[0] == b.registers.tevReg[0]);
    CHECK(out->clearAR() == 0x1234 && out->clearGB() == 0x5678 && out->clearZ() == 0xABCDEF);
    CHECK(out->draws()[1].firstWord == 9 && out->draws()[1].indices[0] == 0);
    std::weak_ptr<Frame> life = f;
    f.reset();
    CHECK(!life.expired());
    CHECK(out->frame().verts.size() == 36);
    out.reset();
    CHECK(life.expired());
}

void primitiveAndNativeWords() {
    auto f = std::make_shared<Frame>();
    f->startXfRegs[0x18] = 63 | (3 << 6) | (6 << 12) | (9 << 18) | (12 << 24);
    f->startXfRegs[0x19] = 15 | (18 << 6) | (21 << 12) | (60 << 18);
    for (unsigned i = 0; i < 66; ++i)
        for (unsigned k = 0; k < 4; ++k) f->startXfMem[i * 4 + k] = float(i * 4 + k);
    for (unsigned i = 0; i < 99; ++i) f->startXfMem[0x400 + i] = float(i + 1000);
    draw(*f, 0x85, 4);
    draw(*f, 0x88, 4);
    draw(*f, 0x90, 3);
    draw(*f, 0x98, 5);
    draw(*f, 0xA0, 4);
    draw(*f, 0xA8, 4);
    draw(*f, 0xB0, 3);
    uint32_t flags = VF_POSMTX | VF_NRM | VF_NBT | VF_CLR0 | VF_CLR1 | VF_TEX0 | (VF_TEX0 << 7) | VF_TEXMTX;
    draw(*f, 0xB8, 1, flags);
    size_t first = f->verts.size() - vtxWords(flags);
    f->verts[first] = bits(1.25f);
    f->verts[first + 1] = bits(-2.5f);
    f->verts[first + 3] = 62; // normal index 30: last complete 3-row normal
    const unsigned char rgba[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(&f->verts[first + 13], rgba, 4);
    f->verts[first + vtxWords(flags) - 2] = 0x3C060300;
    f->verts[first + vtxWords(flags) - 1] = 0x3F15120F;
    draw(*f, 0xB8, 0); // legal empty draw, retained without GPU indices
    auto out = build(f);
    CHECK(out->draws().size() == 9);
    CHECK(out->draws()[0].primitive == 0x85);
    CHECK(out->draws()[0].indices == (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
    CHECK(out->draws()[1].indices == out->draws()[0].indices);
    CHECK(out->draws()[2].indices == (std::vector<uint32_t>{0, 1, 2}));
    CHECK(out->draws()[3].indices == (std::vector<uint32_t>{0, 1, 2, 2, 1, 3, 2, 3, 4}));
    CHECK(out->draws()[4].indices == (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
    CHECK(out->draws()[5].indices == (std::vector<uint32_t>{0, 1, 2, 3}));
    CHECK(out->draws()[6].indices == (std::vector<uint32_t>{0, 1, 1, 2}));
    CHECK(out->draws()[7].indices == (std::vector<uint32_t>{0}));
    CHECK(out->draws()[8].indices.empty());
    const auto& state = *out->draws()[0].state;
    CHECK((state.xf.matrixA & 63) == 63 && ((state.xf.matrixA >> 6) & 63) == 3);
    CHECK(((state.xf.matrixB >> 18) & 63) == 60);
    CHECK(state.xf.pos[65][3] == 263);
    CHECK(state.xf.normal[32][2] == 1098 && state.xf.normal[32][3] == 0);
    CHECK(out->draws()[7].strideWords == vtxWords(flags));
    CHECK(value(out->frame().verts[first]) == 1.25f);
    CHECK(value(out->frame().verts[first + 1]) == -2.5f);
    CHECK(out->frame().verts[first + 3] == 62);
    CHECK(memcmp(&out->frame().verts[first + 13], rgba, 4) == 0);
    CHECK(out->frame().verts[first + vtxWords(flags) - 1] == 0x3F15120F);
    auto large = std::make_shared<Frame>();
    draw(*large, 0xB8, 65537);
    CHECK(build(large)->draws()[0].indices.back() == 65536);
}

void indirectLodAndFullSnapshots() {
    auto f = std::make_shared<Frame>();
    f->startXfRegs[0x12] = 1;
    f->startXfRegs[0x3F] = 8;
    f->startXfRegs[0x50] = 63 | 0x100;
    for (unsigned i = 0; i < 12; ++i) f->startXfMem[0x500 + 63 * 4 + i] = float(i + 20);
    f->startXfMem[0x604] = 40;
    f->startBp[0] = 4 << 16;
    f->startBp[0x25] = 0xABCD;
    f->startBp[0x26] = 0x1234;
    f->startBp[0x27] = 1 | (2 << 6) | (3 << 12) | (4 << 18);
    f->startBp[0x28] = (1 << 6) | 7;
    f->startBp[0x06] = 1023 | (0x7FF << 11) | (1 << 22);
    f->startBp[0x07] = 2;
    f->startBp[0x08] = 3 | (1 << 22); // scale exponent = 17 - 17 = 0
    f->startBp[0x80] = 0x80 << 9;
    f->startBp[0x88] = 63 | (31 << 10);
    f->startBp[0xF3] = 19 | (29 << 8) | (0x81 << 16);
    f->startBp[0xF4] = 0xFFFFFF;
    f->startBp[0xEE] = bits(2.0f) >> 12;
    f->startBp[0xF1] = (bits(3.0f) >> 12) | (4 << 20);
    f->startBp[0xF2] = 0x112233;
    f->startBp[0xE8] = 0x12345; // retain raw not-yet-implemented fog-range registers
    draw(*f);
    bp(*f, 0x80, 0x7F << 9);
    bp(*f, 0xA0, 0xFE << 9);
    bp(*f, 0x25, 0xFEDC);
    bp(*f, 0x26, 0x9876);
    xf(*f, 0x1050, 0);
    xf(*f, 0x604, bits(55));
    draw(*f);
    auto out = build(f);
    const auto& a = *out->draws()[0].state;
    const auto& b = *out->draws()[1].state;
    CHECK(a.uid.indScale[0] == 0xABCD && a.uid.indScale[1] == 0x1234);
    CHECK(b.uid.indScale[0] == 0xFEDC && b.uid.indScale[1] == 0x9876);
    CHECK(a.usedTextureMask == ((1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 7)));
    CHECK(a.pixel.texSize[0][0] == -4 && b.pixel.texSize[0][0] == 127 / 32.0f);
    CHECK(b.pixel.texSize[4][0] == -2 / 32.0f);
    CHECK(a.pixel.texSize[0][1] == 32 && a.pixel.texSize[0][2] == 1 / 64.0f);
    CHECK(a.pixel.indMtx[0][0][0] == 1023 / 1024.0f);
    CHECK(a.pixel.indMtx[0][1][0] == -1 / 1024.0f);
    CHECK(a.pixel.indMtx[0][0][1] == 2 / 1024.0f);
    CHECK(a.xf.post[0][0] == 20 && a.xf.post[2][3] == 31);
    CHECK(b.xf.post[0][0] == 0);
    CHECK(a.xf.lights.lights[0][1][0] == 40 && b.xf.lights.lights[0][1][0] == 55);
    CHECK(a.pixel.alphaRef[0] == 19 && a.pixel.alphaRef[1] == 29 && a.pixel.alphaRef[2] == 16777215);
    CHECK(a.pixel.fogParams[0] == 2 && a.pixel.fogParams[1] == 3);
    CHECK(a.pixel.fogColor[0] == 17 / 255.0f && a.pixel.fogColor[3] == 1);
    CHECK(a.registers.bp[0xE8] == 0x12345);
    CHECK(a.textures[0].mode0 == (0x80 << 9) && b.textures[4].mode0 == (0xFE << 9));
}

void copiesMarkersTexturesAndRaster() {
    auto f = std::make_shared<Frame>();
    auto image = std::make_shared<TexImage>();
    image->width = 2; image->height = 2; image->rgba.resize(4); image->version = 42;
    f->textures.push_back(image);
    f->startXfRegs[0x1A] = bits(320);
    f->startXfRegs[0x1B] = bits(-228);
    f->startXfRegs[0x1D] = bits(326);
    f->startXfRegs[0x1E] = bits(236);
    f->startBp[0x59] = 3 | (4 << 10);
    f->startBp[0x20] = (6 << 12) | 8;
    f->startBp[0x21] = (645 << 12) | 463;
    f->startBp[0x41] = 1 | (1 << 3) | (1 << 4) | (2 << 8) | (3 << 5);
    f->startBp[0x42] = 0x180;
    f->startBp[0x43] = 1;
    f->startBp[0x40] = 1 | (1 << 4) | (3 << 1);
    f->startBp[0] = 2 << 14;
    f->startBp[0x84] = 0x10203;
    cmd(*f, {CMD_CAMERA});
    for (unsigned i = 0; i < kCameraWords; ++i) f->cmds.push_back(i + 100);
    cmd(*f, {CMD_TEX, 0, 0});
    cmd(*f, {CMD_TEX_EFB, 1, 77, 320, 228, 0x123456E0});
    cmd(*f, {CMD_MARKER, MARK_SKY_BEGIN});
    cmd(*f, {CMD_MARKER, MARK_POINTER_BEGIN});
    draw(*f);
    cmd(*f, {CMD_EFB_COPY, 77, 0x111, 319 | (227 << 10), 0x222, 0x333, 0x444, 0x555, 0x666, 0x777, 1 << 14});
    cmd(*f, {CMD_MARKER, MARK_SCENE_DEPTH});
    cmd(*f, {CMD_MARKER, MARK_PLAYER_BEGIN});
    cmd(*f, {CMD_MARKER, MARK_HUD_BEGIN});
    bp(*f, 0x41, (1 << 11) | (1 << 4));
    bp(*f, 0x43, 0);
    bp(*f, 0x40, 7 << 1);
    cmd(*f, {CMD_TEX, 0, ~0u});
    draw(*f);
    cmd(*f, {CMD_CAMERA});
    for (unsigned i = 0; i < kCameraWords; ++i) f->cmds.push_back(i + 200);
    cmd(*f, {CMD_MARKER, MARK_HUD_END});
    cmd(*f, {CMD_MARKER, MARK_PLAYER_END});
    cmd(*f, {CMD_MARKER, MARK_POINTER_END});
    cmd(*f, {CMD_MARKER, MARK_SKY_END});
    draw(*f);
    auto out = build(f);
    CHECK(out->draws().size() == 3 && out->copies().size() == 1);
    const auto& a = *out->draws()[0].state;
    const auto& b = *out->draws()[1].state;
    const auto& c = *out->draws()[2].state;
    CHECK(a.xf.flags == 7 && a.uid.screenTexGens == 0);
    CHECK(a.raster.viewport[0] == 0 && a.raster.viewport[1] == 0);
    CHECK(a.raster.viewport[2] == 640 && a.raster.viewport[3] == 456);
    CHECK(a.raster.scissor[0] == 0 && a.raster.scissor[1] == 0);
    CHECK(a.raster.scissor[2] == 640 && a.raster.scissor[3] == 456);
    CHECK(a.raster.colorMask == 15 && a.raster.blend);
    CHECK(a.raster.src == SpanBlendFactor::DstColor && a.raster.dst == SpanBlendFactor::OneMinusSrcColor);
    CHECK(a.raster.dstAlpha && a.raster.dstAlphaValue == 128 / 255.0f);
    CHECK(a.raster.depthTest && a.raster.depthWrite && a.raster.depthFunc == SpanDepthFunc::LessEqual);
    CHECK(a.raster.cull == SpanCull::Back);
    CHECK(b.raster.colorMask == 0 && b.raster.equation == SpanBlendEquation::ReverseSubtract);
    CHECK(b.raster.src == SpanBlendFactor::One && b.raster.dst == SpanBlendFactor::One && b.raster.blend);
    CHECK(!b.raster.depthTest && !b.raster.depthWrite && b.raster.depthFunc == SpanDepthFunc::Always);
    CHECK(a.textures[0].kind == SpanTextureKind::Image && a.textures[0].imageIndex == 0);
    CHECK(a.textures[0].version == 42 && a.textures[0].width == 2 && a.textures[0].mode1 == 0x10203);
    CHECK(a.textures[1].kind == SpanTextureKind::EfbAddress);
    CHECK(a.textures[1].copyId == 77 && a.textures[1].address == 0x123456E0 && a.textures[1].width == 320);
    CHECK(b.textures[0].kind == SpanTextureKind::Unbound);
    CHECK(a.efbWidth == 640 && a.pixel.efbSize[1] == 456);
    CHECK(b.efbWidth == 320 && b.pixel.efbSize[1] == 228);
    CHECK(b.markers.hud && b.markers.player && b.markers.pointer && b.uid.hud);
    CHECK(!c.markers.hud && !c.markers.sky && !c.markers.pointer && !c.markers.player);
    CHECK(c.camera.words[0] == 100 && out->cameras()[1].words[0] == 200);
    CHECK(out->copies()[0].words[0] == 77 && out->copies()[0].words[8] == 0x777);
    CHECK(out->copies()[0].pixelFormat == 1 && out->copies()[0].depthMode == f->startBp[0x40]);
    CHECK(out->copies()[0].colorMode == f->startBp[0x41] && out->copies()[0].efbWidth == 640);
    CHECK(out->efbWidth() == 320 && out->efbHeight() == 228);
    const auto& events = out->events();
    CHECK(events.size() == 15);
    CHECK(events[0].kind == SpanEventKind::Camera && events[3].kind == SpanEventKind::Draw);
    CHECK(events[4].kind == SpanEventKind::Copy && events[4].index == 0);
    CHECK(events[5].kind == SpanEventKind::Marker && out->markers()[events[5].index].kind == MARK_SCENE_DEPTH);
    CHECK(events[8].kind == SpanEventKind::Draw && events[9].kind == SpanEventKind::Camera);
    CHECK(events.back().kind == SpanEventKind::Draw && events.back().index == 2);
    for (size_t i = 1; i < events.size(); ++i) CHECK(events[i].commandWord > events[i - 1].commandWord);
    auto next = std::make_shared<Frame>();
    draw(*next);
    CHECK(build(next, {out->efbWidth(), out->efbHeight()})->draws()[0].state->pixel.efbSize[1] == 228);
}

void snapshotEdgesAndPrimitiveTails() {
    auto f = std::make_shared<Frame>();
    for (unsigned i = 0; i < 0x1000; ++i) f->startXfMem[i] = float(i) / 8;
    for (unsigned t = 0; t < 8; ++t) {
        f->startXfRegs[0x50 + t] = t * 7;
        f->startXfRegs[0x40 + t] = 0x1200 + t;
    }
    f->startXfRegs[0x3F] = 8;
    f->startXfRegs[0x09] = 2;
    f->startXfRegs[0x12] = 1;
    f->startXfRegs[0x0E] = 0x2345; f->startXfRegs[0x0F] = 0x6789;
    f->startXfRegs[0x10] = 0x3456; f->startXfRegs[0x11] = 0x789A;
    for (unsigned i = 0; i < 6; ++i) f->startXfRegs[0x20 + i] = bits(float(i + 1));
    f->startXfRegs[0x1C] = bits(16777215); f->startXfRegs[0x1F] = bits(-16777215);
    for (unsigned l = 0; l < 8; ++l) {
        uint32_t c = 0x10203040 + l;
        memcpy(&f->startXfMem[0x603 + l * 16], &c, 4);
    }
    const uint32_t modes[8] = {0x80, 0x81, 0x82, 0x83, 0xA0, 0xA1, 0xA2, 0xA3};
    const uint32_t images[8] = {0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB};
    for (unsigned t = 0; t < 8; ++t) {
        f->startBp[modes[t]] = (248 + t) << 9;
        f->startBp[modes[t] + 4] = 0x100 + t;
        f->startBp[images[t]] = (t * 3) | ((t * 5) << 10);
    }
    f->startBp[0] = 15 << 10; // all 16 TEV stages, odd/even order halves
    for (unsigned i = 0; i < 8; ++i) f->startBp[0x28 + i] = (64 | i) | ((64 | (7 - i)) << 12);
    draw(*f, 0xB8, 1);
    // Changes just beyond the old dirty ranges must still be snapshotted.
    xf(*f, 0x107, bits(-11));
    xf(*f, 0x462, bits(-22));
    xf(*f, 0x50B, bits(-33));
    draw(*f, 0xB8, 1);
    auto out = build(f);
    const auto& a = *out->draws()[0].state;
    const auto& b = *out->draws()[1].state;
    CHECK(a.uid.numTevStages == 16 && a.uid.numTexGens == 8 && a.uid.numChans == 2);
    CHECK(a.uid == makeShaderUid(f->startBp, f->startXfRegs, 0, false));
    CHECK(a.usedTextureMask == 255);
    for (unsigned r = 0; r < 66; ++r)
        for (unsigned k = 0; k < 4; ++k) CHECK(a.xf.pos[r][k] == f->startXfMem[r * 4 + k]);
    for (unsigned r = 0; r < 33; ++r) {
        for (unsigned k = 0; k < 3; ++k) CHECK(a.xf.normal[r][k] == f->startXfMem[0x400 + r * 3 + k]);
        CHECK(a.xf.normal[r][3] == 0);
    }
    for (unsigned t = 0; t < 8; ++t) {
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned k = 0; k < 4; ++k)
                CHECK(a.xf.post[t * 3 + r][k] == f->startXfMem[0x500 + t * 28 + r * 4 + k]);
        CHECK(a.textures[t].mode0 == f->startBp[modes[t]]);
        CHECK(a.textures[t].mode1 == f->startBp[modes[t] + 4]);
        CHECK(a.pixel.texSize[t][0] == (int(t) - 8) / 32.0f);
        CHECK(a.pixel.texSize[t][1] == float(t * 5 + 1));
        CHECK(a.pixel.texSize[t][2] == 1 / float(t * 3 + 1));
    }
    for (unsigned l = 0; l < 8; ++l) {
        CHECK(a.xf.lights.lights[l][0][0] == 16 / 255.0f);
        CHECK(a.xf.lights.lights[l][0][3] == (64 + l) / 255.0f);
        for (unsigned v = 1; v < 5; ++v) {
            for (unsigned k = 0; k < 3; ++k)
                CHECK(a.xf.lights.lights[l][v][k] == f->startXfMem[0x600 + l * 16 + 1 + v * 3 + k]);
            CHECK(a.xf.lights.lights[l][v][3] == 0);
        }
    }
    CHECK(a.xf.projection.proj[0][0] == 1 && a.xf.projection.proj[0][2] == 2);
    CHECK(a.xf.projection.proj[1][1] == 3 && a.xf.projection.proj[1][2] == 4);
    CHECK(a.xf.projection.proj[2][2] == 5 && a.xf.projection.proj[2][3] == 6);
    CHECK(a.xf.projection.proj[3][2] == -1 && a.xf.projection.proj[3][3] == 0);
    CHECK(a.xf.projection.depthParams[0] == 16777215 && a.xf.projection.depthParams[1] == -16777215);
    CHECK(b.xf.pos[65][3] == -11 && a.xf.pos[65][3] != -11);
    CHECK(b.xf.normal[32][2] == -22 && a.xf.normal[32][2] != -22);
    CHECK(b.xf.post[2][3] == -33 && a.xf.post[2][3] != -33);
    auto tails = std::make_shared<Frame>();
    draw(*tails, 0x80, 5); draw(*tails, 0x90, 4); draw(*tails, 0xA8, 3);
    draw(*tails, 0x98, 2); draw(*tails, 0xA0, 1); draw(*tails, 0xB0, 1);
    auto tailOut = build(tails);
    // GX's incomplete final primitive produces no geometry, not an invalid
    // recording. Keep the original vertices, and match Builder's expansion.
    CHECK(tailOut->draws()[0].indices == (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
    CHECK(tailOut->draws()[1].indices == (std::vector<uint32_t>{0, 1, 2}));
    CHECK(tailOut->draws()[2].indices == (std::vector<uint32_t>{0, 1}));
    for (unsigned i = 3; i < 6; ++i) CHECK(tailOut->draws()[i].indices.empty());
    CHECK(tailOut->draws()[0].vertexCount == 5);
    auto empty = std::make_shared<Frame>();
    CHECK(build(empty)->events().empty());
}

void commandPrefixes() {
    auto full = std::make_shared<Frame>();
    std::vector<size_t> boundaries{0};
    auto add = [&](std::initializer_list<uint32_t> words) {
        cmd(*full, words); boundaries.push_back(full->cmds.size());
    };
    add({CMD_XF, 0x1000, 2, 0, 0});
    add({CMD_BP, 0x25001234});
    add({CMD_TEX, 0, ~0u});
    add({CMD_TEX_EFB, 1, 0, 64, 32, 0x80001000});
    add({CMD_MARKER, MARK_POINTER_BEGIN});
    cmd(*full, {CMD_CAMERA});
    full->cmds.insert(full->cmds.end(), kCameraWords, 0);
    boundaries.push_back(full->cmds.size());
    draw(*full); boundaries.push_back(full->cmds.size());
    add({CMD_EFB_COPY, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    add({CMD_MARKER, MARK_POINTER_END});
    for (size_t cut = 0; cut <= full->cmds.size(); ++cut) {
        auto prefix = std::make_shared<Frame>(*full);
        prefix->cmds.resize(cut);
        size_t lastBoundary = 0;
        bool complete = false;
        for (size_t end : boundaries) {
            if (end <= cut) lastBoundary = end;
            if (end == cut) complete = true;
        }
        DrawSpanError error;
        std::shared_ptr<const DrawSpans> out;
        CHECK(buildDrawSpans(prefix, out, error) == complete);
        if (!complete) {
            CHECK(!out && !error.message.empty());
            CHECK(error.commandWord == lastBoundary && error.opcode == full->cmds[lastBoundary]);
        } else CHECK(out && error.message.empty());
    }
}

void addressKeyedCopyBarriers() {
    auto f = std::make_shared<Frame>();
    f->startBp[0x43] = 3; // depth-texture source format must survive the barrier
    f->startBp[0x41] = 8; // color-only clear, no alpha update
    f->startBp[0x40] = 16; // depth update independent of depth test
    f->startBp[0x53] = 0x123456; f->startBp[0x54] = 0x654321;
    f->startBp[0x28] = 64;
    cmd(*f, {CMD_TEX_EFB, 0, 99, 64, 32, 0x80002000});
    draw(*f); // reads this address's preceding-frame contents, not future copy 99
    uint32_t firstCopy[10] = {99, 0x102, 63 | (31 << 10), 0x80002000 >> 5, 0x10, 0x20, 0xA1B2, 0xC3D4, 0xFEDCBA, 1 << 11};
    cmd(*f, {CMD_EFB_COPY});
    f->cmds.insert(f->cmds.end(), firstCopy, firstCopy + 10);
    draw(*f); // sees copy 99, before copy 100 overwrites the same address
    bp(*f, 0x41, 16);
    bp(*f, 0x40, 0);
    bp(*f, 0x53, 0xA1B2C3); bp(*f, 0x54, 0xC3B2A1);
    uint32_t secondCopy[10] = {100, 0x203, 63 | (31 << 10), 0x80002000 >> 5, 0x30, 0x40, 0x1122, 0x3344, 0x556677, (1 << 11) | (1 << 9)};
    cmd(*f, {CMD_EFB_COPY});
    f->cmds.insert(f->cmds.end(), secondCopy, secondCopy + 10);
    draw(*f);
    auto out = build(f);
    CHECK(out->events().size() == 5 && out->draws().size() == 3 && out->copies().size() == 2);
    for (unsigned i = 0; i < 5; ++i)
        CHECK(out->events()[i].kind == (i & 1 ? SpanEventKind::Copy : SpanEventKind::Draw));
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(out->draws()[i].state->textures[0].address == 0x80002000);
        CHECK(out->draws()[i].state->textures[0].copyId == 99); // informational only
        CHECK(out->draws()[i].state->efbWidth == 640); // non-XFB copies do not resize EFB
    }
    CHECK(out->draws()[0].state != out->draws()[1].state);
    CHECK(out->draws()[1].state != out->draws()[2].state);
    CHECK(memcmp(out->copies()[0].words, firstCopy, sizeof(firstCopy)) == 0);
    CHECK(memcmp(out->copies()[1].words, secondCopy, sizeof(secondCopy)) == 0);
    CHECK(out->copies()[0].pixelFormat == 3 && out->copies()[1].pixelFormat == 3);
    CHECK(out->copies()[0].colorMode == 8 && out->copies()[0].depthMode == 16);
    CHECK(out->copies()[1].colorMode == 16 && out->copies()[1].depthMode == 0);
    CHECK(out->copies()[0].filter[0] == 0x123456 && out->copies()[0].filter[1] == 0x654321);
    CHECK(out->copies()[1].filter[0] == 0xA1B2C3 && out->copies()[1].filter[1] == 0xC3B2A1);
}

void malformed() {
    auto good = std::make_shared<Frame>();
    draw(*good);
    auto prior = build(good);
    auto reject = [&](std::shared_ptr<Frame> f, size_t at, uint32_t opcode) {
        auto out = prior;
        DrawSpanError error;
        CHECK(!buildDrawSpans(f, out, error));
        CHECK(out == nullptr && !error.message.empty());
        CHECK(error.commandWord == at && error.opcode == opcode);
    };
    reject(nullptr, 0, 0);
    const std::initializer_list<uint32_t> truncated[] = {
        {CMD_XF}, {CMD_XF, 0, 2, 0}, {CMD_BP}, {CMD_DRAW, 0x90, 0, 0},
        {CMD_TEX, 0}, {CMD_TEX_EFB, 0, 0, 1, 1}, {CMD_EFB_COPY, 0}, {CMD_MARKER}, {CMD_CAMERA, 0}
    };
    for (const auto& words : truncated) {
        auto f = std::make_shared<Frame>(); cmd(*f, words); reject(f, 0, *words.begin());
    }
    auto bad = [&](std::initializer_list<uint32_t> words, uint32_t vertexWords = 0) {
        auto f = std::make_shared<Frame>(); cmd(*f, words);
        f->verts.assign(vertexWords, 0); reject(f, 0, *words.begin());
    };
    bad({0xDEAD});
    bad({CMD_XF, 0x10FF, 2, 0, 0});
    bad({CMD_XF, 0xFFFFFFFF, 1, 0});
    bad({CMD_XF, 0, 0});
    bad({CMD_TEX, 8, ~0u});
    bad({CMD_TEX, 0, 0});
    bad({CMD_TEX_EFB, 0, 1, 0, 1, 0});
    bad({CMD_TEX_EFB, 8, 1, 1, 1, 0});
    bad({CMD_TEX_EFB, 0, 1, 1025, 1, 0});
    bad({CMD_MARKER, 10});
    bad({CMD_DRAW, 0xC0, 0, 0, 3}, 9);
    bad({CMD_DRAW, 0x190, 0, 0, 3}, 9);
    bad({CMD_DRAW, 0x90, 1 << 14, 0, 3}, 9);
    bad({CMD_DRAW, 0x90, VF_NBT, 0, 3}, 27);
    bad({CMD_DRAW, 0x90, 0, 0xFFFFFFFF, 3}, 9);
    bad({CMD_DRAW, 0x90, 0, 0, 3}, 8);
    bad({CMD_DRAW, 0xB8, 0, 0, 0xFFFFFFFF}, 9);
    auto f = std::make_shared<Frame>();
    draw(*f); cmd(*f, {CMD_BP}); reject(f, 5, CMD_BP); // no partial output
    for (unsigned kind = 0; kind < 3; ++kind) {
        f = std::make_shared<Frame>();
        if (kind == 0) f->startXfRegs[0x3F] = 9;
        if (kind == 1) f->startXfRegs[0x09] = 3;
        if (kind == 2) f->startBp[0] = 5 << 16;
        draw(*f); reject(f, 0, CMD_DRAW);
    }
    f = std::make_shared<Frame>(); draw(*f, 0xB8, 1, VF_POSMTX);
    f->verts[3] = 64; reject(f, 0, CMD_DRAW);
    f = std::make_shared<Frame>(); draw(*f, 0xB8, 1, VF_POSMTX | VF_NRM);
    f->verts[3] = 63; reject(f, 0, CMD_DRAW); // normal row 33 would be out of range
    f = std::make_shared<Frame>(); f->startXfRegs[0x18] = 31;
    draw(*f, 0xB8, 1, VF_NRM); reject(f, 0, CMD_DRAW);
    f = std::make_shared<Frame>(); draw(*f, 0xB8, 1, VF_TEXMTX);
    f->verts[3] = 0x40000000; reject(f, 0, CMD_DRAW);
    f = std::make_shared<Frame>(); f->textures.push_back(nullptr);
    cmd(*f, {CMD_TEX, 0, 0}); reject(f, 0, CMD_TEX);
    DrawSpanError error;
    auto out = prior;
    CHECK(!buildDrawSpans(good, out, error, {0, 456}) && !out && !error.message.empty());
}
} // namespace

int main() {
    static_assert(std::is_same_v<decltype(DrawSpanState::xf), gpu::ResolvedXf>);
    static_assert(sizeof(SpanTextureKind) == 4 && sizeof(SpanEventKind) == 4 && sizeof(SpanDepthFunc) == 4);
    independentStates();
    primitiveAndNativeWords();
    indirectLodAndFullSnapshots();
    copiesMarkersTexturesAndRaster();
    snapshotEdgesAndPrimitiveTails();
    commandPrefixes();
    addressKeyedCopyBarriers();
    malformed();
    printf("gx_draw_spans: %d checks passed (immutable snapshots, topology, native words, indirect/LOD, ordered barriers, malformed input)\n", checks);
}
