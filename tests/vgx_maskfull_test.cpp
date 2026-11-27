// GXV_MASKFULL (lodmask37): the decision and the plan's alpha liveness, platform/vgx/src/vgx_maskfull.h. No GL, no game data. The plan is built here the way
// PlanFrame builds it (events per position, a backward pass with LiveAlphaStep, the frame's end live), so the negative controls run the production recurrence.
#include "vgx_maskfull.h"
#include <cstdio>
#include <vector>

namespace {
using namespace vgx::maskfull;
unsigned failures = 0, checks = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", name); ++failures; }
}
// A frame: one entry per command; position 2i is the command, 2i+1 the clear after it. Returns the plan's liveness table (size 2n+1, the frame's end live).
std::vector<uint8_t> Plan(const std::vector<uint8_t>& evPerCmd) {
    const size_t positions = 2 * evPerCmd.size();
    std::vector<uint8_t> ev(positions + 1, 0), live(positions + 1, 0);
    for (size_t i = 0; i < evPerCmd.size(); ++i) ev[2 * i] = evPerCmd[i];
    live[positions] = 1;
    for (size_t p = positions; p-- > 0;) live[p] = LiveAlphaStep(ev[p], live[p + 1]);
    return live;
}
In Masked(const std::vector<uint8_t>& live, size_t cmd) { // an unblended RGB-only draw into the EFB at command `cmd`
    In in;
    in.onEfb = in.planValid = in.haveRecord = true;
    in.colorWrite = true;
    in.inRange = 2 * cmd + 1 < live.size();
    in.liveAfter = in.inRange ? live[2 * cmd + 1] : 1;
    return in;
}
}

