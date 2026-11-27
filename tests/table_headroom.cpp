// strk1118: headroom of the fixed-capacity default / fan-out tables of the settings plan (the Strikers port's g_defaulted[160] filled up and defaults
// added after it were silently not undone). Fails when a table is above 75 % of its capacity, so the next row added tells whoever adds it to widen it first.
// Also: an index that does not fit is counted, not ignored. Build and run: tools/test_table_headroom.sh
#include "port/across.h"
#include "port/cpulod.h"
#include "port/mode_keys.h"
#include "port/perf_defaults.h"
#include "port/quality30.h"
#include "port/quality_set.h"
#include <stdio.h>

static int sFail = 0;
#define CHECK(c) do { if (!(c)) { ++sFail; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void Gate(const char* name, int use, int cap) {
    printf("  %-34s %3d of %3d (%2d %%)\n", name, use, cap, use * 100 / cap);
    if (use * 4 > cap * 3) { ++sFail; fprintf(stderr, "FAIL %s: %d of %d is above 75 %% of its capacity: widen the table (and its mask) first\n", name, use, cap); }
}

int main() {
    using namespace port;
    Gate("cpulod fan-out record (g_fanned)", cpulod::kMemberCount, cpulod::kFannedCap);
    Gate("across fan-out record (g_fanned)", across::kMemberCount, across::kFannedCap);
    Gate("perf defaults: cpulod user mask", cpulod::kMemberCount, 64);
    Gate("perf defaults: across user mask", across::kMemberCount, 32);
    Gate("mode keys: rows vs the 128-bit user mask", modekeys::kRowCount, 128);
    Gate("quality set: members vs the 32-bit mask", qualityset::kMemberCount, 32);
    // overflow is counted, and the Quality census says so
    cpulod::g_fannedLost = 0; across::g_fannedLost = 0;
    cpulod::NoteFanned(cpulod::kFannedCap);
    cpulod::NoteFanned(-1);
    across::NoteFanned(across::kFannedCap + 5);
    CHECK(cpulod::g_fannedLost == 2);
    CHECK(across::g_fannedLost == 1);
    cpulod::NoteFanned(cpulod::kFannedCap - 1);
    CHECK(cpulod::g_fanned[cpulod::kFannedCap - 1] && cpulod::g_fannedLost == 2);
    char text[900];
    quality30::Applied() = false;
    const bool first = quality30::Apply([](const char*) -> const char* { return nullptr; }, [](const char*, const char*) {}, text, sizeof text);
    CHECK(first && strstr(text, "WARNING: 2 cpulod and 1 across"));
    cpulod::g_fannedLost = 0; across::g_fannedLost = 0;
    quality30::Applied() = false;
    quality30::Apply([](const char*) -> const char* { return nullptr; }, [](const char*, const char*) {}, text, sizeof text);
    CHECK(!strstr(text, "WARNING"));
    // a member just inside the capacity is recorded and rewritten for Quality
    cpulod::g_fanned[cpulod::kFannedCap - 1] = false;
    printf(sFail ? "table headroom: FAIL\n" : "table headroom: ok\n");
    return sFail ? 1 : 0;
}
