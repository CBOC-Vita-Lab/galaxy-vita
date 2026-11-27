// Host test of the movie pipeline's pacing (movie36; platform/src/sdk/thp_video.cpp): the delay line (GXV_THP_DELAY), the
// order the workers take pictures in, the picture a call shows, and the re-tiling of the hardware decoder's output.
// No game data and no libjpeg: the pure helpers are called directly, a model of a decoder is run over them, and the real
// pipeline (worker threads and all) is fed 16x16 THP frames made here by hand, each a flat picture whose grey level is
// its number.
//
//   1. linearToTiles: every texel of the tiled plane is the texel of the linear plane, for sizes that are and are not
//      multiples of 8 and 16, with a row pitch larger than the width, both load widths; nothing is read past a row's width
//      (the source buffer ends there: the address sanitizer watches) and nothing is written past the plane.
//   2. thpDue / thpChooseShow / thpJobStale / thpPickJob: the cases their comments describe.
//   3. A model: pictures submitted once a turn, decoded by `workers` decoders that take `need` turns each.
//      A decoder that keeps up: every picture is shown, in order, on the call it is due at, with no picture shown twice
//      once the delay line is full, even with a picture now and then that takes three turns (the case the delay is for).
//      A decoder at half the needed speed: the shown pictures never step back, are never newer than the due one, and are
//      spread out (no run of more than three calls without a new picture).
//   4. The pipeline itself (PETARI_THP_WORKERS=2): with GXV_THP_DELAY=3 the picture a call shows is never newer than the
//      due one and never older than the one before; with GXV_THP_DELAY=0 (the rules before movie36) never older than the
//      one before; port_thp_movie_closed() writes the summary line, and the next call starts a new movie.
//
// Usage: thp-pacing            (parts 1-3 and part 4 with the delay the environment gives, 3 from tools/test_thp_pacing.sh)
#define THP_STANDALONE 1
#include "../platform/src/sdk/thp_video.cpp"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" void port_gpu_memory_written(void*, uint32_t) {}

static std::mutex gLogMu;
static std::vector<std::string> gLog;
extern "C" void port_log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(gLogMu);
    gLog.push_back(line);
}

static int gFailures = 0;
#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            gFailures++;                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

// ---- 1. re-tiling ---------------------------------------------------------------------------------------------------
static void testTiles() {
    static const u32 kSizes[][3] = {{640, 368, 640}, {640, 368, 704}, {320, 184, 320}, {16, 16, 16}, {24, 12, 32}, {17, 9, 40},
                                    {33, 5, 33},     {8, 4, 8},        {7, 3, 16},     {1, 1, 1},    {100, 50, 128}};
    for (const auto& sz : kSizes) {
        const u32 w = sz[0], h = sz[1], stride = sz[2];
        // the source ends with the last row's last texel: a load past a row's width in the last row is out of bounds
        std::vector<u8> src((size_t)stride * (h - 1) + w);
        for (size_t i = 0; i < src.size(); i++) src[i] = (u8)((i * 131u + (i >> 7) * 17u + 5u) & 0xFF);
        const u32 bytes = lumaBytes(w, h);
        for (int wide = 0; wide < 2; wide++) {
            std::vector<u8> dst((size_t)bytes + 64, 0xA5);
            linearToTiles(src.data(), stride, w, h, dst.data(), wide != 0);
            int bad = 0;
            for (u32 y = 0; y < h; y++) {
                for (u32 x = 0; x < w; x++) {
                    bad += tileTexel(dst.data(), w, x, y) != src[(size_t)y * stride + x];
                }
            }
            CHECK(bad == 0, "linearToTiles %ux%u pitch %u wide %d: %d texels differ", w, h, stride, wide, bad);
            int over = 0;
            for (size_t i = bytes; i < dst.size(); i++) over += dst[i] != 0xA5;
            CHECK(over == 0, "linearToTiles %ux%u pitch %u wide %d: wrote %d bytes past the plane", w, h, stride, wide, over);
        }
    }
}

