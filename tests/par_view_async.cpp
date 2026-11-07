// GXV_PAR_VIEW=2 host proof (tools/test_par_view_async.sh): the asynchronous view pass, finished by a worker while the "draw" reads
// its results, gives every reader the bytes of the serial pass.
//
// Compiled as it stands in the game: the batch (platform/include/port/par_async.h), the worker (platform/include/port/par_spec.h, its
// host branch, and with -D__vita__ its console branch over the kernel stand-ins of tests/par_stubs) and the arithmetic
// (J3DMtxBuffer.cpp's PORT_PAR_CORE block, extracted into par_core.inc). The scene, the walk and the stand-ins of the matrix helpers
// are par13's (tests/par_exec_view.cpp, included below with its own main renamed).
//
// One trial:
//   1. the view pass: par13's random walk (R record: swap now, arithmetic later; C joint computation: touch first; D direct viewCalc:
//      touch, swap, computed at once; V a new view matrix), recorded into the AsyncBatch, exactly as ModelManager.cpp records it
//      (the outputs of a job are the halves its swap makes current);
//   2. the dispatch to the worker, with no join;
//   3. the draw: a random list of
//        S  a shape draw: J3DSys::setModelDrawMtx / setModelNrmMtx (TouchOut of the current halves), then the "immediate loads"
//           (the game thread reads the bytes) and the FIFO (the address goes to a front-end thread that reads the bytes later);
//        M  the mirror: a second viewCalc of a buffer in the draw (Touch, then swap and compute at once; the front end is drained
//           first, as port_gx_view_write_from makes it);
//        J  a joint computation in the draw (Touch, then new joints);
//   4. the retire (every job done, the worker quiesced), then the front end is drained.
// The reference runs the same walk and draw serially. Every byte the game thread and the front end read, and every byte of every
// buffer afterwards, must match. Some jobs are slow (the draw then reaches jobs the worker has not, and waits for one it is in);
// some trials have no worker (the retire computes everything). Coverage: every path must have been taken.
// Negative control: -DPAR_ASYNC_NO_TOUCH_OUT (the shape draw does not touch) must fail.
#define main par_exec_view_main_unused
#include "par_exec_view.cpp"
#undef main

#include <atomic>
#include <thread>

#include "port/par_async.h"
#include "port/par_spec.h"

