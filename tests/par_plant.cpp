// GXV_PAR_PLANT host proof (tools/test_par_plant.sh; revisit23 round 2): the protocol of decomp/src/Game/LiveActor/ModelManager.cpp's plant
// jobs with the production worker (platform/include/port/par_spec.h SpecWorker) and phase byte (platform/include/port/par_plant.h), under
// ThreadSanitizer and AddressSanitizer.
//   Each picture the game thread lists every "plant" (a job per plant, phase kPlFree, item pending) and dispatches the batch; the WORKER
//   makes each job from the plant's LIVE state (key + inputs, as ParBuild does on it) and then walks it (as xspec::Run: a deterministic
//   function of the inputs only), from the end of the list. The game thread waits a random "front end" time, then goes through the list
//   from the start as port_anim_flush_stale does: port_par_plant_own (PlantGameOwn) BEFORE it writes the plant (the latch, the player's
//   state: here a scratch write and a restore around the real computation, as calcAnm / clearAnm do), then the real key, then the take
//   (claim; the job is used only when built, done and its key equal word for word: ParTakeJob), else the original computation.
//   Checked every picture: every result used is byte for byte the original computation's (the test computes it too); the batch is over
//   (Quiesce) before the next turn writes the plants; ThreadSanitizer sees no race between the worker's reads and the game thread's writes.
//   Negative control (argument "neg"): the game thread does NOT call PlantGameOwn: ThreadSanitizer must report the race (exit status 66).
//   Usage: par_plant [pictures] [plants] [seed] [neg]
// parplant23 (8 Nov): "turns <early mode> <gap us>" runs the game's turn order instead (GameSystem::frameLoop): drawn and skipped turns in
//   a 30-pictures pattern with irregular stretches (D S D S ..., D D, S S), each drawn turn: the flush batch (listed now unless the end of
//   the last turn listed it), the flush from the plants on (they are the flush list's first entries on the console), the end; each turn:
//   the early batch ended unused before update() if one is open (port_par_plant_close_early), update() writing every plant, and at the
//   turn's end port/par_plant.h's PlantTurns prediction and, if it says the next turn draws, the early listing; then the game thread is busy
//   with other work for <gap us> (the retrace wait and the next turn's start). Early mode 0 is the 2.3 round 2 place (listed at the drawn
//   turn's start with no head start: the console's 472-verify case), 1 the prediction, 2 every turn. Every hit is compared with the original
//   walk (what GXV_PAR_PLANT=2 does on the console). "turns-neg" leaves the early batch open into update(): ThreadSanitizer must report the race.
#include "port/par_plant.h"
#include "port/par_spec.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr int kJoints = 12;
struct Live {  // what ParBuild reads of a plant (its latch, player, core, base matrix, scale)
    float frame;
    float rate;
    float base[12];
    float scale;
    int latch;
    float scratch[8];  // (written and restored by the game thread's own computation of the plant: calcAnm's state)
};
struct Input {
    float frame, base[12], scale;
    int latch;
};
struct Job {
    uint32_t key[16];
    uint32_t keyWords;
    Input in;
    float out[kJoints][12];
    uint8_t built, ok;
    uint32_t item;
};
std::vector< Live > gLive;
std::vector< Job > gJobs;
std::atomic< uint8_t >* gItems = nullptr;
std::atomic< uint8_t >* gPhase = nullptr;
uint32_t gCount = 0;
bool gNeg = false;

