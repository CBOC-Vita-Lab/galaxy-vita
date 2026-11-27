// pace33 PATCH9 host test: port/seg_prewait.h (GXV_SEG_PREWAIT_US) and port/hrw_stage.h Auto (GXV_HRW_STAGE_AUTO).
// g++ -std=c++20 -Iplatform/include tests/pace33_patch9_test.cpp && ./a.out
#include "port/hrw_stage.h"
#include "port/seg_prewait.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static uint32_t Win(uint32_t seq, int score) { return (seq << 8) | static_cast<uint32_t>(score); }

int main() {
    {   // ---- the segment pre-wait rule ----
        using namespace port::segpre;
        Config c; c.segSize = 2u << 20; c.idleLimit = 128u << 10; c.waitNs = 4000000; c.remLimit = 64u << 10;
        State s;
        const size_t pic = 170u << 10;   // the Observatory: ~170 KiB a drawn turn, nothing in the undrawn one
        size_t off = 0; int wants = 0, firstWantPic = -1;
        for (int p = 0; p < 12 && off + pic < c.segSize; ++p) {
            // the drawn turn's start: the front end has 20 KiB left; the undrawn turn's start: 92 KiB left
            if (Want(s, c, 7, off, true, 20u << 10)) { ++wants; if (firstWantPic < 0) firstWantPic = p; break; }
            off += pic;
            CHECK(!Want(s, c, 7, off, true, 92u << 10));   // never at the undrawn turn's start (the front end is 12 ms behind)
        }
        CHECK(wants == 1);
        CHECK(firstWantPic >= 9);                                    // only within about two turns of the end
        CHECK(c.segSize - off < 2 * pic + c.margin && c.segSize - off >= pic);   // ... and before the picture that would not fit
        // one failed wait a segment
        Missed(s, 7);
        CHECK(!Want(s, c, 7, off, true, 0));
        // off, small segments, unknown progress
        State s2; Config c0 = c; c0.waitNs = 0;
        for (int i = 0; i < 12; ++i) CHECK(!Want(s2, c0, 1, i * pic, true, 0));
        State s3;
        for (int i = 0; i < 11; ++i) Want(s3, c, 1, i * pic, false, 0);
        CHECK(!Want(s3, c, 1, 11 * pic, false, 0));                  // the front end's position is in another segment
        State s4;   // a light stage (20 KiB a turn): the idle changes keep the segment short; never asks under the idle limit or far from the end
        for (int i = 0; i < 40; ++i) CHECK(!Want(s4, c, 1, i * (20u << 10), true, 0));
        State s5; Config ci = c; ci.idleLimit = 0;
        for (int i = 0; i < 12; ++i) CHECK(!Want(s5, ci, 1, i * pic, true, 0));
    }
    {   // ---- the automatic per-stage fallback ----
        using namespace port::hrwstage;
        Auto a; AutoConfig c; a.Configure(c);
        const uint32_t hub = 0x4903d024u, other = 0x11112222u;
        uint32_t seq = 5;
        Auto::Out o = a.Update(hub, true, Win(seq, 100), true);
        CHECK(o.newStage && !o.remembered && !a.Fallback());
        o = a.Update(hub, true, Win(++seq, 55), true); CHECK(!o.fell && o.bad == 0);        // the window that began before the stage
        o = a.Update(hub, true, Win(++seq, 55), true); CHECK(!o.fell && o.bad == 1);
        for (int i = 0; i < 500; ++i) { o = a.Update(hub, true, Win(seq, 55), true); CHECK(!o.fell); }   // the same window is counted once
        o = a.Update(hub, true, Win(++seq, 90), true); CHECK(!o.fell && o.bad == 0);        // a good window clears the run
        o = a.Update(hub, true, Win(++seq, 60), true); CHECK(o.bad == 1);
        o = a.Update(hub, true, Win(++seq, 62), false); CHECK(!o.fell && o.bad == 0);       // the fresh player is still on: it goes first
        o = a.Update(hub, true, Win(++seq, 60), true); CHECK(o.bad == 1);
        o = a.Update(hub, true, Win(++seq, 53), true); CHECK(o.fell && a.Fallback() && a.Remembered() == 1);
        for (int i = 0; i < 20; ++i) { o = a.Update(hub, true, Win(++seq, 100), true); CHECK(!o.fell && a.Fallback()); }   // no way back inside the stage
        // the Choice takes it only behind a cover
        Choice ch;
        CHECK(!ch.Update(a.Fallback() ? 1 : 0, false) && ch.DeferredNow() && !ch.PairsOff());
        CHECK(ch.Update(1, true) && ch.PairsOff());
        // another stage: the re-projected 60 again; good windows: stays
        o = a.Update(other, true, Win(seq, 100), true); CHECK(o.newStage && !o.remembered && !a.Fallback());
        CHECK(ch.Update(a.Fallback() ? 1 : 0, true) && !ch.PairsOff());
        for (int i = 0; i < 20; ++i) { o = a.Update(other, true, Win(++seq, 96), true); CHECK(!o.fell); }
        o = a.Update(other, true, Win(++seq, 74), true); CHECK(!o.fell);                    // one bad window is not enough in a stage not known
        o = a.Update(other, true, Win(++seq, 80), true); CHECK(!o.fell && !a.Fallback());
        // back in the hub: tried again, remembered, one bad window falls back
        o = a.Update(hub, true, Win(seq, 80), true); CHECK(o.newStage && o.remembered && o.need == 1 && !a.Fallback());
        o = a.Update(hub, true, Win(++seq, 50), true); CHECK(!o.fell);                      // (the window from before)
        o = a.Update(hub, true, Win(++seq, 90), true); CHECK(!o.fell);                      // the fix held: stays at 60
        o = a.Update(hub, true, Win(++seq, 60), true); CHECK(o.fell && a.Remembered() == 1);
        // a row's own word (not eligible), Quality, the switch off: never
        Auto b; b.Configure(c);
        for (int i = 0; i < 20; ++i) { o = b.Update(hub, false, Win(++seq, 10), true); CHECK(!o.fell && !b.Fallback()); }
        Auto d; AutoConfig off; off.on = false; d.Configure(off);
        for (int i = 0; i < 20; ++i) { o = d.Update(hub, true, Win(++seq, 10), true); CHECK(!o.fell && !d.Fallback()); }
        // eligibility lost in a fallen stage (a row arrives): the fallback ends
        Auto e; e.Configure(c);
        for (int i = 0; i < 6; ++i) e.Update(hub, true, Win(++seq, 10), true);
        CHECK(e.Fallback()); e.Update(hub, false, Win(++seq, 10), true); CHECK(!e.Fallback());
        // no windows at all (GXV_EVEN_LOG=0): nothing happens
        Auto f; f.Configure(c);
        for (int i = 0; i < 1000; ++i) { o = f.Update(hub, true, 0, true); CHECK(!o.fell); }
        // the published window: sequence moves, score clamps
        const uint32_t w0 = EvenWindow().load(); PublishWindow(53.4); const uint32_t w1 = EvenWindow().load(); PublishWindow(250.0); const uint32_t w2 = EvenWindow().load();
        CHECK((w1 >> 8) == (w0 >> 8) + 1 && (w1 & 0xFF) == 53 && (w2 >> 8) == (w1 >> 8) + 1 && (w2 & 0xFF) == 100);
        // 40 stages fall back: the table keeps the last 32 without overrun
        Auto g; g.Configure(c); uint32_t q = 100;
        for (uint32_t k = 1; k <= 40; ++k) { g.Update(k, true, Win(q, 10), true); for (int i = 0; i < 4; ++i) g.Update(k, true, Win(++q, 10), true); CHECK(g.Fallback()); }
        CHECK(g.Remembered() == 32 && g.Known(40) && !g.Known(1));
    }
    {   // ---- hrw41: the way back (GXV_HRW_STAGE_AUTO_RETRY) ----
        using namespace port::hrwstage;
        const uint32_t hub = 0x4903d024u;
        AutoConfig c; c.retryWindows = 3;
        Auto a; a.Configure(c);
        uint32_t seq = 9;
        Auto::Out o = a.Update(hub, true, Win(seq, 100), true);
        a.Update(hub, true, Win(++seq, 50), true);
        a.Update(hub, true, Win(++seq, 50), true);
        o = a.Update(hub, true, Win(++seq, 57), true);
        CHECK(o.fell && a.Fallback() && o.tries == 1 && o.wait == 3);
        o = a.Update(hub, true, Win(++seq, 60), true, true); CHECK(!o.retried && o.calm == 0);   // the window of the change itself is not counted
        o = a.Update(hub, true, Win(++seq, 100), true, true); CHECK(!o.retried && o.calm == 1);
        for (int i = 0; i < 300; ++i) { o = a.Update(hub, true, Win(seq, 100), true, true); CHECK(!o.retried && o.calm == 1); }   // a window counts once
        o = a.Update(hub, true, Win(++seq, 100), true, false); CHECK(!o.retried && o.calm == 0);  // no room on the game thread: the run starts again
        o = a.Update(hub, true, Win(++seq, 100), true, true); CHECK(o.calm == 1);
        o = a.Update(hub, true, Win(++seq, 70), true, true); CHECK(o.calm == 0);                  // the locked 30 itself uneven: not now
        o = a.Update(hub, true, Win(++seq, 99), true, true);
        o = a.Update(hub, true, Win(++seq, 99), true, true); CHECK(!o.retried && o.calm == 2);
        o = a.Update(hub, true, Win(++seq, 99), true, true); CHECK(o.retried && !a.Fallback());
        // tried again: the first window (pictures of the locked 30) is not judged; a good one keeps it; ONE bad one falls back, and the wait doubles
        o = a.Update(hub, true, Win(++seq, 10), true); CHECK(!o.fell);
        o = a.Update(hub, true, Win(++seq, 90), true); CHECK(!o.fell && !a.Fallback());
        o = a.Update(hub, true, Win(++seq, 60), false); CHECK(!o.fell);                           // the fresh player is given up first, as ever
        o = a.Update(hub, true, Win(++seq, 60), true); CHECK(o.fell && o.tries == 2 && o.wait == 6 && a.Remembered() == 1);
        int n = 0;
        for (; n < 40 && a.Fallback(); ++n) a.Update(hub, true, Win(++seq, 100), true, true);
        CHECK(n == 7);                                                                            // 1 uncounted + 6
        a.Update(hub, true, Win(++seq, 100), true); o = a.Update(hub, true, Win(++seq, 40), true); CHECK(o.fell && o.wait == 12);
        for (n = 0; n < 80 && a.Fallback(); ++n) a.Update(hub, true, Win(++seq, 100), true, true);
        CHECK(n == 13);
        a.Update(hub, true, Win(++seq, 100), true); o = a.Update(hub, true, Win(++seq, 40), true); CHECK(o.fell && o.wait == 24);
        for (n = 0; n < 80 && a.Fallback(); ++n) a.Update(hub, true, Win(++seq, 100), true, true);
        CHECK(n == 25);
        a.Update(hub, true, Win(++seq, 100), true); o = a.Update(hub, true, Win(++seq, 40), true); CHECK(o.fell && o.wait == 24);   // x8 for good
        // a stage load starts the count of this visit again (remembered: one bad window, the first wait)
        o = a.Update(0x1234u, true, Win(seq, 100), true); CHECK(o.newStage && !a.Fallback());
        o = a.Update(hub, true, Win(seq, 100), true); CHECK(o.newStage && o.remembered && o.tries == 0);
        a.Update(hub, true, Win(++seq, 100), true); o = a.Update(hub, true, Win(++seq, 40), true); CHECK(o.fell && o.wait == 3);
        // Missed publications break consecutive windows. Pressure inside a window
        // must not disappear just because the frame at the boundary is cheap.
        o = a.Update(hub, true, Win(++seq, 100), true, true);
        o = a.Update(hub, true, Win(++seq, 100), true, true); CHECK(o.calm == 1);
        seq += 3;
        o = a.Update(hub, true, Win(seq, 100), true, true); CHECK(o.calm == 0);
        a.Update(hub, true, Win(seq, 100), true, false);
        o = a.Update(hub, true, Win(++seq, 100), true, true); CHECK(o.calm == 0);
        o = a.Update(hub, true, Win(++seq, 100), true, true); CHECK(o.calm == 1);
        // 0 (the default): never, whatever the windows say
        Auto z; AutoConfig c0; z.Configure(c0);
        z.Update(hub, true, Win(seq, 100), true);
        for (int i = 0; i < 4; ++i) z.Update(hub, true, Win(++seq, 10), true);
        CHECK(z.Fallback());
        for (int i = 0; i < 200; ++i) { o = z.Update(hub, true, Win(++seq, 100), true, true); CHECK(!o.retried && z.Fallback() && o.wait == 0); }
    }
    printf(fails ? "pace33_patch9_test: %d FAILED\n" : "pace33_patch9_test: ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