// ---- 2. the helpers ---------------------------------------------------------------------------------------------------
static void testHelpers() {
    CHECK(thpDue(100, 100, 4) == 100 && thpDue(100, 103, 4) == 100 && thpDue(100, 104, 4) == 100 && thpDue(100, 105, 4) == 101,
          "thpDue: the first picture until the line is full, then seq - delay");
    CHECK(thpDue(0, 0, 0) == 0 && thpDue(7, 9, 0) == 9, "thpDue: no delay");

    // slots: seq / finished
    {
        const u64 seqs[8] = {16, 17, 10, 11, 12, 13, 14, 15};
        const bool all[8] = {true, true, true, true, true, true, true, true};
        // call 17, delay 4: picture 13 is due
        CHECK(thpChooseShow(seqs, all, 8, 0, 17, 4, true, 12) == 5, "the due picture");
        // 13 is not finished: 12 (shown last) again
        const bool no13[8] = {true, true, true, true, true, false, true, true};
        CHECK(thpChooseShow(seqs, no13, 8, 0, 17, 4, true, 12) == 4, "the due picture is late: the last one again");
        // 12 was late too and 11 was shown last; now 12 is there but 13 is not: 12
        CHECK(thpChooseShow(seqs, no13, 8, 0, 17, 4, true, 11) == 4, "the newest finished picture not after the due one");
        // nothing finished up to the due one and the last one shown is gone from the ring: the oldest after it
        const bool late[8] = {true, true, false, false, false, false, false, true};
        CHECK(thpChooseShow(seqs, late, 8, 0, 17, 4, true, 5) == 7, "nothing else: the oldest finished picture");
        // never back: finished pictures before the last shown are not candidates
        const bool old[8] = {false, false, true, true, false, false, false, false};
        CHECK(thpChooseShow(seqs, old, 8, 0, 17, 4, true, 12) == -1, "no step back");
        // a picture of the movie before (below base) is not shown
        CHECK(thpChooseShow(seqs, all, 8, 16, 17, 4, false, 0) == 0, "the new movie's first picture, held");
        CHECK(thpChooseShow(seqs, old, 8, 16, 17, 4, false, 0) == -1, "nothing of the new movie is finished");
        // delay 0: the newest finished one
        CHECK(thpChooseShow(seqs, all, 8, 0, 17, 0, true, 12) == 1, "delay 0: the newest");
    }

    CHECK(!thpJobStale(100, 100, 101, 4, false, 0), "the first picture is not stale during the first calls");
    CHECK(thpJobStale(12, 0, 17, 4, true, 11), "older than the picture due at the next call");
    CHECK(!thpJobStale(13, 0, 17, 4, true, 11), "due at the next call: still wanted");
    CHECK(thpJobStale(13, 0, 17, 4, true, 13) && thpJobStale(13, 0, 17, 4, true, 14), "not after the one shown last");

    {
        const u64 q[3] = {15, 13, 14};
        // call 16 was 2 ms ago, a turn is 16.7 ms, delay 4: 13 is due at call 17 (14.7 ms away), 14 at 18, 15 at 19
        CHECK(thpPickJob(q, 3, 17, 4, 2000, 16700, 8000) == 1, "a decoder that keeps up: the oldest first");
        CHECK(thpPickJob(q, 3, 17, 4, 2000, 16700, 20000) == 2, "13 cannot be made in 20 ms any more: 14");
        CHECK(thpPickJob(q, 3, 17, 4, 2000, 16700, 40000) == 0, "40 ms: only 15");
        CHECK(thpPickJob(q, 3, 17, 4, 2000, 16700, 90000) == 0, "none can be made: the newest");
        CHECK(thpPickJob(q, 3, 17, 4, 2000, 16700, 0) == 1, "no decode time known yet: the oldest");
        CHECK(thpPickJob(q, 1, 17, 4, 2000, 16700, 90000) == 0, "one job");
    }
}

// ---- 3. a model of the pipeline -----------------------------------------------------------------------------------------
// Time in microseconds; a call every `period`; picture k submitted at call k; `workers` decoders, each takes the job
// thpPickJob gives it when it is free (checked every 500 us, as a worker woken by a submission or by its own finish would)
// and needs needUs(k) for picture k.
struct ModelResult {
    std::vector<long> shown;  // per call: the picture shown, -1 nothing
    int repeatsAfterFill = 0, backSteps = 0, early = 0, longestGap = 0, fresh = 0;
};