uint32_t Bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
// The key of the computation from a state (MemoKey's words: frame under the latch, base matrix, scale).
uint32_t KeyOf(const Live& l, uint32_t* key) {
    uint32_t w = 0;
    key[w++] = Bits(l.latch ? l.frame : l.frame + l.rate);
    for (int i = 0; i < 12; ++i) key[w++] = Bits(l.base[i]);
    key[w++] = Bits(l.scale);
    return w;
}
void InputOf(const Live& l, Input& in) {
    in.frame = l.latch ? l.frame : l.frame + l.rate;
    memcpy(in.base, l.base, sizeof(in.base));
    in.scale = l.scale;
    in.latch = l.latch;
}
// The walk: a deterministic function of the inputs (a chain of rotations about the frame, as the joints' matrices).
void Walk(const Input& in, float out[kJoints][12]) {
    float cur[12];
    memcpy(cur, in.base, sizeof(cur));
    for (int j = 0; j < kJoints; ++j) {
        const float a = in.frame * 0.01f * static_cast< float >(j + 1);
        const float c = std::cos(a) * in.scale, s = std::sin(a) * in.scale;
        float m[12];
        for (int r = 0; r < 3; ++r) {
            m[r * 4 + 0] = cur[r * 4 + 0] * c - cur[r * 4 + 1] * s;
            m[r * 4 + 1] = cur[r * 4 + 0] * s + cur[r * 4 + 1] * c;
            m[r * 4 + 2] = cur[r * 4 + 2];
            m[r * 4 + 3] = cur[r * 4 + 3] + static_cast< float >(j) * 0.5f;
        }
        memcpy(out[j], m, sizeof(m));
        memcpy(cur, m, sizeof(cur));
    }
}
// The worker's item (ModelManager.cpp ParFlushItem for a plant job).
void WorkerItem(void*, uint32_t i) {
    const uint32_t j = gCount - 1u - i;
    Job& job = gJobs[j];
    if (!port::par::PlantWorkerBegin(gPhase[j])) {
        return;
    }
    const Live& l = gLive[j];  // (the live state: the game thread keeps off it from here to PlantWorkerBuilt)
    job.keyWords = KeyOf(l, job.key);
    InputOf(l, job.in);
    float sum = 0.0f;
    for (int k = 0; k < 8; ++k) sum += l.scratch[k];  // (ParBuild reads the player's state: the game thread writes it in its computation)
    job.built = sum == 0.0f ? 1 : 0;  // (a torn state would be a refusal here, as a key that never matches)
    port::par::PlantWorkerBuilt(gPhase[j]);
    if (!job.built) {
        return;
    }
    Walk(job.in, job.out);
    job.ok = 1;
}
uint32_t gRng = 1;
uint32_t Rnd() {
    gRng ^= gRng << 13;
    gRng ^= gRng >> 17;
    gRng ^= gRng << 5;
    return gRng;
}

