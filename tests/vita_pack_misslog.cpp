// packrefill25: host test of the shader pack's miss log (platform/vgx/vita/vgx_pack_misslog.h): name parsing (full path / relative; every malformed
// shape refused), the first-miss set (distinct, both kinds, hash 0, overflow, 4 threads racing on overlapping keys: exactly one winner per key),
// the log line round trip, the 60-second summary (windows, deltas, no line before the period), and a file of real lines for tools/pack25/misskeys.py
// (argv[1] = the folder to write misses.log into; tools/test_vita_pack_misslog.sh then runs the Python side over it).
#include "../platform/vgx/vita/vgx_pack_misslog.h"
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace vgx::vita;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static uint64_t Mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x; }

int main(int argc, char** argv) {
    // ---- ParseName
    uint32_t kind = 0; uint64_t hash = 0;
    CHECK(PackMissLog::ParseName("v/3A/3A1B2C3D4E5F6071.gxp", kind, hash) && kind == 1 && hash == 0x3A1B2C3D4E5F6071ull);
    CHECK(PackMissLog::ParseName("ux0:data/smg/shadercache/b4d4c2a9/v3/f/E1/E1B74C3C924D80FF.gxp", kind, hash) && kind == 2 && hash == 0xE1B74C3C924D80FFull);
    CHECK(PackMissLog::ParseName("f/00/1.gxp", kind, hash) && kind == 2 && hash == 1);                    // top byte 0: HH = 00, one digit
    CHECK(PackMissLog::ParseName("v/FF/FFFFFFFFFFFFFFFF.gxp", kind, hash) && hash == ~0ull);
    CHECK(!PackMissLog::ParseName("v/3A/3a1b2c3d4e5f6071.gxp", kind, hash));                               // lower case
    CHECK(!PackMissLog::ParseName("v/3B/3A1B2C3D4E5F6071.gxp", kind, hash));                               // HH is not the top byte
    CHECK(!PackMissLog::ParseName("v/00/0A.gxp", kind, hash));                                             // leading zero
    CHECK(!PackMissLog::ParseName("x/3A/3A1B2C3D4E5F6071.gxp", kind, hash));                               // kind
    CHECK(!PackMissLog::ParseName("v/3A/3A1B2C3D4E5F60712.gxp", kind, hash));                              // 17 digits
    CHECK(!PackMissLog::ParseName("v/3A/3A1B2C3D4E5F6071.gx", kind, hash));
    CHECK(!PackMissLog::ParseName("v3A/3A1B2C3D4E5F6071.gxp", kind, hash));
    CHECK(!PackMissLog::ParseName("/v/3A/3A1B2C3D4E5F6071.gxp/", kind, hash));
    CHECK(!PackMissLog::ParseName("", kind, hash) && !PackMissLog::ParseName(nullptr, kind, hash));
    CHECK(!PackMissLog::ParseName("v/3A/.gxp", kind, hash));
    // ---- a disabled log never says "first"
    { PackMissLog off; CHECK(!off.Enabled() && !off.First(1, 5) && off.Distinct() == 0); }
    // ---- the set
    PackMissLog log;
    CHECK(log.Init() && log.Enabled() && log.Init());
    CHECK(log.First(1, 0x1234) && !log.First(1, 0x1234) && log.First(2, 0x1234) && !log.First(2, 0x1234));   // each kind its own
    CHECK(log.First(1, 0) && !log.First(1, 0) && !log.First(1, 1));                                          // 0 and 1 share an entry
    CHECK(!log.First(0, 7) && !log.First(3, 7));                                                              // not a stage
    CHECK(log.Distinct() == 3);
    // fill one kind completely: every distinct key once, then the table is full: overflow counted, nothing logged
    {
        PackMissLog full; full.Init();
        unsigned firsts = 0;
        for (uint32_t i = 0; i < PackMissLog::kSlots; ++i) firsts += full.First(2, Mix(i + 100) | 2) ? 1 : 0;
        CHECK(firsts == PackMissLog::kSlots && full.Distinct() == PackMissLog::kSlots && full.Overflow() == 0);
        CHECK(!full.First(2, Mix(999999) | 2) && full.Overflow() == 1);
        CHECK(!full.First(2, Mix(100) | 2) && full.Overflow() == 1);          // a known key still answers "not first" without counting
        CHECK(full.First(1, 77));                                              // the other kind has its own table
    }
    // ---- 4 threads, overlapping keys: exactly one "first" per key
    {
        PackMissLog race; race.Init();
        const unsigned N = 3000;
        std::atomic<unsigned> firsts{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t) th.emplace_back([&, t] { for (unsigned i = 0; i < N; ++i) { const unsigned k = (i * (t + 1) * 7) % N; if (race.First(1 + (k & 1), Mix(k) | 4)) firsts.fetch_add(1); } });
        for (auto& x : th) x.join();
        std::set<unsigned> keys; for (int t = 0; t < 4; ++t) for (unsigned i = 0; i < N; ++i) keys.insert((i * (t + 1) * 7) % N);
        CHECK(firsts.load() == keys.size() && race.Distinct() == keys.size());
    }
    // ---- the line
    {
        char line[200];
        const int n = PackMissLog::FormatMiss(line, sizeof line, 2, 0x3A1B2C3D4E5F6071ull, 0x099e9a42u, 1, 123456, 17);
        CHECK(n > 0 && std::strcmp(line, "pack-miss: f/3A/3A1B2C3D4E5F6071.gxp stage=099e9a42 play=1 t=123456 n=17") == 0);
        CHECK(PackMissLog::ParseMissLine(line, kind, hash) && kind == 2 && hash == 0x3A1B2C3D4E5F6071ull);
        std::string prefixed = std::string("[  123.456] ") + line + "\r\n";                      // the log's own prefix and a CRLF
        CHECK(PackMissLog::ParseMissLine(prefixed.c_str(), kind, hash) && hash == 0x3A1B2C3D4E5F6071ull);
        CHECK(!PackMissLog::ParseMissLine("pack-miss: f/3A/zz.gxp stage=0", kind, hash) && !PackMissLog::ParseMissLine("pack: hits 1 misses 2", kind, hash));
        char tiny[10];
        CHECK(PackMissLog::FormatMiss(tiny, sizeof tiny, 1, 5, 0, 0, 0, 1) == 0);               // too small: no truncated line
        CHECK(PackMissLog::FormatMiss(line, sizeof line, 1, 0x0A, 0, 0, 0, 1) > 0 && std::strstr(line, "v/00/A.gxp"));   // no leading zero in the digits
    }
    // ---- the summary
    {
        PackSummary s(60000);
        PackSummary::Totals now, d;
        uint64_t w = 0;
        CHECK(!s.Tick(1000, now, d, w));                                   // the first call only starts the window
        now.hits = 500; now.misses = 7; now.compiles = 3; now.compilesInPlay = 1;
        CHECK(!s.Tick(30000, now, d, w) && !s.Tick(60999, now, d, w));     // 59,999 ms in: not yet
        CHECK(s.Tick(61000, now, d, w) && d.hits == 500 && d.misses == 7 && d.compiles == 3 && d.compilesInPlay == 1 && w == 60000);
        now.hits = 530; now.misses = 7; now.compiles = 3; now.compilesInPlay = 1; now.distinct = 7;
        CHECK(!s.Tick(100000, now, d, w));
        now.hits = 530; CHECK(s.Tick(121000, now, d, w) && d.hits == 30 && d.misses == 0 && d.compilesInPlay == 0 && d.distinct == 7);
        char line[400];
        CHECK(PackSummary::FormatSummary(line, sizeof line, d, w, now) > 0);
        CHECK(std::strncmp(line, "pack: hits 30 misses 0 compiled-in-play 0 (window 60 s:", 55) == 0);
        std::printf("summary line: %s\n", line);
        CHECK(!s.Tick(100, now, d, w));                                    // a clock that went back: no line, no underflow
    }
    // ---- a file of real lines for the Python side: 40 distinct keys (4 of them in play) logged 1-3 times, summaries, a banner
    if (argc > 1) {
        std::string path = std::string(argv[1]) + "/misses.log";
        FILE* f = std::fopen(path.c_str(), "w");
        CHECK(f != nullptr);
        if (f) {
            PackMissLog l; l.Init();
            std::fprintf(f, "[    1.000] pack: GXV_PACK_MISSLOG=1: one line per first pack miss\n");
            unsigned ordinal = 0;
            for (int rep = 0; rep < 3; ++rep)
                for (uint32_t i = 0; i < 40; ++i) {
                    const uint64_t h = Mix(i + 1) | 8; const uint32_t k = 1 + (i & 1);
                    if (rep == 0 || (i % 3) == 0) if (l.First(k, h)) { char line[200]; PackMissLog::FormatMiss(line, sizeof line, k, h, 0x099e9a42u + (i % 4), i >= 36 ? 1 : 0, 1000 + i, ++ordinal); std::fprintf(f, "[%9.3f] %s\n", 2.0 + i, line); }
                }
            PackSummary::Totals now, d, tot; tot.hits = 900; tot.misses = 40; tot.compiles = 40; tot.compilesInPlay = 0; tot.distinct = 40;
            d = tot; char line[400];
            PackSummary::FormatSummary(line, sizeof line, d, 60000, tot);
            std::fprintf(f, "[   61.000] %s\n", line);
            std::fclose(f);
            std::printf("wrote %s (%u keys)\n", path.c_str(), ordinal);
            CHECK(ordinal == 40);
        }
    }
    std::printf(g_fail ? "FAILED: %d\n" : "vita_pack_misslog: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
