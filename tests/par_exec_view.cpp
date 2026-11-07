// GXV_PAR_VIEW host proof (tools/test_par_exec_view.sh): the parallel view pass gives the bytes of the serial one.
//
// What is compiled here as it stands in the game:
//   - the executor (platform/include/port/par_exec.h; its host branch, and with -DPAR_TEST_VITA its console branch over the
//     kernel stand-ins of tests/par_stubs);
//   - the pending-job list (platform/include/port/par_batch.h);
//   - the arithmetic (J3DMtxBuffer::portCalcDrawMtxCore / portCalcNrmMtxCore / portCalcBBoardMtxCore), extracted from
//     decomp/src/JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp between its PORT_PAR_CORE markers into par_core.inc.
// The matrix helpers they call (concat, inverse transpose, billboards) are stand-ins: both sides call the same ones.
//
// Synthetic scenes: models with random joint trees (full-weight and envelope draw matrices, billboard joints, the three calc modes),
// some sharing one matrix buffer (an LOD model and its high model), double-buffered draw / normal matrices. A random walk mixes:
//   R  a model's view pass (recorded: the swap now, the arithmetic later),
//   C  a joint computation of a buffer (J3DModel::calc: touch, then new joint matrices and scale flags),
//   D  a direct viewCalc (ModelHolder::updateMtx: touch, swap, computed at once),
//   V  a change of the view matrix (Mario's search light writes j3dSys's).
// The reference runs the same walk with every view pass computed at once. Every byte of every buffer (both halves of draw and normal,
// joints, scale flags) and the buffer pointers must match, for the shared and the split work sharing, with tiny batches (the
// worker cancelled), full batches (jobs computed at once), slow items (the join sleeps) and a small spin.
// Also: an executor stress (every item exactly once, no item of one batch run in another).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "port/par_batch.h"
#include "port/par_exec.h"

typedef float f32;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef f32 Mtx[3][4];
typedef f32 Mtx33[3][3];
typedef f32 (*MtxPtr)[4];
struct Vec {
    f32 x, y, z;
};

