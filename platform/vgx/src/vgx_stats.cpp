// Feature-usage bookkeeping for the status report: which GX features the game
// actually exercised and how many shaders each stretch of frames needed.
#include "vgx_state.h"
#include "vgx_diagnostic_csv.h"

#include <map>
#include <set>
#include <string>

namespace vgx::stats {

namespace {
std::map<uint32_t, uint64_t> g_texFmt, g_copyFmt;
std::set<std::string> g_notes, g_vtxFmt;
std::map<uint32_t, uint64_t> g_layouts;
uint32_t g_maxStages = 0, g_maxTexGens = 0, g_maxInd = 0, g_maxChans = 0, g_maxLights = 0;
uint64_t g_draws = 0, g_verts = 0, g_dispCopies = 0;
bool g_alphaTest = false, g_fog = false, g_perVtxPos = false, g_perVtxTex = false, g_dstAlpha = false, g_konst = false,
     g_swap = false, g_cmp = false, g_proj = false, g_post = false, g_lighting = false, g_srtg = false, g_posTexgen = false,
     g_nrmTexgen = false, g_mipTex = false, g_npot = false;
struct Created { uint64_t frame; bool fragment; };
std::vector<Created> g_created;
uint32_t g_maxTexDim = 0;
std::set<uint32_t> g_fogTypes, g_tevOps;
std::set<uint32_t> g_frameTex, g_framePrograms, g_allPrograms;
uint64_t g_frameTexBytes = 0;
int64_t g_liveTex = 0, g_liveTexBytes = 0;
} // namespace
Frame g_curFrame;

void TexFormat(uint32_t fmt, uint16_t w, uint16_t h, bool mip) {
    if (!diag::kEnabled) return;
    ++g_texFmt[fmt];
    if (mip) g_mipTex = true;
    if ((w & (w - 1)) || (h & (h - 1))) g_npot = true;
    if (w > g_maxTexDim) g_maxTexDim = w;
    if (h > g_maxTexDim) g_maxTexDim = h;
}
void CopyFormat(uint32_t fmt, bool disp) {
    if (!diag::kEnabled) return;
    if (disp) ++g_dispCopies;
    else ++g_copyFmt[fmt];
}
void Note(const char* what) {
    if (!diag::kEnabled) return;
    if (g_notes.insert(what).second) std::fprintf(stderr, "[vgx] note: %s\n", what);
}
void VertexFormat(int attr, int type, const VtxAttrFmt& f) {
    if (!diag::kEnabled) return;
    static const char* const an[21] = {"PNMTXIDX", "TEX0MTXIDX", "TEX1MTXIDX", "TEX2MTXIDX", "TEX3MTXIDX", "TEX4MTXIDX", "TEX5MTXIDX",
                                       "TEX6MTXIDX", "TEX7MTXIDX", "POS", "NRM", "CLR0", "CLR1", "TEX0", "TEX1", "TEX2", "TEX3", "TEX4",
                                       "TEX5", "TEX6", "TEX7"};
    static const char* const tn[4] = {"none", "direct", "index8", "index16"};
    static const char* const cn[5] = {"u8", "s8", "u16", "s16", "f32"};
    static const char* const col[6] = {"rgb565", "rgb8", "rgbx8", "rgba4", "rgba6", "rgba8"};
    char b[96];
    if (attr <= 8) std::snprintf(b, sizeof b, "%s direct u8", an[attr]);
    else if (attr == 11 || attr == 12) std::snprintf(b, sizeof b, "%s %s %s", an[attr], tn[type & 3], col[f.type < 6 ? f.type : 5]);
    else if (attr == 10) std::snprintf(b, sizeof b, "%s %s %s %s", an[attr], tn[type & 3], cn[f.type < 5 ? f.type : 4], f.cnt == 0 ? "xyz" : (f.cnt == 1 ? "nbt" : "nbt3"));
    else std::snprintf(b, sizeof b, "%s %s %s x%d frac %d", an[attr], tn[type & 3], cn[f.type < 5 ? f.type : 4], attr == 9 ? (f.cnt ? 3 : 2) : (f.cnt ? 2 : 1), f.frac);
    g_vtxFmt.insert(b);
}
void Draw(const FsKey& fs, const VsKey& vs, uint32_t layoutMask, uint32_t verts) {
    if (!diag::kEnabled) return;
    ++g_draws;
    g_verts += verts;
    ++g_layouts[layoutMask];
    if (fs.numStages > g_maxStages) g_maxStages = fs.numStages;
    if (fs.numTexGens > g_maxTexGens) g_maxTexGens = fs.numTexGens;
    if (fs.numIndStages > g_maxInd) g_maxInd = fs.numIndStages;
    if (vs.numChans > g_maxChans) g_maxChans = vs.numChans;
    if (fs.alphaTestOn) g_alphaTest = true;
    if (fs.fog) { g_fog = true; g_fogTypes.insert(fs.fog); }
    if (fs.dstAlpha) g_dstAlpha = true;
    if (fs.texProjMask) g_proj = true;
    if (vs.perVtxPos) g_perVtxPos = true;
    for (int s = 0; s < fs.numStages; ++s) {
        if (fs.stage[s].kcsel >= 12 || fs.stage[s].kasel >= 16) g_konst = true;
        if (fs.stage[s].rasSwap != 0xE4 || fs.stage[s].texSwap != 0xE4) g_swap = true;
        if (((fs.stage[s].color >> 16) & 3) == 3 || ((fs.stage[s].alpha >> 16) & 3) == 3) g_cmp = true;
    }
    for (int i = 0; i < 4; ++i) {
        if (vs.chan[i].enable) g_lighting = true;
        uint32_t n = 0;
        for (int l = 0; l < 8; ++l) n += (vs.chan[i].lightMask >> l) & 1;
        if (n > g_maxLights) g_maxLights = n;
    }
    for (int i = 0; i < vs.numTexGens; ++i) {
        if (vs.tg[i].perVtxMtx) g_perVtxTex = true;
        if (vs.tg[i].post) g_post = true;
        if (vs.tg[i].type >= 2) g_srtg = true;
        if (vs.tg[i].type == 0 && vs.tg[i].src == 0) g_posTexgen = true;
        if (vs.tg[i].type == 0 && vs.tg[i].src == 1) g_nrmTexgen = true;
    }
}
void ShaderCreated(bool fragment, uint64_t frame) {
    if (diag::kEnabled) g_created.push_back(Created{frame, fragment});
    if (fragment) ++g_curFrame.shadersFs; else ++g_curFrame.shadersVs;
}
bool g_detailed = false;
void SetDetailed(bool on) { g_detailed = diag::kEnabled && on; }
void TexBound(uint32_t texId, uint32_t bytes) {
    if (!diag::kEnabled) return;
    if (g_frameTex.insert(texId).second) g_frameTexBytes += bytes;
}
void ProgramUsed(uint16_t vsId, uint16_t fsId) {
    if (!diag::kEnabled) return;
    const uint32_t key = (static_cast<uint32_t>(vsId) << 16) | fsId;
    g_framePrograms.insert(key);
    g_allPrograms.insert(key);
}
void TexLive(int32_t dc, int64_t db) {
    if (!diag::kEnabled) return;
    g_liveTex += dc; g_liveTexBytes += db;
}
static diagnosticcsv::Log frameLog;
static_assert(sizeof(diagnosticcsv::Row::data) > 54u * 21u, "CSV row fits worst-case integer fields");
void CloseFrameLog() {
    if (diag::kEnabled) frameLog.Close();
}
int ExportFrameLog() {
    if (!diag::kEnabled) return 0;
    const bool wasFrozen = frameLog.Capture().Frozen();
    const int result = frameLog.ExportPaused();
    if (!wasFrozen && frameLog.Capture().Frozen())
        VGX_LOG("RAM frontend CSV: %u bytes retained, %u rows dropped, export result %d\n",
                static_cast<unsigned>(frameLog.Capture().ExportedBytes()), frameLog.Capture().Dropped(), result);
    return result;
}
void FrameEnd(uint64_t frame) {
    static bool tried = false;
    if (diag::kEnabled && !tried) {
        tried = true;
        if (const char* p = std::getenv("GXV_FRAMESTATS")) {
            frameLog.Open(p);
            if (!frameLog.Enabled()) VGX_ERR("frontend CSV disabled: unable to initialize capture\n");
            if (frameLog.Enabled()) {
                diagnosticcsv::Row header;
                header.Add("frame,prims,primsDL,verts,tris,drawCalls,streams,streamBytes,texBound,texBoundBytes,texUploads,texUploadBytes,"
                                   "texLive,texLiveBytes,copies,copyPixels,programs,programsTotal,shadersVs,shadersFs,vtxBytes,idxBytes,uniBytes,softVerts,vertsDL,dlHit,dlBuild,dlRehash,dlBypass,staticDraws,staticVerts,bufUploads,bufUploadBytes,deadCopies,texHashes,texHashBytes,texUntracked,texEvicted,texEvictBytes,passes,passDraws,passesDirect,fills,fillsRedundant,efbReads,clearsRedundant,efbScenes,offScenes,atlasPasses,atlasRemaps,atlasMerged,atlasEvicted,atlasSorted,hash\n");
                frameLog.Append(header);
            }
        }
    }
    if (diag::kEnabled && frameLog.Enabled()) {
        const Frame& c = g_curFrame;
        diagnosticcsv::Row row;
        row.Add("%llu,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%u,%u,%lld,%lld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%016llx\n", static_cast<unsigned long long>(frame),
                     c.prims, c.primsDL, c.verts, c.tris, c.drawCalls, c.streams, c.streamBytes, static_cast<unsigned>(g_frameTex.size()),
                     static_cast<unsigned long long>(g_frameTexBytes), c.texUploads, c.texUploadBytes, static_cast<long long>(g_liveTex),
                     static_cast<long long>(g_liveTexBytes), c.copies, c.copyPixels, static_cast<unsigned>(g_framePrograms.size()), static_cast<unsigned>(g_allPrograms.size()), c.shadersVs,
                     c.shadersFs, c.vtxBytes, c.idxBytes, c.uniBytes, c.softVerts, c.vertsDL, c.dlHit, c.dlBuild, c.dlRehash, c.dlBypass, c.staticDraws, c.staticVerts, c.bufUploads, c.bufUploadBytes, c.deadCopies, c.texHashes, c.texHashBytes, c.texUntracked, c.texEvicted, c.texEvictBytes, c.passes, c.passDraws, c.passesDirect, c.fills, c.fillsRedundant, c.efbReads, c.clearsRedundant, c.efbScenes, c.offScenes,
                     c.atlasPasses, c.atlasRemaps, c.atlasMerged, c.atlasEvicted, c.atlasSorted, static_cast<unsigned long long>(c.hash));
        frameLog.Append(row);
        if (frame % 256 == 0) frameLog.FlushLive();
    }
    g_curFrame = Frame{};
    g_frameTex.clear();
    g_frameTexBytes = 0;
    g_framePrograms.clear();
}

void Dump(const char* path) {
    if (!diag::kEnabled) return;
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    static const char* const fmtName[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7", "C4", "C8", "C14X2", "?B", "?C", "?D", "CMPR", "?F"};
    std::fprintf(f, "vgx feature usage up to presented frame %llu\n", static_cast<unsigned long long>(CurrentFrame()));
    std::fprintf(f, "draw records emitted (before merging): %llu, vertices: %llu, display copies: %llu\n",
                 static_cast<unsigned long long>(g_draws), static_cast<unsigned long long>(g_verts), static_cast<unsigned long long>(g_dispCopies));
    std::fprintf(f, "max TEV stages: %u, max texgens: %u, max indirect stages: %u, max colour channels: %u, max lights per channel: %u\n",
                 g_maxStages, g_maxTexGens, g_maxInd, g_maxChans, g_maxLights);
    std::fprintf(f, "lighting: %d, alpha test: %d, fog: %d, konst colours: %d, non-identity swap tables: %d, TEV compare ops: %d\n", g_lighting,
                 g_alphaTest, g_fog, g_konst, g_swap, g_cmp);
    std::fprintf(f, "projective texgen: %d, post-transform matrices: %d, colour (SRTG) texgen: %d, position-source texgen: %d, normal-source texgen: %d\n",
                 g_proj, g_post, g_srtg, g_posTexgen, g_nrmTexgen);
    std::fprintf(f, "per-vertex position matrix index: %d, per-vertex texture matrix index: %d, constant dst alpha: %d\n", g_perVtxPos, g_perVtxTex,
                 g_dstAlpha);
    std::fprintf(f, "fog types (fsel | ortho<<3):");
    for (uint32_t t : g_fogTypes) std::fprintf(f, " %u", t);
    std::fprintf(f, "\ntexture uploads by format:");
    for (auto& e : g_texFmt) std::fprintf(f, " %s=%llu", fmtName[e.first & 15], static_cast<unsigned long long>(e.second));
    std::fprintf(f, "\nmipmapped textures: %d, non-power-of-two textures: %d, largest dimension: %u\n", g_mipTex, g_npot, g_maxTexDim);
    std::fprintf(f, "EFB texture copies by format:");
    for (auto& e : g_copyFmt) std::fprintf(f, " 0x%02X=%llu", e.first, static_cast<unsigned long long>(e.second));
    std::fprintf(f, "\nvertex components seen:\n");
    for (auto& s : g_vtxFmt) std::fprintf(f, "  %s\n", s.c_str());
    std::fprintf(f, "decoded vertex layouts (mask=draws):");
    for (auto& e : g_layouts) std::fprintf(f, " 0x%X=%llu", e.first, static_cast<unsigned long long>(e.second));
    std::fprintf(f, "\n");
    DlCacheReport(f);
    TexCacheReport(f);
    PassCheckReport(f);
    std::fprintf(f, "notes:\n");
    for (auto& s : g_notes) std::fprintf(f, "  %s\n", s.c_str());
    // Shader creation timeline, bucketed by 100 frames.
    std::map<uint64_t, std::pair<int, int>> buckets;
    int nv = 0, nf = 0;
    for (auto& c : g_created) {
        auto& b = buckets[c.frame / 100];
        if (c.fragment) { ++b.second; ++nf; } else { ++b.first; ++nv; }
    }
    std::fprintf(f, "shaders generated: %d vertex, %d fragment\n", nv, nf);
    for (auto& b : buckets)
        std::fprintf(f, "  frames %llu..%llu: +%d vertex, +%d fragment\n", static_cast<unsigned long long>(b.first * 100),
                     static_cast<unsigned long long>(b.first * 100 + 99), b.second.first, b.second.second);
    std::fclose(f);
}

} // namespace vgx::stats
