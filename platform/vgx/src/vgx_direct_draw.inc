// GXV_DIRECT_J3D (vgx_direct.h): the templated resolution of a J3D shape's static runs. Included into vgx_draw.cpp (namespace vgx), after the
// resolution cache and before ResolveState, so that it runs the resolver's own pieces. Part 1: everything but SyncForNotes; part 2
// (VGX_DJ_PART2, before EmitStaticDraw): SyncForNotes, which reads the character / shadow-fill switches defined in between.
#ifndef VGX_DJ_PART2
// (vgx_direct.h is included at the top of vgx_draw.cpp)

namespace dj {
const Keys* g_armed = nullptr;
uint32_t g_armedHandle = 0;
const void* g_armedList = nullptr;
uint32_t g_resolves = 0;
uint64_t g_otherRuns = 0;
namespace {
// cur.fs / cur.vs may not hold the keys of cur.fsId / cur.vsId (a templated resolution changed the ids and left the copies undone).
// g_fsHeld / g_vsHeld: the ids whose keys they do hold then (~0u: not known). Otherwise cur.fs is g_fsReg.keys[cur.fsId] (the invariant
// of every other resolution: ResolveKeysCached copies the keys of the ids it sets, ResolveStateFull builds both).
bool g_stale = false;
uint32_t g_fsHeld = ~0u, g_vsHeld = ~0u;
struct Ctr { uint64_t armed = 0, runs = 0, deferred = 0, copies = 0, captured = 0, checked = 0, bad = 0; } g_ctr;
} // namespace

int Mode() {
    const int m = static_cast<int>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_DIRECT_J3D", 0), 0, 2))));
    return m;
}
void CountArmed() { ++g_ctr.armed; }
void CountCapture() { ++g_ctr.captured; }
uint64_t g_notArmed[4] = {};
void CountNotArmed(unsigned why) { ++g_notArmed[why & 3u]; }
void Counters(uint64_t out[12]) {
    for (int i = 0; i < 4; ++i) out[8 + i] = g_notArmed[i];
    out[0] = g_ctr.armed; out[1] = g_ctr.runs; out[2] = g_otherRuns; out[3] = g_ctr.deferred;
    out[4] = g_ctr.copies; out[5] = g_ctr.captured; out[6] = g_ctr.checked; out[7] = g_ctr.bad;
}

// The deferred key copies, made before anything reads cur.fs / cur.vs (the resolutions, FoldGuess, the character and shadow-fill notes).
[[gnu::noinline]] void SyncKeysSlow() {
    if (g_fsHeld != cur.fsId) CopyWords(&cur.fs, &g_fsReg.keys[cur.fsId], sizeof(FsKey) / 4);
    if (g_vsHeld != cur.vsId) CopyWords(&cur.vs, &g_vsReg.keys[cur.vsId], sizeof(VsKey) / 4);
    g_stale = false;
    ++g_ctr.copies;
}
inline void SyncKeys() {
    if (__builtin_expect(g_stale, 0)) SyncKeysSlow();
}
// At the entry of every other resolution (ResolveState). ResolveStateCached copies the keys of the ids it changes itself (ResolveKeysCached):
// the deferred copies needed are those of the ids it will keep. Which those are is worked out as it will: the same memo key (key hash,
// vertex-matrix mask, palette class of the pending row maximum), then the key memo's ids for it. Kept ids (the same memo key as cur's, or
// a memo entry naming cur's id) are copied now; the others ResolveKeysCached copies. Anything not known (another mode, the palette census,
// a memo miss): both now.
[[gnu::noinline]] void SyncKeysForResolveSlow(uint32_t vcdMtxMask) {
    if (RsCacheMode() != 1 || PaletteRowsMode() == 2) { SyncKeysSlow(); return; }
    const uint8_t palCls = PalClass(g_pendingPosMtxMax, VGX_KNOB(UniformPacking()) != 0, (vcdMtxMask & 1u) != 0);
    const uint8_t spo = (vcdMtxMask & 1u) ? palCls : 0;
    if (SameCurKey(vcdMtxMask, spo)) { SyncKeysSlow(); return; }
    uint32_t slot = 0;
    const KeyMemo* m = g_keyMemo.Find(g.keyHash, vcdMtxMask, spo, KeyMemoWays(), slot);
    if (!m) { SyncKeysSlow(); return; }
    if (m->fsId == cur.fsId && g_fsHeld != cur.fsId) CopyWords(&cur.fs, &g_fsReg.keys[cur.fsId], sizeof(FsKey) / 4);
    if (m->vsId == cur.vsId && g_vsHeld != cur.vsId) CopyWords(&cur.vs, &g_vsReg.keys[cur.vsId], sizeof(VsKey) / 4);
    // (ResolveKeysCached copies the keys of the other ids: nothing reads cur.fs / cur.vs before it)
    g_stale = false;
    ++g_ctr.copies;
}
inline void SyncKeysForResolve(uint32_t vcdMtxMask) {
    if (__builtin_expect(g_stale, 0)) SyncKeysForResolveSlow(vcdMtxMask);
}
// The keys of the current ids, wherever they are (no copy).
inline const FsKey& CurFs() { return (g_stale && g_fsHeld != cur.fsId) ? g_fsReg.keys[cur.fsId] : cur.fs; }
inline const VsKey& CurVs() { return (g_stale && g_vsHeld != cur.vsId) ? g_vsReg.keys[cur.vsId] : cur.vs; }

bool Usable() {
    return RsCacheMode() == 1 && !TraceFrame() && !stats::Detailed() && PaletteRowsMode() != 2;
}

namespace {
// ResolveKeysCached's effects with the template's ids (t matched the key hash, the mask and the palette class): the ids, the fold flag of
// the keys, the memo key; the key copies are left for SyncKeys.
inline void KeysFromTemplate(const Keys& t, uint32_t vcdMtxMask, uint8_t spo, bool& fsChanged, bool& vsChanged) {
    fsChanged = !cur.valid || t.fsId != cur.fsId;
    vsChanged = !cur.valid || t.vsId != cur.vsId;
    if ((fsChanged || vsChanged) && !g_stale) {
        g_fsHeld = cur.valid ? cur.fsId : ~0u;
        g_vsHeld = cur.valid ? cur.vsId : ~0u;
        g_stale = true;
    }
    if (fsChanged) { cur.fsId = t.fsId; cur.fsIdFoldValid = false; ++g_ctr.deferred; }
    if (vsChanged) { cur.vsId = t.vsId; ++g_ctr.deferred; }
    if (fsChanged || vsChanged) cur.foldOk = t.foldOk != 0;
    cur.vcdMtxMask = vcdMtxMask;
    // (slot: where the memo keeps these keys, which FoldShaderCached checks before it uses it; 0 is checked like any other)
    g_curKey.h = t.h; g_curKey.mask = vcdMtxMask; g_curKey.spo = spo; g_curKey.slot = 0; g_curKey.valid = true;
}

// ResolveStateCached(vcdMtxMask, 0xFFFFFFFF) (a static run), line for line, with two differences: the keys come from the template when it
// matches (else from ResolveKeysCached, after SyncKeys), and the keys are read where they are (CurFs / CurVs) instead of from cur.fs / cur.vs.
// KEEP IN STEP WITH ResolveStateCached: GXV_DIRECT_J3D=2 compares the two for every templated run.
void ResolveBody(uint32_t vcdMtxMask, const Keys& t) {
    const uint32_t vertexCount = 0xFFFFFFFFu;
    const bool packing = VGX_KNOB(UniformPacking()) != 0;
    const uint8_t palCls = PalClass(g_pendingPosMtxMax, packing, (vcdMtxMask & 1u) != 0);
    g_pendingPosMtxMax = 0xFF;
    const uint8_t spo = (vcdMtxMask & 1u) ? palCls : 0;
    const bool sameKeys = SameCurKey(vcdMtxMask, spo);
    uint32_t fw[3];
    std::memcpy(fw, &g.bpDirty, 12);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const bool otherFlags = ((fw[0] & 0x00FF00FFu) | (fw[1] & 0xFFFFFF00u) | fw[2]) != 0;
#else
    const bool otherFlags = g.bpDirty || g.bpRsDirty || g.xfViewDirty || g.xfProjDirty || g.xfColorDirty || g.xfMtxDirty || g.xfLightDirty || g.texDirty;
#endif
    if (sameKeys && !g_blocksStale && !otherFlags && (g.xfPosRowsDirty & cur.mtxRowsUsed) == 0) {
        const bool legacyKeysDirty = g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || ((vcdMtxMask & 1u) && palCls != cur.palCls);
        if (legacyKeysDirty && ZTexReplace()) goto full;
        {
        const bool soft = vertexCount <= SoftXformLimit() && (SyncKeys(), SoftEligibleCur());
        if (soft == cur.soft) {
            const bool fold = soft && cur.foldOk && FoldValuesOk();
            if (fold == cur.fold && (!fold || cur.fsIdFoldValid) && (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) == 0) {
                if (legacyKeysDirty) cur.palCls = palCls;
                ClearResolveDirty();
                cur.vcdGen = g.vcdGen;
                ++g_ctr.runs;
                return;
            }
        }
        }
    }
full:
    {
    // ---- keys
    const bool legacyKeysDirty = LegacyKeysDirty(vcdMtxMask, palCls);
    const bool vcdChanged = !cur.valid || vcdMtxMask != cur.vcdMtxMask;
    bool fsChanged = false, vsChanged = false;
    if (legacyKeysDirty) cur.palCls = palCls;
    const bool keysChanged = !sameKeys;
    if (keysChanged) {
        if (t.h == g.keyHash && t.mask == vcdMtxMask && t.spo == spo) {
            KeysFromTemplate(t, vcdMtxMask, spo, fsChanged, vsChanged);
            ++g_ctr.runs;
        } else {
            SyncKeys();
            ResolveKeysCached(vcdMtxMask, palCls, spo, fsChanged, vsChanged);
            ++g_otherRuns;
        }
    } else {
        ++g_ctr.runs;
    }
    // ---- render state
    if (keysChanged || g.xfViewDirty || g.bpRsDirty || (legacyKeysDirty && ZTexReplace())) {
        BuildRenderStateMemo(cur.rs);
        cur.cullAll = cur.rs.cull == CULL_ALL;
        ++cur.stateSerial;
    }
    // ---- texture bindings
    const bool texRebuild = fsChanged || g.texDirty;
    const bool fromT = cur.fsId == t.fsId; // (the template's texture mask and indirect stage count are those of its fragment key)
    if (texRebuild) {
        uint8_t mask = 0;
        if (fromT) {
            mask = t.texMask;
        } else {
            const FsKey& fs = CurFs();
            for (int s = 0; s < fs.numStages; ++s)
                if (fs.stage[s].texEnable) mask |= static_cast<uint8_t>(1u << fs.stage[s].texMap);
            for (int i = 0; i < fs.numIndStages; ++i) mask |= static_cast<uint8_t>(1u << fs.ind[i].texMap);
        }
        const uint32_t visit = static_cast<uint32_t>(mask | cur.texMask);
        cur.texMask = mask;
        ++cur.stateSerial;
        const bool perSlot = VGX_KNOB(EnvInt("GXV_TEXSLOTS", 1) != 0) != 0;
        cur.slotResolved &= static_cast<uint8_t>(~g.texSlotDirty);
        if (!perSlot) cur.slotResolved = 0;
        g.texSlotDirty = 0;
        for (uint32_t vm = visit; vm; vm &= vm - 1) {
            const int m = __builtin_ctz(vm);
            const uint8_t bit = static_cast<uint8_t>(1u << m);
            if (!(mask & bit)) {
                cur.tex[m] = TexBind{};
                cur.texW[m] = cur.texH[m] = 0;
                continue;
            }
            if (!(cur.slotResolved & bit)) {
                cur.slotW[m] = cur.slotH[m] = 0;
                ResolveTexture(m, cur.slotBind[m], cur.slotW[m], cur.slotH[m]);
                cur.slotResolved |= bit;
            }
            cur.tex[m] = cur.slotBind[m];
            cur.texW[m] = cur.slotW[m];
            cur.texH[m] = cur.slotH[m];
        }
        LodSamplerFill(CurFs(), cur.tex); // (GXV_LODBIAS_SAMPLER)
    }
    // ---- uniform blocks
    const bool soft = vertexCount <= SoftXformLimit() && (SyncKeys(), SoftEligibleCur());
    if (g_blocksStale || g.xfProjDirty) BlkProj();
    if (g_blocksStale || g.xfMtxDirty || (g.xfPosRowsDirty & cur.mtxRowsUsed) != 0 || vsChanged || vcdChanged || soft != cur.soft)
        cur.mtxRowsUsed = BlkMtx(CurVs(), soft);
    cur.soft = soft;
    if (g_blocksStale || g.xfLightDirty || g.xfColorDirty || (vsChanged && (UniformPacking() || LightNorm()))) BlkLight(CurVs());
    const bool fold = soft && cur.foldOk && FoldValuesOk();
    if (fold && !cur.fsIdFoldValid) {
        SyncKeys();
        cur.fsIdFold = FoldShaderCached();
        cur.fsIdFoldValid = true;
    }
    const uint8_t numInd = fromT ? t.numInd : CurFs().numIndStages;
    if (g_blocksStale || g.bpDirty || (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) != 0 || fsChanged || fold != cur.fold || (texRebuild && numInd))
        BlkTev(CurFs(), cur.texW, cur.texH, fold);
    cur.fold = fold;
    cur.useFs = PixUseFs(fold ? cur.fsIdFold : cur.fsId, cur.rs);
    g_blocksStale = false;
    ClearResolveDirty();
    cur.vcdGen = g.vcdGen;
    cur.valid = true;
    }
}

// ---- GXV_DIRECT_J3D=2: the run resolved both ways from the same start (split off, into the player's own arena), compared; then for real.
struct Snap {
    uint8_t dirty[12];
    uint64_t posRows;
    uint8_t tevCol, texSlot;
    uint32_t pal;
    bool stale;
    uint32_t off[UB_COUNT], len[UB_COUNT];
    size_t uni;
    CurKey key;
    bool djStale;
    uint32_t fsHeld, vsHeld;
};
Snap Take() {
    Snap s;
    std::memcpy(s.dirty, &g.bpDirty, 12);
    s.posRows = g.xfPosRowsDirty;
    s.tevCol = g.tevColDirty;
    s.texSlot = g.texSlotDirty;
    s.pal = g_pendingPosMtxMax;
    s.stale = g_blocksStale;
    std::memcpy(s.off, g_blockOff, sizeof s.off);
    std::memcpy(s.len, g_blockLen, sizeof s.len);
    s.uni = g_buf.uni.size();
    s.key = g_curKey;
    s.djStale = g_stale;
    s.fsHeld = g_fsHeld;
    s.vsHeld = g_vsHeld;
    return s;
}
void Put(const Snap& s) {
    std::memcpy(&g.bpDirty, s.dirty, 12);
    g.xfPosRowsDirty = s.posRows;
    g.tevColDirty = s.tevCol;
    g.texSlotDirty = s.texSlot;
    g_pendingPosMtxMax = s.pal;
    g_blocksStale = s.stale;
    std::memcpy(g_blockOff, s.off, sizeof s.off);
    std::memcpy(g_blockLen, s.len, sizeof s.len);
    g_buf.uni.resize(s.uni);
    g_curKey = s.key;
    g_stale = s.djStale;
    g_fsHeld = s.fsHeld;
    g_vsHeld = s.vsHeld;
}
// What a resolution left, with the keys as they logically are (cur.fs / cur.vs of the ids even where their copies were left undone).
struct Out {
    Resolved r;
    Snap after;
    std::vector<float> uni;
};
void Grab(Out& o, size_t uniFrom) {
    o.r = cur;
    if (g_stale) {
        if (g_fsHeld != cur.fsId) o.r.fs = g_fsReg.keys[cur.fsId];
        if (g_vsHeld != cur.vsId) o.r.vs = g_vsReg.keys[cur.vsId];
    }
    o.after = Take();
    o.uni.assign(g_buf.uni.begin() + static_cast<std::ptrdiff_t>(uniFrom), g_buf.uni.end());
}
void Verify(uint32_t vcdMtxMask, const Keys& t) {
    static Resolved pre; // (static: large)
    static Out a, b;
    // (the deferred copies made first: the normal path's own entry does it; then the two runs start from the same cur)
    SyncKeys();
    pre = cur;
    const Snap s0 = Take();
    const Ctr c0 = g_ctr; // (the trial runs are not counted)
    const uint64_t other0 = g_otherRuns;
    const bool splitOn = split::g_on;
    split::g_on = false;
    ResolveStateCached(vcdMtxMask, 0xFFFFFFFFu);
    Grab(a, s0.uni);
    cur = pre;
    Put(s0);
    ResolveBody(vcdMtxMask, t);
    Grab(b, s0.uni);
    cur = pre;
    Put(s0);
    split::g_on = splitOn;
    g_ctr = c0;
    g_otherRuns = other0;
    // (the memo slot is where the keys were found, not part of the state; the counters are not compared)
    const bool curOk = std::memcmp(&a.r, &b.r, sizeof a.r) == 0;
    const bool keyOk = a.after.key.valid == b.after.key.valid && a.after.key.h == b.after.key.h && a.after.key.mask == b.after.key.mask &&
                       a.after.key.spo == b.after.key.spo;
    const bool blocksOk = std::memcmp(a.after.off, b.after.off, sizeof a.after.off) == 0 && std::memcmp(a.after.len, b.after.len, sizeof a.after.len) == 0 &&
                          a.uni.size() == b.uni.size() && (a.uni.empty() || std::memcmp(a.uni.data(), b.uni.data(), a.uni.size() * sizeof(float)) == 0);
    const bool flagsOk = std::memcmp(a.after.dirty, b.after.dirty, 12) == 0 && a.after.posRows == b.after.posRows && a.after.tevCol == b.after.tevCol &&
                         a.after.texSlot == b.after.texSlot && a.after.stale == b.after.stale && a.after.pal == b.after.pal;
    ++g_ctr.checked;
    if (!(curOk && keyOk && blocksOk && flagsOk)) {
        ++g_ctr.bad;
        const char* what = !curOk ? (a.r.fsId != b.r.fsId || a.r.vsId != b.r.vsId ? "shader ids" : !SameStruct(a.r.rs, b.r.rs) ? "render state"
                                     : a.r.texMask != b.r.texMask || std::memcmp(a.r.tex, b.r.tex, sizeof a.r.tex) != 0 ? "texture bindings" : "resolved state")
                                 : !keyOk ? "key memo key" : !blocksOk ? "uniform blocks" : "dirty flags";
        VGX_LOG_N(32, "direct: MISMATCH (%s) frame %llu shape handle %u list %p: normal fs %u vs %u / templated fs %u vs %u, template fs %u vs %u, key hash %016llx (template %016llx), mask %x spo %u\n",
                  what, static_cast<unsigned long long>(g_frame), g_armedHandle, g_armedList, a.r.fsId, a.r.vsId, b.r.fsId, b.r.vsId, t.fsId, t.vsId,
                  static_cast<unsigned long long>(g.keyHash), static_cast<unsigned long long>(t.h), vcdMtxMask, t.spo);
    }
    if ((g_ctr.checked & 0x3FFFu) == 0)
        VGX_LOG_N(200, "direct: %llu templated runs checked, %llu mismatching\n", static_cast<unsigned long long>(g_ctr.checked),
                  static_cast<unsigned long long>(g_ctr.bad));
    ResolveBody(vcdMtxMask, t); // (for real)
}
} // namespace

void Resolve(uint32_t vcdMtxMask) {
    if (Mode() == 2) Verify(vcdMtxMask, *g_armed); // (ends with the real resolution)
    else ResolveBody(vcdMtxMask, *g_armed);
}

bool Capture(Keys& out, uint32_t resolvesBefore) {
    if (g_resolves == resolvesBefore || !cur.valid || !g_curKey.valid || g_curKey.h != g.keyHash || g_curKey.mask != cur.vcdMtxMask ||
        g_curKey.mask > 0xFFFFu || g_curKey.spo > 3 || RsCacheMode() != 1)
        return false;
    if (out.valid && out.h == g_curKey.h && out.mask == g_curKey.mask && out.spo == g_curKey.spo && out.fsId == cur.fsId && out.vsId == cur.vsId)
        return false; // (the template it has)
    // (functions of the ids alone: from the registry's keys)
    const FsKey& fs = g_fsReg.keys[cur.fsId];
    const VsKey& vs = g_vsReg.keys[cur.vsId];
    uint8_t mask = 0;
    for (int s = 0; s < fs.numStages; ++s)
        if (fs.stage[s].texEnable) mask |= static_cast<uint8_t>(1u << fs.stage[s].texMap);
    for (int i = 0; i < fs.numIndStages; ++i) mask |= static_cast<uint8_t>(1u << fs.ind[i].texMap);
    out.h = g_curKey.h;
    out.mask = static_cast<uint16_t>(g_curKey.mask);
    out.spo = g_curKey.spo & 3u;
    out.fsId = cur.fsId;
    out.vsId = cur.vsId;
    out.foldOk = (FoldRegsEnabled() && FoldKeysOk(fs, vs)) ? 1 : 0;
    out.texMask = mask;
    out.numInd = fs.numIndStages & 7u;
    out.valid = 1;
    return true;
}
} // namespace dj
void DirectJ3DCounters(uint64_t out[12]) { dj::Counters(out); }

