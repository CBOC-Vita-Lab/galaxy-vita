// GXV_SANITIZE through the production GX front end and GLES backend (Mesa on the host). No game data.
// Usage: vgx_sanitize_gl <scenario>; the mode is the environment's GXV_SANITIZE (read once by the backend).
//   valid     ordinary draws and copies: the picture is the same in every mode and nothing is counted
//   nan-proj  a draw whose projection holds NaN
//   viewport  a draw whose viewport has no area
//   copy      an EFB copy of a rectangle with no area, after a good copy to the same texture
//   feedback-copy  a copy out of a texture rectangle into that same texture (bit 4 counts it, nothing is skipped)
#include "vgx_state.h"
#include "vgx_sanitize.h"
#include <GLES2/gl2.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
namespace backend { const sanitize::Counts& DebugSanitizeCounts(); }
}
namespace {
using namespace vgx;
alignas(32) unsigned char destination[32];
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport(float scaleX = 32, float scaleY = -32) {
    const uint32_t w[] = {f2u(scaleX), f2u(scaleY), f2u(16777215.0f + 1), f2u(374), f2u(374), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, w);
}
void Projection(float a) {
    const uint32_t proj[] = {f2u(a), 0, f2u(a), 0, f2u(1), 0, 1};
    WriteXF(0x1020, 7, proj);
}
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
void Pass() {
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
}
void Quad() {
    const float v[] = {-1,-1,-0.5f,0, 1,-1,-0.5f,0, 1,1,-0.5f,0, -1,1,-0.5f,0};
    DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(v), 4);
}
void Flush() { FrontPresent(false, nullptr); }
uint32_t CopyTo(void* dest, int w, int h) {
    g.texCopySrc = EfbRect{0, 0, static_cast<int16_t>(w), static_cast<int16_t>(h)};
    g.texCopyDstW = static_cast<uint16_t>(w < 1 ? 64 : w);
    g.texCopyDstH = static_cast<uint16_t>(h < 1 ? 64 : h);
    g.texCopyFmt = 6;
    QueueCopyTex(dest, false);
    return Pending().copyTex.back().texId;
}
bool Read(uint32_t id, std::vector<uint8_t>& px) {
    int w = 0, h = 0;
    return backend::DebugReadTexture(id, px, w, h) && w >= 1 && h >= 1;
}
const uint8_t* Centre(const std::vector<uint8_t>& p) { return &p[(32 * 64 + 32) * 4]; }
void Scene() {
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12));
    BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0, 12, m);
    Projection(1);
    Viewport();
    Pass();
}
}
int main(int argc, char** argv) {
    const std::string scenario = argc > 1 ? argv[1] : "valid";
    const char* env = std::getenv("GXV_SANITIZE");
    const unsigned mode = sanitize::ParseMode(env && *env ? std::strtol(env, nullptr, 0) : 0);
    const bool acts = sanitize::Acts(mode);
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    Scene();
    Color(0, 255, 0, 255);
    Quad(); // the green background every scenario starts from
    std::vector<uint8_t> px;
    if (scenario == "valid") {
        Color(255, 0, 0, 255);
        Quad();
        const uint32_t id = CopyTo(destination, 64, 64);
        Flush();
        Check(Read(id, px), "read the copy");
        std::printf("centre %u %u %u\n", Centre(px)[0], Centre(px)[1], Centre(px)[2]);
        Check(Centre(px)[0] == 255 && Centre(px)[1] == 0, "the second quad covers the first");
    } else if (scenario == "nan-proj") {
        Color(255, 0, 0, 255);
        Projection(NAN);
        Quad();
        const uint32_t id = CopyTo(destination, 64, 64);
        Flush();
        Check(Read(id, px), "read the copy");
        std::printf("centre %u %u %u\n", Centre(px)[0], Centre(px)[1], Centre(px)[2]);
        if (acts) Check(Centre(px)[1] == 255 && Centre(px)[0] == 0, "the NaN-projection draw did not reach the picture");
    } else if (scenario == "viewport") {
        Color(255, 0, 0, 255);
        Viewport(0, 0);
        Quad();
        const uint32_t id = CopyTo(destination, 64, 64);
        Flush();
        Check(Read(id, px), "read the copy");
        std::printf("centre %u %u %u\n", Centre(px)[0], Centre(px)[1], Centre(px)[2]);
        if (acts) Check(Centre(px)[1] == 255 && Centre(px)[0] == 0, "the draw with no viewport did not reach the picture");
    } else if (scenario == "copy") {
        const uint32_t id = CopyTo(destination, 64, 64); // green, into the texture
        Flush(); // (two copies to one texture in a frame make the first a dead copy)
        const uint32_t again = CopyTo(destination, 0, 0); // a copy of nothing into the same texture
        Check(id == again, "same texture");
        Color(255, 0, 0, 255);
        Quad();
        Flush();
        Check(Read(id, px), "read the texture");
        std::printf("texture centre %u %u %u (%zu bytes)\n", Centre(px)[0], Centre(px)[1], Centre(px)[2], px.size());
        if (acts) Check(px.size() == 64u * 64u * 4u && Centre(px)[1] == 255 && Centre(px)[0] == 0, "the good copy is still there");
    } else if (scenario == "feedback-copy") {
        const uint32_t id = CopyTo(destination, 64, 64); // green, into the texture
        Flush();
        CopyTexCmd self; // (an atlas-style copy whose source texture is its own destination: a copy quad that reads what it writes)
        self.texId = id;
        self.srcTex = id;
        self.srcTexW = self.srcTexH = 64;
        self.src = EfbRect{0, 0, 64, 64};
        self.dstW = self.dstH = 64;
        self.dstTexW = self.dstTexH = 64;
        for (int i = 0; i < 4; ++i) self.mat[i][i] = 1.0f;
        Pending().copyTex.push_back(self);
        Pending().cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(Pending().copyTex.size() - 1)});
        Flush();
        Check(Read(id, px), "read the texture");
        std::printf("texture centre %u %u %u (%zu bytes)\n", Centre(px)[0], Centre(px)[1], Centre(px)[2], px.size());
        Check(Centre(px)[1] == 255 && Centre(px)[0] == 0, "the identity self copy leaves the green picture (nothing is skipped in any mode)");
    } else {
        Check(false, "unknown scenario");
    }
    const sanitize::Counts& c = backend::DebugSanitizeCounts();
    std::printf("mode %u: findings %llu, skipped draws %llu copies %llu uploads %llu; projection words %llu viewport %llu copy %llu\n", mode,
                static_cast<unsigned long long>(c.Total()), static_cast<unsigned long long>(c.skippedDraws), static_cast<unsigned long long>(c.skippedCopies),
                static_cast<unsigned long long>(c.skippedUploads), static_cast<unsigned long long>(c.nonFiniteUniform[UB_PROJ]),
                static_cast<unsigned long long>(c.badViewport), static_cast<unsigned long long>(c.badCopy));
    if (!mode || scenario == "valid") {
        Check(c.Total() == 0 && c.skippedDraws == 0 && c.skippedCopies == 0, "nothing counted or skipped");
    } else if (scenario == "nan-proj") {
        Check(c.nonFiniteUniform[UB_PROJ] >= 1, "NaN projection counted");
        Check(c.skippedDraws == (acts ? 1u : 0u), acts ? "one draw skipped" : "log-only mode skips nothing");
    } else if (scenario == "viewport") {
        Check(c.badViewport >= 1, "empty viewport counted");
        Check(c.skippedDraws == (acts ? 1u : 0u), acts ? "one draw skipped" : "log-only mode skips nothing");
    } else if (scenario == "feedback-copy") {
        std::printf("feedback draws %llu copies %llu\n", static_cast<unsigned long long>(c.feedbackDraws), static_cast<unsigned long long>(c.feedbackCopies));
        Check(c.feedbackCopies == ((mode & sanitize::kFeedback) ? 1u : 0u), (mode & sanitize::kFeedback) ? "the self copy is counted once" : "no feedback counting without bit 4");
        Check(c.feedbackDraws == 0 && c.skippedDraws == 0 && c.skippedCopies == 0, "nothing else counted, nothing skipped");
    } else if (scenario == "copy") {
        Check(c.badCopy == 1, "empty copy counted");
        Check(c.skippedCopies == (acts ? 1u : 0u), acts ? "the copy was reduced to its clear" : "log-only mode skips nothing");
    }
    return failures ? 1 : 0;
}
