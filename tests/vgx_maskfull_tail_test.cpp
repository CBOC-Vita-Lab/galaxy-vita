// Host unit test for exact GXV_MASKFULL_TAIL (port::maskfull::TailAlphaPredictor) + alpha liveness & census verification.
// Exercises:
//   1. 60-frame warmup (no drop for frames 1..59; drop activates on frame 60)
//   2. Apply mode (GXV_MASKFULL_TAIL=1) unmasking masked opaque draws (including Dreadnought shape: 0 readers, and mid-frame reader protection)
//   3. Census mode (GXV_MASKFULL_TAIL=2) zero GL changes while counting exact wouldUnmask draws & screens
//   4. HRW pair refusal (clean HRW A resets streak and refuses drop without false trip; non-clean HRW after drop trips fail-safe)
//   5. Fail-safe trip-off on sudden pre-establish alpha read (permanent disable, staleTrips==1, multi-frame carry across RGB8/kNeither frames)
//   6. Unplanned pictures (unverified counter + run reset)
//   7. End-to-end EFB alpha memory model across 500 frames proving 0 stale reads on clean streams and immediate fail-safe catch on read-before-establish
//
// Run via: tools/test_vgx_maskfull_tail.sh
#include "port/maskfull.h"
#include "vgx_maskfull.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using namespace port::maskfull;
using vgx::maskfull::Decide;
using vgx::maskfull::In;
using vgx::maskfull::kAlphaLive;
using vgx::maskfull::kTake;

unsigned g_checks = 0, g_failures = 0;
void Check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) {
        std::printf("FAIL: %s\n", name);
        ++g_failures;
    }
}

struct FrameSim {
    std::vector<uint8_t> ev;
    std::vector<uint8_t> estab;
    std::vector<size_t> maskedCmds;
    std::vector<double> maskedScreens;
    bool hrwPair = false;
    bool planned = true;
};

// Build a Dreadnought-like picture: cmd 0 = whole-EFB clear draw (EV_FA), cmds 1..nMasked = masked opaque draws, last cmd = COPY_DISP.
// If midReadAt > 0, insert an EV_RA copy/blend at command midReadAt (e.g. silhouette capture in the middle of the frame).
FrameSim MakeCleanFrame(size_t nMasked, size_t midReadAt = 0, bool useCopyClearEstablish = false) {
    const size_t nCmds = 2 + nMasked + (midReadAt > 0 ? 1 : 0);
    const size_t positions = 2 * nCmds;
    FrameSim f;
    f.ev.assign(positions + 1, 0);
    f.estab.assign(positions + 1, 0);
    if (useCopyClearEstablish) {
        // CMD_COPY_TEX / CMD_COPY_DISP clear at position 2*0 + 1
        f.ev[1] = kEvFullAlpha;
        f.estab[1] = 1;
    } else {
        // Whole-EFB clear draw at command 0 (position 0)
        f.ev[0] = kEvFullAlpha;
        f.estab[0] = 1;
    }
    size_t cmd = 1;
    for (size_t i = 0; i < nMasked; ++i) {
        if (midReadAt > 0 && cmd == midReadAt) {
            f.ev[2 * cmd] = kEvReadAlpha;
            ++cmd;
        }
        f.maskedCmds.push_back(cmd);
        f.maskedScreens.push_back(0.5); // 0.5 screen each
        ++cmd;
    }
    return f;
}

FrameSim MakePreEstablishReadFrame(bool alsoEstablishLater) {
    const size_t nCmds = 4;
    const size_t positions = 2 * nCmds;
    FrameSim f;
    f.ev.assign(positions + 1, 0);
    f.estab.assign(positions + 1, 0);
    f.ev[0] = kEvReadAlpha; // command 0 reads EFB alpha BEFORE any establish!
    if (alsoEstablishLater) {
        f.ev[2] = kEvFullAlpha;
        f.estab[2] = 1;
    }
    f.maskedCmds.push_back(2);
    f.maskedScreens.push_back(1.0);
    return f;
}