namespace {

uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast< const uint8_t* >(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// ---- the front end: reads the halves named in the FIFO, after the game thread published them (release / acquire) ----
struct FifoEntry {
    const void* draw;
    const void* nrm;
    uint32_t drawBytes, nrmBytes;
};
constexpr uint32_t kFifo = 1u << 14;
FifoEntry gFifo[kFifo];
std::atomic< uint32_t > gFifoHead{0}, gFifoTail{0};
std::atomic< bool > gFeQuit{false};
std::vector< uint64_t > gFeSums;  // (the front end's only)
void FeLoop() {
    for (;;) {
        const uint32_t head = gFifoHead.load(std::memory_order_acquire);
        uint32_t tail = gFifoTail.load(std::memory_order_relaxed);
        if (tail == head) {
            if (gFeQuit.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
            continue;
        }
        while (tail != head) {
            const FifoEntry& e = gFifo[tail % kFifo];
            gFeSums.push_back(Fnv(e.nrm, e.nrmBytes, Fnv(e.draw, e.drawBytes)));
            ++tail;
        }
        gFifoTail.store(tail, std::memory_order_release);
    }
}
void FifoPush(const FifoEntry& e) {
    const uint32_t head = gFifoHead.load(std::memory_order_relaxed);
    while (head - gFifoTail.load(std::memory_order_acquire) >= kFifo) std::this_thread::yield();
    gFifo[head % kFifo] = e;
    gFifoHead.store(head + 1, std::memory_order_release);
}
void FifoDrain() {  // (the draw-done wait)
    while (gFifoTail.load(std::memory_order_acquire) != gFifoHead.load(std::memory_order_relaxed)) std::this_thread::yield();
}

enum DrawOp { kShape, kMirror, kJoints };
struct DrawStep {
    DrawOp op;
    int target;  // a model (kShape, kMirror) or a buffer (kJoints)
    uint32_t seed;
};
void MakeDraw(const Scene& s, std::vector< DrawStep >& d, int n) {
    d.clear();
    for (int i = 0; i < n; i++) {
        const uint32_t k = Rnd() % 40;
        DrawStep st;
        st.seed = Rnd();
        if (k < 37) {
            st.op = kShape;
            st.target = Rnd() % s.models.size();
        } else if (k < 39) {
            st.op = kMirror;
            st.target = Rnd() % s.models.size();
        } else {
            st.op = kJoints;
            st.target = Rnd() % s.bufs.size();
        }
        d.push_back(st);
    }
}

// What a shape draw reads: the bytes the game thread sees (immediate loads), and the FIFO entry for the front end.
void ShapeRead(Buf& B, std::vector< uint64_t >& seen) {
    const size_t d = B.tree.drawNum;
    seen.push_back(Fnv(B.nrm[1], d * sizeof(Mtx33), Fnv(B.draw[1], d * sizeof(Mtx))));
    FifoPush(FifoEntry{B.draw[1], B.nrm[1], static_cast< uint32_t >(d * sizeof(Mtx)), static_cast< uint32_t >(d * sizeof(Mtx33))});
}
void Direct(Scene& s, const Model& m) {  // a viewCalc computed at once (the mirror's second view pass)
    Buf& B = s.bufs[m.buf];
    Swap(B);
    Job j;
    Capture(j, s, m);
    RunJob(j);
    B.serial += m.bboard ? 1 : 0;
}

void DrawSerial(Scene& s, const std::vector< DrawStep >& d, std::vector< uint64_t >& seen) {
    for (const DrawStep& st : d) {
        if (st.op == kShape) {
            ShapeRead(s.bufs[s.models[st.target].buf], seen);
        } else if (st.op == kMirror) {
            FifoDrain();
            Direct(s, s.models[st.target]);
        } else {
            ApplyCalc(s.bufs[st.target], st.seed);
        }
    }
    FifoDrain();
}

port::par::AsyncBatch< Job > gAsync;
port::par::SpecWorker& gWorker = *new port::par::SpecWorker;  // (never destroyed: its worker waits on it until the process ends)
struct AsyncTotals {
    uint64_t jobs = 0, inlineRuns = 0, overflow = 0, dispatched = 0, byToucher = 0, byRetire = 0, waits = 0, noWorker = 0;
} gAT;

void WalkAsync(Scene& s, const std::vector< Step >& w, uint32_t slowEvery, uint32_t slowUs) {
    gAsync.Begin();
    uint32_t nrec = 0;
    for (const Step& st : w) {
        if (st.op == kRecord) {
            const Model& m = s.models[st.target];
            Buf& B = s.bufs[m.buf];
            Job* j = gAsync.Add(&B, B.draw[0], B.nrm[0]);  // (the halves the swap below makes current)
            Swap(B);
            if (j == nullptr) {
                Job now;
                Capture(now, s, m);
                RunJob(now);
            } else {
                Capture(*j, s, m);
                j->delayUs = slowEvery && (++nrec % slowEvery) == 0 ? slowUs : 0;
            }
            B.serial += m.bboard ? 1 : 0;
        } else if (st.op == kCalc) {
            gAsync.Touch(&s.bufs[st.target]);
            ApplyCalc(s.bufs[st.target], st.seed);
        } else if (st.op == kDirect) {
            const Model& m = s.models[st.target];
            gAsync.Touch(&s.bufs[m.buf]);
            Direct(s, m);
        } else {
            ApplyView(s, st.seed);
        }
    }
}
void DrawAsync(Scene& s, const std::vector< DrawStep >& d, std::vector< uint64_t >& seen, bool worker) {
    if (gAsync.Dispatch() != 0) {
        if (worker) {
            gWorker.Dispatch(&port::par::AsyncBatch< Job >::Item, &gAsync, gAsync.Items(), gAsync.Count());
        } else {
            ++gAT.noWorker;
        }
    }
    for (const DrawStep& st : d) {
        if (st.op == kShape) {
            Buf& B = s.bufs[s.models[st.target].buf];
#ifndef PAR_ASYNC_NO_TOUCH_OUT
            gAsync.TouchOut(B.draw[1]);  // (J3DSys::setModelDrawMtx)
            gAsync.TouchOut(B.nrm[1]);   // (J3DSys::setModelNrmMtx)
#endif
            ShapeRead(B, seen);
        } else if (st.op == kMirror) {
            const Model& m = s.models[st.target];
            gAsync.Touch(&s.bufs[m.buf]);  // (J3DModel::viewCalc)
            FifoDrain();
            Direct(s, m);
        } else {
            gAsync.Touch(&s.bufs[st.target]);  // (J3DModel::calc)
            ApplyCalc(s.bufs[st.target], st.seed);
        }
    }
    gAsync.Retire();
    gWorker.Quiesce();
    FifoDrain();
    const auto& c = gAsync.Counters();
    gAT.jobs += c.jobs;
    gAT.inlineRuns += c.inlineRuns;
    gAT.overflow += c.overflow;
    gAT.dispatched += c.dispatched;
    gAT.byToucher += c.byToucher;
    gAT.byRetire += c.byRetire;
    gAT.waits += c.waits;
}

}  // namespace

int main(int argc, char** argv) {
    const int trials = argc > 1 ? atoi(argv[1]) : 300;
    if (!gAsync.Init(256, &RunJob) || !gWorker.Start(3, 0, "smg_view_async")) {
        printf("init failed\n");
        return 1;
    }
    std::thread fe(FeLoop);
    long mismatches = 0, cases = 0;
    for (int t = 0; t < trials; t++) {
        gRng = 5000 + t;
        Scene base;
        MakeScene(base, 8 + Rnd() % 120);
        std::vector< Step > walk;
        MakeWalk(base, walk, 20 + Rnd() % 400);  // (more records than the batch's 256 sometimes: jobs computed at once)
        std::vector< DrawStep > draw;
        MakeDraw(base, draw, 50 + Rnd() % 600);
        // variants: worker at full speed; slow jobs (the draw overtakes the worker); slow and few (waits); no worker at all
        const uint32_t slowEvery[4] = {0, 3, 1, 0};
        const uint32_t slowUs[4] = {0, 150, 30, 0};
        for (int k = 0; k < 4; k++) {
            Scene a, b;
            CopyScene(base, a);
            CopyScene(base, b);
            std::vector< uint64_t > seenA, seenB;
            gFeSums.clear();
            WalkSerial(a, walk);
            DrawSerial(a, draw, seenA);
            const std::vector< uint64_t > feA = gFeSums;  // (the front end is drained: no race on the vector)
            gFeSums.clear();
            WalkAsync(b, walk, slowEvery[k], slowUs[k]);
            DrawAsync(b, draw, seenB, k != 3);
            const std::vector< uint64_t > feB = gFeSums;
            cases++;
            if (!SameScene(a, b) || seenA != seenB || feA != feB) {
                mismatches++;
                if (mismatches < 5) {
                    printf("MISMATCH: trial %d variant %d (scene %d, game reads %d, front end reads %d)\n", t, k, (int)!SameScene(a, b),
                           (int)(seenA != seenB), (int)(feA != feB));
                }
            }
        }
    }
    gFeQuit.store(true, std::memory_order_release);
    fe.join();
    uint32_t busyUs = 0, workerJobs = 0, sleeps = 0;
    gWorker.TakeStats(&busyUs, &workerJobs, &sleeps);
    printf("async view passes: %ld cases, %ld mismatches; jobs %llu (run in the walk %llu, computed at once %llu, handed out %llu): worker %u, "
           "taken by touches %llu, at the retire %llu; touches that waited for the worker %llu; batches with no worker %llu\n",
           cases, mismatches, (unsigned long long)gAT.jobs, (unsigned long long)gAT.inlineRuns, (unsigned long long)gAT.overflow,
           (unsigned long long)gAT.dispatched, workerJobs, (unsigned long long)gAT.byToucher, (unsigned long long)gAT.byRetire,
           (unsigned long long)gAT.waits, (unsigned long long)gAT.noWorker);
#ifndef PAR_ASYNC_NO_TOUCH_OUT
    if (workerJobs == 0 || gAT.byToucher == 0 || gAT.byRetire == 0 || gAT.waits == 0 || gAT.inlineRuns == 0 || gAT.overflow == 0 ||
        gAT.noWorker == 0) {
        printf("coverage: a path was not exercised\n");
        return 1;
    }
#endif
    return mismatches != 0;
}
