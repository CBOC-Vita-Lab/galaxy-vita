// GXV_PRESENT_SYNC and GXV_SKIP_PATTERN on a model of the console's pipeline (a model, not a console measurement): the game's turns on
// the display's vblanks, the frame-skip rule of "Game speed Full" (port::pace::LegacySkip, PETARI_FRAMESKIP=5, pipe depth 2), and per
// drawn turn a front end, a back end and a GPU stage, each one picture at a time, with random costs around what the r21 Observatory
// log shows (front end ~20 ms a picture and the slowest stage, ~44 pictures a second). The display queue holds two pictures (three
// buffers); its callback (frame_pace.h PresentSync, as SyncDisplay runs it) shows a picture on a vblank. Prints, per policy, the
// pictures a second, the holds, the "judder" (|vblanks held - turns advanced| per picture: 0 = Mario's displayed speed is his real
// speed) and the spread of shown-minus-turn (the error of the displayed game time against the clock).
//   g++ -std=c++17 -O2 -I platform/include tests/present_sync_model.cpp -o /tmp/present_sync_model && /tmp/present_sync_model
#include "port/frame_pace.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

namespace {
constexpr double kV = 16.68335;  // ms per vblank

struct Policy { const char* name; int sync; int pattern; unsigned maxLat = 5; unsigned raise = 4; };

struct Result { double fps, judder, latMean, latSd; uint64_t hold[5]; double late; };

Result Run(const Policy& pol, double feMean, unsigned seed, bool print) {
    std::mt19937 rng(seed);
    auto cost = [&](double mean, double sd, double lo) { std::normal_distribution<double> d(mean, sd); double v = d(rng); return v < lo ? lo : v; };
    port::pace::LegacySkip rule(5, 2, 6);
    port::pace::SkipPattern pattern(pol.pattern);
    port::pace::PresentSync sync;
    sync.Configure(pol.sync == 2, 1, pol.maxLat, 3, pol.raise);
    double feFree = 0, beFree = 0, gpuFree = 0, cbFree = 0;
    std::deque<double> beDone;     // back-end completion (swap returned) of the pictures still counted as pending
    std::deque<double> cbReturn;   // the display callback's return time per picture, for the queue's back-pressure
    uint64_t pictures = 0;
    const unsigned turns = 60 * 120;
    for (unsigned t = 0; t < turns; ++t) {
        const double start = t * kV + 0.3;  // the turn starts just after its vblank (the vblank thread raises the retrace)
        while (!beDone.empty() && beDone.front() <= start) beDone.pop_front();
        const unsigned pending = static_cast<unsigned>(beDone.size());
        auto v = rule.Turn(static_cast<int64_t>(start * 1e6), true, pending);
        if (pattern.Enabled()) {
            const bool offered = pattern.Offer(false);
            if (!offered && v.code == 0) v.code = 1;
            pattern.Result(offered, v.code == 0);
        }
        if (v.code != 0) continue;
        // The picture: recorded by the game thread (~4 ms), parsed by the front end, executed by the back end, drawn by the GPU.
        const double fe0 = std::fmax(start + 1.0, feFree), fe1 = fe0 + cost(feMean, 4.0, 12.0);
        feFree = fe1;
        const double be0 = std::fmax(fe1, beFree);
        double be1 = be0 + cost(9.0, 2.0, 5.0);
        // vglSwapBuffers blocks while the display queue holds two pictures: the one two before must have been called back.
        if (cbReturn.size() >= 2) be1 = std::fmax(be1, cbReturn[cbReturn.size() - 2]);
        beFree = be1;
        beDone.push_back(be1);
        const double g0 = std::fmax(be1 - 6.0, gpuFree), g1 = std::fmax(g0 + cost(11.0, 3.0, 5.0), be1);  // (the GPU follows the back end)
        gpuFree = g1;
        // The display queue: callbacks in order, each after the GPU finished its picture and the previous callback returned.
        const double cb = std::fmax(g1, cbFree);
        const uint32_t earliest = static_cast<uint32_t>(std::floor(cb / kV)) + 1;
        const uint32_t target = sync.Target(earliest, true, static_cast<uint16_t>(t));
        sync.Shown(target, target != earliest);
        cbFree = target != earliest ? (target - 1) * kV + 0.2 : cb + 0.1;  // (a wait returns at the vblank before the slot)
        cbReturn.push_back(cbFree);
        if (cbReturn.size() > 4) cbReturn.pop_front();
        ++pictures;
    }
    const auto& st = sync.GetStats();
    Result r{};
    r.fps = pictures / (turns * kV / 1000.0);
    r.judder = st.pairs ? static_cast<double>(st.errSum) / st.pairs : 0;
    const double n = st.stamped ? static_cast<double>(st.stamped) : 1;
    r.latMean = st.latSum / n;
    r.latSd = std::sqrt(std::fmax(0.0, st.latSq / n - r.latMean * r.latMean));
    for (int i = 0; i < 5; ++i) r.hold[i] = st.hold[i];
    r.late = 100.0 * st.misses / n;
    if (print) {
        std::printf("  %-34s %5.1f fps  held 0/1/2/3/4+ %4llu/%4llu/%4llu/%4llu/%3llu  judder %.3f vblanks (%4.1f ms)/picture  "
                    "shown-minus-turn %.2f +- %.2f vblanks (%.0f ms)  L=%u\n",
                    pol.name, r.fps, (unsigned long long)st.hold[0], (unsigned long long)st.hold[1], (unsigned long long)st.hold[2],
                    (unsigned long long)st.hold[3], (unsigned long long)st.hold[4], r.judder, r.judder * kV, r.latMean, r.latSd,
                    r.latMean * kV, sync.Latency());
    }
    return r;
}
}  // namespace