// ---- stand-ins of J3D's types and helpers (the same on both sides of the comparison) ----
struct J3DJoint {
    u8 type;
    u8 getMtxType() const { return type; }
};
struct J3DJointTree {
    u16 drawNum = 0, fullWgt = 0, wevlpNum = 0, jointNum = 0;
    std::vector< u8 > drawFlag;
    std::vector< u16 > drawIdx;
    std::vector< J3DJoint > joints;
    u16 getDrawMtxNum() const { return drawNum; }
    u16 getDrawFullWgtMtxNum() const { return fullWgt; }
    u16 getWEvlpMtxNum() const { return wevlpNum; }
    u8 getDrawMtxFlag(u16 i) const { return drawFlag[i]; }
    u16 getDrawMtxIndex(u16 i) const { return drawIdx[i]; }
    J3DJoint* getJointNodePointer(u16 i) const { return const_cast< J3DJoint* >(&joints[i]); }
};
static void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx t;
    for (int r = 0; r < 3; r++) {
        t[r][0] = a[r][0] * b[0][0] + a[r][1] * b[1][0] + a[r][2] * b[2][0];
        t[r][1] = a[r][0] * b[0][1] + a[r][1] * b[1][1] + a[r][2] * b[2][1];
        t[r][2] = a[r][0] * b[0][2] + a[r][1] * b[1][2] + a[r][2] * b[2][2];
        t[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
    }
    memcpy(ab, t, sizeof(Mtx));
}
static void PSMTXCopy(const Mtx src, Mtx dst) {
    if (src != (const f32(*)[4])dst) memmove(dst, src, sizeof(Mtx));
}
static void J3DPSMtxArrayConcat(Mtx mA, Mtx mB, Mtx mAB, u32 count) {
    Mtx* pB = reinterpret_cast< Mtx* >(mB);
    Mtx* pAB = reinterpret_cast< Mtx* >(mAB);
    for (u32 i = 0; i < count; i++) PSMTXConcat(mA, pB[i], pAB[i]);
}
static void J3DCalcViewBaseMtx(Mtx view, Vec const& scale, const Mtx& base, Mtx dst) {
    Mtx m;
    for (int r = 0; r < 3; r++) {
        m[r][0] = base[r][0] * scale.x;
        m[r][1] = base[r][1] * scale.y;
        m[r][2] = base[r][2] * scale.z;
        m[r][3] = base[r][3];
    }
    PSMTXConcat(view, m, dst);
}
static void J3DPSCalcInverseTranspose(Mtx src, Mtx33 dst) {
    f32 c00 = src[1][1] * src[2][2] - src[2][1] * src[1][2], c01 = src[1][2] * src[2][0] - src[2][2] * src[1][0];
    f32 c02 = src[1][0] * src[2][1] - src[1][1] * src[2][0], c10 = src[2][1] * src[0][2] - src[0][1] * src[2][2];
    f32 c11 = src[2][2] * src[0][0] - src[0][2] * src[2][0], c12 = src[0][1] * src[2][0] - src[0][0] * src[2][1];
    f32 c20 = src[0][1] * src[1][2] - src[1][1] * src[0][2], c21 = src[0][2] * src[1][0] - src[1][2] * src[0][0];
    f32 c22 = src[0][0] * src[1][1] - src[0][1] * src[1][0];
    f32 det = src[0][0] * c00 + src[1][0] * c10 + src[2][0] * c20;
    if (det == 0.0f) return;
    f32 inv = 1.0f / det;
    dst[0][0] = c00 * inv; dst[0][1] = c01 * inv; dst[0][2] = c02 * inv;
    dst[1][0] = c10 * inv; dst[1][1] = c11 * inv; dst[1][2] = c12 * inv;
    dst[2][0] = c20 * inv; dst[2][1] = c21 * inv; dst[2][2] = c22 * inv;
}
static void J3DCalcBBoardMtx(Mtx m) {
    f32 x = m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0];
    f32 y = m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1];
    f32 z = m[0][2] * m[0][2] + m[1][2] * m[1][2] + m[2][2] * m[2][2];
    if (x > 0.0f) x = std::sqrt(x);
    if (y > 0.0f) y = std::sqrt(y);
    if (z > 0.0f) z = std::sqrt(z);
    m[0][0] = x; m[0][1] = 0.0f; m[0][2] = 0.0f; m[2][0] = 0.0f; m[2][1] = 0.0f; m[1][0] = 0.0f; m[1][1] = y; m[1][2] = 0.0f; m[2][2] = z;
}
static void J3DCalcYBBoardMtx(Mtx m) {
    f32 x = m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0];
    f32 z = m[0][2] * m[0][2] + m[1][2] * m[1][2] + m[2][2] * m[2][2];
    if (x > 0.0f) x = 1.0f / (1.0f / std::sqrt(x));
    if (z > 0.0f) z = 1.0f / (1.0f / std::sqrt(z));
    f32 vy = -m[2][1], vz = m[1][1];
    f32 l = std::sqrt(vy * vy + vz * vz);
    if (l > 0.0f) { vy /= l; vz /= l; }
    m[0][0] = x; m[0][2] = 0.0f; m[1][0] = 0.0f; m[1][2] = vy * z; m[2][0] = 0.0f; m[2][2] = vz * z;
}
struct J3DMtxBuffer {
    static void portCalcDrawMtxCore(J3DJointTree* tree, u32 mdlFlag, Vec const& scale, Mtx const& base, MtxPtr view, Mtx* anm, Mtx* wevlp,
                                    Mtx* draw);
    static void portCalcNrmMtxCore(J3DJointTree* tree, const u8* scaleFlag, const u8* envScaleFlag, Mtx* draw, Mtx33* nrm);
    static void portCalcBBoardMtxCore(J3DJointTree* tree, Mtx* draw, Mtx33* nrm);
    static void portSetNrmMtx(Mtx33& dst, const Mtx mtx) {
        for (int r = 0; r < 3; r++) {
            dst[r][0] = mtx[r][0];
            dst[r][1] = mtx[r][1];
            dst[r][2] = mtx[r][2];
        }
    }
};
// ---- the production arithmetic (J3DMtxBuffer.cpp, PORT_PAR_CORE_BEGIN .. PORT_PAR_CORE_END) ----
#include "par_core.inc"

