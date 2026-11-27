// gpucensus31: the arithmetic of GXV_GPU_CENSUS (vgx_gpucensus.h).
#include "vgx_gpucensus.h"
#include <cstdio>
#include <cstring>
using namespace gpucensus;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static bool Near(double a, double b) { return a - b < 1e-6 && b - a < 1e-6; }
int main() {
    Census c;
    // empty window: no division by zero, no NaN
    char l[600];
    c.EndPicture();
    c.Report(l, sizeof l);
    CHECK(std::strstr(l, "nan") == nullptr && std::strstr(l, "inf") == nullptr);
    CHECK(std::strstr(l, "textures per fragment opaque 0.00") != nullptr);
    // two opaque draws: 1000 px x 3 tex, 3000 px x 1 tex -> (3000+3000)/4000 = 1.5; one discard 500 px x 2; one blended 100 px x 1 (bounded)
    c = Census{};
    c.Draw(kOpaque, 1000, false, 3, true, 100, 50, 1);
    c.Draw(kOpaque, 3000, false, 1, false, 10, 5, 0);
    c.Draw(kDiscard, 500, false, 2, true, 0, 0, 0);
    c.Draw(kBlended, 100, true, 1, true, 0, 0, 0);
    CHECK(Near(c.texPx[kOpaque] / c.px[kOpaque], 1.5));
    CHECK(Near(Census::Ratio(c.efbOpaqueTexPx, c.efbOpaquePx), 3.0));   // only the EFB draw
    CHECK(Near(c.boundPx[kBlended], 100));
    // PB: 100 x (16 + 8 + 4) + 12 x 50 = 3400; 10 x 20 + 60 = 260
    CHECK(Near(c.curPb, 3660.0));
    c.EndPicture();
    CHECK(c.pics == 1 && Near(c.pbMaxB, 3660.0) && c.pbPicsOver == 0 && Near(c.curPb, 0.0));
    // over the limit: 9 MiB x 2 in flight against 16
    c.Draw(kOpaque, 1, false, 1, true, 9u * 1048576u / 20u, 0, 0);
    c.EndPicture();
    CHECK(c.pbPicsOver == 1);
    c.Report(l, sizeof l);
    CHECK(std::strstr(l, "1 pictures over") != nullptr || std::strstr(l, "pictures over") != nullptr);
    // the report clears the window but keeps settings
    CHECK(c.pics == 0 && c.px[kOpaque] == 0 && c.flight == 2 && Near(c.limitMiB, 16.0));
    // other class: pixels counted nowhere in the texture figure
    c.Draw(kOther, 5000, false, 4, true, 0, 0, 0);
    CHECK(Near(c.px[kOpaque], 0.0) && Near(c.px[kOther], 5000));
    if (fails) { std::printf("FAILED %d\n", fails); return 1; }
    std::printf("PASS: gpucensus\n");
    return 0;
}
