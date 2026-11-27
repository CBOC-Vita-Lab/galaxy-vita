// Null back end: accepts command buffers and draws nothing. Selected at build
// time (BACKEND=null) for targets without a GL library, i.e. the armhf Linux
// build that runs under qemu to count what the front end costs the game
// thread. GXV_NULL_WALK=1 makes it touch every command the way a real
// back end would have to (so the cost of *reading* a frame's buffer shows up
// on the render thread in profiles).
#include "vgx_cmd.h"

namespace vgx {

namespace host {
bool CreateContext() { return true; }
}

namespace backend {

namespace {
Stats g_stats;
std::vector<uint8_t> g_none;
uint64_t g_walkSum = 0;
}

bool Init(int, int) { return true; }
int ExportCountLog() { return 0; }
uint64_t Cache24Hints() { return 0; }

// The same answers as the GL back end's (vgx_backend_gl.cpp), so that a run on this back
// end hands over exactly what a run on that one would be handed.
const Caps& GetCaps() {
    static const Caps caps = [] {
        const long vita = EnvInt("GXV_VITA", 0);
        Caps c;
        c.dxt1 = EnvInt("GXV_DXT1", 1) != 0;
        c.mipGen = EnvInt("GXV_MIPGEN", vita) != 0;
        c.mipClamp = EnvInt("GXV_MIPCLAMP", vita) != 0;
        const long la88 = EnvInt("GXV_LA88", vita);
        c.la88 = la88 != 0;
        c.i8 = la88 >= 2;
        return c;
    }();
    return caps;
}

void Execute(const CmdBuf& buf) {
    static const bool walk = EnvInt("GXV_NULL_WALK", 0) != 0;
    g_stats.drawCallsThisFrame = 0;
    for (const Cmd& c : buf.cmds) {
        switch (c.type) {
        case CMD_DRAW: ++g_stats.drawCallsThisFrame; break;
        case CMD_SHADER:
            if (buf.shaders[c.arg]->fragment) ++g_stats.fragmentShaders; else ++g_stats.vertexShaders;
            break;
        case CMD_TEX_UPLOAD:
            ++g_stats.textures;
            for (const auto& l : buf.uploads[c.arg].levels) g_stats.textureBytes += l.size();
            break;
        default: break;
        }
    }
    if (walk) {
        uint64_t s = 0;
        for (size_t i = 0; i < buf.vtx.size(); i += 64) s += buf.vtx[i];
        for (size_t i = 0; i < buf.idx.size(); i += 32) s += buf.idx[i];
        for (size_t i = 0; i < buf.uni.size(); i += 16) s += static_cast<uint64_t>(buf.uni[i]);
        g_walkSum += s;
    }
}

const std::vector<uint8_t>& LastDisplayCopy(int& width, int& height) {
    width = height = 0;
    return g_none;
}
void RequestCapture(bool) {}
void SetPresentHook(PresentHook) {}
void SetHudNativeHost(int, int) {}
int PresentPhase() { return 0; }
void SetHudNativeDepth(bool) {}
void SetHudNativeTarget(unsigned, int, int, int, int, bool) {}
void TailFoldCounters(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = 0; }
void HudNativeCounters(uint64_t out[kHudNativeReasons + 4]) { for (int i = 0; i < kHudNativeReasons + 4; ++i) out[i] = 0; }
const char* HudNativeReasonName(int) { return "?"; }
int PresentCharaAA(unsigned, int, int, int, int, int, int, int, int, float, bool) { return 0; }
void CharaAACounters(uint64_t& a, uint64_t& b, uint64_t& c, uint64_t& d) { a = b = c = d = 0; }
bool PresentEdgeAA(unsigned, int, int, int, int, int, int, int, int, float, bool) { return false; }
void WarmEdgeAA() {}
void EdgeAACounters(uint64_t& a, uint64_t& b, uint64_t& c, uint64_t& d) { a = b = c = d = 0; }
void PresentFastCounters(uint64_t& a, uint64_t& b, uint64_t& c) { a = b = c = 0; }
void UpscaleCounters(uint64_t& a, uint64_t& b, uint64_t& c) { a = b = c = 0; }
void PlayerPassCounters(uint64_t& a, uint64_t& b, uint64_t& c) { a = b = c = 0; }
int PlayerLookText(char* buf, size_t n) { if (n) buf[0] = 0; return 0; }
void QualityRungSize(int& w, int& h, int& samples) { w = h = 0; samples = 1; }
void DebugSetQualitySteps(bool, bool) {}
void DebugSetEdgeStep(bool) {}
void SetAnisotropicHook(AnisoHook) {}
void DebugSetAnisotropic(bool, unsigned) {}
bool AnisotropicOn() { return false; }
unsigned DebugLastFrameScenes() { return 0; }
void DebugHsr(uint64_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
void DebugHsrGeo(uint64_t out[8]) { for (int i = 0; i < 8; ++i) out[i] = 0; }
void DebugAtpp(uint64_t out[5]) { out[0] = out[1] = out[2] = out[3] = out[4] = 0; }
unsigned TextureSlots() { return 0; }
void NoteSceneGone() {}
void DebugPxCuts(uint64_t& a, uint64_t& b, uint64_t& c, uint64_t& d, uint64_t& e) { a = b = c = d = e = 0; }
void SetProgressDisplay(unsigned, int, int, ProgressShowHook) {}
bool SetPicture(const uint32_t*, int, int) { return false; }
bool DrawPicture() { return false; }
void ReleasePicture() {}
void SetProgressHostDraws(bool) {}
bool PlayStarted() { return false; }
uint32_t PlayStage() { return 0; }
bool PlayScreenLive() { return false; }
void SetWarmBudget(double) {}
unsigned WarmPending() { return 0; }
void WarmIdle() {}
unsigned StageWarmPending() { return 0; }
void SetColorLoadHook(ColorLoadHook) {}
unsigned DebugEfbLoadFroms() { return 0; }
bool StageWarmIdleWanted() { return false; }
void StageWarmIdle() {}
unsigned WarmTotal() { return 0; }
void WarmSliceStats(unsigned& slices, double& worstMs, double& totalMs) { slices = 0; worstMs = totalMs = 0.0; }
void EndWaitCumulative(EndWaitCum& out) { out = EndWaitCum{}; }
void EndWaitTag(uint64_t) {}
void SetProgressPolicy(uint64_t, unsigned) {}
void SetSkipDraws(bool) {}
Stats GetStats() { return g_stats; }

} // namespace backend
} // namespace vgx