FrameSim MakeNeitherFrame() {
    // e.g. RGB8 loading screen frame: no alpha read, no whole-EFB alpha establish
    const size_t nCmds = 3;
    const size_t positions = 2 * nCmds;
    FrameSim f;
    f.ev.assign(positions + 1, 0);
    f.estab.assign(positions + 1, 0);
    f.maskedCmds.push_back(1);
    f.maskedScreens.push_back(1.0);
    return f;
}

In MakeDrawIn(const std::vector<uint8_t>& liveA, size_t cmd) {
    In in;
    in.onEfb = true;
    in.planValid = true;
    in.haveRecord = true;
    in.colorWrite = true;
    in.inRange = (2 * cmd + 1) < liveA.size();
    in.liveAfter = in.inRange ? liveA[2 * cmd + 1] : 1;
    return in;
}

void TestClassifyStartAlpha() {
    {
        const uint8_t ev[] = {0, kEvFullAlpha, kEvReadAlpha, 0};
        Check(ClassifyStartAlpha(ev, nullptr, 4) == StartAlpha::kCleanEstablish, "classify: establish before read -> kCleanEstablish");
    }
    {
        const uint8_t ev[] = {0, kEvReadAlpha, kEvFullAlpha, 0};
        Check(ClassifyStartAlpha(ev, nullptr, 4) == StartAlpha::kReadBeforeEstablish, "classify: read before establish -> kReadBeforeEstablish");
    }
    {
        const uint8_t ev[] = {kEvReadAlpha | kEvFullAlpha, 0};
        Check(ClassifyStartAlpha(ev, nullptr, 2) == StartAlpha::kReadBeforeEstablish, "classify: read+establish at same position -> read wins conservatively");
    }
    {
        const uint8_t ev[] = {0, 0, 0, 0};
        Check(ClassifyStartAlpha(ev, nullptr, 4) == StartAlpha::kNeither, "classify: neither read nor establish -> kNeither");
    }
    {
        // Conservative estab mask: ev has kEvFullAlpha, but estab mask refuses it (e.g. overscan or failing stencil) -> kNeither
        const uint8_t ev[] = {kEvFullAlpha, 0};
        const uint8_t estab[] = {0, 0};
        Check(ClassifyStartAlpha(ev, estab, 2) == StartAlpha::kNeither, "classify: estab mask overrides ev conservatively");
    }
    {
        // Conservative estab mask accepts coverCull clear draw even when EV_FA wasn't set
        const uint8_t ev[] = {0, 0, kEvReadAlpha, 0};
        const uint8_t estab[] = {1, 0, 0, 0};
        Check(ClassifyStartAlpha(ev, estab, 4) == StartAlpha::kCleanEstablish, "classify: estab mask before read -> kCleanEstablish");
    }
}