// ---- a synthetic scene ----
static uint32_t gRng = 1;
static uint32_t Rnd() { gRng = gRng * 1664525u + 1013904223u; return gRng >> 1; }
static f32 Rf() {
    uint32_t k = Rnd() % 12;
    if (k == 0) return 0.0f;
    if (k == 1) return -0.0f;
    if (k == 2) return 1.0f;
    return ((int32_t)Rnd() / 1073741824.0f) * (float)(1 << (Rnd() % 8));
}
static void RandMtx(Mtx m) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) m[r][c] = Rf();
}
struct M34 {
    Mtx m;
};
struct M33 {
    Mtx33 m;
};
template < class T >
static Mtx* P34(std::vector< T >& v) { return reinterpret_cast< Mtx* >(v.data()); }
template < class T >
static const Mtx* P34(const std::vector< T >& v) { return reinterpret_cast< const Mtx* >(v.data()); }
template < class T >
static Mtx33* P33(std::vector< T >& v) { return reinterpret_cast< Mtx33* >(v.data()); }
template < class T >
static const Mtx33* P33(const std::vector< T >& v) { return reinterpret_cast< const Mtx33* >(v.data()); }
struct Buf {  // a J3DMtxBuffer's arrays
    J3DJointTree tree;
    std::vector< M34 > anm, wevlp, drawA, drawB;
    std::vector< M33 > nrmA, nrmB;
    std::vector< u8 > sf, esf;
    Mtx* draw[2];  // [0] the other half, [1] the current (as mpDrawMtxArr[0/1][view])
    Mtx33* nrm[2];
    u32 serial = 0;
};
struct Model {
    int buf;
    u32 mode, bboard;
    Vec scale;
    Mtx base;
};
struct Scene {
    std::vector< Buf > bufs;
    std::vector< Model > models;
    Mtx view;
};
static void MakeScene(Scene& s, int nbuf) {
    s.bufs.resize(nbuf);
    for (int b = 0; b < nbuf; b++) {
        Buf& B = s.bufs[b];
        J3DJointTree& t = B.tree;
        t.jointNum = 1 + Rnd() % 40;
        t.drawNum = 1 + Rnd() % 48;
        t.fullWgt = Rnd() % 3 == 0 ? t.drawNum : Rnd() % (t.drawNum + 1);
        t.wevlpNum = t.drawNum - t.fullWgt;
        t.drawFlag.resize(t.drawNum);
        t.drawIdx.resize(t.drawNum);
        for (u16 i = 0; i < t.drawNum; i++) {
            t.drawFlag[i] = i >= t.fullWgt ? 1 : 0;
            t.drawIdx[i] = t.drawFlag[i] ? (u16)(Rnd() % (t.wevlpNum ? t.wevlpNum : 1)) : (u16)(Rnd() % t.jointNum);
        }
        t.joints.resize(t.jointNum);
        for (auto& j : t.joints) j.type = Rnd() % 6 == 0 ? (u8)(1 + Rnd() % 2) : 0;
        B.anm.resize(t.jointNum);
        B.sf.resize(t.jointNum);
        B.wevlp.resize(t.wevlpNum ? t.wevlpNum : 1);
        B.esf.resize(B.wevlp.size());
        for (auto& m : B.anm) RandMtx(m.m);
        for (auto& m : B.wevlp) RandMtx(m.m);
        for (auto& f : B.sf) f = Rnd() & 1;
        for (auto& f : B.esf) f = Rnd() & 1;
        B.drawA.resize(t.drawNum);
        B.drawB.resize(t.drawNum);
        B.nrmA.resize(t.drawNum);
        B.nrmB.resize(t.drawNum);
        for (auto& m : B.drawA) RandMtx(m.m);
        for (auto& m : B.drawB) RandMtx(m.m);
        for (auto& m : B.nrmA) for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m.m[r][c] = Rf();
        for (auto& m : B.nrmB) for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m.m[r][c] = Rf();
        B.draw[0] = P34(B.drawA);
        B.draw[1] = P34(B.drawB);
        B.nrm[0] = P33(B.nrmA);
        B.nrm[1] = P33(B.nrmB);
    }
    // one or two models a buffer (two: an LOD model sharing its high model's buffer)
    s.models.clear();
    for (int b = 0; b < nbuf; b++) {
        const int n = Rnd() % 5 == 0 ? 2 : 1;
        for (int k = 0; k < n; k++) {
            Model m;
            m.buf = b;
            m.mode = Rnd() % 2 ? 2 : 0;  // (mode 1 is never recorded: ModelManager.cpp)
            m.bboard = Rnd() % 4 == 0;
            m.scale = Vec{Rf(), Rf(), Rf()};
            RandMtx(m.base);
            s.models.push_back(m);
        }
    }
    RandMtx(s.view);
}
// A copy of a scene whose pointers point into the copy.
static void CopyScene(const Scene& from, Scene& to) {
    to = from;
    for (size_t b = 0; b < from.bufs.size(); b++) {
        const Buf& F = from.bufs[b];
        Buf& T = to.bufs[b];
        for (int h = 0; h < 2; h++) {
            T.draw[h] = F.draw[h] == P34(F.drawA) ? P34(T.drawA) : P34(T.drawB);
            T.nrm[h] = F.nrm[h] == P33(F.nrmA) ? P33(T.nrmA) : P33(T.nrmB);
        }
    }
}
static bool SameScene(const Scene& a, const Scene& b) {
    for (size_t i = 0; i < a.bufs.size(); i++) {
        const Buf& A = a.bufs[i];
        const Buf& B = b.bufs[i];
        const size_t d = A.tree.drawNum;
        if (memcmp(A.drawA.data(), B.drawA.data(), d * sizeof(Mtx)) || memcmp(A.drawB.data(), B.drawB.data(), d * sizeof(Mtx)) ||
            memcmp(A.nrmA.data(), B.nrmA.data(), d * sizeof(Mtx33)) || memcmp(A.nrmB.data(), B.nrmB.data(), d * sizeof(Mtx33)) ||
            memcmp(A.anm.data(), B.anm.data(), A.anm.size() * sizeof(Mtx)) || A.sf != B.sf || A.serial != B.serial ||
            (A.draw[1] == P34(A.drawA)) != (B.draw[1] == P34(B.drawA)) || (A.nrm[1] == P33(A.nrmA)) != (B.nrm[1] == P33(B.nrmA))) {
            return false;
        }
    }
    return true;
}

