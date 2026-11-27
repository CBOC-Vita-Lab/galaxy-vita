// noprog26: host test of vgx_play_compile.h (GXV_NO_PLAY_COMPILE_STOP / GXV_STAGE_PRECOMPILE decisions) and of the gate's missOnly mode. tools/test_play_compile.sh
#include "../platform/vgx/src/vgx_async_link.h"
#include "../platform/vgx/src/vgx_play_compile.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::pcs;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
int main() {
    // the shader command: deferred only with the switch on, in play, and when no cache has the text
    Require(ShaderAtCommand(true, true, false) == ShaderVerdict::Defer, "play, uncached: deferred");
    Require(ShaderAtCommand(true, true, true) == ShaderVerdict::Compile, "play, cached: compiled (a file read)");
    Require(ShaderAtCommand(true, false, false) == ShaderVerdict::Compile, "not in play (a closed screen): compiled as before");
    Require(ShaderAtCommand(false, true, false) == ShaderVerdict::Compile, "switch off: compiled as before");
    // the fallback: one of two compiles
    Require(FirstCompile(true, true) == Which::VertexOnly, "both need the compiler: the vertex shader first");
    Require(FirstCompile(true, false) == Which::Both && FirstCompile(false, true) == Which::Both && FirstCompile(false, false) == Which::Both, "otherwise the program is made whole");
    // the progress picture
    Require(ProgressInPlay(true, true) && !ProgressInPlay(true, false) && !ProgressInPlay(false, true), "progress: never in play, only with the switch");
    // the queue's entries
    Require(PrecompileItem(false, false, false, Engine::Unknown, true) == ItemVerdict::Normal, "mode off: ordinary");
    Require(PrecompileItem(true, true, false, Engine::Unknown, true) == ItemVerdict::Normal, "linked: ordinary");
    Require(PrecompileItem(true, false, true, Engine::Unknown, true) == ItemVerdict::Normal, "cached: ordinary");
    Require(PrecompileItem(true, false, false, Engine::Unknown, false) == ItemVerdict::AskRotate, "no cache, never asked: hand to the worker, keep");
    Require(PrecompileItem(true, false, false, Engine::Queued, true) == ItemVerdict::Rotate && PrecompileItem(true, false, false, Engine::Compiling, false) == ItemVerdict::Rotate, "worker has it: keep");
    Require(PrecompileItem(true, false, false, Engine::Ready, true) == ItemVerdict::Normal, "worker done: link");
    Require(PrecompileItem(true, false, false, Engine::Failed, true) == ItemVerdict::Drop && PrecompileItem(true, false, false, Engine::Failed, false) == ItemVerdict::Normal, "failed: dropped in play, tried behind a closed screen");
    Require(PrecompileItem(true, false, false, Engine::None, true) == ItemVerdict::Drop && PrecompileItem(true, false, false, Engine::None, false) == ItemVerdict::Normal, "no worker: never compiled in play");
    // a queue that only rotates ends its slice after one turn
    {
        RotateGuard g;
        g.Begin(5);
        unsigned n = 0;
        while (!g.Spent()) ++n;
        Require(n == 5, "five entries, five rotations, then the slice ends");
        g.Progress(5);
        Require(!g.Spent(), "progress starts the count again");
    }
    // the gate, missOnly: cached links go at once; misses are spaced; a draw that must not wait still links
    {
        vgx::alink::Gate g;
        g.cfg.missOnly = true;
        g.Picture(10);
        for (uint32_t k = 0; k < 5; ++k) {
            const auto d = g.Decide(k, vgx::alink::kSyncNone, vgx::alink::kCacheHit, false);
            Require(d.v == vgx::alink::Verdict::Link && !d.sliced, "missOnly: five cached links in one picture, none sliced");
            g.Linked(k, 12.0, false, d.why);
        }
        auto d = g.Decide(100, vgx::alink::kSyncNone, vgx::alink::kCacheMiss, false);
        Require(d.v == vgx::alink::Verdict::Link && d.why == vgx::alink::kSyncMissNoWorker, "the first miss links");
        g.Linked(100, 300.0, false, d.why);
        d = g.Decide(101, vgx::alink::kSyncNone, vgx::alink::kCacheMiss, false);
        Require(d.v == vgx::alink::Verdict::Defer, "the next miss, same picture, waits");
        g.Picture(20);
        Require(g.Decide(101, vgx::alink::kSyncNone, vgx::alink::kCacheMiss, false).v == vgx::alink::Verdict::Defer, "and still, 10 pictures later");
        g.Picture(40);
        Require(g.Decide(101, vgx::alink::kSyncNone, vgx::alink::kCacheMiss, false).v == vgx::alink::Verdict::Link, "linked after the gap (30)");
        // half a program: MissSpent uses the slot
        g.Picture(41);
        g.MissSpent(250.0);
        g.Picture(50);
        Require(g.Decide(102, vgx::alink::kSyncNone, vgx::alink::kCacheMiss, false).v == vgx::alink::Verdict::Defer, "a first-of-two compile used the slot");
        Require(g.Decide(103, vgx::alink::kSyncMustChara, vgx::alink::kCacheMiss, false).v == vgx::alink::Verdict::Link, "Mario links at once, whatever the gap");
        // with a worker a miss is not the gate's business: cached-path budget (here missOnly: a miss that reaches the gate links under the budget)
        g.Picture(60);
        Require(g.Decide(104, vgx::alink::kSyncNone, vgx::alink::kCacheMiss, true).v == vgx::alink::Verdict::Link, "worker: a stale miss still links within the budget");
    }
    // not missOnly: the nohitch26 behaviour (cached links sliced) is unchanged
    {
        vgx::alink::Gate g;
        g.Picture(10);
        auto d = g.Decide(1, vgx::alink::kSyncNone, vgx::alink::kCacheHit, false);
        Require(d.v == vgx::alink::Verdict::Link && d.sliced, "missOnly off: sliced");
        g.Linked(1, 12.0, true, d.why);
        Require(g.Decide(2, vgx::alink::kSyncNone, vgx::alink::kCacheHit, false).v == vgx::alink::Verdict::Defer, "missOnly off: one a picture");
    }
    std::printf("PASS play compile model\n");
    return 0;
}