int main() {
    // the recurrence
    Check(LiveAlphaStep(0, 1) == 1 && LiveAlphaStep(0, 0) == 0, "no event: as after");
    Check(LiveAlphaStep(kEvReadAlpha, 0) == 1, "a read makes it live");
    Check(LiveAlphaStep(kEvFullAlpha, 1) == 0, "a whole overwrite makes it dead");
    Check(LiveAlphaStep(kEvReadAlpha | kEvFullAlpha, 0) == 1, "a read at the same position wins (the draw reads before it overwrites)");
    Check(LiveAlphaStep(2 | 16 | 64, 0) == 0 && LiveAlphaStep(2 | 16 | 64, 1) == 1, "other events (depth read / write / colour) do not touch it");
    // what reads alpha
    Check(!DrawReadsAlpha(false, true, true, true), "an unblended draw reads nothing");
    Check(DrawReadsAlpha(true, true, false, false), "a blend that writes alpha reads it");
    Check(DrawReadsAlpha(true, false, true, false), "the override forms read it");
    Check(DrawReadsAlpha(true, false, false, true), "a destination-alpha factor reads it");
    Check(!DrawReadsAlpha(true, false, false, false), "a colour-only blend with plain factors does not");

    // the decision, one condition at a time (each from the base case that is taken)
    std::vector<uint8_t> dead = Plan({kEvFullAlpha, 0, 0});            // cover quad writes the alpha whole at cmd 0, then two draws, nothing reads: but the frame's end is live
    // (the end of the frame counts as a read: the next frame is unknown)
    Check(Decide(Masked(dead, 1)) == kAlphaLive, "after the last overwrite the frame's end keeps the alpha live");
    std::vector<uint8_t> next = Plan({0, 0, kEvFullAlpha});            // a later whole overwrite in the same frame
    Check(Decide(Masked(next, 0)) == kTake && Decide(Masked(next, 1)) == kTake, "a later whole overwrite makes the alpha dead before it: taken");
    Check(Decide(Masked(next, 2)) == kAlphaLive, "the overwrite itself and what follows it are live again at the frame's end");
    In base = Masked(next, 0);
    { In x = base; x.alphaWrite = true; Check(Decide(x) == kNotCandidate, "alpha already written: nothing to do"); }
    { In x = base; x.colorWrite = false; Check(Decide(x) == kNotCandidate, "no colour written: not a candidate"); }
    { In x = base; x.blend = true; Check(Decide(x) == kNotCandidate, "blended: not a candidate"); }
    { In x = base; x.dstAlphaZero = true; Check(Decide(x) == kNotCandidate, "the destination-alpha forms: not a candidate"); }
    { In x = base; x.stencilOp = true; Check(Decide(x) == kNotCandidate, "stencil work: not a candidate"); }
    { In x = base; x.onEfb = false; Check(Decide(x) == kNotEfb, "a pass or copy target: refused, counted"); }
    { In x = base; x.planValid = false; Check(Decide(x) == kNoPlan, "no plan: refused, counted"); }
    { In x = base; x.haveRecord = false; Check(Decide(x) == kNoPlan, "no record (the stencil operation is unknown): refused as no plan"); }
    { In x = base; x.inRange = false; Check(Decide(x) == kNoPlan, "a position outside the table: refused as no plan"); }
    { In x = base; x.liveAfter = 1; Check(Decide(x) == kAlphaLive, "alpha live after: refused, counted"); }
    { In x = base; x.onEfb = false; x.planValid = false; Check(Decide(x) == kNotEfb, "not the EFB is reported first"); }

    // negative controls: a later reader of the alpha makes every earlier draw refuse
    const uint8_t blendDstAlpha = DrawReadsAlpha(true, false, false, true) ? kEvReadAlpha : 0;
    // [masked draw][masked draw][blend with a destination-alpha factor][whole overwrite]
    std::vector<uint8_t> neg = Plan({0, 0, blendDstAlpha, kEvFullAlpha});
    Check(Decide(Masked(neg, 0)) == kAlphaLive && Decide(Masked(neg, 1)) == kAlphaLive, "a later destination-alpha blend reads the alpha: both earlier draws refuse");
    // the same frame without the reader: both are taken
    std::vector<uint8_t> pos = Plan({0, 0, 0, kEvFullAlpha});
    Check(Decide(Masked(pos, 0)) == kTake && Decide(Masked(pos, 1)) == kTake && Decide(Masked(pos, 2)) == kTake, "without the reader they are taken");
    // a reader before the draw does not matter (the draw does not read; what it writes is dead after it)
    std::vector<uint8_t> before = Plan({blendDstAlpha, 0, kEvFullAlpha});
    Check(Decide(Masked(before, 1)) == kTake, "a reader before the draw: the draw's alpha is still dead after it");
    // a copy that reads the alpha (EV_RA at its position) between the draw and the overwrite
    std::vector<uint8_t> copy = Plan({0, kEvReadAlpha, 0, kEvFullAlpha});
    Check(Decide(Masked(copy, 0)) == kAlphaLive && Decide(Masked(copy, 2)) == kTake, "a copy that reads the alpha: the draws before it refuse, those after it are taken");
    // a draw whose own position reads the alpha (a blended draw) is not a candidate; the unblended draw before it is live
    std::vector<uint8_t> self = Plan({0, kEvReadAlpha});
    Check(Decide(Masked(self, 0)) == kAlphaLive, "a read in the next command: refuse");
    // (a clear that follows a command is position 2i+1: its overwrite makes the alpha dead after that command's draws)
    {
        std::vector<uint8_t> ev(2 * 3 + 1, 0), live(2 * 3 + 1, 0);
        ev[2 * 0 + 1] = kEvFullAlpha; // the clear after command 0
        live[6] = 1;
        for (size_t p = 6; p-- > 0;) live[p] = LiveAlphaStep(ev[p], live[p + 1]);
        In in = Masked(live, 0);
        Check(live[1] == 0 && Decide(in) == kTake, "an EFB clear after the command: dead after it");
    }

    // hsr24: the clean prefix (modes 3, 4, 5). kinds: 0 inert, 1 establish (a clear leaving alpha 0), 2 candidate, 3 reader, 4 writer
    {
        const uint8_t k[] = {kInert, kEstablish, kCandidate, kCandidate, kInert, kCandidate, kReader, kCandidate, kWriter, kCandidate};
        const Prefix p = AnalysePrefix(k, sizeof k);
        Check(p.e == 1 && p.endRW == 6 && p.endW == 8, "prefix: clear at 1, first reader at 6, first writer at 8");
        Check(p.candPrefixRW == 3 && p.candAfterRW == 2 && p.candPrefixW == 4 && p.candBefore == 0, "prefix: candidates before the reader / from it on / before the writer");
        Check(p.readers == 1 && p.writers == 1 && p.readersBeforeE == 0, "prefix: counts of readers and writers");
        const QuadPlan q = PlanQuad(p, k, 99, false);
        Check(q.draw() && q.at == 6 && q.why == kQuadAtReader, "quad: before the first reader");
    }
    {
        const uint8_t k[] = {kCandidate, kEstablish, kCandidate, kWriter, kReader, kCandidate};
        const Prefix p = AnalysePrefix(k, sizeof k);
        Check(p.candBefore == 1 && p.endRW == 3 && p.endW == 3 && p.readersBeforeE == 0, "prefix: a writer ends both ends; candidates before the clear are counted apart");
        const QuadPlan q = PlanQuad(p, k, SIZE_MAX, false);
        Check(q.draw() && q.at == 3 && q.why == kQuadAtWriter, "quad: before the first writer");
    }
    {
        const uint8_t k[] = {kReader, kEstablish, kCandidate, kCandidate};
        const Prefix p = AnalysePrefix(k, sizeof k);
        Check(p.readersBeforeE == 1 && p.endRW == SIZE_MAX && p.candPrefixRW == 2, "prefix: a reader BEFORE the clear does not end it (and the picture did not begin clean)");
        Check(PlanQuad(p, k, SIZE_MAX, false).why == kQuadNoDisp && !PlanQuad(p, k, SIZE_MAX, false).draw(), "no reader and no display copy: no quad, the mode stands aside");
        const QuadPlan t = PlanQuad(p, k, 3, false);
        Check(t.draw() && t.at == 3 && t.why == kQuadAtDisp, "no reader: the quad goes before the display copy");
        Check(!PlanQuad(p, k, 3, true).draw() && PlanQuad(p, k, 3, true).why == kQuadTailSkipped, "the tail predictor leaves it out");
    }
    {
        const uint8_t k[] = {kCandidate, kReader};
        Check(PlanQuad(AnalysePrefix(k, sizeof k), k, SIZE_MAX, false).why == kQuadNoPrefix, "no clear: no prefix");
        const uint8_t k2[] = {kEstablish, kReader, kCandidate};
        Check(PlanQuad(AnalysePrefix(k2, sizeof k2), k2, SIZE_MAX, false).why == kQuadNoCandidates, "no candidate in the prefix: no quad");
        const uint8_t k3[] = {kEstablish, kEstablish, kCandidate, kReader};
        Check(AnalysePrefix(k3, sizeof k3).e == 0, "the first clear starts the prefix");
        TailPredictor t; t.need = 3;
        t.Observe(true); t.Observe(true); Check(!t.Predicts(), "tail predictor: not yet");
        t.Observe(true); Check(t.Predicts(), "tail predictor: after the run");
        t.Observe(false); Check(!t.Predicts(), "tail predictor: a picture that did not begin clean resets it");
    }
    std::printf("maskfull: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