// ---- the view pass job, as ModelManager.cpp records it ----
struct Job {
    J3DJointTree* tree;
    Mtx* anm;
    Mtx* wevlp;
    const u8* sf;
    const u8* esf;
    Mtx* draw;
    Mtx33* nrm;
    u32 mode, bboard;
    Vec scale;
    Mtx base;
    Mtx view;
    uint32_t delayUs;  // (a slow item, so that a join outlasts its spin)
};
static void RunJob(Job& j) {
    J3DMtxBuffer::portCalcDrawMtxCore(j.tree, j.mode, j.scale, j.base, j.view, j.anm, j.wevlp, j.draw);
    J3DMtxBuffer::portCalcNrmMtxCore(j.tree, j.sf, j.esf, j.draw, j.nrm);
    if (j.bboard) J3DMtxBuffer::portCalcBBoardMtxCore(j.tree, j.draw, j.nrm);
    if (j.delayUs) {
        const uint64_t t0 = port::par::NowUs();
        while (port::par::NowUs() - t0 < j.delayUs) {
        }
    }
}
static void Capture(Job& j, Scene& s, const Model& m) {
    Buf& B = s.bufs[m.buf];
    j.tree = &B.tree;
    j.anm = P34(B.anm);
    j.wevlp = P34(B.wevlp);
    j.sf = B.sf.data();
    j.esf = B.esf.data();
    j.draw = B.draw[1];
    j.nrm = B.nrm[1];
    j.mode = m.mode;
    j.bboard = m.bboard;
    j.scale = m.scale;
    memcpy(j.base, m.base, sizeof(Mtx));
    memcpy(j.view, s.view, sizeof(Mtx));
    j.delayUs = 0;
}
static void Swap(Buf& B) {
    std::swap(B.draw[0], B.draw[1]);
    std::swap(B.nrm[0], B.nrm[1]);
    B.serial += 2;
}
enum Op { kRecord, kCalc, kDirect, kView };
struct Step {
    Op op;
    int target;
    uint32_t seed;
};
static void MakeWalk(const Scene& s, std::vector< Step >& w, int n) {
    w.clear();
    for (int i = 0; i < n; i++) {
        const uint32_t k = Rnd() % 20;
        Step st;
        st.seed = Rnd();
        if (k < 15) {
            st.op = kRecord;
            st.target = Rnd() % s.models.size();
        } else if (k < 17) {
            st.op = kCalc;
            st.target = Rnd() % s.bufs.size();
        } else if (k < 19) {
            st.op = kDirect;
            st.target = Rnd() % s.models.size();
        } else {
            st.op = kView;
            st.target = 0;
        }
        w.push_back(st);
    }
}
static void ApplyCalc(Buf& B, uint32_t seed) {  // J3DModel::calc: new joints and scale flags
    const uint32_t keep = gRng;
    gRng = seed;
    for (auto& m : B.anm) RandMtx(m.m);
    for (auto& f : B.sf) f = Rnd() & 1;
    gRng = keep;
}
static void ApplyView(Scene& s, uint32_t seed) {
    const uint32_t keep = gRng;
    gRng = seed;
    RandMtx(s.view);
    gRng = keep;
}
// The serial pass: every view pass computed at its place.
static void WalkSerial(Scene& s, const std::vector< Step >& w) {
    for (const Step& st : w) {
        if (st.op == kRecord || st.op == kDirect) {
            const Model& m = s.models[st.target];
            Buf& B = s.bufs[m.buf];
            Swap(B);
            Job j;
            Capture(j, s, m);
            RunJob(j);
            B.serial += m.bboard ? 1 : 0;
        } else if (st.op == kCalc) {
            ApplyCalc(s.bufs[st.target], st.seed);
        } else {
            ApplyView(s, st.seed);
        }
    }
}
// The parallel pass: records, touches, then the batch on both threads.
static port::par::PendingBatch< Job > gBatch;
static port::par::Executor& gExec = *new port::par::Executor;  // (never destroyed: its worker waits on it until the process ends)
struct RunTotals {
    uint64_t batches = 0, parallel = 0, cancelled = 0, slept = 0, byWorker = 0, byGame = 0, inlineRuns = 0, overflow = 0;
} gTotals;
static void WalkParallel(Scene& s, const std::vector< Step >& w, int split, uint32_t slowEvery) {
    gBatch.Begin();
    uint32_t nrec = 0;
    for (const Step& st : w) {
        if (st.op == kRecord) {
            const Model& m = s.models[st.target];
            Buf& B = s.bufs[m.buf];
            Job* j = gBatch.Add(&B);
            Swap(B);
            if (j == nullptr) {  // (full: computed at once)
                Job now;
                Capture(now, s, m);
                RunJob(now);
            } else {
                Capture(*j, s, m);
                j->delayUs = slowEvery && (++nrec % slowEvery) == 0 ? 200 : 0;
            }
            B.serial += m.bboard ? 1 : 0;
        } else if (st.op == kCalc) {
#ifndef PAR_TEST_NO_TOUCH  // (the negative control: without the touch, a pending job reads the new joints and the test must fail)
            gBatch.Touch(&s.bufs[st.target]);
#endif
            ApplyCalc(s.bufs[st.target], st.seed);
        } else if (st.op == kDirect) {
            const Model& m = s.models[st.target];
            Buf& B = s.bufs[m.buf];
            gBatch.Touch(&B);
            Swap(B);
            Job j;
            Capture(j, s, m);
            RunJob(j);
            B.serial += m.bboard ? 1 : 0;
        } else {
            ApplyView(s, st.seed);  // (nothing to do: a recorded job holds the view matrix by value)
        }
    }
    gBatch.Close();
    const port::par::BatchStats st = gExec.Run(&port::par::PendingBatch< Job >::Item, &gBatch, gBatch.Count(), true, split);
    gTotals.batches++;
    gTotals.parallel += st.parallel;
    gTotals.cancelled += st.cancelled;
    gTotals.slept += st.slept;
    gTotals.byWorker += st.byWorker;
    gTotals.byGame += st.byGame;
    gTotals.inlineRuns += gBatch.InlineRuns();
    gTotals.overflow += gBatch.Overflow();
}