#else // VGX_DJ_PART2
namespace dj {
// Before EmitStaticRun's shadow-fill bounds and character notes, which read cur.fs / cur.vs: the deferred copies, when they will read them
// (SfStaticBounds: the shadow-fill switch; CharaNote: a HUD draw, or a character's draw that writes colour, ChaBounds; ChaClassify is
// what CharaNote calls there itself, a function of the state's serial).
// (GXV_SHADOW_RECT_FE) Whether SfStaticBounds would go past SfAlphaOther for this draw: the same test, with the fragment key read where it
// is (CurFs) instead of from the copy. False: it would return at once, and the keys need not be made for it.
inline bool SfKeysNeeded() {
    const RenderState& rs = cur.rs;
    if (!rs.alphaWrite) return false;
    if (rs.blend && rs.dstAlphaZero == 1) return false;
    if (!rs.blend && rs.dstAlphaZero == 0 && CurFs().dstAlpha && bits(g.bp[0x42], 0, 8) == 0) return false;
    return true;
}
inline void SyncForNotes() {
    if (!g_stale) return;
    if (ShadowRectFeMode() != 0 && SfBoundsOn() && !SfKeysNeeded()) ++g_sfFe.keysSkipped; // (counted once per draw; the notes below may still sync)
    if ((SfBoundsOn() && (ShadowRectFeMode() == 0 || SfKeysNeeded())) || (SsTileWanted() && SfKeysNeeded()) || (ChaOn() && (g_cha.hudDepth > 0 || (CharaNoteOn() && (ChaClassify(), g_cha.chara && cur.rs.colorWrite)))))
        SyncKeysSlow();
}
} // namespace dj
#endif
