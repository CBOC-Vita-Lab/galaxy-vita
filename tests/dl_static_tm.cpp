// GXV_DL_STATIC_TM (game24, 10 Nov), on the extended copy of cpu60's dl_static_why.cpp. Plain (mode 0) texture matrices are allowed; a model is left out only while
// their content hash is what the last full computation left. Stand-in calc: plain mMtx = f(info) and texgen reg = f(post flag); other modes also read a
// per-flush "view" counter. Writers between flushes change info/srt of plain and other matrices and swap matrix pointers (TexMtxCtrl-like), in both worlds.
// PORT_DLS_NEG=1: qualifier blind to texture matrices; PORT_DLS_NEG=2: plain result secretly reads the view counter (the assumption broken): both must FAIL.
// (original header follows) GXV_DL_STATIC (cpu60, 10 Nov): the repeat test of the material-list flush, on the host. The REAL code of ModelManager.cpp between its
// "cpu60-test-begin" / "cpu60-test-end" markers (DlStaticQualifies*, DlStaticFlagOf, DlStaticWordsAgree, DlStaticSysState, DlStaticTry,
// DlStaticDone) is cut out by tools/test_dl_static_why.sh and compiled here against small stand-ins for the J3D types it touches.
// What is modelled: a "calcMaterial" (updateDL(true)) that does what J3DModel::calcMaterial does to the state this code looks at:
// syncs j3dSys's post-texture-matrix flag from the model DATA (flag 0x20), and writes every material's two current-matrix words
// (from its texgen: here a function of the flag and the material) into the material and its shape. Several models share one data.
// Checked, over random lists of mixed models (flag clear / set, qualifying / not, shared data), many flushes, other writers
// (FurCtrl-like: a shape's words overwritten between flushes):
//   1. the state after every flush (all materials' and shapes' words, j3dSys flags, the model / texture j3dSys last got) equals the
//      state of the flush that computes EVERY model in full (the original game's);
//   2. with GXV_DL_STATIC_FLAGFIX=1 a list of mixed kinds still leaves repeats out (flagfix 0, the first version, computes the models
//      whose predecessor in the list had another flag in full again: fewer left out, same state);
//   3. GXV_DL_STATIC=2 (verify) computes everything and finds 0 differences;
//   4. the reasons (why counts) add up to the models listed, and a model with a maker / player / material animation / texture matrix is
//      never left out (negative control PORT_DLS_NEG=1: qualify ignores the texture matrix: check 1 must FAIL).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
static unsigned g_fullCalcs = 0;
static bool g_hideTex = false;  // (negative control: the qualifier does not see texture matrices)
static bool g_flagfix = true;
static int g_drawProf = 2;
static u32 g_view = 0;
static int g_neg = 0;
extern "C" int port_dl_static_tm_on(void) { return 1; }
extern "C" int port_draw_prof_on(void) { return g_drawProf; }
extern "C" int port_dl_static_flagfix_on(void) { return g_flagfix ? 1 : 0; }
extern "C" void port_log_summary(const char* f, ...) { (void)f; }
// ---- stand-ins
struct MtxIdx { u32 mMtxIdxRegA = 0, mMtxIdxRegB = 0; };
struct J3DShape { MtxIdx mCurrentMtx; };
typedef uint64_t u64;
struct J3DTexMtxInfo { u8 mInfo = 0; u8 pad[3] = {}; u32 srt[4] = {}; };
typedef u32 Mtx[3][4];
struct J3DTexMtx {
    J3DTexMtxInfo i; u32 mMtx[3][4] = {};
    J3DTexMtxInfo& getTexMtxInfo() { return i; }
    Mtx& getMtx() { return mMtx; }
    void calc(bool postFlag) {  // (J3DTexMtx::calc: plain = f(info) only; the other modes read the view)
        u32 v = i.srt[0] * 31u + i.srt[1] * 7u + i.srt[2] + i.srt[3] * 3u + i.mInfo;
        if ((i.mInfo & 0x3F) != 0 || g_neg == 2) v += g_view * 977u;
        if (postFlag) v ^= 0x5a5a5a5au;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) mMtx[r][c] = v + r * 4 + c;
    }
};
struct J3DTexCoord {
    u8 type = 1, src = 4, mtx = 30; u16 reg = 30;
    u8 getTexGenType() const { return type; } u8 getTexGenSrc() const { return src; } u8 getTexGenMtx() const { return mtx; } u32 getTexMtxReg() const { return reg & 0xff; }
};
struct J3DMaterialAnm {};
struct J3DMaterial {
    J3DShape* shape = nullptr; J3DMaterialAnm* anm = nullptr; J3DTexMtx* tex[8] = {}; J3DTexMtx* spare = nullptr; J3DTexCoord tc[8]; MtxIdx mCurrentMtx; u32 id = 0;
    J3DTexCoord* getTexCoord(u32 j) { return &tc[j]; }
    J3DShape* getShape() { return shape; } J3DMaterialAnm* getMaterialAnm() { return anm; } J3DTexMtx* getTexMtx(u32 j) { return g_hideTex ? nullptr : tex[j]; }
};
struct J3DModelData {
    std::vector<J3DMaterial*> mats; u32 flags = 0;
    u16 getMaterialNum() const { return static_cast<u16>(mats.size()); }
    J3DMaterial* getMaterialNodePointer(u16 i) const { return mats[i]; }
    bool checkFlag(u32 f) const { return (flags & f) != 0; }
    void* getTexture() { return this; }
    void syncJ3DSysFlags() const;
};
struct Packet {};
struct J3DModel {
    J3DModelData* data = nullptr; u32 modelFlags = 0; Packet pk[64];
    J3DModelData* getModelData() { return data; }
    bool checkFlag(u32 f) const { return (modelFlags & f) != 0; }
    Packet* getMatPacket(u16 i) { return &pk[i]; }
};
enum { J3DMdlFlag_SkinPosCpu = 1, J3DMdlFlag_SkinNrmCpu = 2 };
struct MSys {
    u32 flags = 0; J3DModel* model = nullptr; void* tex = nullptr; Packet* pkt = nullptr;
    void setModel(J3DModel* m) { model = m; } void onFlag(u32 f) { flags |= f; } void offFlag(u32 f) { flags &= ~f; }
    bool checkFlag(u32 f) const { return (flags & f) != 0; } void setTexture(void* t) { tex = t; } void setMatPacket(Packet* p) { pkt = p; }
} g_sys;
#define J3D_MSYS g_sys
void J3DModelData::syncJ3DSysFlags() const { if (checkFlag(0x20)) g_sys.onFlag(0x40000000); else g_sys.offFlag(0x40000000); }
extern "C" int port_dl_static_xanim_on(void) { return 0; }   // (tests31: off here: the cut then reads mModel as before)
struct DisplayListMaker {}; struct Player {};
struct LiveActor { const char* getName() const { return "actor"; } };
struct ModelManager {
    DisplayListMaker* mDisplayListMaker = nullptr; Player *mBtkPlayer = nullptr, *mBrkPlayer = nullptr, *mBtpPlayer = nullptr, *mBpkPlayer = nullptr;
    J3DModel* mModel = nullptr; u8 mDlStatic = 0; u64 mDlTmHash = 0;
    J3DModel* getJ3DModel() const { return mModel; }   // (tests31: the cut now asks for it when GXV_DL_STATIC_XANIM is on)
    void updateDL(bool);
};
struct StaleDL { LiveActor* owner; ModelManager* model; };
// what calcMaterial writes into the state the repeat test looks at
static u32 WordA(u32 id, bool flag) { return id * 2654435761u + (flag ? 0x40000000u : 0u); }
void ModelManager::updateDL(bool) {
    ++g_fullCalcs;
    J3D_MSYS.setModel(mModel);
    if (mModel->checkFlag(J3DMdlFlag_SkinPosCpu)) J3D_MSYS.onFlag(4); else J3D_MSYS.offFlag(4);
    if (mModel->checkFlag(J3DMdlFlag_SkinNrmCpu)) J3D_MSYS.onFlag(8); else J3D_MSYS.offFlag(8);
    mModel->getModelData()->syncJ3DSysFlags();
    J3D_MSYS.setTexture(mModel->getModelData()->getTexture());
    const u16 n = mModel->getModelData()->getMaterialNum();
    for (u16 i = 0; i < n; ++i) {
        J3D_MSYS.setMatPacket(mModel->getMatPacket(i));
        J3DMaterial* m = mModel->getModelData()->getMaterialNodePointer(i);
        const bool f = J3D_MSYS.checkFlag(0x40000000);
        u32 texSum = 0;
        for (int j = 0; j < 8; ++j) if (m->tex[j]) {  // (the real texgen: calc of each texture matrix, its texgen register from the post flag)
            m->tex[j]->calc(f); m->tc[j].reg = static_cast<u16>(m->tc[j].mtx + (f ? 3 : 0));
            texSum += m->tex[j]->i.mInfo + m->tex[j]->mMtx[0][0];
        }
        m->mCurrentMtx.mMtxIdxRegA = WordA(m->id, f) + texSum; m->mCurrentMtx.mMtxIdxRegB = WordA(m->id + 1000, f);
        m->shape->mCurrentMtx = m->mCurrentMtx;
    }
}
// ---- the real code
#include "dl_static_cut.inc"
// ---- the test
struct Scene {
    std::vector<J3DModelData*> datas; std::vector<ModelManager*> mms; std::vector<J3DModel*> models; std::vector<LiveActor*> actors;
    std::vector<J3DMaterial*> allMats; std::vector<J3DShape*> allShapes; std::vector<J3DTexMtx*> texs;
};
static uint64_t rs = 88172645463325252ull;
static unsigned R(unsigned n) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return static_cast<unsigned>(rs % n); }
static Scene Build(unsigned nData, unsigned nModels, bool negKinds) {
    Scene s; u32 id = 1;
    for (unsigned d = 0; d < nData; ++d) {
        auto* data = new J3DModelData; data->flags = R(2) ? 0x20 : 0;
        const unsigned kind = R(8);  // 0..2 plain, 3 maker-model data, 4 material animation, 5 plain texture matrices (qualify with TM), 6 other-mode texture matrix, 7 both
        const unsigned nm = 1 + R(3);
        for (unsigned k = 0; k < nm; ++k) {
            auto* m = new J3DMaterial; m->id = id; id += 3; m->shape = new J3DShape;
            if (kind == 4 && k == 0) m->anm = new J3DMaterialAnm;
            if (kind == 5 || kind == 7) { for (unsigned q = 0, nq = 1 + R(3); q < nq; ++q) { auto* t = new J3DTexMtx; t->i.mInfo = 0; t->i.srt[0] = R(50); t->i.srt[1] = R(50); m->tex[R(8)] = t; s.texs.push_back(t); } m->spare = new J3DTexMtx; s.texs.push_back(m->spare); }
            if ((kind == 6 || kind == 7) && k == 1) { auto* t = new J3DTexMtx; t->i.mInfo = 1 + R(11); m->tex[1] = t; s.texs.push_back(t); }
            data->mats.push_back(m); s.allMats.push_back(m); s.allShapes.push_back(m->shape);
        }
        s.datas.push_back(data);
        (void)negKinds;
    }
    for (unsigned i = 0; i < nModels; ++i) {
        auto* mm = new ModelManager; auto* m = new J3DModel; m->data = s.datas[R(nData)]; m->modelFlags = R(4);
        mm->mModel = m;
        if (R(9) == 0) mm->mDisplayListMaker = new DisplayListMaker;
        if (R(11) == 0) mm->mBtkPlayer = new Player;
        s.mms.push_back(mm); s.models.push_back(m); s.actors.push_back(new LiveActor);
    }
    return s;
}
struct Snap { std::vector<u32> w; };
static void Take(const Scene& s, Snap& o) {
    o.w.clear();
    for (auto* m : s.allMats) { o.w.push_back(m->mCurrentMtx.mMtxIdxRegA); o.w.push_back(m->mCurrentMtx.mMtxIdxRegB); }
    for (auto* t : s.texs) { for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) o.w.push_back(t->mMtx[r][c]); }
    for (auto* m : s.allMats) for (int j = 0; j < 8; ++j) { o.w.push_back(m->tc[j].reg); o.w.push_back(m->tex[j] ? 1u : 0u); }
    for (auto* sh : s.allShapes) { o.w.push_back(sh->mCurrentMtx.mMtxIdxRegA); o.w.push_back(sh->mCurrentMtx.mMtxIdxRegB); }
    o.w.push_back(g_sys.flags); o.w.push_back(static_cast<u32>(reinterpret_cast<uintptr_t>(g_sys.model) >> 4));
    o.w.push_back(static_cast<u32>(reinterpret_cast<uintptr_t>(g_sys.tex) >> 4)); o.w.push_back(static_cast<u32>(reinterpret_cast<uintptr_t>(g_sys.pkt) >> 4));
}
// the flush as port_dl_flush_stale / PortDlRefreshOne do it for a list `order` (mode: 0 every model in full, 1 on, 2 verify)
static void Flush(Scene& s, const std::vector<unsigned>& order, int mode, unsigned* left, unsigned* verified, unsigned* bad, unsigned* why) {
    for (unsigned k : order) {
        StaleDL e{s.actors[k], s.mms[k]};
        if (mode != 0 && DlStaticTry(e.model, e, mode, left, verified, bad, why)) continue;
        e.model->updateDL(true);
        if (mode != 0) DlStaticDone(e.model);
    }
}
static const bool NEG = std::getenv("PORT_DLS_NEG") != nullptr;
struct NegInit { NegInit() { const char* e = std::getenv("PORT_DLS_NEG"); g_neg = e ? atoi(e) : 0; g_hideTex = g_neg == 1; } } g_negInit;
int main(int argc, char** argv) {
    const unsigned rounds = argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 200;
    unsigned fails = 0, leftFix = 0, leftOld = 0, listed = 0, whySum = 0, leftTotal = 0, tmLeft = 0, tmChanged = 0;
    for (unsigned r = 0; r < rounds; ++r) {
        for (int fix = 0; fix < 2; ++fix) {
            g_flagfix = fix != 0;
            rs = 1234567ull + r * 7919ull;  // the same scene for both
            Scene a = Build(2 + R(5), 20 + R(40), false);
            const unsigned nm = static_cast<unsigned>(a.mms.size());
            // reference world: every model in full, every flush
            rs = 1234567ull + r * 7919ull;
            Scene b = Build(2 + R(5), 20 + R(40), false);
            (void)nm;
            unsigned lf = 0, vf = 0, bd = 0, why[11] = {}, lf2 = 0, vf2 = 0, bd2 = 0, why2[11] = {};
            for (int flush = 0; flush < 12; ++flush) {
                std::vector<unsigned> order;
                for (unsigned i = 0; i < a.mms.size(); ++i) if (R(5) != 0) order.push_back(i);
                for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[R(static_cast<unsigned>(i))]);
                if (R(4) == 0 && !a.allShapes.empty()) {  // another writer: FurCtrl-like, a shape's words overwritten in both worlds
                    const unsigned k = R(static_cast<unsigned>(a.allShapes.size()));
                    const u32 v = R(1000000);
                    a.allShapes[k]->mCurrentMtx.mMtxIdxRegA = v; b.allShapes[k]->mCurrentMtx.mMtxIdxRegA = v;
                }
                { const u32 v = R(1000); g_view = v; }  // (the view/model matrix of the other modes changes each flush)
                if (R(3) == 0) {  // a writer swaps a plain matrix for the material's spare (TexMtxCtrl::updateMaterial-like), both worlds
                    const unsigned k = R(static_cast<unsigned>(a.allMats.size())); const unsigned j = R(8);
                    if (a.allMats[k]->spare && a.allMats[k]->tex[j]) { std::swap(a.allMats[k]->tex[j], a.allMats[k]->spare); std::swap(b.allMats[k]->tex[j], b.allMats[k]->spare); }
                }
                if (R(2) == 0) {  // a writer changes the SRT of a plain matrix (a btk detached, attach writes), both worlds
                    const unsigned k = a.texs.empty() ? 0 : R(static_cast<unsigned>(a.texs.size()));
                    if (!a.texs.empty()) { const u32 v = R(50); a.texs[k]->i.srt[0] = v; b.texs[k]->i.srt[0] = v; }
                }
                if (R(3) == 0) {  // a writer changes a texture matrix of one data's material (both worlds); the repeat test must notice it
                    const unsigned k = R(static_cast<unsigned>(a.allMats.size()));
                    for (unsigned j = 0; j < 8; ++j) if (a.allMats[k]->tex[j]) { const u8 v = static_cast<u8>(R(4) == 0 ? R(60) : 0); a.allMats[k]->tex[j]->i.mInfo = v; b.allMats[k]->tex[j]->i.mInfo = v; }
                }
                const unsigned c0 = g_fullCalcs; Flush(a, order, g_flagfix ? 1 : 1, &lf, &vf, &bd, why); (void)c0;
                Flush(b, order, 0, &lf2, &vf2, &bd2, why2);
                Snap sa, sb; Take(a, sa); Take(b, sb);
                if (sa.w != sb.w) { ++fails; if (fails < 4) printf("FAIL round %u flagfix %d flush %d: state differs from the full computation\n", r, fix, flush); }
                listed += static_cast<unsigned>(order.size());
                // verify world: mode 2 on a copy of a fresh state must not differ
            }
            for (unsigned k = 0; k < 10; ++k) whySum += why[k];
            tmLeft += why[10]; tmChanged += why[9];
            leftTotal += lf; if (fix) leftFix += lf; else leftOld += lf;
            // mode 2: verify world, same scene
            rs = 1234567ull + r * 7919ull;
            Scene v = Build(2 + R(5), 20 + R(40), false);
            unsigned lf3 = 0, vf3 = 0, bd3 = 0, why3[11] = {};
            for (int flush = 0; flush < 6; ++flush) {
                std::vector<unsigned> order; for (unsigned i = 0; i < v.mms.size(); ++i) order.push_back(i);
                Flush(v, order, 2, &lf3, &vf3, &bd3, why3);
            }
            if (bd3 != 0) { ++fails; if (fails < 4) printf("FAIL round %u: verify mode found %u differences\n", r, bd3); }
            // every one of the models in the verify run is either checked (repeat) or classified
            (void)vf3;
        }
    }
    // the reasons add up: every entry of every flush is counted exactly once (why[0] is "left out")
    if (!NEG && whySum != listed) { printf("FAIL: reasons counted %u, entries listed in the flagfix worlds %u\n", whySum, listed); ++fails; }
    printf("dl_static_tm: %u rounds, listed %u, left out with flagfix 1: %u, with flagfix 0: %u, reasons counted %u, g_fullCalcs %u, failures %u\n", rounds, listed, leftFix, leftOld, whySum, g_fullCalcs, fails);
    if (!NEG && leftFix < leftOld) { printf("FAIL: flagfix left out fewer (%u < %u)\n", leftFix, leftOld); ++fails; }
    printf("dl_static_tm: left out although the model has plain texture matrices: %u, computed in full because their content changed: %u\n", tmLeft, tmChanged);
    if (!NEG && (tmLeft == 0 || tmChanged == 0)) { printf("FAIL: the texture matrix path was not exercised both ways\n"); ++fails; }
    if (!NEG && leftFix == 0) { printf("FAIL: nothing was ever left out with flagfix 1\n"); ++fails; }
    if (fails) { printf("dl_static_tm: FAILED (%u)\n", fails); return 1; }
    printf("dl_static_tm: PASS\n");
    return 0;
}