// ---- executor stress: every item exactly once per batch ----
struct Stress {
    std::vector< std::atomic< uint32_t > >* hits;
    uint32_t batch;
    std::vector< uint32_t >* owner;
};
static void StressItem(void* ctx, uint32_t i) {
    Stress* s = static_cast< Stress* >(ctx);
    (*s->hits)[i].fetch_add(1, std::memory_order_relaxed);
    (*s->owner)[i] = s->batch;  // (plain write: a race here is what -fsanitize=thread would catch)
    if ((i * 2654435761u) % 97 == 0) {
        volatile uint32_t x = 0;
        for (int k = 0; k < 2000; k++) x += k;
    }
}

int main(int argc, char** argv) {
    const int trials = argc > 1 ? atoi(argv[1]) : 400;
    if (!gBatch.Init(256, &RunJob)) {
        printf("init failed\n");
        return 1;
    }
    if (!gExec.Start(1, 0, 20)) {
        printf("worker failed\n");
        return 1;
    }
    // 1. executor stress
    {
        std::vector< std::atomic< uint32_t > > hits(5000);
        std::vector< uint32_t > owner(5000);
        long bad = 0;
        for (uint32_t b = 1; b <= 3000; b++) {
            const uint32_t n = (b % 7 == 0) ? 1 + Rnd() % 3 : 1 + Rnd() % 5000;
            for (uint32_t i = 0; i < n; i++) hits[i].store(0, std::memory_order_relaxed);
            Stress s{&hits, b, &owner};
            gExec.Run(&StressItem, &s, n, true, (int)(b % 3 == 0 ? Rnd() % 100 : 0));
            for (uint32_t i = 0; i < n; i++) {
                if (hits[i].load(std::memory_order_relaxed) != 1 || owner[i] != b) bad++;
            }
        }
        printf("executor: 3000 batches, %ld items run other than exactly once\n", bad);
        if (bad) return 1;
    }
    // 2. serial vs parallel view passes
    long mismatches = 0, cases = 0;
    for (int t = 0; t < trials; t++) {
        gRng = 1000 + t;
        Scene base;
        MakeScene(base, 8 + Rnd() % 120);
        std::vector< Step > walk;
        MakeWalk(base, walk, 20 + Rnd() % 400);  // (more records than the batch's 256 sometimes: jobs computed at once)
        const int splits[3] = {0, 50, (int)(1 + Rnd() % 99)};
        for (int k = 0; k < 3; k++) {
            Scene a, b;
            CopyScene(base, a);
            CopyScene(base, b);
            WalkSerial(a, walk);
            WalkParallel(b, walk, splits[k], (t % 5 == 0) ? 7 : 0);
            cases++;
            if (!SameScene(a, b)) {
                mismatches++;
                if (mismatches < 5) printf("MISMATCH: trial %d split %d\n", t, splits[k]);
            }
        }
    }
    printf("view passes: %ld cases, %ld mismatches; batches %llu (%llu with the worker, %llu cancelled, %llu joins slept); jobs: game %llu, "
           "worker %llu, run in the walk %llu, computed at once %llu\n",
           cases, mismatches, (unsigned long long)gTotals.batches, (unsigned long long)gTotals.parallel, (unsigned long long)gTotals.cancelled,
           (unsigned long long)gTotals.slept, (unsigned long long)gTotals.byGame, (unsigned long long)gTotals.byWorker,
           (unsigned long long)gTotals.inlineRuns, (unsigned long long)gTotals.overflow);
    if (gTotals.byWorker == 0 || gTotals.inlineRuns == 0 || gTotals.overflow == 0) {
        printf("coverage: a path was not exercised\n");
        return 1;
    }
    return mismatches != 0;
}