void TestWarmupAndApply() {
    TailAlphaPredictor pred; // default N = 60
    Check(pred.Need() == 60, "default warmup streak is N=60");
    const FrameSim f = MakeCleanFrame(20);
    const size_t positions = f.ev.size() - 1;
    std::vector<uint8_t> liveA(positions + 1, 1);

    // Frames 1..59: warmup, must NOT drop tail
    for (int i = 1; i <= 59; ++i) {
        const StartAlpha st = ClassifyStartAlpha(f.ev.data(), f.estab.data(), positions);
        const auto obs = pred.Observe(true, st, false);
        const bool drop = pred.DropTail(false);
        Check(!obs.tripped && !drop && pred.Run() == static_cast<uint32_t>(i), "warmup frame 1..59 does not drop");
        ComputeLiveAlpha(f.ev.data(), positions, drop ? 0 : 1, liveA.data());
        unsigned unmasked = 0;
        for (size_t cmd : f.maskedCmds)
            if (Decide(MakeDrawIn(liveA, cmd)) == kTake) ++unmasked;
        Check(unmasked == 0, "warmup frame leaves masked draws kAlphaLive");
    }
    Check(pred.Drops() == 0 && pred.StaleTrips() == 0 && !pred.Off(), "after 59 frames: 0 drops, 0 trips, active");

    // Frame 60: threshold reached -> drops tail into dedicated g_liveATail while shared g_liveA stays conservative (tail=1)!
    {
        const StartAlpha st = ClassifyStartAlpha(f.ev.data(), f.estab.data(), positions);
        const auto obs = pred.Observe(true, st, false);
        const bool drop = pred.DropTail(false);
        Check(!obs.tripped && drop && pred.Run() == 60 && pred.Drops() == 1, "frame 60 activates TailAlpha");
        std::vector<uint8_t> liveATail(positions + 1, 1);
        ComputeLiveAlpha(f.ev.data(), positions, 1, liveA.data());     // shared g_liveA stays conservative (tail=1) for EfbSwap / directd::Settle
        ComputeLiveAlpha(f.ev.data(), positions, 0, liveATail.data()); // dedicated g_liveATail (tail=0) read only by MaskFullWant
        Check(liveA[positions] == 1 && liveATail[positions] == 0, "shared g_liveA tail stays 1 while g_liveATail drops tail to 0");
        unsigned unmasked = 0;
        for (size_t cmd : f.maskedCmds)
            if (Decide(MakeDrawIn(liveATail, cmd)) == kTake) ++unmasked;
        Check(unmasked == 20, "frame 60 unmasks all 20 masked opaque draws via g_liveATail");
    }

    // Frame 61 with copy-clear establish and a mid-frame alpha reader at cmd 6 (after 5 masked draws, before 15 masked draws)
    {
        const FrameSim mid = MakeCleanFrame(20, 6, true);
        const size_t pos = mid.ev.size() - 1;
        const StartAlpha st = ClassifyStartAlpha(mid.ev.data(), mid.estab.data(), pos);
        Check(st == StartAlpha::kCleanEstablish, "mid-frame reader after copy-clear is still kCleanEstablish");
        const auto obs = pred.Observe(true, st, false);
        const bool drop = pred.DropTail(false);
        Check(!obs.tripped && drop && pred.Drops() == 2, "mid-frame reader after clear does not trip and still drops tail");
        std::vector<uint8_t> la(pos + 1, 1);
        ComputeLiveAlpha(mid.ev.data(), pos, 0, la.data());
        unsigned liveBefore = 0, unmaskedAfter = 0;
        for (size_t cmd : mid.maskedCmds) {
            const auto why = Decide(MakeDrawIn(la, cmd));
            if (cmd < 6 && why == kAlphaLive) ++liveBefore;
            if (cmd > 6 && why == kTake) ++unmaskedAfter;
        }
        Check(liveBefore == 5 && unmaskedAfter == 15, "mid-frame reader protects 5 draws before it and unmasks 15 draws after it");
    }
}

void TestCensusZeroGlChange() {
    TailAlphaPredictor predApply(60), predCensus(60);
    const FrameSim f = MakeCleanFrame(18);
    const size_t positions = f.ev.size() - 1;
    std::vector<uint8_t> liveApply(positions + 1, 1), liveCensusGl(positions + 1, 1), liveCensusHyp(positions + 1, 1);

    uint64_t applyUnmasked = 0, censusGlUnmasked = 0, censusWouldUnmask = 0;
    double applyScreens = 0.0, censusWouldScreens = 0.0;

    for (int frame = 0; frame < 80; ++frame) {
        const StartAlpha st = ClassifyStartAlpha(f.ev.data(), f.estab.data(), positions);

        // Mode 1 (apply)
        predApply.Observe(true, st, false);
        const bool drop1 = predApply.DropTail(false);
        ComputeLiveAlpha(f.ev.data(), positions, drop1 ? 0 : 1, liveApply.data());
        for (size_t i = 0; i < f.maskedCmds.size(); ++i) {
            if (Decide(MakeDrawIn(liveApply, f.maskedCmds[i])) == kTake) {
                ++applyUnmasked;
                applyScreens += f.maskedScreens[i];
            }
        }

        // Mode 2 (count-only census): GL always uses tailLive = 1!
        predCensus.Observe(true, st, false);
        const bool wouldDrop = predCensus.DropTail(false);
        ComputeLiveAlpha(f.ev.data(), positions, 1, liveCensusGl.data()); // GL untouched!
        if (wouldDrop) ComputeLiveAlpha(f.ev.data(), positions, 0, liveCensusHyp.data());
        for (size_t i = 0; i < f.maskedCmds.size(); ++i) {
            const size_t cmd = f.maskedCmds[i];
            const auto glWhy = Decide(MakeDrawIn(liveCensusGl, cmd));
            if (glWhy == kTake) ++censusGlUnmasked;
            if (glWhy == kAlphaLive && wouldDrop && liveCensusHyp[2 * cmd + 1] == 0) {
                ++censusWouldUnmask;
                censusWouldScreens += f.maskedScreens[i];
            }
        }
    }

    Check(censusGlUnmasked == 0, "census mode (GXV_MASKFULL_TAIL=2) makes ZERO GL mask changes");
    Check(applyUnmasked == 21 * 18, "apply mode unmasked 21 active frames x 18 draws = 378");
    Check(censusWouldUnmask == applyUnmasked, "census wouldUnmask matches apply unmasked count exactly");
    Check(std::fabs(censusWouldScreens - applyScreens) < 1e-9, "census wouldScreens matches apply screens exactly");
}

