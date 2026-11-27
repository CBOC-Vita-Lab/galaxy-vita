// GXV_DIRECT_J3D: "direct draws" of J3D shapes (2.3, stage 1 of the direct J3D path; see agents/directj3d/REPORT.txt).
//
// A J3D shape call that the retained-shape record (GXV_RETAINED, vgx_dlcache.cpp) finds as a hit goes to its cache entry's static runs
// without the geometry lookup. What is left per run is EmitStaticRun's resolution of the draw state (ResolveStateCached in vgx_draw.cpp)
// and the hand-over of the job. The resolution's costliest part on a material change is the shader keys: the key memo's set (two lines)
// and the copies of the 412-byte fragment key and the 100-byte vertex key out of the registry into `cur` (on the console some six cache
// lines missed per change, about a third of a static draw's misses: retained REPORT, section 1). Here each shape keeps a template of the
// resolved keys it was last drawn with:
//   key hash of the key registers (State::keyHash), vertex-matrix mask, palette class  ->  fragment / vertex shader ids, the fold flag,
//   the texture mask and the indirect stage count of those keys (functions of the keys alone).
// A run of a shape whose template matches the registers as they are now (the same three inputs; compared exactly) takes the ids from the
// template and leaves the two key copies undone ("keys held stale": cur.fsId / cur.vsId are right, cur.fs / cur.vs are copied from the
// registry only when something reads them: SyncKeys, at the entry of every other resolution and of FoldGuess, and before the character /
// shadow-fill bookkeeping of a run that reads them). Everything else is ResolveStateCached's code, line for line (DjResolveBody), with the
// keys read from the registry where it read cur.fs / cur.vs: the render state, the texture bindings, the uniform blocks, the dirty flags,
// the serials, the job. So the command buffer is the normal path's, byte for byte (tools/test_vgx_direct_j3d.sh).
//   GXV_DIRECT_J3D=0  (default) off: nothing changes (no template is kept, EmitStaticRun resolves as before)
//   GXV_DIRECT_J3D=1  on (implies the retained-shape records: the game's shape handles, GXV_RETAINED=1, unless GXV_RETAINED is set)
//   GXV_DIRECT_J3D=2  verify: each templated resolution is also made the normal way from the same starting point (split off, into the
//                     player's own arena, as GXV_RSCACHE=2 does) and the two are compared field by field: the resolved state (ids, render
//                     state, texture bindings, mask, flags), the uniform block offsets, lengths and words appended, the dirty flags, the
//                     key memo key. A difference is logged ("direct: MISMATCH ... handle, list, fs / vs ids, key hash") and the picture
//                     is the normal path's. Then the templated path runs for real.
// Declined (the normal path, exact by construction): GXV_RSCACHE != 1, a traced picture, statistics detail, the palette census
// (GXV_PALROWS=2), learned copies (GXV_RIGIDMERGE_DLLEARN), lists without a static buffer, a template made for other keys.
#pragma once
#include <cstdint>

namespace vgx {
namespace dj {

// The template of one shape (kept in its retained record, RtSlot::dj, ahead of the array records: 16 bytes).
struct Keys {
    uint64_t h = 0;          // State::keyHash
    uint16_t fsId = 0, vsId = 0;
    uint16_t mask = 0;       // the vertex-matrix mask (VcdMtxMask: 9 bits)
    uint8_t texMask = 0;     // texmaps the fragment key samples
    uint8_t spo : 2;         // the palette class part of the memo key
    uint8_t foldOk : 1;      // FoldRegsEnabled() && FoldKeysOk(fs, vs)
    uint8_t numInd : 3;      // the fragment key's indirect stages
    uint8_t valid : 1;
    Keys() : spo(0), foldOk(0), numInd(0), valid(0) {}
};
static_assert(sizeof(Keys) == 16, "a template is 16 bytes");

int Mode(); // GXV_DIRECT_J3D (read once): 0, 1, 2

// (vgx_dlcache.cpp) armed for the static runs of one shape call: EmitStaticRun resolves with Resolve() instead of ResolveState().
extern const Keys* g_armed;
extern uint32_t g_armedHandle;   // (the verify log's names of the shape: its handle and list)
extern const void* g_armedList;
extern uint32_t g_resolves;      // static runs resolved by EmitStaticRun (either way)
// (vgx_draw.cpp) EmitStaticRun's resolution while armed.
void Resolve(uint32_t vcdMtxMask);
// Whether the shape call's runs may go the templated way at all (the switches that read the keys per draw are off).
bool Usable();
// The template of the state the last resolution left, if it can be one (false otherwise). `resolvesBefore`: Resolves() before the call;
// a call that resolved nothing (all its runs culled) leaves no template.
bool Capture(Keys& out, uint32_t resolvesBefore);
inline uint64_t OtherRuns(); // (runs resolved the normal way so far: a call none of whose runs did leaves its template as it is)
inline uint32_t Resolves() { return g_resolves; }
// calls armed, runs that took the template's keys, runs whose keys the template did not match (resolved the normal way), key copies left
// undone, deferred copies made later, templates captured, verify checks, verify mismatches
void Counters(uint64_t out[12]); // [8..11] fe27: DjEmit calls not armed: arena list, learned copy, no runs, mode / diagnostics
void CountNotArmed(unsigned why);
void CountArmed(); // (vgx_dlcache.cpp)
void CountCapture();
extern uint64_t g_otherRuns;
inline uint64_t OtherRuns() { return g_otherRuns; }

} // namespace dj
void DirectJ3DCounters(uint64_t out[12]); // (the fixture and the workload line; weak there)
} // namespace vgx
