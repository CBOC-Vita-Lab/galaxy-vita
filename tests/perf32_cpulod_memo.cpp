// perf32 (20 Nov 2026): host test of GXV_CPULOD_MEMO (platform/include/port/cpulod.h): TableValueMemo (the member found through the address memo, PerfValue's
// name chain as a class table) against TableValueOld (the search and the chain as before), for every member and non-member name, every level (plain, the last
// resort, coded Quality levels), both picture modes and every far-field setting; names passed as the table's own pointers, as literals, as copies at other
// addresses (so that memo slots collide and are overwritten) and from two threads at once. The answer and the value's bits must be the same, and `out` must
// be left alone when the answer is "no". -DCPULOD_MEMO_NEG=1: the negative control (the memo's hit not confirmed against the name): must FAIL.
//   tools/test_perf32.sh
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "port/cpulod.h"
using namespace port::cpulod;

static std::atomic<unsigned long long> gChecks{0}, gBad{0};
#ifdef CPULOD_MEMO_NEG
// (the control: a memo that trusts its slot)
static bool Memo(int level, const char* name, float* out) {
    if (!LevelOk(level)) return false;
    const unsigned h = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(name)) * 2654435761u) >> 27;
    unsigned c = g_memoSlot[h].load();
    int i;
    if (c != 0) i = static_cast<int>(c) - 1; else { i = FindMember(name); if (i >= 0) g_memoSlot[h].store(static_cast<uint8_t>(i + 1)); }
    if (i < 0) return false;
    if (port::stageknob::Quality()) return QualityOf(kMembers[i], level, out);
    if (Coded(level)) return false;
    if (out) *out = PerfValueClass(kMembers[i], kFarClass.c[i], level);
    return true;
}
#else
static bool Memo(int level, const char* name, float* out) { return TableValueMemo(level, name, out); }
#endif
static void One(int level, const char* name) {
    float a, b;
    const uint32_t canary = 0x7fc12345u;   // (a NaN pattern no table value has)
    std::memcpy(&a, &canary, 4);
    std::memcpy(&b, &canary, 4);
    const bool ra = TableValueOld(level, name, &a), rb = Memo(level, name, &b);
    gChecks.fetch_add(1, std::memory_order_relaxed);
    if (ra != rb || std::memcmp(&a, &b, 4) != 0) {
        if (gBad.fetch_add(1) < 5) std::printf("  DIFFERENT: level %#x name %s: old %d %g, memo %d %g\n", level, name, ra ? 1 : 0, (double)a, rb ? 1 : 0, (double)b);
    }
    if (TableValueOld(level, name, nullptr) != Memo(level, name, nullptr)) gBad.fetch_add(1);
}
static std::vector<int> Levels() {
    std::vector<int> l = {-1, 0, 1, 2, 3, 4, 5, 9};
    for (int s = 0; s < 64; ++s) l.push_back(kLockFlag | s);   // (coded: bit 0 the baseline, bits 1..5 the steps 3a..3e)
    return l;
}
static void Sweep(const std::vector<const char*>& names, unsigned seed) {
    const std::vector<int> levels = Levels();
    uint32_t x = seed;
    for (int rep = 0; rep < 40; ++rep)
        for (size_t k = 0; k < names.size() * levels.size(); ++k) {
            x = x * 1664525u + 1013904223u;
            One(levels[(x >> 8) % levels.size()], names[(x >> 16) % names.size()]);
        }
}
int main() {
    // (the class table is PerfValue's chain: every member, far field on, every level; and the names are unique, which the memo's confirmation relies on)
    for (int i = 0; i < kMemberCount; ++i)
        for (int j = i + 1; j < kMemberCount; ++j)
            if (std::strcmp(kMembers[i].name, kMembers[j].name) == 0) { std::printf("FAIL: duplicate member %s\n", kMembers[i].name); return 1; }
    std::vector<std::string> store;
    std::vector<const char*> names;
    static const char* const kLit[] = {"GXV_PB_LOD", "GXV_PB_DECOR", "GXV_PB_PTCL", "GXV_PB_SHADOW", "GXV_PB_FUR", "GXV_PB_ANIM", "GXV_MATLOD", "GXV_MATLOD_DIST", "GXV_FAMPOSE",
                                       "GXV_FAMPOSE_DIST", "GXV_FAMPOSE_MOVE", "GXV_PIECETHIN", "GXV_PIECETHIN_DIST", "GXV_PTCL_THIN", "GXV_PTCL_THIN_D0", "GXV_PTCL_THIN_D1",
                                       "GXV_CHARPOSE", "GXV_CHARPOSE_LINES", "GXV_FARANIM", "GXV_FARDRAW_KINDS", "GXV_NOPE", "", "GXV_PB_LO", "GXV_PB_LODX", "gxv_pb_lod"};
    for (const char* l : kLit) names.push_back(l);
    for (int i = 0; i < kMemberCount; ++i) names.push_back(kMembers[i].name);
    store.reserve(4096);
    for (int c = 0; c < 40; ++c) {   // (copies at other addresses: 32 slots, so they collide and overwrite one another)
        for (int i = 0; i < kMemberCount; ++i) store.push_back(std::string(c % 7, ' ') + kMembers[i].name);
        store.push_back(std::string(c % 7, ' ') + "GXV_UNKNOWN_" + std::to_string(c));
    }
    for (size_t i = 0; i < store.size(); ++i) names.push_back(store[i].c_str() + (i / (kMemberCount + 1)) % 7);
    for (int quality = 0; quality < 2; ++quality)
        for (int far = 0; far < 4; ++far) {
            port::stageknob::ClearAll();
            port::stageknob::Set("GXV_FARFIELD", (far & 1) ? 1.0f : 0.0f);
            port::stageknob::Set("GXV_FARFIELD_LAST", (far & 2) ? 1.0f : 0.0f);
            port::stageknob::Commit();
            port::stageknob::g_quality.store(quality, std::memory_order_relaxed);
            if (FarOn() != ((far & 1) != 0) || LastOn() != ((far & 2) != 0) || port::stageknob::Quality() != (quality != 0)) { std::printf("FAIL: the fixture did not set the mode\n"); return 1; }
            Sweep(names, 12345u + static_cast<unsigned>(quality * 4 + far));
            std::thread t1(Sweep, std::cref(names), 777u), t2(Sweep, std::cref(names), 999u);   // (two threads on the one memo)
            t1.join();
            t2.join();
        }
    // the switch itself: TableValue is the old code at 0, the memo at 1, and counts in verify
    port::stageknob::g_quality.store(0, std::memory_order_relaxed);
    for (int mode = 0; mode < 3; ++mode) {
        setenv("GXV_CPULOD_MEMO", mode == 0 ? "0" : mode == 1 ? "1" : "2", 1);
        port::stageknob::Commit();
        if (MemoMode() != mode) { std::printf("FAIL: MemoMode %d, wanted %d\n", MemoMode(), mode); return 1; }
        for (const char* n : names)
            for (int l : Levels()) {
                float a = -1.0f, b = -1.0f;
                const bool ra = TableValueOld(l, n, &a), rb = TableValue(l, n, &b);
                gChecks.fetch_add(1);
                if (ra != rb || std::memcmp(&a, &b, 4) != 0) gBad.fetch_add(1);
            }
    }
    if (g_memoChecks.load() == 0 || g_memoBad.load() != 0) { std::printf("FAIL: verify mode counted %u checks, %u different\n", (unsigned)g_memoChecks.load(), (unsigned)g_memoBad.load()); return 1; }
    std::printf("perf32_cpulod_memo: %llu lookups compared (%d members, %u names, %u levels, 2 modes, 4 far-field settings, 1 and 2 threads), %llu different; verify mode: %u checked, %u different\n",
                gChecks.load(), kMemberCount, (unsigned)names.size(), (unsigned)Levels().size(), gBad.load(), (unsigned)g_memoChecks.load(), (unsigned)g_memoBad.load());
    std::printf(gBad.load() ? "perf32_cpulod_memo: FAIL\n" : "perf32_cpulod_memo: PASS\n");
    return gBad.load() ? 1 : 0;
}