void TestHrwRefusal() {
    TailAlphaPredictor pred(60);
    // Warm up 60 clean non-HRW frames and activate
    for (int i = 0; i < 60; ++i) {
        pred.Observe(true, StartAlpha::kCleanEstablish, false);
        pred.DropTail(false);
    }
    Check(pred.Drops() == 1 && pred.StaleArmed() && !pred.Off(), "HRW test: active before HRW pair");

    // HRW pair A: establishes alpha cleanly at start, then SceneSnap/Snapshot reads EFB alpha at end
    {
        const auto obs = pred.Observe(true, StartAlpha::kCleanEstablish, true);
        const bool drop = pred.DropTail(true);
        Check(!obs.tripped && !drop && !pred.Off(), "HRW pair A with clean establish does not trip and refuses DropTail");
        Check(pred.Run() == 0 && !pred.StaleArmed() && pred.HrwSkips() == 1, "HRW pair A clears staleArmed, resets run to 0, counts hrwSkip");
    }
    // HRW pair B: no establish, follows pair A (which did NOT drop tail)
    {
        const auto obs = pred.Observe(true, StartAlpha::kNeither, true);
        const bool drop = pred.DropTail(true);
        Check(!obs.tripped && !drop && !pred.Off() && pred.HrwSkips() == 2, "HRW pair B after non-dropped pair A does not trip and refuses DropTail");
    }
    // After HRW ends, must warm up all 60 consecutive clean non-HRW frames again
    for (int i = 1; i <= 59; ++i) {
        pred.Observe(true, StartAlpha::kCleanEstablish, false);
        Check(!pred.DropTail(false), "post-HRW warmup frames 1..59 do not drop");
    }
    pred.Observe(true, StartAlpha::kCleanEstablish, false);
    Check(pred.DropTail(false) && pred.Drops() == 2, "post-HRW frame 60 resumes DropTail");

    // Conservative HRW safety: if an HRW frame WITHOUT clean establish arrives while staleArmed is set -> trip!
    {
        const auto obs = pred.Observe(true, StartAlpha::kNeither, true);
        Check(obs.tripped && pred.StaleTrips() == 1 && pred.Off(), "HRW picture without clean establish after dropped tail trips fail-safe");
        Check(!pred.DropTail(false), "disabled after HRW fail-safe trip");
    }
}

