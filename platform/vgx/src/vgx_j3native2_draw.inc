// GXV_J3DNATIVE=4 / 5 (vgx_j3n2.h; the state side is vgx_j3native2.inc): the draw side of stage 2. Included into vgx_draw.cpp (namespace vgx)
// after the direct-J3D part 1 and before ResolveState, so that it runs the resolver's own pieces.
//
// A static run of a shape inside an active chain resolves through J3n2ResolveRun:
//   template mode (=5): the template for the chain's lazy-state version, the vertex-matrix mask, the descriptor words and the palette class
//     (vgx_j3n2.h TplKey). With one whose textures the texture memo still gives exactly (TexTouch, with the hit's side effects), the run is
//     resolved from it:
//       - the ids, the render state, the texture bindings and sizes, the texture mask, the culled-all flag go into `cur` as ResolveBody
//         leaves them (the key copies left to SyncKeys, as GXV_DIRECT_J3D does; the memo key invalid: nothing may take it as current);
//       - the register words the uniform blocks read and the lists wrote (Img) are written back into the register file (they are the
//         values those registers have in the chain's lazy state: the chain's lists are not applied, so the file holds older ones);
//       - the blocks: the matrices always (BlkMtx, as every shape's matrices are new), the projection, light and TEV blocks when what they
//         are built from differs from what the template path sent last (Mirror) or the picture's arena is stale. A block sent with the
//         contents the finishing thread's current block has already is not appended (PushBlock compares), so sending more than the normal
//         path would changes nothing; sending less never happens: a block is skipped only when its inputs are the ones last sent, and the
//         normal path's own sends invalidate the mirror (the Blk* wrappers);
//       - a new state serial whenever the template is not the last one (records and the character notes key on it; serials only grow).
//     Without such a template the chain is materialised (Bail), the run resolved the normal way and its template captured.
//   recording (and after a bail): the normal resolution, then the capture of the template (the register file is real).
//   verify (=4): the normal resolution; a template there is compared with what it produced (ids, render state, bindings, sizes, mask,
//     the image words, the culled flag); a mismatch is logged and the template dropped.
namespace j3n2 {
DrawCtx* g_draw = nullptr;
DrawCtr g_dctr;
#if defined(VGX_CLONE_B)
// (GXV_FE_TWOCORE's second worker: stage 2 is the player's only, and the texture cache is not part of the clone)
bool TexTouch(int, const uint32_t*, const TexBind&, uint16_t, uint16_t) { return false; }
bool TexKnown(int, const uint32_t*, const TexBind&, uint16_t, uint16_t) { return false; }
void TexRegsOf(int, uint32_t r[5]) { for (int i = 0; i < 5; ++i) r[i] = 0; }
#endif
namespace {
// Img layout: tevReg 16, tevKonst 16, bp 06..0E 9, bp 25 26 42, bp EE..F4 7, xf 0A..0D 4, xf 50..57 8, texture mode0 8, mode1 8
constexpr uint32_t kImTevReg = 0, kImTevKonst = 16, kImInd = 32, kIm25 = 41, kIm26 = 42, kIm42 = 43, kImFog = 44, kImCol = 51, kImPost = 55, kImLod = 63;
static_assert(kImLod + 16 == kImgWords, "the image layout");
inline void ImgSave(Img& im) {
    std::memcpy(im.w + kImTevReg, g.tevReg, 64);
    std::memcpy(im.w + kImTevKonst, g.tevKonst, 64);
    std::memcpy(im.w + kImInd, &g.bp[0x06], 9 * 4);
    im.w[kIm25] = g.bp[0x25];
    im.w[kIm26] = g.bp[0x26];
    im.w[kIm42] = g.bp[0x42];
    std::memcpy(im.w + kImFog, &g.bp[0xEE], 7 * 4);
    std::memcpy(im.w + kImCol, &g.xfReg[0x0A], 4 * 4);
    std::memcpy(im.w + kImPost, &g.xfReg[0x50], 8 * 4);
    for (unsigned m = 0; m < 8; ++m) {
        im.w[kImLod + m] = g.bp[fixes::TexMode0Reg(m)];
        im.w[kImLod + 8 + m] = g.bp[fixes::TexMode1Reg(m)];
    }
}
// What the template path sent last of the projection, light and TEV blocks, and from what. The normal path's sends clear the flags
// (g_j3n2Mir, at the Blk* wrappers).
struct Mirror {
    uint32_t proj[7] = {};
    uint16_t lightVs = 0;
    uint32_t lightN = 0;
    uint32_t light[4 + 8 * 16] = {};
    uint16_t tevFs = 0;
    uint32_t tev[kImLod + 16] = {};
    uint32_t tevLive[4] = {}; // xf 0x1C 0x1F (the viewport's z: GXV_FOGVTX) and 0x24 0x25 (the projection's z), which BuildTevC reads as well
    uint16_t texW[8] = {}, texH[8] = {};
} g_mir;
const Tpl* g_lastTpl = nullptr;
inline uint32_t TplImg(TplSet& set, const Img& im) {
    // (the images of a chain repeat: the shapes of a material share one; a short look back finds most)
    const size_t n = set.img.size();
    for (size_t i = n; i > 0 && i + 8 > n; --i)
        if (std::memcmp(set.img[i - 1].w, im.w, sizeof im.w) == 0) return static_cast<uint32_t>(i - 1);
    set.img.push_back(im);
    return static_cast<uint32_t>(n);
}
inline TplKey KeyNow(uint32_t vcdMtxMask) {
    TplKey k;
    k.v = g_draw->v;
    k.mask = vcdMtxMask;
    k.vcd0 = g.cpRaw[0];
    k.vcd1 = g.cpRaw[1];
    k.palCls = PalClass(g_pendingPosMtxMax, VGX_KNOB(UniformPacking()) != 0, (vcdMtxMask & 1u) != 0);
    return k;
}
// The run from template t (true), or nothing changed (false; why: 0 none, 1 a texture, 2 not usable).
bool TplApply(const TplKey& k, uint32_t vcdMtxMask, int& why) {
    TplSet& set = *g_draw->set;
    Tpl* const t = set.Find(k);
    if (!t) { why = 0; return false; }
    if (!t->ok) { why = 2; return false; }
    for (uint32_t m = t->texMask; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        if (!TexTouch(i, t->texRegs[i], t->tex[i], t->texW[i], t->texH[i])) { why = 1; return false; }
    }
    // ---- the run takes the template (nothing below fails)
    g_pendingPosMtxMax = 0xFF; // (consumed, as the resolution consumes it)
    Img im; // (the words the TEV block reads: the register file is real at a templated run)
    ImgSave(im);
    const bool fsChanged = !cur.valid || t->fsId != cur.fsId, vsChanged = !cur.valid || t->vsId != cur.vsId;
    if ((fsChanged || vsChanged) && !dj::g_stale) {
        dj::g_fsHeld = cur.valid ? cur.fsId : ~0u;
        dj::g_vsHeld = cur.valid ? cur.vsId : ~0u;
        dj::g_stale = true;
    }
    if (fsChanged) { cur.fsId = t->fsId; cur.fsIdFoldValid = false; }
    if (vsChanged) cur.vsId = t->vsId;
    cur.foldOk = t->foldOk != 0;
    cur.vcdMtxMask = vcdMtxMask;
    cur.palCls = k.palCls;
    g_curKey.valid = false;
    if (t != g_lastTpl || !cur.valid) ++cur.stateSerial;
    g_lastTpl = t;
    cur.rs = t->rs;
    cur.pixBlendOff = 0;
    cur.cullAll = t->cullAll != 0;
    cur.texMask = t->texMask;
    CopyWords(cur.tex, t->tex, static_cast<uint32_t>(sizeof cur.tex / 4));
    std::memcpy(cur.texW, t->texW, sizeof cur.texW);
    std::memcpy(cur.texH, t->texH, sizeof cur.texH);
    cur.slotResolved = 0;
    // ---- the uniform blocks
    const bool stale = g_blocksStale;
    if (stale || !g_j3n2Mir.proj || !SameWords(g_mir.proj, &g.xfReg[0x20], 7)) {
        BlkProj();
        CopyWords(g_mir.proj, &g.xfReg[0x20], 7);
        g_j3n2Mir.proj = true;
        ++g_dctr.projSent;
    }
    const VsKey& vs = dj::CurVs();
    cur.mtxRowsUsed = BlkMtx(vs, false);
    cur.soft = false;
    {
        const int nvec = UniformPacking() ? VsLightCount(vs) : 44;
        const uint32_t nl = static_cast<uint32_t>((nvec - 4) / 5);
        if (stale || !g_j3n2Mir.light || g_mir.lightVs != cur.vsId || g_mir.lightN != nl || !SameWords(g_mir.light, &g.xfReg[0x0A], 4) ||
            !SameWords(g_mir.light + 4, &g.xfLight[0][0], 16 * nl)) {
            BlkLight(vs);
            g_mir.lightVs = cur.vsId;
            g_mir.lightN = nl;
            CopyWords(g_mir.light, &g.xfReg[0x0A], 4);
            CopyWords(g_mir.light + 4, &g.xfLight[0][0], 16 * nl);
            g_j3n2Mir.light = true;
            ++g_dctr.lightSent;
        }
    }
    {
        const uint32_t live[4] = {g.xfReg[0x1C], g.xfReg[0x1F], g.xfReg[0x24], g.xfReg[0x25]};
        if (stale || !g_j3n2Mir.tev || g_mir.tevFs != cur.fsId || !SameWords(g_mir.tev, im.w, kImCol) || !SameWords(g_mir.tev + kImLod, im.w + kImLod, 16) ||
            !SameWords(g_mir.tevLive, live, 4) || std::memcmp(g_mir.texW, cur.texW, sizeof g_mir.texW) != 0 ||
            std::memcmp(g_mir.texH, cur.texH, sizeof g_mir.texH) != 0) {
            BlkTev(dj::CurFs(), cur.texW, cur.texH, false);
            g_mir.tevFs = cur.fsId;
            CopyWords(g_mir.tev, im.w, kImCol);
            CopyWords(g_mir.tev + kImLod, im.w + kImLod, 16);
            CopyWords(g_mir.tevLive, live, 4);
            std::memcpy(g_mir.texW, cur.texW, sizeof g_mir.texW);
            std::memcpy(g_mir.texH, cur.texH, sizeof g_mir.texH);
            g_j3n2Mir.tev = true;
            ++g_dctr.tevSent;
        }
    }
    cur.fold = false;
    cur.useFs = PixUseFs(cur.fsId, cur.rs);
    g_blocksStale = false;
    cur.vcdGen = g.vcdGen;
    cur.valid = true;
    return true;
}
// After a normal resolution of the run (the register file real): its template, made or (verify) compared.
void Capture(const TplKey& k) {
    if (!cur.valid) return;
    TplSet& set = *g_draw->set;
    const bool verify = g_draw->mode == kShadow;
    Tpl* t = set.Find(k);
    Tpl now;
    now.k = k;
    now.fsId = cur.fsId;
    now.vsId = cur.vsId;
    now.useFs = cur.useFs;
    now.texMask = cur.texMask;
    now.foldOk = cur.foldOk ? 1 : 0;
    now.cullAll = cur.cullAll ? 1 : 0;
    now.rs = cur.rs;
    CopyWords(now.tex, cur.tex, static_cast<uint32_t>(sizeof cur.tex / 4));
    std::memcpy(now.texW, cur.texW, sizeof now.texW);
    std::memcpy(now.texH, cur.texH, sizeof now.texH);
    bool ok = !cur.fold && !cur.soft && cur.pixBlendOff == 0;
    for (uint32_t m = cur.texMask; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        TexRegsOf(i, now.texRegs[i]);
        if (!TexKnown(i, now.texRegs[i], now.tex[i], now.texW[i], now.texH[i])) ok = false;
    }
    now.ok = ok ? 1 : 0;
    Img im;
    ImgSave(im);
    if (t && verify && t->ok) {
        // (verify: what the template would have given against what the resolution gave)
        ++g_dctr.checked;
        const Img& ti = set.img[t->img];
        const bool same = t->fsId == now.fsId && t->vsId == now.vsId && t->useFs == now.useFs && t->texMask == now.texMask && t->cullAll == now.cullAll &&
                          std::memcmp(&t->rs, &now.rs, sizeof now.rs) == 0 && std::memcmp(t->tex, now.tex, sizeof now.tex) == 0 &&
                          std::memcmp(t->texW, now.texW, sizeof now.texW) == 0 && std::memcmp(t->texH, now.texH, sizeof now.texH) == 0 &&
                          std::memcmp(ti.w, im.w, sizeof im.w) == 0;
        if (!same) {
            ++g_dctr.bad;
            const char* what = (t->fsId != now.fsId || t->vsId != now.vsId || t->useFs != now.useFs) ? "shader ids"
                               : std::memcmp(&t->rs, &now.rs, sizeof now.rs) != 0                     ? "render state"
                               : (t->texMask != now.texMask || std::memcmp(t->tex, now.tex, sizeof now.tex) != 0 || std::memcmp(t->texW, now.texW, sizeof now.texW) != 0 ||
                                  std::memcmp(t->texH, now.texH, sizeof now.texH) != 0) ? "texture bindings"
                               : t->cullAll != now.cullAll ? "culled flag" : "register image";
            VGX_LOG_N(32, "j3n: MISMATCH (stage 2 template: %s) frame %llu: lazy version %u, mask %x, palette class %u: template fs %u vs %u, normal fs %u vs %u\n", what,
                      static_cast<unsigned long long>(g_frame), k.v, k.mask, k.palCls, t->fsId, t->vsId, now.fsId, now.vsId);
            t->ok = 0;
        }
        return;
    }
    if (!ok) ++g_dctr.refused;
    if (!t) t = set.Add(k);
    now.img = TplImg(set, im);
    *t = now;
    ++g_dctr.captured;
}
} // namespace

void ResolverCold() {
    cur.valid = false; // (the serial stays: serials only grow; ChaClassify and the records key on them)
    cur.slotResolved = 0;
    g_curKey.valid = false;
    g_softOkVs = 0xFFFF;
    dj::g_stale = false; // (cur.valid false: the next resolution copies the keys of whatever ids it sets)
    g_lastTpl = nullptr;
    g_j3n2Mir.proj = g_j3n2Mir.light = g_j3n2Mir.tev = false;
}
} // namespace j3n2