template <class Need> static ModelResult runModel(int calls, u64 delay, int workers, int64_t period, Need needUs, int64_t needEstimate) {
    struct Job {
        u64 seq;
        int state;  // 0 queued, 1 busy, 2 done, 3 dropped
        int64_t finish;
    };
    std::vector<Job> jobs;
    std::vector<int64_t> freeAt((size_t)workers, 0);
    ModelResult r;
    bool anyShown = false;
    u64 lastShown = 0;
    int gap = 0;
    for (int64_t t = 0, call = 0; call < calls; t += 500) {
        for (Job& j : jobs) {
            if (j.state == 1 && j.finish <= t) j.state = 2;
        }
        const bool isCall = t % period == 0;
        if (isCall) {
            const u64 seq = (u64)call;
            jobs.push_back({seq, 0, 0});
            // the ring: only the last kRing pictures exist
            u64 seqs[kRing];
            bool done[kRing];
            int n = 0;
            for (const Job& j : jobs) {
                if (j.seq + kRing > seq && n < kRing) {
                    seqs[n] = j.seq;
                    done[n] = j.state == 2;
                    n++;
                }
            }
            const int use = thpChooseShow(seqs, done, n, 0, seq, delay, anyShown, lastShown);
            const long pic = use >= 0 ? (long)seqs[use] : -1;
            r.shown.push_back(pic);
            if (pic >= 0) {
                const u64 due = thpDue(0, seq, delay);
                if (anyShown && (u64)pic < lastShown) r.backSteps++;
                if ((u64)pic > due) r.early++;
                if (anyShown && (u64)pic == lastShown) {
                    if (seq >= delay + 1) r.repeatsAfterFill++;
                    gap++;
                } else {
                    r.fresh++;
                    gap = 0;
                }
                if (gap > r.longestGap) r.longestGap = gap;
                lastShown = (u64)pic;
                anyShown = true;
            }
            call++;
        }
        const u64 callsDone = (u64)call;
        const int64_t sinceCall = isCall ? 0 : t % period;
        for (int w = 0; w < workers; w++) {
            if (freeAt[(size_t)w] > t) continue;
            u64 seqs[64];
            int idx[64], n = 0;
            for (size_t i = 0; i < jobs.size(); i++) {
                Job& j = jobs[i];
                if (j.state != 0) continue;
                if (thpJobStale(j.seq, 0, callsDone, delay, anyShown, lastShown) || j.seq + (u64)(kRing - 2) < callsDone) {
                    j.state = 3;
                    continue;
                }
                if (n < 64) {
                    seqs[n] = j.seq;
                    idx[n++] = (int)i;
                }
            }
            if (!n) break;
            const int pick = thpPickJob(seqs, n, callsDone, delay, sinceCall, period, needEstimate);
            Job& j = jobs[(size_t)idx[pick]];
            j.state = 1;
            j.finish = t + needUs(j.seq);
            freeAt[(size_t)w] = j.finish;
        }
    }
    return r;
}

static void testModel() {
    const int64_t period = 16500;  // (a multiple of the model's 500 us step)
    {
        // keeps up: 8 ms a picture, three workers, delay 4
        const ModelResult r = runModel(600, 4, 3, period, [](u64) { return (int64_t)8000; }, 10000);
        int wrong = 0;
        for (size_t c = 5; c < r.shown.size(); c++) wrong += r.shown[c] != (long)c - 4;
        CHECK(wrong == 0 && r.repeatsAfterFill == 0 && r.backSteps == 0 && r.early == 0, "a decoder that keeps up: %d calls off the due picture, %d repeats", wrong,
              r.repeatsAfterFill);
    }
    {
        // every 20th picture takes three turns (a card read in the way, a busy scene): the delay line absorbs it ...
        auto need = [](u64 k) { return k % 20 == 7 ? (int64_t)50000 : (int64_t)8000; };
        const ModelResult r = runModel(600, 4, 3, period, need, 10000);
        int wrong = 0;
        for (size_t c = 5; c < r.shown.size(); c++) wrong += r.shown[c] != (long)c - 4;
        CHECK(wrong == 0 && r.repeatsAfterFill == 0, "slow pictures under delay 4: %d calls off the due picture, %d repeats", wrong, r.repeatsAfterFill);
        // ... and a delay of one call does not: the slow picture's calls show the one before it, then the movie is back on time
        const ModelResult r1 = runModel(600, 1, 3, period, need, 10000);
        int late = 0, onTime = 0;
        for (size_t c = 2; c < r1.shown.size(); c++) {
            late += r1.shown[c] < (long)c - 1;
            onTime += r1.shown[c] == (long)c - 1;
        }
        CHECK(late > 0 && late <= 3 * 30 && r1.backSteps == 0 && r1.early == 0 && onTime >= 500, "slow pictures under delay 1: %d late calls, %d on time", late, onTime);
    }
    {
        // half the needed speed: one worker, 33 ms a picture
        const ModelResult r = runModel(600, 4, 1, period, [](u64) { return (int64_t)33000; }, 33000);
        CHECK(r.backSteps == 0 && r.early == 0, "an overloaded decoder: %d steps back, %d early", r.backSteps, r.early);
        CHECK(r.fresh >= 250 && r.longestGap <= 3, "an overloaded decoder: %d new pictures of 600 calls, longest run without one %d", r.fresh, r.longestGap);
        long worstLag = 0;
        for (size_t c = 8; c < r.shown.size(); c++) {
            const long lag = (long)c - r.shown[c];
            if (lag > worstLag) worstLag = lag;
        }
        CHECK(worstLag <= 4 + 3, "an overloaded decoder: a picture %ld calls old was shown (the movie drifts from its sound)", worstLag);
    }
}