int main() {
    // Unit checks of the pieces.
    {
        port::pace::TurnClock c;
        Require(c.Turn(100) == 100 && c.Turn(101) == 101 && c.Turn(101) == 101, "a turn is never ahead of the display");
        Require(c.Turn(110) == 106, "at most four vblanks behind");
        Require(c.Turn(110) == 107, "then it advances one per turn (the skip rule's catch-up)");
        port::pace::SkipPattern p3(3);
        int drawn = 0;
        for (int i = 0; i < 400; ++i) { const bool o = p3.Offer(false); p3.Result(o, o); drawn += o; }
        Require(drawn == 300, "3 of 4 turns offered");
        port::pace::SkipPattern p(3);
        Require(!p.Offer(false), "the rhythm starts with the turn left out");
        Require(p.Offer(false), "offered");
        p.Result(true, false);  // (left out by the rule)
        Require(p.Offer(false) && p.Offer(false), "offered as due");
        p.Result(true, true);
        Require(!p.Offer(false), "back in the rhythm");
        port::pace::SkipPattern off(0);
        Require(!off.Enabled(), "0 is off");
        port::pace::PresentSync s;
        s.Configure(true, 1, 5, 3);
        Require(s.Target(1003, true, 1000) == 1003, "a picture ready at its slot is not held");
        s.Shown(1003, false);
        Require(s.Target(1004, true, 1002) == 1005, "an early picture is held to stamp + latency");
        s.Shown(1005, true);
        Require(s.Target(1005, true, 1001) == 1006, "never two pictures in one vblank");
        s.Shown(1006, true);
        Require(s.Target(1007, true, static_cast<uint16_t>(1007 - 200)) == 1007, "an implausible stamp is shown when ready");
        s.Shown(1007, false);
        Require(s.Target(1008, false, 0) == 1008, "an unstamped picture is shown when ready");
        port::pace::PresentSync m;
        m.Configure(false, 1, 5, 3);
        Require(m.Target(1004, true, 1002) == 1004, "measure only: never held");
        // Adaptation: pictures that keep coming 4 vblanks after their turn raise the latency to 4; quiet ones bring it down.
        port::pace::PresentSync a;
        a.Configure(true, 1, 5, 3);
        uint32_t vb = 5000;
        for (int i = 0; i < 4; ++i, vb += 2) { const uint32_t t = a.Target(vb + 4, true, static_cast<uint16_t>(vb)); a.Shown(t, false); }
        Require(a.Latency() == 4, "raised after two late pictures");
        for (int i = 0; i < 2000; ++i, vb += 2) { const uint32_t t = a.Target(vb + 1, true, static_cast<uint16_t>(vb)); a.Shown(t, t != vb + 1); }
        Require(a.Latency() == 1, "lowered again when every picture is ready after one vblank");
        for (int i = 0; i < 50; ++i, vb += 2) { const uint32_t t = a.Target(vb + 7, true, static_cast<uint16_t>(vb)); a.Shown(t, false); }
        Require(a.Latency() == 1, "pictures beyond the largest latency (a pause menu) do not raise it");
    }
    const Policy policies[] = {
        {"now: shown when ready", 1, 0},
        {"GXV_PRESENT_SYNC=2", 2, 0},
        {"GXV_PRESENT_SYNC=2 .._MAX=3", 2, 0, 3},
        {"GXV_PRESENT_SYNC=2 .._RAISE=2", 2, 0, 5, 2},
        {"GXV_PRESENT_SYNC=2 .._RAISE=8", 2, 0, 5, 8},
        {"GXV_PRESENT_SYNC=2 GXV_SKIP_PATTERN=3", 2, 3},
        {"GXV_PRESENT_SYNC=1 GXV_SKIP_PATTERN=3", 1, 3},
        {"GXV_PRESENT_SYNC=2 GXV_SKIP_PATTERN=2", 2, 2},
    };
    for (double fe : {14.0, 18.0, 20.0, 23.0}) {
        std::printf("front end %.0f ms a picture (sd 4):\n", fe);
        for (const Policy& p : policies) Run(p, fe, 7, true);
    }
    // The claims of the report, on 8 seeds at the Observatory's ~20 ms.
    double jNow = 0, jSync = 0, fNow = 0, fSync = 0, sdNow = 0, sdSync = 0;
    for (unsigned seed = 1; seed <= 8; ++seed) {
        const Result a = Run(policies[0], 20.0, seed, false), b = Run(policies[1], 20.0, seed, false);
        jNow += a.judder; jSync += b.judder; fNow += a.fps; fSync += b.fps; sdNow += a.latSd; sdSync += b.latSd;
    }
    std::printf("8 seeds, front end 20 ms: judder %.3f -> %.3f vblanks a picture, timing spread %.2f -> %.2f vblanks, %.1f -> %.1f fps\n",
                jNow / 8, jSync / 8, sdNow / 8, sdSync / 8, fNow / 8, fSync / 8);
    Require(jSync < jNow * 0.5, "aligned presentation at least halves the judder");
    Require(fSync > fNow - 3.0, "and costs at most a few pictures a second");
    std::printf("present_sync_model: ok\n");
    return 0;
}
