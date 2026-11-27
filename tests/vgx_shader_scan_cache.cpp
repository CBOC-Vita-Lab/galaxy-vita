#define main quiet_fixture_main
#define VGX_TEST_NATIVE_CAPACITY
unsigned pressureScanVisits = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
static unsigned queryCount = 0, utilities = 0, deleted = 0;
static uint64_t digest = 1469598103934665603ull;
static std::vector<GLuint> deletedNames;
static std::vector<std::pair<uint64_t, GLuint>> deletedEvents;
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() { return __real_glCreateProgram(); }
static unsigned Residents() {
    unsigned n = 0;
    for (const auto& entry : vgx::backend::g_programs) n += entry.second.id != 0;
    return n;
}
extern "C" unsigned vglGetFreeProgramSlots() {
    ++queryCount; Require(Residents() + utilities <= 1024, "fixture native table valid");
    return 1024 - Residents() - utilities;
}
extern "C" void __wrap_glDeleteProgram(GLuint id) {
    ++deleted; deletedNames.push_back(id);
    deletedEvents.emplace_back(vgx::backend::g_serial, id);
    digest = (digest ^ id) * 1099511628211ull;
    digest = (digest ^ vgx::backend::g_serial) * 1099511628211ull;
}
static void Add(unsigned key, uint64_t linked, uint64_t used = 0) {
    vgx::backend::Program p; p.id = key + 1; p.ok = true; p.linkSerial = linked; p.usedSerial = used;
    vgx::backend::g_programs[key] = p;
}
int main(int argc, char** argv) {
    Require(argc == 2, "one mode"); const std::string mode = argv[1];
    using namespace vgx::backend;
    setenv("GXV_MAXPROGRAMS", mode == "off" ? "0" : "900", 1);
    if (mode == "reset") {
#ifdef PRESSURE_SCAN_CACHE
        Context(); g_capacityNextScanSerial = 999999;
        Require(Init(32,32), "actual Init succeeds");
        Require(g_capacityNextScanSerial == 0, "actual Init invalidates scan deadline");
#endif
        return 0;
    }
    // Keep the two new/retried entries at the legacy soft limit so this case
    // isolates pressure eligibility; ordinary above-limit eviction has its own case.
    const unsigned count = mode == "utility" ? 800 : mode == "trace" ? 850 : mode == "new-retry" ? 898 : 900;
    for (unsigned i = 0; i < count; ++i) Add(i, 1000, 1000);
    utilities = 900 - count; g_serial = 1000; g_nativeCapacityPressure = true;
    EvictPrograms(); const unsigned firstVisits = pressureScanVisits;
    if (mode == "trace") {
        unsigned seed = 7, nextKey = 900;
        for (unsigned step = 0; step < 10000; ++step) {
            seed = seed * 1664525u + 1013904223u; g_serial += 1 + (seed % 7);
            if (step % 3 == 0 && Residents() + utilities < 1024) Add(nextKey++, g_serial);
            if (step % 5 == 0 && !g_programs.empty()) {
                auto it = g_programs.begin(); std::advance(it, seed % g_programs.size());
                it->second.usedSerial = g_serial;
            }
            if (step % 13 == 0) {
                vgx::backend::Program failed; failed.failures = 2; g_programs.emplace(nextKey++, failed);
            }
            if (step % 17 == 0 && Residents() + utilities < 1024) {
                for (auto& entry : g_programs) if (!entry.second.id) {
                    Add(entry.first, g_serial); break;
                }
            }
            if (step % 19 == 0) utilities = std::min(100u, 1024 - Residents());
            if (step % 23 == 0) utilities = 0;
            if (Residents() + utilities >= 900) g_nativeCapacityPressure = true;
            EvictPrograms(); EvictPrograms();
        }
        char text[256]; int n = std::snprintf(text, sizeof text,
            "TRACE deleted=%u digest=%llu resident=%u metadata=%zu serial=%llu\n", deleted,
            static_cast<unsigned long long>(digest), Residents(), g_programs.size(), static_cast<unsigned long long>(g_serial));
        ::write(STDOUT_FILENO, text, n);
        for (const auto& event : deletedEvents) {
            const int len = std::snprintf(text, sizeof text, "EVENT serial=%llu id=%u\n", static_cast<unsigned long long>(event.first), event.second);
            ::write(STDOUT_FILENO, text, len);
        }
        Require(deleted == 3975 && digest == 15334784246378271531ull && Residents() == 798 && g_programs.size() == 979,
                "randomized retirement matches frozen V2 trace");
        return 0;
    }
    if (mode == "off") {
        for (g_serial = 1001; g_serial < 1700; ++g_serial) EvictPrograms();
        Require(pressureScanVisits == 0 && deleted == 0 && queryCount == 700, "off keeps query but never scans/reclaims");
        return 0;
    }
    Require(firstVisits == count, "first full scan visits each resident");
    if (mode == "utility") {
        utilities = 0; ++g_serial; EvictPrograms();
        Require(!g_nativeCapacityPressure && pressureScanVisits == firstVisits, "native utility release clears pressure without scan");
        utilities = 100; g_nativeCapacityPressure = true;
    }
    if (mode == "new-retry") {
        g_serial = 1100; Add(900, g_serial);
        g_programs[901] = vgx::backend::Program{}; g_programs[901].failures = 2;
        g_serial = 1200; Add(901, g_serial);
    }
    if (mode == "legacy") {
        g_serial = 1100; for (unsigned i = 900; i < 1002; ++i) Add(i, g_serial);
        EvictPrograms();
        Require(deleted == 102 && g_programs.size() == 900, "ordinary use-only eviction unchanged");
    }
    const unsigned beforeQueries = queryCount;
    EvictPrograms(); EvictPrograms();
    Require(queryCount <= beforeQueries + 1, "same serial adds at most one query");
    for (g_serial += 1; g_serial <= 1600; ++g_serial) EvictPrograms();
    Require(pressureScanVisits == firstVisits, "no protected pressure rescan before saved deadline");
    if (mode == "postpone") for (auto& entry : g_programs) entry.second.usedSerial = 1500;
    g_serial = 1601; EvictPrograms();
    if (mode == "postpone") {
        Require(deleted == 0 && pressureScanVisits == firstVisits + count, "demand use postpones eligibility at deadline");
        for (g_serial = 1602; g_serial <= 2100; ++g_serial) EvictPrograms();
        Require(pressureScanVisits == firstVisits + count, "postponed cache skips full scans");
        g_serial = 2101; EvictPrograms();
    }
    Require(deleted == (mode == "legacy" ? 106u : 4u), "deadline releases exactly four additional names");
    if (mode == "new-retry") Require(g_programs.count(900) && g_programs.count(901), "new and retry links cannot retire early");
    const auto before = deleted; EvictPrograms(); Require(deleted == before, "same serial never doubles pressure batch");
    char text[200]; const int n = std::snprintf(text,sizeof text,"PASS %s scans=%u queries=%u deletes=%u\n",argv[1],pressureScanVisits,queryCount,deleted);
    ::write(STDOUT_FILENO,text,n);
}
