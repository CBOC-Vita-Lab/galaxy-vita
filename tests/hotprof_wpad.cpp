// GXV_HOTPROF_WPAD host check (hotprof25 round 2): the game's WPadAcceleration.cpp, three objects fed the same samples with the switch
// at 0, 1 and 2; every field must be bit-equal after every update. tools/test_hotprof_wpad.sh
#include "Game/System/WPadAcceleration.hpp"
#include "Game/System/WPad.hpp"
#include <revolution/wpad.h>
#include <revolution/kpad.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#undef vsnprintf  // (port/compat.h points it at the game's own formatter)

static int gMode;
static int gMismatchLines;
static unsigned gKnown;
extern "C" int port_hotprof_wpad_mode(void) { return gMode; }
extern "C" void port_log_summary(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strstr(buf, "MISMATCH")) gMismatchLines++;
    unsigned a, b, c;
    if (sscanf(buf, "wpad-still: %u updates, %u known, %u mismatches", &a, &b, &c) == 3) { gKnown = b; if (c) gMismatchLines++; }
}

static KPADStatus gStatus[4];
static s32 gCount;
static bool gFree = true;
static bool gNull[4];
KPADStatus* WPad::getKPadStatus(u32 index) const { return gNull[index & 3] ? nullptr : &gStatus[index & 3]; }
s32 WPad::getValidStatusCount() const { return gCount; }
extern "C" void KPADSetAccParam(s32, f32, f32) {}
namespace MR { bool isDeviceFreeStyle(const KPADStatus*) { return gFree; } }

struct Rng {
    unsigned long long s;
    unsigned next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (unsigned)(s >> 33); }
    float unit() { return (float)(next() & 0xFFFFFF) / 16777216.0f; }
};

static bool same(const WPadAcceleration& a, const WPadAcceleration& b) {
    return memcmp(&a._10, &b._10, sizeof(a._10)) == 0 && memcmp(&a._1C, &b._1C, 4) == 0 && a._20 == b._20 &&
           memcmp(a.mHistory, b.mHistory, sizeof(a.mHistory)) == 0 && a._624 == b._624 && a._628 == b._628 &&
           memcmp(&a._62C, &b._62C, sizeof(a._62C)) == 0 && memcmp(&a._638, &b._638, sizeof(a._638)) == 0 && a._644 == b._644 &&
           a._648 == b._648 && a._64C == b._64C;
}

int main(int argc, char** argv) {
    const int seeds = argc > 1 ? atoi(argv[1]) : 200;
    static char padMem[3][256];  // (WPad is only used through the two stubs above and mChannel)
    unsigned long long updates = 0, fails = 0, stillTurns = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        Rng r{(unsigned long long)seed * 7919u + 3};
        const u32 device = seed & 1;
        WPadAcceleration* obj[3];
        for (int m = 0; m < 3; m++) obj[m] = new WPadAcceleration(reinterpret_cast<const WPad*>(padMem[m]), device);
        float cur[3] = {0.0f, -1.0f, 0.0f};
        int hold = 0;
        const int turns = 3000;
        for (int t = 0; t < turns; t++) {
            // a held value for a random stretch (often longer than the history), then movement
            if (hold > 0) hold--;
            else {
                const unsigned k = r.next() % 16;
                if (k < 6) hold = 100 + r.next() % 400;        // still for long
                else if (k < 9) hold = r.next() % 140;         // still around the history's length
                else hold = 0;
                for (float& c : cur) c = (r.unit() - 0.5f) * 4.0f;
                const unsigned sp = r.next() % 64;
                if (sp == 0) cur[r.next() % 3] = NAN;
                if (sp == 1) cur[r.next() % 3] = INFINITY;
                if (sp == 2) cur[r.next() % 3] = -INFINITY;
                if (sp == 3) cur[0] = cur[1] = cur[2] = 0.0f;
                if (sp == 4) cur[r.next() % 3] = -0.0f;
                if (sp == 5) cur[r.next() % 3] = 1e-42f;       // (denormal)
                if (sp == 6) cur[r.next() % 3] = 3e38f;
            }
            // within a held stretch a rare sign-of-zero flip: equal by ==, not by bits
            if (hold > 0 && r.next() % 300 == 0 && cur[0] == 0.0f) cur[0] = -cur[0];
            const unsigned ck = r.next() % 32;
            gCount = ck == 0 ? 0 : ck == 1 ? 2 : ck == 2 ? 3 : ck == 3 ? -1 : 1;
            for (int i = 0; i < 4; i++) {
                gNull[i] = r.next() % 200 == 0;
                float v[3] = {cur[0], cur[1], cur[2]};
                if (hold == 0 && i > 0) for (float& c : v) c += (r.unit() - 0.5f) * 0.5f;
                // update() maps acc (x, y, z) to (-x, z, -y)
                gStatus[i].acc.x = -v[0]; gStatus[i].acc.z = v[1]; gStatus[i].acc.y = -v[2];
                gStatus[i].ex_status.fs.acc.x = -v[0]; gStatus[i].ex_status.fs.acc.z = v[1]; gStatus[i].ex_status.fs.acc.y = -v[2];
            }
            gFree = r.next() % 100 != 0;
            if (r.next() % 2000 == 0) { const s32 w = 2 + (r.next() % 2) * 126; for (int m = 0; m < 3; m++) obj[m]->_644 = w; }  // (the rotate window: 2 or 128)
            for (int m = 0; m < 3; m++) { gMode = m; obj[m]->update(); }
            updates++;
            if (hold > 130) stillTurns++;
            if (!same(*obj[0], *obj[1]) || !same(*obj[0], *obj[2])) {
                if (fails++ < 5) printf("FAIL seed %d turn %d: fields differ (mode1 %d, mode2 %d)\n", seed, t, (int)same(*obj[0], *obj[1]), (int)same(*obj[0], *obj[2]));
            }
        }
        // a new object at the address of an old one must start with nothing known
        for (int m = 0; m < 3; m++) { obj[m]->~WPadAcceleration(); new (obj[m]) WPadAcceleration(reinterpret_cast<const WPad*>(padMem[m]), device); }
        gCount = 1; gFree = true; memset(gNull, 0, sizeof(gNull));
        gStatus[0].acc.x = 1.0f; gStatus[0].acc.y = 2.0f; gStatus[0].acc.z = 3.0f; gStatus[0].ex_status.fs.acc = gStatus[0].acc;
        for (int t = 0; t < 140; t++) {
            for (int m = 0; m < 3; m++) { gMode = m; obj[m]->update(); }
            updates++;
            if (!same(*obj[0], *obj[1]) || !same(*obj[0], *obj[2])) { if (fails++ < 5) printf("FAIL seed %d rebuilt object turn %d\n", seed, t); }
        }
        for (int m = 0; m < 3; m++) delete obj[m];
    }
    printf("hotprof_wpad: %llu updates per mode, %llu in long still stretches, known-result updates logged %u, %llu differing, %d mismatch lines\n",
           updates, stillTurns, gKnown, fails, gMismatchLines);
    if (fails || gMismatchLines || gKnown == 0) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
