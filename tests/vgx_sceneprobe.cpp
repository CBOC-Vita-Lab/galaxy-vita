// Host test of vgx_sceneprobe.h (GXV_SCENE_PROBE: the clamp, the window's line, the marginal-cost arithmetic). tools/test_vgx_sceneprobe.sh builds and runs it.
#include "vgx_sceneprobe.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
int g_failed = 0, g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failed; std::printf("FAILED: %s\n", what); }
}
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }
} // namespace

int main() {
    using namespace vgx::sceneprobe;
    Check(Clamp(0) == 0 && Clamp(-3) == 0, "0 and negative: off");
    Check(Clamp(1) == 1 && Clamp(4) == 4 && Clamp(16) == 16, "1..16 kept");
    Check(Clamp(17) == 16 && Clamp(1000000) == 16, "above 16: 16 (the scene watcher's limit)");
    {
        Window w;
        for (int i = 0; i < 300; ++i) w.Picture(4, 0);
        char line[320];
        const int len = w.Format(line, sizeof line, 4, 5.0);
        Check(len > 0 && len < static_cast<int>(sizeof line), "line fits 320 bytes");
        Check(std::strstr(line, "scene-probe: GXV_SCENE_PROBE=4: 300 pictures in 5.0 s, 4.00 empty 64x64 scenes a picture (1200 made, 0 refused)") == line, "line text");
        Check(!std::strstr(line, "REFUSED"), "no refusal note when none refused");
        w.Picture(3, 1);
        w.Format(line, sizeof line, 4, 5.0);
        Check(std::strstr(line, "(1203 made, 1 refused): REFUSED") != nullptr, "a refused scene invalidates the run");
        w.Reset();
        Check(w.pictures == 0 && w.made == 0 && w.refused == 0, "reset");
        w.Format(line, sizeof line, 2, 5.0);
        Check(std::strstr(line, "0.00 empty") != nullptr, "empty window: 0 a picture, no division by zero");
    }
    {
        double m = 0;
        Check(MarginalMs(26.0, 24.0, 4, &m) && Near(m, 0.5), "marginal cost: (26 - 24) / 4 = 0.5 ms");
        Check(!MarginalMs(26.0, 24.0, 0, &m), "n = 0 has no marginal cost");
        Check(MarginalMs(23.0, 24.0, 2, &m) && Near(m, -0.5), "a negative marginal (noise) is reported as it is");
        const unsigned n[] = {0, 2, 4};
        const double busy[] = {22.6, 23.6, 24.6};
        double s = 0;
        Check(SlopeMs(n, busy, 3, &s) && Near(s, 0.5), "slope of n = 0, 2, 4: 0.5 ms a scene");
        const unsigned same[] = {2, 2};
        const double v[] = {1.0, 2.0};
        Check(!SlopeMs(same, v, 2, &s), "no slope from one distinct n");
        Check(!SlopeMs(n, busy, 1, &s), "no slope from one point");
    }
    if (g_failed) { std::printf("sceneprobe: %d of %d checks FAILED\n", g_failed, g_checks); return 1; }
    std::printf("sceneprobe: %d checks passed\n", g_checks);
    return 0;
}
