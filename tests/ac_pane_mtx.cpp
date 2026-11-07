// GXV_AC_PANEMTX host proof (animcut, tools/test_ac_pane_mtx.sh): the real lyt_pane.cpp CalculateMtx over random pane trees and SRT,
// visibility and location-adjust changes; prints a hash of every pane's local and global matrix and alpha after every turn. The
// script runs it with GXV_AC_PANEMTX=0, 1 and 2 (the switch is read once per process) and requires the same hash.
#include "nw4r/lyt/pane.h"
#include "nw4r/lyt/drawInfo.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <new>
using namespace nw4r::lyt;
extern "C" void port_log_summary(const char* f, ...) {}
static unsigned rs = 1; static unsigned rnd() { rs = rs * 1103515245u + 12345u; return rs >> 8; }
static float rv() { static const float v[] = {0.0f, -0.0f, 1.0f, 0.75f, 90.0f, 12.5f, -33.0f, 1e-30f}; return (rnd() & 1) ? v[rnd() % 8] : (float)(int)(rnd() % 2000 - 1000) / 7.0f; }
static unsigned long long H = 1469598103934665603ull; static size_t N = 0;
static void hb(const void* p, size_t n) { const unsigned char* b = (const unsigned char*)p; for (size_t i = 0; i < n; i++) { H ^= b[i]; H *= 1099511628211ull; } N += n; }
static void run(int seed) {
    rs = seed;
    res::Pane r; memset(&r, 0, sizeof(r)); r.scale.x = r.scale.y = 1.0f; r.alpha = 255; r.flag = 1 | 4;
    std::vector<Pane*> p;
    for (int i = 0; i < 24; i++) { Pane* q = new (malloc(sizeof(Pane))) Pane(&r); q->mTranslate.x = rv(); q->mRotate.z = rv(); if (i) q->mpParent = p[rnd() % i];  // (set directly: the 64-bit host cannot use nw4r::ut lists, whose node offset is 32-bit)
        p.push_back(q); }
    DrawInfo di;
    for (int t = 0; t < 2000; t++) {
        for (int k = rnd() % 4; k > 0; k--) { Pane* q = p[rnd() % p.size()]; int w = rnd() % 9; float v = rv();
            if (w < 3) (&q->mTranslate.x)[w] = v; else if (w < 6) (&q->mRotate.x)[w - 3] = v; else if (w < 8) (&q->mScale.x)[w - 6] = v; else q->SetVisible(!q->IsVisible()); }
        if ((rnd() & 63) == 0) { nw4r::math::VEC2 s; s.x = (rnd() & 1) ? 0.75f : 1.0f; s.y = 1.0f; di.SetLocationAdjustScale(s); di.SetLocationAdjust(rnd() & 1); }
        for (Pane* q : p) q->CalculateMtx(di);  // (parents first: a parent index is below its child's)
        if (!getenv("NOHASH")) for (Pane* q : p) { hb(&q->mMtx, sizeof(q->mMtx)); hb(&q->mGlbMtx, sizeof(q->mGlbMtx)); hb(&q->mGlbAlpha, 1); }
    }
}
int main() {
    // (the switch is read once per process, PORT_HOT_KNOB: the script runs this once per GXV_AC_PANEMTX value and compares the sums)
    for (int s = 1; s <= 8; s++) run(s);
    printf("%016llx %zu\n", H, N); return 0;
}