// ---- 4. the pipeline ----------------------------------------------------------------------------------------------------
// A 16x16 THP frame (one MCU) of a flat grey: quantisation 8 everywhere (so the DC value is the level above 128), a DC
// table of two codes ("0": no difference, "10": a 7-bit difference) and an AC table of one ("0": end of block).
static std::vector<u8> flatFrame(int level /* 64..127: grey 128 + level */) {
    std::vector<u8> f = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00};
    f.insert(f.end(), 64, 8);
    const u8 sof[] = {0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x00, 0x03, 0x11, 0x00};
    f.insert(f.end(), sof, sof + sizeof(sof));
    const u8 dhtDc[] = {0xFF, 0xC4, 0x00, 0x15, 0x00, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x07};
    f.insert(f.end(), dhtDc, dhtDc + sizeof(dhtDc));
    const u8 dhtAc[] = {0xFF, 0xC4, 0x00, 0x14, 0x10, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00};
    f.insert(f.end(), dhtAc, dhtAc + sizeof(dhtAc));
    const u8 sos[] = {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x3F, 0x00};
    f.insert(f.end(), sos, sos + sizeof(sos));
    // Y0: "10" + 7 bits of the level + "0"; Y1-Y3, Cb, Cr: "0" "0" each
    const u32 bits = (2u << 8 | (u32)level << 1) << 22;  // 10 bits at the top of the word, the other 22 are the zeros that follow
    f.push_back((u8)(bits >> 24));
    f.push_back((u8)(bits >> 16));
    f.push_back((u8)(bits >> 8));
    f.push_back((u8)bits);
    f.insert(f.end(), 12, 0);
    return f;
}

static int countLog(const char* what) {
    std::lock_guard<std::mutex> lock(gLogMu);
    int n = 0;
    for (const std::string& l : gLog) n += l.find(what) != std::string::npos;
    return n;
}

static void testPipeline() {
    const int delay = envInt("GXV_THP_DELAY", 0);
    std::vector<u8> y(lumaBytes(16, 16)), u(chromaBytes(16, 16)), v(chromaBytes(16, 16)), work(64);
    for (int movie = 0; movie < 2; movie++) {
        long last = -1;
        int back = 0, early = 0, errors = 0, shownCalls = 0;
        const int frames = 60;
        for (int k = 0; k < frames; k++) {
            std::vector<u8> f = flatFrame(64 + k);
            const u32 size = (u32)f.size();
            f.resize(kFrameCap + 64, 0);  // (the pipeline copies the size it was told, or kFrameCap)
            port_thp_frame_size(size);
            const s32 st = THPVideoDecode(f.data(), y.data(), u.data(), v.data(), work.data());
            if (st != 0) {
                errors++;
            } else {
                const long pic = (long)tileTexel(y.data(), 16, 0, 0) - 128 - 64;
                CHECK(pic >= 0 && pic <= k, "movie %d call %d: the picture shown is number %ld", movie, k, pic);
                back += pic < last;
                if (delay > 0 && k >= delay) early += pic > k - delay;
                last = pic;
                shownCalls++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        CHECK(errors == 0, "movie %d: %d calls returned an error", movie, errors);
        CHECK(back == 0, "movie %d: the picture stepped back %d times", movie, back);
        CHECK(early == 0, "movie %d: %d calls showed a picture newer than the due one", movie, early);
        CHECK(last >= frames - 1 - delay - 8 && shownCalls == frames, "movie %d: the last picture shown is %ld of %d", movie, last, frames);
        const int before = countLog("THP movie ended");
        port_thp_movie_closed();
        CHECK(countLog("THP movie ended") == before + 1, "movie %d: the close writes one summary line", movie);
        port_thp_movie_closed();
        CHECK(countLog("THP movie ended") == before + 1, "movie %d: a second close writes none", movie);
        // the next movie follows at once (no quarter second of pause): its pictures start at 0 again, see the check on `pic`
    }
    {
        std::lock_guard<std::mutex> lock(gLogMu);
        for (const std::string& l : gLog) printf("  log: %s\n", l.c_str());
    }
}

int main() {
    testTiles();
    testHelpers();
    testModel();
    if (envInt("PETARI_THP_WORKERS", 0) > 0) {
        testPipeline();
    } else {
        printf("(pipeline part skipped: PETARI_THP_WORKERS is not set)\n");
    }
    if (gFailures) {
        printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("thp-pacing: all checks passed (GXV_THP_DELAY=%d)\n", envInt("GXV_THP_DELAY", 0));
    return 0;
}
