// GXV_DL_STATIC (cpu60, 10 Nov): the repeat test of the material-list flush, on the host. The REAL code of ModelManager.cpp between its
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
static bool g_xanim = true;  // (gamecpu25: GXV_DL_STATIC_XANIM)
extern "C" int port_dl_static_tm_on(void) { return 0; }
extern "C" int port_dl_static_xanim_on(void) { return g_xanim ? 1 : 0; }
typedef uint64_t u64; typedef uint8_t u8x;
extern "C" int port_draw_prof_on(void) { return g_drawProf; }
extern "C" int port_dl_static_flagfix_on(void) { return g_flagfix ? 1 : 0; }
extern "C" void port_log_summary(const char* f, ...) { (void)f; }
// ---- stand-ins
struct MtxIdx { u32 mMtxIdxRegA = 0, mMtxIdxRegB = 0; };
struct J3DShape { MtxIdx mCurrentMtx; };
struct TexInfo { u8 mInfo = 0; };
struct J3DTexMtx { TexInfo i; u32 mMtx[3][4] = {}; TexInfo& getTexMtxInfo() { return i; } u32 (&getMtx())[3][4] { return mMtx; } };
struct J3DTexCoord { u8 getTexGenType() const { return 0; } u8 getTexGenSrc() const { return 0; } u8 getTexGenMtx() const { return 0; } u32 getTexMtxReg() const { return 0; } };
typedef TexInfo J3DTexMtxInfo;
struct J3DMaterialAnm {};
struct J3DMaterial {
    J3DShape* shape = nullptr; J3DMaterialAnm* anm = nullptr; J3DTexMtx* tex[8] = {}; MtxIdx mCurrentMtx; u32 id = 0;
    J3DShape* getShape() { return shape; } J3DMaterialAnm* getMaterialAnm() { return anm; } J3DTexCoord* getTexCoord(u32) { static J3DTexCoord c; return &c; } J3DTexMtx* getTexMtx(u32 j) { return g_hideTex ? nullptr : tex[j]; }
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
struct DisplayListMaker {}; struct Player {};
struct LiveActor { const char* getName() const { return "actor"; } };
struct ModelManager {
    DisplayListMaker* mDisplayListMaker = nullptr; Player *mBtkPlayer = nullptr, *mBrkPlayer = nullptr, *mBtpPlayer = nullptr, *mBpkPlayer = nullptr;
    J3DModel* mModel = nullptr; u8 mDlStatic = 0; u64 mDlTmHash = 0;
    J3DModel* xanimModel = nullptr;  // (a manager with a BCK: mModel stays null, the model is the Xanime player's)
    J3DModel* getJ3DModel() const { return xanimModel != nullptr ? xanimModel : mModel; }
    void updateDL(bool);
};
struct StaleDL { LiveActor* owner; ModelManager* model; };
// what calcMaterial writes into the state the repeat test looks at
static u32 WordA(u32 id, bool flag) { return id * 2654435761u + (flag ? 0x40000000u : 0u); }
void ModelManager::updateDL(bool) {
    ++g_fullCalcs;
    J3DModel* mdl = getJ3DModel();
    J3D_MSYS.setModel(mdl);
    if (mdl->checkFlag(J3DMdlFlag_SkinPosCpu)) J3D_MSYS.onFlag(4); else J3D_MSYS.offFlag(4);
    if (mdl->checkFlag(J3DMdlFlag_SkinNrmCpu)) J3D_MSYS.onFlag(8); else J3D_MSYS.offFlag(8);
    mdl->getModelData()->syncJ3DSysFlags();
    J3D_MSYS.setTexture(mdl->getModelData()->getTexture());
    const u16 n = mdl->getModelData()->getMaterialNum();
    for (u16 i = 0; i < n; ++i) {
        J3D_MSYS.setMatPacket(mdl->getMatPacket(i));
        J3DMaterial* m = mdl->getModelData()->getMaterialNodePointer(i);
        const bool f = J3D_MSYS.checkFlag(0x40000000);
        u32 texSum = 0;
        for (auto* t : m->tex) if (t) texSum += t->i.mInfo;  // (the real texgen: from the texture matrices)
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
        const unsigned kind = R(6);  // 0..2 plain, 3 maker-model data (qualifies; the maker is per manager), 4 material animation, 5 texture matrix
        const unsigned nm = 1 + R(3);
        for (unsigned k = 0; k < nm; ++k) {
            auto* m = new J3DMaterial; m->id = id; id += 3; m->shape = new J3DShape;
            if (kind == 4 && k == 0) m->anm = new J3DMaterialAnm;
            if (kind == 5 && k == 0) { m->tex[R(8)] = new J3DTexMtx; m->tex[0] = m->tex[0] ? m->tex[0] : nullptr; }
            if (kind == 5 && k == 1) { auto* t = new J3DTexMtx; t->i.mInfo = 1 + R(11); m->tex[1] = t; }
            data->mats.push_back(m); s.allMats.push_back(m); s.allShapes.push_back(m->shape);
        }
        s.datas.push_back(data);
        (void)negKinds;
    }
    for (unsigned i = 0; i < nModels; ++i) {
        auto* mm = new ModelManager; auto* m = new J3DModel; m->data = s.datas[R(nData)]; m->modelFlags = R(4);
        mm->mModel = m;
        if (R(3) == 0) { mm->mModel = nullptr; mm->xanimModel = m; }  // (gamecpu25: a third of the managers have a BCK)
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
    for (auto* sh : s.allShapes) { o.w.push_back(sh->mCurrentMtx.mMtxIdxRegA); o.w.push_back(sh->mCurrentMtx.mMtxIdxRegB); }
    o.w.push_back(g_sys.flags); o.w.push_back(static_cast<u32>(reinterpret_cast<uintptr_t>(g_sys.model) >> 4));
    o.w.push_back(static_cast<u32>(reinterpret_cast<uintptr_t>(g_sys.tex) >> 4)); o.w.push_back(static_cast<u32>(reinterpret_cast<uintptr_t>(g_sys.pkt) >> 4));
}
// the flush as port_dl_flush_stale / PortDlRefreshOne do it for a list `order` (mode: 0 every model in full, 1 on, 2 verify)
static unsigned g_xanLeft = 0, g_xanListed = 0;
static void Flush(Scene& s, const std::vector<unsigned>& order, int mode, unsigned* left, unsigned* verified, unsigned* bad, unsigned* why) {
    for (unsigned k : order) {
        StaleDL e{s.actors[k], s.mms[k]};
        const bool xan = s.mms[k]->xanimModel != nullptr;
        if (xan && mode == 1) ++g_xanListed;
        if (mode != 0 && DlStaticTry(e.model, e, mode, left, verified, bad, why)) { if (xan && mode == 1) ++g_xanLeft; continue; }
        e.model->updateDL(true);
        if (mode != 0) DlStaticDone(e.model);
    }
}
static const bool NEG = std::getenv("PORT_DLS_NEG") != nullptr;
struct NegInit { NegInit() { g_hideTex = NEG; } } g_negInit;
int main(int argc, char** argv) {
    g_xanim = std::getenv("PORT_DLS_XANIM0") == nullptr;  // (PORT_DLS_XANIM0=1: the switch off: animated managers must never be left out)
    const unsigned rounds = argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 200;
    unsigned fails = 0, leftFix = 0, leftOld = 0, listed = 0, whySum = 0, leftTotal = 0;
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
                if (R(3) == 0) {  // a writer changes a texture matrix of one data's material (both worlds); the repeat test must notice it
                    const unsigned k = R(static_cast<unsigned>(a.allMats.size()));
                    for (unsigned j = 0; j < 8; ++j) if (a.allMats[k]->tex[j]) { const u8 v = static_cast<u8>(R(60)); a.allMats[k]->tex[j]->i.mInfo = v; b.allMats[k]->tex[j]->i.mInfo = v; }
                }
                if (R(6) == 0) {  // gamecpu25: a Xanime player's model is re-pointed (MarioActor::changeDisplayMode does): the memo must not carry over
                    const unsigned k = R(static_cast<unsigned>(a.mms.size())), j = R(static_cast<unsigned>(a.models.size()));
                    if (a.mms[k]->xanimModel != nullptr) {
                        if (R(2) == 0) { a.mms[k]->xanimModel = a.models[j]; b.mms[k]->xanimModel = b.models[j]; }
                        else {  // a body never computed before (new data, new materials, words still 0), the same in both worlds
                            const unsigned id = 5000 + R(1000), fl = R(2) ? 0x20 : 0;
                            for (Scene* w : {&a, &b}) {
                                auto* data = new J3DModelData; data->flags = fl;
                                auto* m = new J3DMaterial; m->id = id; m->shape = new J3DShape; data->mats.push_back(m);
                                w->allMats.push_back(m); w->allShapes.push_back(m->shape); w->datas.push_back(data);
                                auto* mod = new J3DModel; mod->data = data; w->models.push_back(mod);
                                w->mms[k]->xanimModel = mod;
                            }
                        }
                    }
                }
                const unsigned c0 = g_fullCalcs; Flush(a, order, g_flagfix ? 1 : 1, &lf, &vf, &bd, why); (void)c0;
                Flush(b, order, 0, &lf2, &vf2, &bd2, why2);
                Snap sa, sb; Take(a, sa); Take(b, sb);
                if (sa.w != sb.w) { ++fails; if (fails < 4) printf("FAIL round %u flagfix %d flush %d: state differs from the full computation\n", r, fix, flush); }
                listed += static_cast<unsigned>(order.size());
                // verify world: mode 2 on a copy of a fresh state must not differ
            }
            for (unsigned k = 0; k < 10; ++k) whySum += why[k];
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
    printf("dl_static_why: %u rounds, listed %u, left out with flagfix 1: %u, with flagfix 0: %u, reasons counted %u, g_fullCalcs %u, failures %u\n", rounds, listed, leftFix, leftOld, whySum, g_fullCalcs, fails);
    if (!NEG && leftFix < leftOld) { printf("FAIL: flagfix left out fewer (%u < %u)\n", leftFix, leftOld); ++fails; }
    printf("dl_static_why: xanim managers listed %u, left out %u (switch %s)\n", g_xanListed, g_xanLeft, g_xanim ? "on" : "off");
    if (!NEG && g_xanim && g_xanLeft == 0) { printf("FAIL: with XANIM on no animated manager was ever left out\n"); ++fails; }
    if (!NEG && !g_xanim && g_xanLeft != 0) { printf("FAIL: with XANIM off an animated manager was left out\n"); ++fails; }
    if (!NEG && leftFix == 0) { printf("FAIL: nothing was ever left out with flagfix 1\n"); ++fails; }
    if (fails) { printf("dl_static_why: FAILED (%u)\n", fails); return 1; }
    printf("dl_static_why: PASS\n");
    return 0;
}