// ---- parplant23: the turn order (see the file comment)
struct TurnStats {
    uint64_t listed = 0, made = 0, gameFirst = 0, hits = 0, misses = 0, notReady = 0, bad = 0, waits = 0, waitUs = 0, early = 0, earlyUsed = 0,
             cancelled = 0, wasted = 0, drawn = 0, skipped = 0;
};
void Busy(uint32_t us) {
    const uint64_t until = port::par::NowUs() + us;
    while (port::par::NowUs() < until) port::par::CpuRelax();
}
// The batch: every plant listed (phase kPlFree, item pending), dispatched (ModelManager.cpp FlushListAndRun).
void ListAndRun(port::par::SpecWorker& worker, uint32_t n, TurnStats& st) {
    worker.Quiesce();
    gCount = n;
    for (uint32_t j = 0; j < n; ++j) {
        gJobs[j].built = 0;
        gJobs[j].ok = 0;
        gJobs[j].item = n - 1u - j;
        gPhase[j].store(port::par::kPlFree, std::memory_order_relaxed);
        gItems[j].store(port::par::kJobPending, std::memory_order_relaxed);
    }
    st.listed += n;
    worker.Dispatch(&WorkerItem, nullptr, gItems, n);
}
// The flush (port_anim_flush_stale from its first plant): own, the real computation, the take (verify: every hit compared).
void Flush(uint32_t n, TurnStats& st) {
    for (uint32_t j = 0; j < n; ++j) {
        Job& job = gJobs[j];
        uint64_t w = 0;
        const uint8_t ph = port::par::PlantGameOwn(gPhase[j], &w);
        if (w) {
            ++st.waits;
            st.waitUs += w;
        }
        if (ph == port::par::kPlGame) {
            ++st.gameFirst;
            uint8_t v = port::par::kJobPending;
            gItems[job.item].compare_exchange_strong(v, port::par::kJobTaken);
        }
        Live& l = gLive[j];
        for (int k = 0; k < 8; ++k) l.scratch[k] = static_cast< float >(k + 1);
        uint32_t real[16];
        const uint32_t words = KeyOf(l, real);
        float orig[kJoints][12];
        Input in;
        InputOf(l, in);
        Walk(in, orig);  // (the original: GXV_PAR_PLANT=2 runs it for every hit and compares)
        for (int k = 0; k < 8; ++k) l.scratch[k] = 0.0f;
        if (ph == port::par::kPlBuilt && job.built) {
            const port::par::SpecWorker::ClaimResult c = port::par::SpecWorker::Claim(gItems[job.item], 300);
            if (c == port::par::SpecWorker::kClaimDone && job.ok) {
                if (job.keyWords == words && memcmp(job.key, real, words * 4) == 0) {
                    ++st.hits;
                    st.bad += memcmp(job.out, orig, sizeof(orig)) != 0 ? 1 : 0;
                } else {
                    ++st.misses;
                }
            } else {
                ++st.notReady;
            }
        }
    }
}
uint32_t BuiltCount(uint32_t n) {
    uint32_t b = 0;
    for (uint32_t j = 0; j < n; ++j) b += gJobs[j].built;
    return b;
}
int Turns(int turns, uint32_t n, int earlyMode, uint32_t gapUs, bool neg) {
    port::par::SpecWorker& worker = *new port::par::SpecWorker;
    if (!worker.Start(3, 0, "plant_turns")) {
        printf("par_plant turns: FAIL: no worker\n");
        return 1;
    }
    TurnStats st;
    port::par::PlantTurns hist;
    bool open = false, early = false;
    bool drawn = true;
    for (int t = 0; t < turns; ++t) {
        // the pacing: 30 pictures (D S D S) with an irregular turn now and then (a late turn drawn twice, a catch-up skip)
        const uint32_t r = Rnd() % 16;
        drawn = r == 0 ? drawn : (r == 1 ? !drawn : (t & 1) == 0);
        if (drawn) {
            ++st.drawn;
            // port_par_flush_prepare: the early batch is kept, else the batch is listed now (no head start: the flush follows at once)
            if (!(open && early)) {
                ListAndRun(worker, n, st);
                open = true;
                early = false;
            } else {
                ++st.earlyUsed;
            }
            Flush(n, st);
            worker.Quiesce();  // (port_par_flush_end)
            st.made += BuiltCount(n);
            open = early = false;
        } else {
            ++st.skipped;
        }
        // GameSystem::update: an early batch still open ends first (port_par_plant_close_early), then the plants move
        if (open && !neg) {
            worker.Quiesce();
            ++st.cancelled;
            st.wasted += BuiltCount(n);
            open = early = false;
        }
        for (uint32_t j = 0; j < n; ++j) {
            Live& l = gLive[j];
            l.frame += l.rate;
            l.latch = static_cast< int >(Rnd() & 1);
            if ((Rnd() & 31u) == 0) l.base[3] += 1.0f;
        }
        if (neg && open) {
            worker.Quiesce();
            open = early = false;
        }
        // the turn's end (port_par_plant_turn_end): the prediction, the early listing; then other work (retrace wait, next turn's start)
        hist.Note(drawn);
        if (hist.NextDraws(earlyMode)) {
            ListAndRun(worker, n, st);
            ++st.early;
            open = early = true;
        }
        Busy(gapUs);
    }
    worker.Quiesce();
    const double drawnLists = st.listed > 0 ? static_cast< double >(st.listed - st.cancelled * n) : 1.0;
    printf("par_plant turns: early mode %d, gap %u us, %d turns (%llu drawn, %llu skipped) x %u plants%s: of the plants the drawn turns computed "
           "(%llu), made by the worker %llu (%.1f %%), reached first by the game thread %llu; hits %llu, misses %llu, not ready %llu; waits %llu "
           "(%.1f us avg); early batches %llu (used %llu, ended unused before update() %llu, %llu jobs built for nothing); "
           "verify (as GXV_PAR_PLANT=2): %llu checked, %llu mismatches\n",
           earlyMode, gapUs, turns, (unsigned long long)st.drawn, (unsigned long long)st.skipped, n, neg ? " (NEGATIVE CONTROL: no close before update)" : "",
           (unsigned long long)(st.drawn * n), (unsigned long long)st.made, 100.0 * st.made / (double(st.drawn) * n + 1e-9),
           (unsigned long long)st.gameFirst, (unsigned long long)st.hits, (unsigned long long)st.misses, (unsigned long long)st.notReady,
           (unsigned long long)st.waits, st.waits ? double(st.waitUs) / st.waits : 0.0, (unsigned long long)st.early,
           (unsigned long long)st.earlyUsed, (unsigned long long)st.cancelled, (unsigned long long)st.wasted, (unsigned long long)st.hits,
           (unsigned long long)st.bad);
    (void)drawnLists;
    const double share = st.made / (double(st.drawn) * n + 1e-9);
    // PASS: no result differs; with the early listing the worker makes most of the plants' jobs (>= 50 %); a hit is a made job whose key held
    const bool pass = st.bad == 0 && (earlyMode == 0 || neg || (share >= 0.5 && st.hits > 0));
    printf("par_plant turns: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    const int pictures = argc > 1 ? atoi(argv[1]) : 200;
    const uint32_t n = argc > 2 ? static_cast< uint32_t >(atoi(argv[2])) : 200;
    gRng = argc > 3 ? static_cast< uint32_t >(atoi(argv[3])) | 1u : 12345u;
    gNeg = argc > 4 && strcmp(argv[4], "neg") == 0;
    const bool turns = argc > 4 && strncmp(argv[4], "turns", 5) == 0;
    gLive.resize(n);
    gJobs.resize(n);
    gItems = new std::atomic< uint8_t >[n];
    gPhase = new std::atomic< uint8_t >[n];
    for (uint32_t i = 0; i < n; ++i) {
        Live& l = gLive[i];
        memset(&l, 0, sizeof(l));
        l.frame = static_cast< float >(Rnd() % 1000) * 0.25f;
        l.rate = 0.5f;
        for (int k = 0; k < 12; ++k) l.base[k] = (k % 5 == 0) ? 1.0f : static_cast< float >(Rnd() % 100) * 0.01f;
        l.scale = 1.0f;
        l.latch = static_cast< int >(Rnd() & 1);
        gItems[i].store(port::par::kJobTaken);
        gPhase[i].store(port::par::kPlGame);
    }
    if (turns) {
        const int earlyMode = argc > 5 ? atoi(argv[5]) : 1;
        const uint32_t gapUs = argc > 6 ? static_cast< uint32_t >(atoi(argv[6])) : 1500;
        return Turns(pictures, n, earlyMode, gapUs, strcmp(argv[4], "turns-neg") == 0);
    }
    port::par::SpecWorker& worker = *new port::par::SpecWorker;  // (never destroyed: its thread waits on it, as in perf.cpp)
    if (!worker.Start(3, 0, "plant_test")) {
        printf("par_plant: FAIL: no worker\n");
        return 1;
    }
    uint64_t hits = 0, gameFirst = 0, waits = 0, waitUs = 0, notReady = 0, misses = 0, bad = 0, used = 0;
    for (int p = 0; p < pictures; ++p) {
        // prepare (port_par_flush_prepare): list, reset, dispatch
        worker.Quiesce();
        gCount = n;
        for (uint32_t j = 0; j < n; ++j) {
            gJobs[j].built = 0;
            gJobs[j].ok = 0;
            gJobs[j].item = n - 1u - j;
            gPhase[j].store(port::par::kPlFree, std::memory_order_relaxed);
            gItems[j].store(port::par::kJobPending, std::memory_order_relaxed);
        }
        worker.Dispatch(&WorkerItem, nullptr, gItems, n);
        // the wait for the front end
        const uint64_t until = port::par::NowUs() + Rnd() % 400;
        while (port::par::NowUs() < until) port::par::CpuRelax();
        // the flush (port_anim_flush_stale)
        for (uint32_t j = 0; j < n; ++j) {
            Job& job = gJobs[j];
            uint8_t ph = port::par::kPlGame;
            if (!gNeg) {
                uint64_t w = 0;
                ph = port::par::PlantGameOwn(gPhase[j], &w);
                if (w) {
                    ++waits;
                    waitUs += w;
                }
                if (ph == port::par::kPlGame) {
                    ++gameFirst;
                    uint8_t v = port::par::kJobPending;
                    gItems[job.item].compare_exchange_strong(v, port::par::kJobTaken);
                }
            }
            Live& l = gLive[j];
            // (the flush's writes: the latch, then calcAnm's state, as the real computation makes them)
            const int latch = l.latch;
            l.latch = latch;
            for (int k = 0; k < 8; ++k) l.scratch[k] = static_cast< float >(k + 1);
            uint32_t real[16];
            const uint32_t words = KeyOf(l, real);
            float orig[kJoints][12];
            Input in;
            InputOf(l, in);
            Walk(in, orig);
            for (int k = 0; k < 8; ++k) l.scratch[k] = 0.0f;  // (clearAnm)
            bool took = false;
            if (!gNeg && ph == port::par::kPlBuilt && job.built) {
                const port::par::SpecWorker::ClaimResult c = port::par::SpecWorker::Claim(gItems[job.item], 300);
                if (c == port::par::SpecWorker::kClaimDone && job.ok) {
                    if (job.keyWords == words && memcmp(job.key, real, words * 4) == 0) {
                        took = true;
                        ++hits;
                        if (memcmp(job.out, orig, sizeof(orig)) != 0) {
                            ++bad;
                        }
                    } else {
                        ++misses;
                    }
                } else {
                    ++notReady;
                }
            }
            used += took ? 1 : 0;
        }
        // the end of the flush (port_par_flush_end): the batch is over before the next turn writes the plants
        worker.Quiesce();
        // the turns until the next picture: the plants' clocks run on; a few move
        for (uint32_t j = 0; j < n; ++j) {
            Live& l = gLive[j];
            l.frame += l.rate;
            l.latch = static_cast< int >(Rnd() & 1);
            if ((Rnd() & 31u) == 0) l.base[3] += 1.0f;
        }
    }
    uint32_t busyUs = 0, jobs = 0, sleeps = 0;
    worker.TakeStats(&busyUs, &jobs, &sleeps);
    printf("par_plant: %d pictures x %u plants%s: hits %llu (%.1f %%), misses %llu, not ready %llu, reached first by the game thread %llu, "
           "waits %llu (%.1f us avg), worker %u jobs; results that differ from the original: %llu\n",
           pictures, n, gNeg ? " (NEGATIVE CONTROL: no PlantGameOwn)" : "", (unsigned long long)hits, 100.0 * hits / (double(pictures) * n),
           (unsigned long long)misses, (unsigned long long)notReady, (unsigned long long)gameFirst, (unsigned long long)waits,
           waits ? double(waitUs) / waits : 0.0, jobs, (unsigned long long)bad);
    const bool pass = bad == 0 && (gNeg || hits > 0);
    printf("par_plant: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
