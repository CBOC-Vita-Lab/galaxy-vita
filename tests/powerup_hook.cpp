// crash24: platform/include/port/powerup_hook.h, the pure half of the PETARI_POWERUP hook: list parsing, the schedule, the scene limit and the
// "could a real pickup happen now" rule. Plus a small simulation of the hook across scene changes (the actor restarts the list; a limited list
// stays quiet in later scenes; a held entry waits for the transformation to end and is dropped when the form's assets are missing).
#include "port/powerup_hook.h"
#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static PortPowerupState idle(bool ok = true) { PortPowerupState s; s.currentMode = 0; s.health = 3; s.transformTimer = 0; s.morphResetTimer = 0; s.nerveChangeEnabled = true; s.formAccepted = ok; return s; }

int main() {
    unsigned m = 99;
    // list parsing
    CHECK(portPowerupEntry("4,0,4,0,1,0", 0, &m) && m == 4);
    CHECK(portPowerupEntry("4,0,4,0,1,0", 1, &m) && m == 0);
    CHECK(portPowerupEntry("4,0,4,0,1,0", 4, &m) && m == 1);
    CHECK(portPowerupEntry("4,0,4,0,1,0", 5, &m) && m == 0);
    CHECK(!portPowerupEntry("4,0,4,0,1,0", 6, &m));      // used up
    CHECK(!portPowerupEntry("4,0,4,0,1,0", 60, &m));
    CHECK(portPowerupEntry(" 7 , 2", 0, &m) && m == 7);
    CHECK(portPowerupEntry("4,12", 1, &m) && m == 12);
    CHECK(!portPowerupEntry("4,x,2", 1, &m));            // a typo is not "cure"
    CHECK(!portPowerupEntry("4,,2", 1, &m));
    CHECK(!portPowerupEntry("", 0, &m) && !portPowerupEntry(nullptr, 0, &m));
    CHECK(!portPowerupEntry("99999999999", 0, &m));
    // the schedule: AT, AT+EVERY, ...
    CHECK(!portPowerupDue(239, 240, 360) && portPowerupDue(240, 240, 360) && !portPowerupDue(241, 240, 360));
    CHECK(portPowerupDue(600, 240, 360) && portPowerupDue(960, 240, 360) && !portPowerupDue(959, 240, 360));
    CHECK(!portPowerupDue(5, 0, 0));
    CHECK(portPowerupDue(0, 0, 10) && portPowerupDue(10, 0, 10));
    // the scene limit
    CHECK(portPowerupSceneAllowed(0, 1) && portPowerupSceneAllowed(0, 50));
    CHECK(portPowerupSceneAllowed(1, 1) && !portPowerupSceneAllowed(1, 2));
    CHECK(portPowerupSceneAllowed(2, 2) && !portPowerupSceneAllowed(2, 3));
    // the pickup rule
    const char* why = nullptr;
    CHECK(portPowerupBlocked(4, idle(), &why) == PORT_POWERUP_OFFER);
    { PortPowerupState s = idle(false); why = nullptr; CHECK(portPowerupBlocked(4, s, &why) == PORT_POWERUP_DROP && why); }
    { PortPowerupState s = idle(); s.transformTimer = 30; why = nullptr; CHECK(portPowerupBlocked(2, s, &why) == PORT_POWERUP_WAIT && why); }
    { PortPowerupState s = idle(); s.morphResetTimer = 5; why = nullptr; CHECK(portPowerupBlocked(2, s, &why) == PORT_POWERUP_WAIT && why); }
    { PortPowerupState s = idle(); s.health = 0; why = nullptr; CHECK(portPowerupBlocked(3, s, &why) == PORT_POWERUP_WAIT && why); }
    { PortPowerupState s = idle(); s.nerveChangeEnabled = false; why = nullptr; CHECK(portPowerupBlocked(7, s, &why) == PORT_POWERUP_WAIT && why); }
    // a cure is offered whatever the state
    { PortPowerupState s = idle(false); s.transformTimer = 30; s.health = 0; s.nerveChangeEnabled = false; s.morphResetTimer = 9; CHECK(portPowerupBlocked(0, s, &why) == PORT_POWERUP_OFFER); }
    // the same mode again is the caller's canEnterPlayerMode (true for it): offered
    { PortPowerupState s = idle(); s.currentMode = 4; CHECK(portPowerupBlocked(4, s, &why) == PORT_POWERUP_OFFER); }

    // simulation of the hook: list "4,0,4,0,1,0", AT 240, EVERY 360; three scenes (actors), SCENES limit 0 / 1.
    for (unsigned limit = 0; limit <= 1; limit++) {
        const char* list = "4,0,4,0,1,0";
        unsigned offered = 0, scenesRun = 0;
        for (unsigned scene = 1; scene <= 3; scene++) {  // each scene: a new actor, turn and next restart (portPlayerPowerupReset)
            unsigned turn = 0, next = 0, got = 0;
            const bool on = portPowerupSceneAllowed(limit, scene);
            for (unsigned t = 0; t < 3000 && on; t++) {
                turn++;
                if (!portPowerupDue(turn, 240, 360)) continue;
                unsigned mode;
                if (!portPowerupEntry(list, next, &mode)) continue;
                next++;
                got++;
                offered++;
            }
            if (on) { scenesRun++; CHECK(got == 6); }
            else CHECK(got == 0);
        }
        CHECK(limit == 0 ? (offered == 18 && scenesRun == 3) : (offered == 6 && scenesRun == 1));
    }
    // a held entry: forced mode 2 while the 64-turn transformation of the entry before is still running waits, then goes
    {
        PortPowerupState s = idle(); s.transformTimer = 64;
        unsigned waited = 0; bool offered = false;
        for (unsigned t = 0; t < 200 && !offered; t++) {
            if (s.transformTimer) s.transformTimer--;
            if (portPowerupBlocked(2, s, &why) == PORT_POWERUP_OFFER) offered = true; else waited++;
        }
        CHECK(offered && waited == 63);
    }
    std::printf(fails ? "FAILED %d\n" : "PASS powerup_hook\n", fails);
    return fails ? 1 : 0;
}