void TestFailSafeTripOff() {
    // 1. Direct pre-establish alpha read immediately after an active frame
    {
        TailAlphaPredictor pred(60);
        for (int i = 0; i < 65; ++i) {
            pred.Observe(true, StartAlpha::kCleanEstablish, false);
            pred.DropTail(false);
        }
        Check(pred.Drops() == 6 && !pred.Off() && pred.StaleTrips() == 0, "trip test: 6 active drops before sudden read");

        const FrameSim bad = MakePreEstablishReadFrame(true);
        const StartAlpha st = ClassifyStartAlpha(bad.ev.data(), bad.estab.data(), bad.ev.size() - 1);
        Check(st == StartAlpha::kReadBeforeEstablish, "sudden pre-establish read classified as kReadBeforeEstablish");
        const auto obs = pred.Observe(true, st, false);
        const bool drop = pred.DropTail(false);
        Check(obs.tripped && !drop && pred.StaleTrips() == 1 && pred.Off() && pred.Run() == 0, "fail-safe trips immediately and disables predictor");

        // 100 clean frames afterwards must NEVER re-enable TailAlpha (matches TailDepth policy)
        const uint64_t dropsAtTrip = pred.Drops();
        for (int i = 0; i < 100; ++i) {
            const auto o2 = pred.Observe(true, StartAlpha::kCleanEstablish, false);
            const bool d2 = pred.DropTail(false);
            Check(!o2.tripped && !d2, "remains permanently disabled after trip");
        }
        Check(pred.Drops() == dropsAtTrip && pred.StaleTrips() == 1 && pred.Off(), "no drops and no double-trips after fail-safe disable");
    }

    // 2. StaleArmed carries across kNeither (RGB8 / fade) frames and catches a later pre-establish read
    {
        TailAlphaPredictor pred(60);
        for (int i = 0; i < 60; ++i) {
            pred.Observe(true, StartAlpha::kCleanEstablish, false);
            pred.DropTail(false);
        }
        Check(pred.StaleArmed() && pred.Drops() == 1, "carry test: staleArmed after drop");

        const FrameSim neither = MakeNeitherFrame();
        const StartAlpha stN = ClassifyStartAlpha(neither.ev.data(), neither.estab.data(), neither.ev.size() - 1);
        Check(stN == StartAlpha::kNeither, "RGB8/empty frame is kNeither");
        const auto obs1 = pred.Observe(true, stN, false);
        Check(!obs1.tripped && !pred.DropTail(false) && pred.StaleArmed() && pred.Run() == 0 && !pred.Off(),
              "kNeither does not false-trip, resets run, refuses drop, keeps staleArmed");

        const auto obs2 = pred.Observe(true, StartAlpha::kReadBeforeEstablish, false);
        Check(obs2.tripped && pred.StaleTrips() == 1 && pred.Off(), "pre-establish read after kNeither is caught and trips fail-safe");
    }

    // 3. kNeither followed by kCleanEstablish clears staleArmed cleanly without tripping
    {
        TailAlphaPredictor pred(60);
        for (int i = 0; i < 60; ++i) {
            pred.Observe(true, StartAlpha::kCleanEstablish, false);
            pred.DropTail(false);
        }
        pred.Observe(true, StartAlpha::kNeither, false);
        const auto obs = pred.Observe(true, StartAlpha::kCleanEstablish, false);
        Check(!obs.tripped && !pred.Off() && !pred.StaleArmed() && pred.Run() == 1, "kCleanEstablish after kNeither clears staleArmed without trip");
    }

    // 4. Unplanned frame while staleArmed counts unverified and resets run (matching TailDepth)
    {
        TailAlphaPredictor pred(60);
        for (int i = 0; i < 60; ++i) {
            pred.Observe(true, StartAlpha::kCleanEstablish, false);
            pred.DropTail(false);
        }
        const auto obs = pred.Observe(false, StartAlpha::kNeither, false);
        Check(!obs.tripped && !pred.Off() && pred.Unverified() == 1 && pred.Run() == 0 && !pred.StaleArmed(),
              "unplanned frame increments unverified and resets run");
        pred.Observe(true, StartAlpha::kCleanEstablish, false);
        Check(!pred.DropTail(false) && pred.Run() == 1, "does not drop until 60 clean frames after unplanned frame");
    }
}

