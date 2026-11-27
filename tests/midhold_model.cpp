// midload26: host test of vgx_midhold.h (which scene loads may be held on a closed screen for the stage warm-up).
#include <cstdio>
#include "../platform/vgx/src/vgx_midhold.h"
using namespace vgx::midhold;
static int fails = 0;
#define REQUIRE(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)
int main() {
    const double minPlay = 3000.0;
    {   // switch off: everything is allowed, as before
        Facts f{0, false, 7, 90000.0, 7, minPlay};
        REQUIRE(Judge(f) == kOff && Allowed(Judge(f)), "off allows a restart");
    }
    {   // first load after boot / file select: allowed
        Facts f{1, false, 0, 0.0, 5, minPlay};
        REQUIRE(Judge(f) == kNoPlay && Allowed(Judge(f)), "no play yet");
    }
    {   // star select: allowed even after long play of another stage, and even when it is the same stage (replay a star from the map)
        Facts f{1, true, 7, 120000.0, 8, minPlay};
        REQUIRE(Judge(f) == kStarSelect, "star select, other stage");
        f.targetKey = 7;
        REQUIRE(Judge(f) == kStarSelect, "star select, same stage");
    }
    {   // passed through: file select / a hub crossed in a moment
        Facts f{1, false, 7, 1500.0, 8, minPlay};
        REQUIRE(Judge(f) == kPassedThrough && Allowed(Judge(f)), "short stay");
    }
    {   // death restart: same stage, long play, no star select: suppressed
        Facts f{1, false, 7, 45000.0, 7, minPlay};
        REQUIRE(Judge(f) == kRestart && !Allowed(Judge(f)), "restart suppressed");
    }
    {   // cut-scene / boss / star-get exit: another stage, long play, no star select: suppressed
        Facts f{1, false, 7, 45000.0, 9, minPlay};
        REQUIRE(Judge(f) == kInMission && !Allowed(Judge(f)), "in-mission change suppressed");
    }
    {   // the boundary: exactly minPlay counts as played
        Facts f{1, false, 7, 3000.0, 7, minPlay};
        REQUIRE(Judge(f) == kRestart, "boundary is play");
    }
    REQUIRE(HardCap(12000.0, 6000.0, false) == 6000.0, "hard cap lowers the own-programs cap");
    REQUIRE(HardCap(5000.0, 6000.0, false) == 5000.0, "hard cap never raises");
    REQUIRE(HardCap(60000.0, 6000.0, true) == 60000.0, "tour exempt");
    REQUIRE(HardCap(12000.0, 0.0, false) == 12000.0, "0 = no hard cap");
    std::printf(fails ? "midhold_model: %d FAILED\n" : "midhold_model: all ok\n", fails);
    return fails ? 1 : 0;
}
