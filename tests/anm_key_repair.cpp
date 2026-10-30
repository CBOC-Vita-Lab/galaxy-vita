// Host test of port/anm_key_repair.h against key animations written by tools/cook/j3d.py (tests/anm_key_fixture.py).
//
//   anm_key_repair FIXTURE_DIR            repair tests
//   anm_key_repair --env EXPECTED         checks that the GXV_FIX_BCKCNT in the environment gives Enabled() == EXPECTED
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "port/anm_key_repair.h"

namespace {
int g_fail = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_fail;                                                              \
        }                                                                          \
    } while (0)

using Bytes = std::vector<uint8_t>;
constexpr size_t kBlock = 0x20;  // the block follows the 0x20 byte file header

Bytes ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

uint16_t U16(const Bytes& b, size_t o) {
    uint16_t v;
    std::memcpy(&v, &b[o], 2);
    return v;
}
uint32_t U32(const Bytes& b, size_t o) {
    uint32_t v;
    std::memcpy(&v, &b[o], 4);
    return v;
}
float F32(const Bytes& b, size_t o) {
    float v;
    std::memcpy(&v, &b[o], 4);
    return v;
}
void Put32(Bytes& b, size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); }

// What the game's J3D reader expects of an array: every track of the joint table runs its key times 0, 1, 2, ...
// (the fixture writes them so). Returns the number of times that are wrong.
size_t BadTimes(const Bytes& b) {
    const size_t blk = kBlock;
    const size_t tbl = blk + U32(b, blk + 0x14), sc = blk + U32(b, blk + 0x18), ro = blk + U32(b, blk + 0x1C), tr = blk + U32(b, blk + 0x20);
    size_t bad = 0;
    for (int e = 0; e < 9; ++e) {
        const int kind = e % 3;  // 0 scale, 1 rotation, 2 translation
        const uint16_t keys = U16(b, tbl + e * 6), off = U16(b, tbl + e * 6 + 2);
        for (uint16_t k = 0; k < keys; ++k) {
            if (kind == 1) {
                bad += U16(b, ro + 2 * (off + 3 * static_cast<size_t>(k))) != k;
            } else {
                const size_t base = kind == 0 ? sc : tr;
                bad += F32(b, base + 4 * (off + 3 * static_cast<size_t>(k))) != static_cast<float>(k);
            }
        }
    }
    return bad;
}

struct Counts {
    uint32_t s, r, t;
    bool Big() const { return s >= 0x8000 || r >= 0x8000 || t >= 0x8000; }
    int NumBig() const { return (s >= 0x8000) + (r >= 0x8000) + (t >= 0x8000); }
};

void RunCase(const std::string& dir, const std::string& name) {
    std::fprintf(stderr, "case %s\n", name.c_str());
    Counts c{};
    {
        std::ifstream f(dir + "/" + name + ".counts");
        f >> c.s >> c.r >> c.t;
    }
    const Bytes oldb = ReadFile(dir + "/" + name + ".old.bin");
    const Bytes newb = ReadFile(dir + "/" + name + ".new.bin");
    CHECK(!oldb.empty() && oldb.size() == newb.size());
    if (oldb.empty() || oldb.size() != newb.size()) return;

    // the fixture is what it claims to be
    CHECK(U32(newb, kBlock) == port::anmkey::kTag);
    CHECK(U16(newb, kBlock + 0x0E) == c.s && U16(newb, kBlock + 0x10) == c.r && U16(newb, kBlock + 0x12) == c.t);
    const bool marked = std::memcmp(&newb[kBlock + port::anmkey::kMarkerOffset], port::anmkey::kMarker, 4) == 0;
    CHECK(marked == c.Big());                                        // the cooker stamps the marker only when a count is 0x8000 or more
    CHECK((std::memcmp(&oldb[kBlock + 0x24], port::anmkey::kMarker, 4) == 0) == false);  // the older converter never did
    CHECK((oldb == newb) == !c.Big());                                // counts below the limit: both converters wrote identical bytes
    CHECK(BadTimes(newb) == 0);
    CHECK((BadTimes(oldb) == 0) == !c.Big());                         // old output has big-endian arrays only where a count is 0x8000+

    // switch off: nothing changes
    {
        Bytes b = oldb;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], false) == 0);
        CHECK(b == oldb);
    }
    // switch on: old output becomes exactly the new output, once
    {
        Bytes b = oldb;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], true) == c.NumBig());
        CHECK(b == newb);
        CHECK(BadTimes(b) == 0);
        const Bytes after = b;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], true) == 0);  // second load
        CHECK(b == after);
    }
    // output of the fixed cooker is left alone
    {
        Bytes b = newb;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], true) == 0);
        CHECK(b == newb);
    }
}

void RunGuards(const std::string& dir) {
    std::fprintf(stderr, "guards\n");
    const Bytes oldb = ReadFile(dir + "/all.old.bin");
    const Bytes newb = ReadFile(dir + "/all.new.bin");
    CHECK(!oldb.empty());
    if (oldb.empty()) return;
    CHECK(port::anmkey::RepairInPlace(nullptr, true) == 0);
    {  // not an ANK1 block
        Bytes b = oldb;
        Put32(b, kBlock, 0x414E4632);  // 'ANF2'
        const Bytes before = b;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], true) == 0);
        CHECK(b == before);
    }
    {  // no room for the marker: the joint table starts inside the header
        Bytes b = oldb;
        Put32(b, kBlock + 0x14, 0x24);
        const Bytes before = b;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], true) == 0);
        CHECK(b == before);
    }
    {  // block size too small to hold the header
        Bytes b = oldb;
        Put32(b, kBlock + 4, 0x20);
        const Bytes before = b;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], true) == 0);
        CHECK(b == before);
    }
    {  // an array that runs past the end of the block is skipped; the others are still repaired
        Bytes b = oldb;
        Put32(b, kBlock + 0x1C, U32(b, kBlock + 4) - 10);  // rotation array offset
        const Bytes before = b;
        CHECK(port::anmkey::RepairInPlace(&b[kBlock], true) == 2);
        CHECK(std::memcmp(&b[kBlock + U32(b, kBlock + 0x1C)], &before[kBlock + U32(b, kBlock + 0x1C)], 10) == 0);  // rotation array untouched
        // scale and translation arrays now match the cooker's output
        const size_t sc = kBlock + U32(newb, kBlock + 0x18), sc_end = sc + 4 * 33000;
        CHECK(std::memcmp(&b[sc], &newb[sc], sc_end - sc) == 0);
        const size_t tr = kBlock + U32(newb, kBlock + 0x20), tr_end = tr + 4 * 40000;
        CHECK(std::memcmp(&b[tr], &newb[tr], tr_end - tr) == 0);
        CHECK(std::memcmp(&b[kBlock + port::anmkey::kMarkerOffset], port::anmkey::kMarker, 4) == 0);
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--env") {
        const bool want = std::atoi(argv[2]) != 0;
        CHECK(port::anmkey::Enabled() == want);
        std::fprintf(stderr, "%s\n", g_fail ? "FAIL" : "ok");
        return g_fail ? 1 : 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: anm_key_repair FIXTURE_DIR | --env EXPECTED\n");
        return 2;
    }
    const std::string dir = argv[1];
    for (const char* name : {"small", "edge", "rot", "trans", "scale", "all"}) RunCase(dir, name);
    RunGuards(dir);
    std::fprintf(stderr, "%s (%d failure%s)\n", g_fail ? "FAIL" : "PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
