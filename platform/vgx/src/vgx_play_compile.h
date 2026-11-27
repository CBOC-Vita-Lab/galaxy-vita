// noprog26 (GXV_NO_PLAY_COMPILE_STOP, GXV_STAGE_PRECOMPILE): the pure decisions of "no shader compile stops the picture in play". Standard library only
// (the back end and tests/play_compile_model.cpp share it).
//
// What the owner saw (console job 1786c, Dreadnought, 82.9 s in): a launch star carried him to another part of the map, the front end met materials no cache had,
// the picture stopped for 4.35 s and the port's branded "Preparing graphics" picture came up. The path: the front end puts a CMD_SHADER for every new shader text at the
// start of the picture's command list; the back end's CMD_SHADER handler calls glCompileShader on the render thread, vitaGL finds no cache entry and runs libshacccg
// (about 270 ms a text; 9 texts = 2,429 ms of compiler runs, 2,937 ms with the rest); after 1.5 s of such work (SetProgressPolicy(90, 1500)) ShowProgress asks the host for
// the branded picture, which stays up for at least 900 ms and fades out (playprogress::Leave). The three programs the draws then linked cost 1 ms: the stop is the compiles.
//
// The remedy, in four pure decisions (the back end applies them; the census names which one fired):
//   ShaderAtCommand   a CMD_SHADER in play whose text no cache has is not compiled at the command: the shader stays pending, the draws that need it ask the worker
//                     (GXV_ASYNC_SHADERS' engine, on while this switch is on) or, without a worker, the gate spaces the compile.
//   FirstCompile      without a worker, a program whose vertex AND fragment shader both need the compiler compiles ONE of them now; the other follows one gap later.
//   ProgressInPlay    the progress picture is never shown while the game is in play.
//   PrecompileItem    GXV_STAGE_PRECOMPILE: what to do with a stage-queue entry whose shaders are in no cache.
#pragma once
#include <cstdint>

namespace vgx::pcs {

enum class ShaderVerdict : uint8_t { Compile, Defer };
// `on`: GXV_NO_PLAY_COMPILE_STOP. `inPlay`: the screen is live, the stage is the queue's target, no menu layer. `cached`: some cache (RAM pack, user pack, the card, the seed) has the text.
inline ShaderVerdict ShaderAtCommand(bool on, bool inPlay, bool cached) { return (on && inPlay && !cached) ? ShaderVerdict::Defer : ShaderVerdict::Compile; }

enum class Which : uint8_t { Both, VertexOnly, FragmentOnly };
// A program's two shaders, each `needs` the compiler (pending and in no cache). With a worker the question does not come up. Without one, two compiles in one go would be
// ~540 ms: the vertex shader first, the fragment shader at the next allowed slot.
inline Which FirstCompile(bool vsNeedsCompiler, bool fsNeedsCompiler) { return (vsNeedsCompiler && fsNeedsCompiler) ? Which::VertexOnly : Which::Both; }

inline bool ProgressInPlay(bool on, bool inPlay) { return on && inPlay; } // true: do not show the progress picture or the plain bar

// GXV_STAGE_PRECOMPILE, one stage-queue entry (a draw variant of a program).
enum class Engine : uint8_t { Unknown, Queued, Compiling, Ready, Failed, None }; // None: no worker
enum class ItemVerdict : uint8_t {
    Normal,  // the ordinary path (WarmVariant): linked already, or its shaders are in a cache, or the worker has them
    AskRotate, // not in any cache: hand the pair to the worker (it is Unknown), keep the entry for later (to the back of the queue)
    Rotate,  // the worker has it (Queued / Compiling): keep the entry for later
    Drop,    // in play and the worker failed it: the draw that needs it decides (no compile in play from the queue)
};
inline ItemVerdict PrecompileItem(bool mode, bool linked, bool cachedByProbe, Engine st, bool live) {
    if (!mode || linked || cachedByProbe) return ItemVerdict::Normal;
    switch (st) {
    case Engine::None: return live ? ItemVerdict::Drop : ItemVerdict::Normal; // (no worker: behind a closed screen the queue compiles as it always did)
    case Engine::Unknown: return ItemVerdict::AskRotate;
    case Engine::Queued:
    case Engine::Compiling: return ItemVerdict::Rotate;
    case Engine::Ready: return ItemVerdict::Normal;
    case Engine::Failed: return live ? ItemVerdict::Drop : ItemVerdict::Normal;
    }
    return ItemVerdict::Normal;
}

// A queue that can only rotate (every entry waits for the worker) must not spin: after one full turn without progress the slice ends.
struct RotateGuard {
    unsigned turns = 0, size = 0;
    void Begin(unsigned queueSize) { turns = 0; size = queueSize; }
    bool Spent() { return ++turns > size; } // call per rotation; true when the whole queue was passed over
    void Progress(unsigned queueSize) { turns = 0; size = queueSize; }
};

// The census of one stage.
struct Census {
    uint64_t shadersDeferred = 0;     // CMD_SHADER in play, no cache had the text: compile left to the worker / the gate
    uint64_t shadersCompiledInPlay = 0; // CMD_SHADER in play that a cache answered (a file or RAM-pack read)
    uint64_t progressSuppressed = 0;  // calls of the progress hook that were not shown
    uint64_t busyDeferred = 0;        // draws whose link waited because the worker held the compiler (the link would have waited on its lock)
    uint64_t busyAged = 0;            // revA: of them, programs that had waited GXV_NOPROG_BUSY_MAXWAIT pictures and were linked anyway (a worker that stays busy cannot hold a draw for ever)
    uint64_t failedToGate = 0;        // revA: programs whose worker job failed (compile error, heap guard): handed to the spaced gate, as with no worker
    uint64_t firstOnly = 0;           // fallback (no worker): programs whose second compile was left to a later slot
    uint64_t drawMisses = 0;          // programs a draw in play asked the worker for that no cache had (misses in play)
    uint64_t preEnumerated = 0, preCold = 0, preAsked = 0, preRotations = 0, preLinked = 0, preDropped = 0, preWorkerReady = 0;
    double preMs = 0;                 // render-thread time of the queue's slices while precompile was on
    bool Any() const { return shadersDeferred || shadersCompiledInPlay || progressSuppressed || busyDeferred || busyAged || failedToGate || firstOnly || drawMisses || preEnumerated || preCold || preAsked || preRotations || preLinked || preDropped; }
};

} // namespace vgx::pcs
