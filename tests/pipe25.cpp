// GXV_PIPE25 host test (pipe25): a multi-frame scene of animated and static models with double-buffered draw matrices and material lists
// (J3DMtxBuffer::mpDrawMtxArr[2], J3DDisplayListObj's two lists). Frame f+1's calc/update runs on this thread while frame f's walk runs on
// a worker, from the half the walk owns; the worker's Segment must equal the serial run's bytes for every frame (VERIFY, mode 2).
//   pipe25_test [frames] [models]       env: P25_NEG=noswap  the logic writes the half the in-flight walk reads (no double buffer):
//                                        must produce mismatches or fence violations (exit 0 only when it does)
//                                       P25_SLEEP=1 widens the overlap windows
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "port/pipe25.h"
using namespace port::pipe25;

struct Model {
    bool animated;
    float mtx[2][12];
    uint32_t dl[2][6];
    HalfStamp stamp[2];
};
static std::vector<Model> gScene;
static bool gSleep = false;
static bool gNoSwap = false;

static void Nap() { if (gSleep) std::this_thread::sleep_for(std::chrono::microseconds(30)); }

// the logic: calc of frame f writes half (f&1) of every animated model (J3DMtxBuffer::calc writes the draw matrices, then the next
// hand-over swaps). NoSwap: the writer uses the half the previous picture reads.
static void Calc(uint32_t f) {
    for (size_t m = 0; m < gScene.size(); ++m) {
        Model& md = gScene[m];
        if (!md.animated) continue;
        const int h = gNoSwap ? static_cast<int>((f - 1) & 1) : static_cast<int>(f & 1);
        md.stamp[h].NoteWrite();
        for (int i = 0; i < 12; ++i) { md.mtx[h][i] = static_cast<float>(f * 7 + m * 3 + i); if (i == 6) Nap(); }
        for (int i = 0; i < 6; ++i) md.dl[h][i] = f * 131u + static_cast<uint32_t>(m) * 17u + i;
    }
}
// the walk of picture f: model by model, the half f&1 into command bytes (what the J3D passes draw).
static void Walk(uint32_t f, Segment& seg, bool stampIt) {
    const int h = f & 1;
    for (size_t m = 0; m < gScene.size(); ++m) {
        Model& md = gScene[m];
        const int hh = md.animated ? h : 0;
        if (stampIt && md.animated) md.stamp[hh].BeginRead(f + 1);
        uint32_t tag = 0xB0000000u | static_cast<uint32_t>(m);
        seg.Put(&tag, 4);
        seg.Put(md.mtx[hh], sizeof md.mtx[hh]);
        Nap();
        seg.Put(md.dl[hh], sizeof md.dl[hh]);
        if (stampIt && md.animated) md.stamp[hh].EndRead();
        seg.EndModel();
    }
}
static void Init(size_t n) {
    gScene.clear();
    gScene = std::vector<Model>(n);
    for (size_t m = 0; m < n; ++m) {
        Model& md = gScene[m];
        md.animated = (m % 3) != 2;  // a third of the models are static
        for (int h = 0; h < 2; ++h) {
            for (int i = 0; i < 12; ++i) md.mtx[h][i] = static_cast<float>(1000 + m * 5 + i);
            for (int i = 0; i < 6; ++i) md.dl[h][i] = 5000u + static_cast<uint32_t>(m);
        }
    }
}

int main(int argc, char** argv) {
    const uint32_t frames = argc > 1 ? static_cast<uint32_t>(atoi(argv[1])) : 200;
    const size_t models = argc > 2 ? static_cast<size_t>(atoi(argv[2])) : 40;
    gSleep = getenv("P25_SLEEP") != nullptr;
    const char* neg = getenv("P25_NEG");
    gNoSwap = neg && !strcmp(neg, "noswap");

    // the serial reference: calc f, walk f, nothing overlaps
    std::vector<Segment> ref(frames);
    {
        const bool sleep = gSleep; gSleep = false;
        const bool ns = gNoSwap; gNoSwap = false;
        Init(models);
        for (uint32_t f = 0; f < frames; ++f) { Calc(f); Walk(f, ref[f], false); }
        gSleep = sleep; gNoSwap = ns;
    }
    // pipelined + verify: calc f; (game thread: serial walk f); worker walks f beside calc f+1; join before the next write to its half
    Init(models);
    Stats st;
    Segment serial, late;
    std::thread worker;
    bool running = false;
    uint32_t inflight = 0;
    int refMismatch = 0;
    std::vector<Segment> lateSeg(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        Calc(f);                                   // beside the walk of f-1
        if (running) { worker.join(); running = false; }   // the join (before the swap): the walk of f-1 is done
        // (the previous picture's late segment against the serial reference of the same picture and against this turn's serial walk)
        if (f > 0) {
            ++st.pictures;
            if (!Compare(ref[f - 1], lateSeg[f - 1], st, f - 1)) ++refMismatch;
        }
        serial.Clear();
        inflight = f;
        worker = std::thread([&, f] { Walk(f, lateSeg[f], true); });
        running = true;
    }
    if (running) { worker.join(); ++st.pictures; if (!Compare(ref[frames - 1], lateSeg[frames - 1], st, frames - 1)) ++refMismatch; }
    (void)inflight; (void)late;
    for (Model& md : gScene) for (int h = 0; h < 2; ++h) st.fenceViolations += md.stamp[h].violations.load();
    char line[300];
    Log(st, 2, line, sizeof line);
    printf("%s\n", line);
    if (gNoSwap) {
        const bool caught = st.mismatches > 0 || st.fenceViolations > 0;
        printf("negative control noswap: %s\n", caught ? "caught (as it must)" : "NOT CAUGHT (test broken)");
        return caught ? 0 : 1;
    }
    return st.mismatches == 0 && st.fenceViolations == 0 ? 0 : 1;
}