// End-to-end EFB alpha memory model: tracks logical alpha vs actual EFB alpha when TailAlpha unmasks opaque draws.
void TestAlphaMemoryModel() {
    TailAlphaPredictor pred(60);
    bool alphaDirty = false; // true iff EFB alpha differs from logical alpha because an unmasked draw wrote shader alpha
    uint64_t wrongReads = 0;
    uint64_t totalUnmasked = 0;

    auto runFrame = [&](const FrameSim& f) {
        const size_t pos = f.ev.size() - 1;
        const StartAlpha st = ClassifyStartAlpha(f.ev.data(), f.estab.data(), pos);
        pred.Observe(f.planned, st, f.hrwPair);
        const bool drop = f.planned ? pred.DropTail(f.hrwPair) : false;
        std::vector<uint8_t> liveA(pos + 1, 1);
        ComputeLiveAlpha(f.ev.data(), pos, drop ? 0 : 1, liveA.data());

        // Execute positions in order
        for (size_t p = 0; p < pos; ++p) {
            if (f.ev[p] & kEvReadAlpha) {
                if (alphaDirty) ++wrongReads;
            }
            if (f.estab[p]) {
                alphaDirty = false; // whole EFB alpha overwritten to exact logical value
            }
            if ((p & 1u) == 0) {
                const size_t cmd = p / 2;
                for (size_t mc : f.maskedCmds) {
                    if (mc == cmd && Decide(MakeDrawIn(liveA, cmd)) == kTake) {
                        alphaDirty = true; // draw wrote arbitrary shader alpha into EFB!
                        ++totalUnmasked;
                    }
                }
            }
        }
        // HRW SceneSnap/Snapshot reads EFB alpha at end of frame
        if (f.hrwPair && alphaDirty) ++wrongReads;
    };

    // 250 clean frames (mix of 0-reader and mid-frame-reader)
    for (int i = 0; i < 250; ++i) {
        runFrame((i % 7 == 0) ? MakeCleanFrame(15, 5, false) : MakeCleanFrame(20, 0, (i % 3 == 0)));
    }
    Check(wrongReads == 0 && totalUnmasked > 3000 && pred.StaleTrips() == 0, "alpha memory model: 0 wrong reads across 250 clean frames with >3000 unmasked draws");

    // Insert an HRW pair: still 0 wrong reads!
    FrameSim hrwA = MakeCleanFrame(15, 0, false);
    hrwA.hrwPair = true;
    runFrame(hrwA);
    FrameSim hrwB = MakeNeitherFrame();
    hrwB.hrwPair = true;
    runFrame(hrwB);
    Check(wrongReads == 0 && pred.StaleTrips() == 0 && !pred.Off(), "alpha memory model: HRW pair A+B has 0 wrong reads");

    // Re-warm 70 clean frames, then hit a sudden pre-establish read (which then establishes alpha later in that frame):
    // exactly 1 wrong read caught & permanently disabled; afterwards neither clean frames nor pre-establish read frames ever dirty or misread alpha again.
    for (int i = 0; i < 70; ++i) runFrame(MakeCleanFrame(10, 0, false));
    Check(wrongReads == 0 && pred.Drops() > 190, "alpha memory model: re-warmed cleanly");
    runFrame(MakePreEstablishReadFrame(true));
    Check(wrongReads == 1 && pred.StaleTrips() == 1 && pred.Off(), "alpha memory model: sudden pre-establish read caught by fail-safe");
    const uint64_t unmaskedAtTrip = totalUnmasked;
    for (int i = 0; i < 50; ++i) runFrame(MakeCleanFrame(10, 0, false));
    for (int i = 0; i < 50; ++i) runFrame(MakePreEstablishReadFrame(false));
    Check(totalUnmasked == unmaskedAtTrip && wrongReads == 1 && pred.StaleTrips() == 1,
          "alpha memory model: 0 additional unmasked draws and 0 additional wrong reads after permanent fail-safe disable");
}

} // namespace

int main() {
    TestClassifyStartAlpha();
    TestWarmupAndApply();
    TestCensusZeroGlChange();
    TestHrwRefusal();
    TestFailSafeTripOff();
    TestAlphaMemoryModel();
    std::printf("vgx_maskfull_tail_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
