// nohitch26: host test of vgx_async_link.h (GXV_ASYNC_LINK's gate, census and stand-in choice). tools/test_async_link.sh
#include "../platform/vgx/src/vgx_async_link.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace vgx::alink;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }
int main() {
    // a cluster of 6 programs asked in one picture: one link a picture, the rest wait; all linked after 6 pictures
    {
        Gate g;
        g.cfg.mode = 2;
        unsigned linked = 0, pictures = 0;
        bool done[6] = {};
        for (uint64_t pic = 10; pic < 40 && linked < 6; ++pic, ++pictures) {
            g.Picture(pic);
            unsigned linksThisPicture = 0;
            for (uint32_t k = 0; k < 6; ++k) {
                if (done[k]) continue;
                const Decision d = g.Decide(k, kSyncNone, kCacheHit, false);
                if (d.v == Verdict::Link) { Require(d.sliced && d.why == kSyncNone, "a sliced link"); g.Linked(k, 12.0, true, kSyncNone); done[k] = true; ++linked; ++linksThisPicture; }
                else g.DrawSkipped(false, false);
            }
            Require(linksThisPicture <= 1, "at most one link a picture (gap 1)");
        }
        g.Flush();
        Require(linked == 6 && pictures == 6, "six programs, six pictures");
        Require(g.c.deferredKeys == 5, "five distinct programs waited");
        Require(g.c.picturesAffected == 5 && g.c.runMax == 5, "five pictures in a row had a skipped draw");
        Require(g.c.waitMax == 5 && g.c.slicedLinks == 6 && g.c.worstLinkMs == 12.0, "wait, links, worst link");
        Require(g.Waiting() == 0, "nobody waits at the end");
    }
    // the ms budget with gap 0: fast links share a picture until the budget is spent
    {
        Gate g;
        g.cfg.gap = 0;
        g.cfg.budgetMs = 4.0;
        g.Picture(1);
        unsigned n = 0;
        for (uint32_t k = 0; k < 10; ++k) {
            const Decision d = g.Decide(k, kSyncNone, kCacheHit, false);
            if (d.v == Verdict::Link) { g.Linked(k, 1.5, true, kSyncNone); ++n; }
        }
        Require(n == 3, "1.5 ms links: the third starts at 3.0 ms spent, the fourth at 4.5 ms is refused");
        g.Picture(2);
        Require(g.Decide(9, kSyncNone, kCacheHit, false).v == Verdict::Link, "a new picture has a new budget");
    }
    // what must not wait links at once and is counted by reason
    {
        Gate g;
        g.Picture(1);
        const Sync must[] = {kSyncMustFbo, kSyncMustFlat, kSyncMustChara, kSyncMustStatic};
        for (unsigned i = 0; i < 4; ++i) {
            const Decision d = g.Decide(100 + i, must[i], kCacheHit, false);
            Require(d.v == Verdict::Link && d.why == must[i] && !d.sliced, "must: link at once, with its reason");
            g.Linked(100 + i, 30.0, false, d.why);
        }
        Require(g.c.sync[kSyncMustFbo] == 1 && g.c.sync[kSyncMustStatic] == 1 && g.c.linksInPlay == 4 && g.c.slicedLinks == 0, "census of synchronous links");
        // a must-block link does not take the budget of waiting programs away from a later picture, but it spends this picture's
        Require(g.Decide(1, kSyncNone, kCacheHit, false).v == Verdict::Defer, "the same picture's budget is spent by the 30 ms link");
        g.Picture(2);
        Require(g.Decide(1, kSyncNone, kCacheHit, false).v == Verdict::Link, "next picture: free");
    }
    // not in the cache and no worker: one in missGap pictures; with a worker the miss is not asked here
    {
        Gate g;
        g.cfg.missGap = 30;
        g.Picture(5);
        Decision d = g.Decide(1, kSyncNone, kCacheMiss, false);
        Require(d.v == Verdict::Link && d.why == kSyncMissNoWorker, "first miss: linked (counted)");
        g.Linked(1, 150.0, false, d.why);
        g.Picture(6);
        Require(g.Decide(2, kSyncNone, kCacheMiss, false).v == Verdict::Defer, "second miss waits");
        g.Picture(34);
        Require(g.Decide(2, kSyncNone, kCacheMiss, false).v == Verdict::Defer, "still inside the gap (5 + 30 = 35)");
        g.Picture(35);
        d = g.Decide(2, kSyncNone, kCacheMiss, false);
        Require(d.v == Verdict::Link && d.why == kSyncMissNoWorker, "gap over");
        Require(g.c.sync[kSyncMissNoWorker] == 1 && g.c.syncMs[kSyncMissNoWorker] == 150.0, "miss census");
        g.Picture(36);
        d = g.Decide(3, kSyncNone, kCacheMiss, true);
        Require(d.v == Verdict::Link && d.sliced, "with a worker a miss that reaches the gate is a failed job: linked in budget");
    }
    // a program that waits too long is linked anyway
    {
        Gate g;
        g.cfg.maxWait = 5;
        g.cfg.gap = 1;
        g.Picture(1);
        g.Linked(0, 10.0, true, kSyncNone); // (spends picture 1)
        Require(g.Decide(7, kSyncNone, kCacheHit, false).v == Verdict::Defer, "waits");
        for (uint64_t p = 2; p < 6; ++p) { g.Picture(p); g.Linked(50 + p, 20.0, true, kSyncNone); Require(g.Decide(7, kSyncNone, kCacheHit, false).v == Verdict::Defer, "still waits"); }
        g.Picture(6);
        const Decision d = g.Decide(7, kSyncNone, kCacheHit, false);
        Require(d.v == Verdict::Link && d.why == kSyncAged, "aged out");
        g.Linked(7, 20.0, false, d.why);
        Require(g.c.sync[kSyncAged] == 1 && g.c.waitMax == 5, "aged census");
    }
    // announcements wait like draws but do not make a picture "affected"
    {
        Gate g;
        g.Picture(1);
        g.Linked(1, 9.0, true, kSyncNone);
        Require(g.Decide(2, kSyncNoDraw, kCacheHit, false).v == Verdict::Defer, "announcement deferred");
        g.Flush();
        Require(g.c.picturesAffected == 0 && g.c.deferredKeys == 1, "an announcement is not a skipped picture");
    }
    // stand-ins
    {
        StandInIndex ix;
        ix.Add(3, 10, 0x01, 28);
        ix.Add(3, 11, 0x01, 12);
        ix.Add(3, 12, 0x03, 12);  // other samplers
        ix.Add(3, 13, 0x01, 40);  // larger TEV block: unsafe
        ix.Add(4, 10, 0x01, 28);  // other vertex shader
        ix.Add(3, 10, 0x01, 28);  // duplicate
        auto all = [](uint16_t, uint64_t* last) { *last = 5; return true; };
        Require(ix.Pick(3, 99, 0x01, 28, all) == 10, "closest TEV block without being larger");
        Require(ix.Pick(3, 10, 0x01, 28, all) == 11, "never itself");
        Require(ix.Pick(3, 99, 0x02, 28, all) == -1, "samplers must match exactly");
        Require(ix.Pick(5, 99, 0x01, 28, all) == -1, "no program of that vertex shader");
        auto recent = [](uint16_t fs, uint64_t* last) { *last = fs == 11 ? 9 : 1; return true; };
        ix.Add(3, 14, 0x01, 12);
        Require(ix.Pick(3, 99, 0x01, 12, recent) == 11, "equal size: the most recently drawn");
        auto gone = [](uint16_t fs, uint64_t* last) { *last = 1; return fs != 10; };
        Require(ix.Pick(3, 99, 0x01, 28, gone) == 11 || ix.Pick(3, 99, 0x01, 28, gone) == 14, "an unusable program is passed over");
        ix.Remove(3, 11); ix.Remove(3, 14);
        Require(ix.Pick(3, 99, 0x01, 12, all) == 11 ? false : true, "removed entries are gone");
    }
    // the line
    {
        Gate g;
        g.Picture(1); g.Linked(1, 20.0, false, kSyncMustChara); g.Decide(2, kSyncNone, kCacheHit, false); g.DrawSkipped(true, false); g.DrawSkipped(false, true);
        g.Flush();
        char t[900];
        g.c.Format(t, sizeof t, "TestGalaxy", 2);
        std::printf("%s\n", t);
        Require(std::strstr(t, "Mario/main character 1 (20 ms)") && std::strstr(t, "asynclink: stage TestGalaxy: mode 2"), "the line names the synchronous link and its reason");
    }
    std::printf("PASS async link model\n");
    return 0;
}
