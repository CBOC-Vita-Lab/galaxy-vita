// Host test of the core-3 yield cap and meter (platform/include/port/core3_budget.h): the policy keys, when a worker must nap, that a nap is never
// 0 and never counted as busy time, the audio priority rule, the 1 s ring and the one line per stage load, and a real worker on a host thread.
#include "port/core3_budget.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

using namespace port::core3;

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static std::map<std::string, std::string> gEnv;
static const char* Get(const char* n) { const auto it = gEnv.find(n); return it == gEnv.end() ? nullptr : it->second.c_str(); }

int main() {
    // keys
    Policy p = ReadPolicy(Get);
    Require(p.yield && p.budgetUs == 1000 && p.napUs == 100 && p.audioPrio == 0, "defaults: yield on, 1000 us budget, 100 us nap, audio prio off");
    gEnv["GXV_CORE3_YIELD"] = "0";
    Require(!ReadPolicy(Get).yield, "GXV_CORE3_YIELD=0 is the old behaviour");
    gEnv["GXV_CORE3_YIELD"] = "1"; gEnv["GXV_CORE3_NAP_US"] = "0"; gEnv["GXV_CORE3_BUDGET_US"] = "5";
    p = ReadPolicy(Get);
    Require(p.napUs == 50 && p.budgetUs == 200, "nap never under 50 us (never 0), budget never under 200 us");
    gEnv["GXV_CORE3_NAP_US"] = "999999"; gEnv["GXV_CORE3_BUDGET_US"] = "x"; gEnv["GXV_CORE3_AUDIO_PRIO"] = "0x50";
    p = ReadPolicy(Get);
    Require(p.napUs == 1000 && p.budgetUs == 1000 && p.audioPrio == 0x50, "nap clamped, garbage budget falls back, audio prio parsed");
    gEnv["GXV_CORE3_AUDIO_PRIO"] = "0x20";
    Require(ReadPolicy(Get).audioPrio == 0, "audio prio below 0x41 is refused");

    // budget: nap after >= budget of continuous work, never before; the nap is not busy time
    Policy d;
    Budget b;
    b.Begin(10000);
    uint32_t nap = 9, run = 0;
    uint32_t busy = b.Step(10400, d, &nap, &run);
    Require(busy == 400 && nap == 0 && run == 400, "400 us in: no nap");
    busy = b.Step(10999, d, &nap, &run);
    Require(busy == 599 && nap == 0, "999 us in: no nap");
    busy = b.Step(11000, d, &nap, &run);
    Require(busy == 1 && nap == 100 && run == 1000, "1000 us in: nap 100 us");
    b.Resumed(11100);  // (slept 100 us: not busy)
    busy = b.Step(11500, d, &nap, &run);
    Require(busy == 400 && nap == 0 && run == 400, "after the nap the stretch starts again and the nap was not counted");
    Policy off = d; off.yield = false;
    b.Begin(0);
    b.Step(50000, off, &nap, &run);
    Require(nap == 0 && run == 50000, "yield off: never a nap, the run is still measured");
    // a 1 ms cap holds for any item length pattern: simulate items of 300 us, nap at >= 1 ms, the longest continuous stretch <= 1 ms + one item
    {
        Budget c; uint64_t t = 0; c.Begin(t); uint32_t longest = 0, naps = 0;
        for (int i = 0; i < 1000; ++i) {
            t += 300;
            uint32_t n, r; c.Step(t, d, &n, &r);
            if (r > longest) longest = r;
            if (n) { ++naps; t += n; c.Resumed(t); }
        }
        Require(longest <= 1000 + 300 && naps > 200 && naps < 300, "simulated 300 us items: stretches stay within budget plus one item, a nap about every 4 items");
        const double loss = naps * 100.0 / t;
        Require(loss < 0.09, "throughput loss under 9 % (nap 100 us per ~1.2 ms)");
    }

    // audio priority: only on core 3, only lowered
    Policy a; a.audioPrio = 0x50;
    Require(AudioPriority(Policy{}, false, true, 0x40) == 0, "off by default");
    Require(AudioPriority(a, false, false, 0x40) == 0, "not on core 3: untouched");
    Require(AudioPriority(a, false, true, 0x40) == 0x50, "output thread 0x40 -> 0x50");
    Require(AudioPriority(a, true, true, 0x50) == 0x60, "mixer 0x50 -> 0x60");
    Require(AudioPriority(a, false, true, 0x60) == 0, "never raised");
    a.audioPrio = 0xB5;
    Require(AudioPriority(a, true, true, 0x50) == 0xBF, "mixer clamped to 0xBF");

    // meter: ring, window, one line per load
    Meter m;
    const int fin = m.Register("vgx-finish"), pv = m.Register("par-view");
    Require(m.Register("vgx-finish") == fin && fin != pv, "a name keeps its slot");
    m.SetOn(fin, true);
    m.SetOn(pv, true);
    m.SetOn(pv, false);  // (it went back: still listed, it was there)
    const uint64_t S = 1000000;
    m.Add(fin, 3000, 100 * S + 100);   // second 100
    m.Add(fin, 4000, 103 * S);          // second 103
    m.Add(fin, 5000, 107 * S);          // second 107
    m.Add(pv, 2000, 106 * S);
    Require(m.Sum(fin, 95, 105) == 7000 && m.Sum(fin, 106, 110) == 5000, "ring sums by second");
    m.Add(fin, 900, 116 * S);  // second 116 reuses the bucket of second 100: the old value is gone
    Require(m.Sum(fin, 100, 100) == 0 && m.Sum(fin, 116, 116) == 900, "an old bucket is reused, never summed twice");
    Meter n;
    const int f2 = n.Register("vgx-finish");
    n.SetOn(f2, true);
    n.Add(f2, 2000, 197 * S);  // 3 s before the load
    n.Add(f2, 6000, 202 * S);  // 2 s after
    n.NoteRun(f2, 1300, true);
    n.NoteRun(f2, 800, false);
    n.StageLoad(200 * S);
    char line[512];
    Require(!n.Poll(203 * S, d, line, sizeof line), "no line before 5 s have passed");
    Require(n.Poll(205 * S, d, line, sizeof line), "one line 5 s after the load");
    Require(!n.Poll(206 * S, d, line, sizeof line), "and only one");
    Require(std::strstr(line, "vgx-finish 8.0 (before 2.0, after 6.0;") != nullptr, "the line: busy ms before / after");
    Require(std::strstr(line, "yield cap on (budget 1000 us, nap 100 us)") != nullptr, "the line names the cap");
    std::printf("%s\n", line);
    // the per-load counters start with each load (the counters above were noted BEFORE StageLoad: reset)
    Require(std::strstr(line, "yields 0, longest run 0.00 ms") != nullptr, "per-load counters start at the load");
    Meter e;
    e.StageLoad(10 * S);
    Require(e.Poll(15 * S, d, line, sizeof line) && std::strstr(line, "no thread of ours is on core 3") != nullptr, "nothing on core 3: the line says so");

    // a worker on a host thread: not on core 3 means no clock reads and no nap; on core 3 it naps and is metered
    {
        const int slot = RegisterThread("test-worker");
        Worker w(slot);
        w.Wake();
        const uint64_t t0 = NowUs();
        while (NowUs() - t0 < 3000) {}
        w.Checkpoint();
        w.Sleep();
        Require(TheMeter().Sum(slot, 0, 1000000) == 0, "off core 3: nothing metered");
        MarkOnCore3(slot, true);
        w.Wake();
        const uint64_t t1 = NowUs();
        while (NowUs() - t1 < 3000) {}
        const uint64_t before = NowUs();
        w.Checkpoint();  // (3 ms of work: over the 1 ms budget: it naps >= 100 us)
        Require(NowUs() - before >= 90, "on core 3 after 3 ms the checkpoint sleeps");
        w.Sleep();
        Require(TheMeter().Sum(slot, 0, 1000000) >= 2500, "on core 3 the busy time is metered");
        Require(TheMeter().slots[slot].yields.load() == 1, "one yield counted");
    }
    // mp3c24: GXV_MP_CORE3
    {
        gEnv.clear();
        Policy q = ReadPolicy(Get);
        Require(!q.mpCore3 && q.mpBudgetUs == 600 && q.mpNapUs == 150, "mp: default off, 600/150");
        uint32_t bu = 0, nu = 0;
        EffectiveCap(q, true, true, &bu, &nu);
        Require(bu == 1000 && nu == 100, "mp: switch off: discretionary worker online keeps 1000/100");
        gEnv["GXV_MP_CORE3"] = "1";
        q = ReadPolicy(Get);
        Require(q.mpCore3, "mp: GXV_MP_CORE3=1 reads");
        EffectiveCap(q, true, false, &bu, &nu);
        Require(bu == 1000 && nu == 100, "mp: single player: full speed");
        EffectiveCap(q, false, true, &bu, &nu);
        Require(bu == 1000 && nu == 100, "mp: online but not discretionary (vgx-finish, audio): untouched");
        EffectiveCap(q, true, true, &bu, &nu);
        Require(bu == 600 && nu == 150, "mp: online + discretionary: 600/150");
        gEnv["GXV_MP_CORE3_BUDGET_US"] = "1"; gEnv["GXV_MP_CORE3_NAP_US"] = "0";
        q = ReadPolicy(Get);
        Require(q.mpBudgetUs == 200 && q.mpNapUs == 50, "mp: clamps, nap never 0");
        gEnv["GXV_MP_CORE3"] = "2";
        Require(!ReadPolicy(Get).mpCore3, "mp: only =1 turns it on");

        // the line: at the switch's moment, silent until 10 s, then every 10 s; off-line resets
        gEnv.clear(); gEnv["GXV_MP_CORE3"] = "1";
        q = ReadPolicy(Get);
        Meter m;
        const int sl = m.Register("par-test");
        m.SetOn(sl, true); m.SetDiscretionary(sl);
        char l[400];
        Require(!m.PollMp(false, 50000000, q, l, sizeof l) && !m.online.load(), "mp: single player: no line");
        Require(m.PollMp(true, 100000000, q, l, sizeof l) && m.online.load() && std::strstr(l, "session on") && std::strstr(l, "ON (GXV_MP_CORE3=1)"), "mp: line at join");
        Require(!m.PollMp(true, 105000000, q, l, sizeof l), "mp: none at 5 s");
        m.Add(sl, 500000, 108000000);  // 0.5 s busy in second 108
        Require(m.PollMp(true, 110000000, q, l, sizeof l) && std::strstr(l, "discretionary 5%"), "mp: 10 s line shows 5% duty");
        Require(!m.PollMp(false, 111000000, q, l, sizeof l) && !m.online.load(), "mp: leave: flag cleared");
        Require(m.PollMp(true, 112000000, q, l, sizeof l) && std::strstr(l, "session on"), "mp: rejoin: line again");
        gEnv.clear();
    }
    std::puts("core3_budget_test: ok");
    return 0;
}
