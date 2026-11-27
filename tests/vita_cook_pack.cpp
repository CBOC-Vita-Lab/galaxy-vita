// cook24: host test of the user shader pack (platform/vgx/vita/vgx_cook_pack.h): round trip, every truncation and every flipped byte refused,
// ABI mismatch, A/B slots (a torn newer slot falls back to the older one), LRU eviction under a cap, bounds, duplicates, bad records.
#include "../platform/vgx/vita/vgx_cook_pack.h"
#include <filesystem>
#include <string>
#include <vector>

using namespace vgx::vita;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::vector<uint8_t> MakeRecord(uint32_t stage, unsigned seed, uint32_t gxpSize) {
    // u32 matrices(1) + index 0 + a GXP of gxpSize bytes: "GXP\0", size at +8, stage bit at +20, no parameters
    std::vector<uint8_t> r(8 + ((gxpSize + 3u) & ~3u), 0);
    cook::W32(r.data(), 1);
    uint8_t* g = r.data() + 8;
    std::memcpy(g, "GXP", 4);
    cook::W32(g + 8, gxpSize);
    cook::W32(g + 20, stage - 1u);
    for (uint32_t i = 156; i < gxpSize; ++i) g[i] = static_cast<uint8_t>(seed * 31u + i);
    // one parameter so that matrix index 0 is legal: count at +36, table offset at +40 (base = 40 + off >= 156)
    cook::W32(g + 36, 1);
    cook::W32(g + 40, 116);  // base 156
    return r;
}
static std::string Hex16(uint64_t h) { char b[32]; std::snprintf(b, sizeof b, "%llX", static_cast<unsigned long long>(h)); return b; }
static std::string PathFor(const std::string& root, uint32_t stage, uint64_t h) {
    char b[512]; std::snprintf(b, sizeof b, "%s/%c/%02X/%s.gxp", root.c_str(), stage == 1 ? 'v' : 'f', unsigned(h >> 56), Hex16(h).c_str());
    return b;
}
static std::vector<uint8_t> Slurp(const std::string& p) {
    std::vector<uint8_t> v; FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return v;
    uint8_t b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n); std::fclose(f); return v;
}
static void Spit(const std::string& p, const std::vector<uint8_t>& v) { FILE* f = std::fopen(p.c_str(), "wb"); if (!v.empty()) std::fwrite(v.data(), 1, v.size(), f); std::fclose(f); }

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "/tmp/vita_cook_pack_test";
    std::filesystem::create_directories(dir);
    for (const char* n : {"user.pack.a", "user.pack.b"}) std::filesystem::remove(dir + "/" + n);
    const std::string root = dir + "/v3";
    const uint64_t abi = 0x1234abcd5678ef01ull;

    // fresh records, hashes spread over both stages
    std::vector<cook::NewRecord> fresh;
    for (unsigned i = 0; i < 50; ++i) {
        const uint64_t h = (uint64_t(i * 2654435761u) << 32) | (0x10000000u + i * 977u);
        fresh.push_back({i % 2 ? 2u : 1u, h, MakeRecord(i % 2 ? 2u : 1u, i, 200 + (i % 7) * 40)});
    }
    ShaderPack empty;
    cook::Loaded none;
    std::vector<uint8_t> file;
    cook::MergeStats st;
    CHECK(cook::Merge(empty, none, {}, fresh, abi, 4u << 20, file, st));
    CHECK(st.added == 50 && st.kept == 50 && st.evicted == 0);
    int slot = -1;
    CHECK(cook::Save(dir.c_str(), -1, file, 4096, nullptr, nullptr, &slot) && slot == 0);

    // round trip: every record answers, bit exact, by the path vitaGL would ask with
    {
        ShaderPack p; cook::Loaded ld;
        CHECK(cook::Load(dir.c_str(), root.c_str(), abi, p, ld));
        CHECK(ld.slot == 0 && ld.gen == 1 && p.Entries() == 50 && ld.ages.size() == 50);
        for (const auto& r : fresh) {
            const void* d = nullptr; size_t n = 0;
            CHECK(p.FindIndex(PathFor(root, r.stage, r.hash).c_str(), &d, &n) >= 0 && n == r.data.size() && !std::memcmp(d, r.data.data(), n));
        }
        const void* d; size_t n;
        CHECK(p.FindIndex(PathFor(root, 1, 0x7777777777777777ull).c_str(), &d, &n) < 0);
    }
    // ABI mismatch (a driver / shader generator change) refuses the file
    { ShaderPack p; cook::Loaded ld; CHECK(!cook::Load(dir.c_str(), root.c_str(), abi ^ 1, p, ld)); CHECK(ld.slotsRejected == 1); }

    // truncation at every length and a flipped byte at every offset: refused, never accepted, never a crash (ASan/UBSan clean)
    {
        const std::vector<uint8_t> good = Slurp(dir + "/user.pack.a");
        CHECK(good == file);
        unsigned accepted = 0, tried = 0;
        const std::string bad = dir + "/bad.pack";
        for (size_t cut = 0; cut < good.size(); cut += (cut < 400 || cut + 400 > good.size()) ? 1 : 37) {
            std::vector<uint8_t> v(good.begin(), good.begin() + cut);
            std::filesystem::create_directories(dir + "/t"); Spit(dir + "/t/user.pack.a", v);
            std::filesystem::remove(dir + "/t/user.pack.b");
            ShaderPack p; cook::Loaded ld; ++tried;
            if (cook::Load((dir + "/t").c_str(), root.c_str(), abi, p, ld)) ++accepted;
        }
        CHECK(accepted == 0);
        unsigned flipAccepted = 0, flipBad = 0, flips = 0;
        const size_t off = cook::R32(good.data() + 20);
        for (size_t at = 0; at < good.size(); at += (at < 400 || at + 400 > good.size()) ? 1 : 13) {
            std::vector<uint8_t> v = good; v[at] ^= 0x40; Spit(dir + "/t/user.pack.a", v);
            ShaderPack p; cook::Loaded ld; ++flips;
            if (cook::Load((dir + "/t").c_str(), root.c_str(), abi, p, ld)) {
                ++flipAccepted;
                // only the generation and the LRU ages are outside the image's checksum (they change a rank, never a shader)
                if (!((at >= 8 && at < 16) || (at >= cook::kPrefixBytes && at < cook::kPrefixBytes + 4u * 50) )) ++flipBad;
                (void)off;
            }
        }
        CHECK(flipBad == 0);
        std::printf("cook-test: %u truncations all refused; %u flips: %u accepted, all in the generation / age table (%u elsewhere)\n", tried, flips, flipAccepted, flipBad);
        std::filesystem::remove(bad);
    }

    // A/B: the next save goes to the other slot; a torn newer slot (cut, or never finished) leaves the older generation in force
    {
        ShaderPack p; cook::Loaded ld;
        CHECK(cook::Load(dir.c_str(), root.c_str(), abi, p, ld) && ld.slot == 0);
        std::vector<cook::NewRecord> more;
        more.push_back({1u, 0x0100000000000001ull, MakeRecord(1, 900, 300)});
        std::vector<uint8_t> used(p.Entries(), 0);
        std::vector<uint8_t> f2; cook::MergeStats s2;
        CHECK(cook::Merge(p, ld, used, more, abi, 4u << 20, f2, s2) && s2.added == 1 && s2.kept == 51);
        int w = -1;
        // a save that is stopped halfway (the game started a stage): the slot has no magic, the pack in use is untouched
        struct Stop { static bool Now(void*) { return false; } };
        CHECK(!cook::Save(dir.c_str(), ld.slot, f2, 512, Stop::Now, nullptr, &w));
        { ShaderPack q; cook::Loaded l2; CHECK(cook::Load(dir.c_str(), root.c_str(), abi, q, l2) && l2.slot == 0 && l2.gen == 1 && q.Entries() == 50); }
        // a save that is cut after the body but before the prefix: same
        { std::vector<uint8_t> cut(f2.begin(), f2.end() - 100); Spit(dir + "/user.pack.b", cut);
          ShaderPack q; cook::Loaded l2; CHECK(cook::Load(dir.c_str(), root.c_str(), abi, q, l2) && l2.slot == 0 && q.Entries() == 50); }
        // a complete save: the newer generation wins and both slots stay on the card
        CHECK(cook::Save(dir.c_str(), ld.slot, f2, 4096, nullptr, nullptr, &w) && w == 1);
        { ShaderPack q; cook::Loaded l2; CHECK(cook::Load(dir.c_str(), root.c_str(), abi, q, l2) && l2.slot == 1 && l2.gen == 2 && q.Entries() == 51); }
        // the newer slot damaged afterwards: the older one is used (still generation 1, 50 records)
        { std::vector<uint8_t> v = Slurp(dir + "/user.pack.b"); v[v.size() / 2] ^= 1; Spit(dir + "/user.pack.b", v);
          ShaderPack q; cook::Loaded l2; CHECK(cook::Load(dir.c_str(), root.c_str(), abi, q, l2) && l2.slot == 0 && l2.gen == 1 && l2.slotsRejected == 1); }
        // restore b for the next test
        CHECK(cook::Save(dir.c_str(), 0, f2, 4096, nullptr, nullptr, &w));
    }

    // duplicates and invalid records are dropped; LRU under a cap: unused oldest records go first, used ones stay
    {
        ShaderPack p; cook::Loaded ld;
        CHECK(cook::Load(dir.c_str(), root.c_str(), abi, p, ld) && ld.gen == 2);
        std::vector<cook::NewRecord> junk;
        junk.push_back({1u, 0x0100000000000001ull, MakeRecord(1, 900, 300)});  // already in the pack
        cook::NewRecord bad{2u, 0x0200000000000009ull, MakeRecord(2, 1, 300)};
        bad.data[8] = 'X';                                                     // not a GXP
        junk.push_back(bad);
        cook::NewRecord wrongStage{2u, 0x0200000000000010ull, MakeRecord(1, 1, 300)};  // a vertex image offered as a fragment record
        junk.push_back(wrongStage);
        std::vector<uint8_t> used(p.Entries(), 0), f3; cook::MergeStats s3;
        CHECK(cook::Merge(p, ld, used, junk, abi, 4u << 20, f3, s3) && s3.added == 0 && s3.duplicate == 1 && s3.kept == 51);
        // cap to ~half: 10 records are marked used (they get the new generation and must survive)
        size_t half = ShaderPack::kHeaderBytes;
        for (uint32_t i = 0; i < 25; ++i) { uint32_t s_; uint64_t h; const uint8_t* d; uint32_t n; p.At(i, s_, h, d, n); half += ShaderPack::kEntryBytes + ((n + 3u) & ~3u); }
        std::vector<uint8_t> usedSome(p.Entries(), 0);
        std::vector<uint64_t> keep;
        for (uint32_t i = 30; i < 40; ++i) { usedSome[i] = 1; uint32_t s_; uint64_t h; const uint8_t* d; uint32_t n; p.At(i, s_, h, d, n); keep.push_back(h); }
        std::vector<uint8_t> f4; cook::MergeStats s4;
        CHECK(cook::Merge(p, ld, usedSome, {}, abi, half, f4, s4));
        CHECK(s4.bytes <= half + 4u * s4.kept + 64 + 8 && s4.evicted > 0 && s4.kept < 51);
        Spit(dir + "/lru.pack", f4);
        ShaderPack q; cook::Loaded l4;
        std::filesystem::create_directories(dir + "/l"); Spit(dir + "/l/user.pack.a", f4);
        std::filesystem::remove(dir + "/l/user.pack.b");
        CHECK(cook::Load((dir + "/l").c_str(), root.c_str(), abi, q, l4) && l4.gen == 3);
        unsigned found = 0;
        for (uint32_t i = 0; i < q.Entries(); ++i) { uint32_t s_; uint64_t h; const uint8_t* d; uint32_t n; q.At(i, s_, h, d, n); for (uint64_t k : keep) if (k == h) ++found; }
        CHECK(found == keep.size());
        std::printf("cook-test: cap %zu bytes -> kept %u of 51 records (%zu bytes), evicted %u, the 10 records used this run all kept\n", half, s4.kept, s4.bytes, s4.evicted);
    }

    // bounds: the 8192-record / 8 MiB limits hold; an over-large record is refused
    {
        ShaderPack p; cook::Loaded ld;
        std::vector<cook::NewRecord> many;
        for (unsigned i = 0; i < 9000; ++i) many.push_back({1u, (uint64_t(i + 1) << 40) | i, MakeRecord(1, i, 200)});
        std::vector<uint8_t> f5; cook::MergeStats s5;
        CHECK(cook::Merge(p, ld, {}, many, abi, 64u << 20, f5, s5));
        CHECK(s5.kept == 8192 && f5.size() <= cook::kMaxFileBytes);
        ShaderPack::Record big{1u, 1, nullptr, 300u << 10};
        std::vector<uint8_t> out;
        CHECK(!ShaderPack::ValidRecord(MakeRecord(1, 1, 160).data(), 300u << 10, 1));
        (void)big; (void)out;
        std::printf("cook-test: 9000 offered, %u kept (limit 8192), file %zu bytes\n", s5.kept, f5.size());
    }
    std::printf("%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
