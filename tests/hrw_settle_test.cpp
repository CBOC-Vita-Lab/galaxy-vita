// stutter37 host test: port/hrw_settle.h (GXV_HRW_SETTLE / GXV_HRW_SETTLE_RESUME). tools/test_hrw_settle.sh builds and runs it (no dependencies).
#include "../platform/include/port/hrw_settle.h"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using port::hrwsettle::Settle;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// hrw29's rule as vgx_galaxy.cpp had it before stutter37 (HrwSettled), word for word
struct Old {
    uint32_t setKey = 0, settle = 0;
    unsigned need = 90;
    bool Update(uint32_t key, bool live, bool advance) {
        if (!live || key != setKey) {
            setKey = live ? key : 0;
            settle = 0;
            return false;
        }
        if (advance && settle < need) ++settle;
        return settle >= need;
    }
};
static uint32_t Rnd(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

int main() {
    // 1. Without GXV_HRW_SETTLE_RESUME (-1), and with a value at or above GXV_HRW_SETTLE, every answer is the old rule's, on random histories
    for (long resume : {-1L, 200L, 5000L}) {
        for (uint32_t seed = 1; seed <= 40; ++seed) {
            Settle s;
            Old o;
            const unsigned need = seed % 3 == 0 ? 7 : seed % 3 == 1 ? 90 : 200;
            s.Configure(need, resume < 0 ? resume : static_cast<long>(need) + (resume == 200 ? 0 : 50));
            o.need = need;
            uint32_t r = seed * 2654435761u, key = 0;
            bool live = false, loading = false;
            for (int t = 0; t < 20000; ++t) {
                const uint32_t x = Rnd(r);
                if (x % 400 == 0) live = !live;
                if (x % 1700 == 1) { key = (x >> 8) % 4; }
                if (x % 900 == 2) loading = !loading;
                const bool liveNow = live && key != 0 && !loading;   // (as HrwSettled: live already excludes a load)
                const bool adv = (x >> 20) & 1;
                const bool a = s.Update(key, liveNow, loading, adv), b = o.Update(key, liveNow, adv);
                if (a != b) { CHECK(a == b); break; }
            }
        }
    }
    // 2. The short count: the same scene, the screen only covered
    {
        Settle s;
        s.Configure(200, 60);
        int n = 0;
        while (!s.Update(7, true, false, true)) ++n;
        CHECK(n == 200);                       // (the turn that starts the count, then 199 more: the 200th counted turn is the first true)
        CHECK(!s.Resumed());
        CHECK(s.Update(7, true, false, false));   // (an undrawn turn only tests)
        for (int i = 0; i < 40; ++i) CHECK(!s.Update(7, false, false, true));   // a pause / a closed fade: not live, nothing loaded
        CHECK(!s.Update(7, true, false, true));   // the first live turn only starts the count (as the old rule)
        CHECK(s.Resumed() && s.Need() == 60);
        n = 0;
        while (!s.Update(7, true, false, true)) ++n;
        CHECK(n == 59);
        // an undrawn turn does not advance the count
        for (int i = 0; i < 3; ++i) CHECK(!s.Update(7, false, false, true));
        CHECK(!s.Update(7, true, false, true));
        for (int i = 0; i < 500; ++i) CHECK(!s.Update(7, true, false, false));
        CHECK(s.Count() == 0);
    }
    // 3. A load, another scene, no scene: the whole count again
    for (int how = 0; how < 4; ++how) {
        Settle s;
        s.Configure(200, 60);
        while (!s.Update(7, true, false, true)) {}
        if (how == 0) { for (int i = 0; i < 5; ++i) s.Update(7, false, true, true); }                    // the same stage loaded again (a death)
        else if (how == 1) { for (int i = 0; i < 5; ++i) s.Update(0, false, false, true); }              // no scene between two scenes
        else if (how == 2) { s.Update(7, false, false, true); s.Update(9, true, false, true); s.Update(9, false, false, true); }   // another scene was live in between
        else { s.Update(7, false, false, true); s.Update(7, false, true, false); }                       // the load seen by an undrawn turn only
        const uint32_t key = 7;
        CHECK(!s.Update(key, true, false, true));
        CHECK(!s.Resumed() && s.Need() == 200);
        int n = 0;
        while (!s.Update(key, true, false, true)) ++n;
        CHECK(n == 199);
    }
    // 4. A scene that was covered before it had settled has nothing to resume
    {
        Settle s;
        s.Configure(200, 60);
        for (int i = 0; i < 150; ++i) CHECK(!s.Update(7, true, false, true));
        s.Update(7, false, false, true);
        CHECK(!s.Update(7, true, false, true));
        CHECK(!s.Resumed());
        int n = 0;
        while (!s.Update(7, true, false, true)) ++n;
        CHECK(n == 199);
    }
    // 5. The short count is never longer than the whole one, and 0 means the first live drawn turn after the one that starts the count
    {
        Settle s;
        s.Configure(30, 0);
        while (!s.Update(3, true, false, true)) {}
        s.Update(3, false, false, true);
        CHECK(!s.Update(3, true, false, true));
        CHECK(s.Update(3, true, false, true));
        Settle t;
        t.Configure(30, 90);
        while (!t.Update(3, true, false, true)) {}
        t.Update(3, false, false, true);
        t.Update(3, true, false, true);
        CHECK(t.Need() == 30);
    }
    if (fails) { printf("hrw_settle_test: %d FAILED\n", fails); return 1; }
    puts("hrw_settle_test: ok");
    return 0;
}
