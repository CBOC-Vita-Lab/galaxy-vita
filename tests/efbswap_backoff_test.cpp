// Host test of GXV_EFBSWAP2_BACKOFF's hold length (vgx_efbswap_backoff.h).
#include "vgx_efbswap_backoff.h"
#include <cstdio>
using namespace vgx::swapback;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, m); ++fails; } else std::printf("PASS: %s\n", m); } while (0)
int main() {
    Backoff b;
    uint64_t t = 1000;
    CHECK(b.Next(7, t) == 16, "first conversion: 16 pictures");
    t += 16 + 10;   // 10 pictures after the hold ended: a second conversion
    CHECK(b.Next(7, t) == 32, "second within 60 pictures: 32");
    t += 32 + 59; CHECK(b.Next(7, t) == 64, "third just inside the window: 64");
    t += 64 + 1; CHECK(b.Next(7, t) == 128, "128");
    t += 128; CHECK(b.Next(7, t) == 256, "256");
    t += 256; CHECK(b.Next(7, t) == 300, "capped at 300");
    t += 300; CHECK(b.Next(7, t) == 300, "stays at 300 for a steady reader (Good Egg)");
    t += 300 + 61; CHECK(b.Next(7, t) == 16, "61 clean pictures after the hold: back to 16");
    CHECK(b.Next(9, t) == 16, "another texture starts at 16 on its own");
    CHECK(b.longest == 300 && b.starts == 2 && b.doubled == 6, "verify counters");
    // one transient wipe costs 16 pictures, not 1800
    Backoff w; CHECK(w.Next(3, 5000) == 16 && w.Next(3, 5000 + 16 + 61) == 16, "a lone transient: 16 each time");
    // a steady reader reaches the cap at the 6th conversion and never exceeds it
    Backoff s2; uint64_t u = 0; uint32_t mx = 0;
    for (int k = 0; k < 40; ++k) { const uint32_t l = s2.Next(1, u); mx = l > mx ? l : mx; u += l + 5; }
    CHECK(mx == 300, "a steady reader is capped at 300 pictures (the old hold was 1800)");
    // (fix24) the table is full at 64 ids: a 65th id is still tracked (the oldest-until entry is replaced) and backs off, and a steady reader on it reaches the cap
    Backoff f; uint64_t ft = 100;
    for (uint32_t id = 1; id <= 64; ++id) { f.Next(id, ft); ft += 3; }
    CHECK(f.e.size() == 64, "table full at 64 ids");
    uint64_t v = ft + 1000; uint32_t fm = 0;
    for (int k = 0; k < 20; ++k) { const uint32_t l = f.Next(500, v); fm = l > fm ? l : fm; v += l + 5; }
    CHECK(fm == 300 && f.e.size() == 64, "a new id after 64 backs off to the cap (evicts the oldest)");
    CHECK(f.Next(1, v + 5000) == 16, "the evicted oldest id restarts at 16 (the first-conversion hold)");
    return fails ? 1 : 0;
}
