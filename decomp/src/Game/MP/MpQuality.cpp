// Galaxy 3.0 multiplayer: the automatic puppet quality governor, game side (owner: perf3). See MpQualityLogic.hpp for the rules.
//
// Once a logic turn (MpPuppets::update, session and scene live): a turn that draws a picture gives one picture interval (the time
// since the last drawn turn: a slow game thread, a renderer or GPU that holds the game back, a skipped picture all show there,
// on the console and on the desktop alike). The governor judges 3 s windows of them against the picture rate the game aims at
// (port: mp_frame_target_us) and the renderer's own work estimate when there is one (mp_frame_work_us), and moves the level one
// step at a time; each change is logged once: "[mp] quality level N (frame p95 X ms)". The puppets ask params() / halfRate().
//
// SMG_MP_PERFLOG=1 (env.txt): one "[mp] perf ..." line every ~10 s with the game thread's multiplayer time a turn (avg / max), the
// puppets' draw time a picture, the picture interval p95 and the level (port/mp_prof.h lite counters). Off: no line, no timing.
// optfix: by default the governor never changes the level (ceiling 0; SMG_MP_QUALITY_AUTO=1..4 raises the ceiling, testing only).
// Test switches: SMG_MP_QUALITY=0..4 holds that level (the governor still measures); desktop only SMG_MP_TEST_SLOW_MS=<ms>[:<L>]
// sleeps <ms> in every drawn turn while the level is below L (default: always), to see the governor work.
#include "Game/MP/MpQuality.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/MP/MpSpectate.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "port/mp_prof.h"

extern "C" int64_t port_host_time_ns(void);
#ifdef TARGET_PC
extern "C" int port_turn_skipped_raw(void);       // platform/src/port/perf.cpp: this turn draws no picture
extern "C" int port_skip_fast_forwarding(void);   // a cutscene being skipped: turns without pictures, back to back
#endif

namespace {
    MpQuality::Governor sGov;
    MpQuality::LevelParams sParams = MpQuality::paramsFor(0);
    int sLevel = 0;
    bool sDistant[MP_MAX_PLAYERS];
    uint32_t sFullMask = 0xFu;     // level 3: slots allowed full rate
    uint64_t sLastPictureUs = 0;   // the last drawn turn (0: none since the scene began)
    uint64_t sLastTurnUs = 0;
    uint32_t sInjectedUs = 0;      // desktop test switch: the sleep of the last turn (not the multiplayer code's cost)
    uint32_t sTargetTurns = 0;     // turns until the target is read again
    bool sForcedLogged = false;

    // the perf line's window
    struct PerfWindow {
        uint64_t startUs;
        uint32_t turns, pictures;
        uint64_t totalUs, calcUs, drawUs;
        uint32_t totalMax, drawMax;
        uint32_t shownPictures;
    };
    PerfWindow sPerf;
    MpQuality::FrameWindow sPerfFrames;

    bool turnDraws() {
#ifdef TARGET_PC
        return port_turn_skipped_raw() == 0 && port_skip_fast_forwarding() == 0;
#else
        return true;
#endif
    }

    void resetScene() {
        sGov.restart();
        sLastPictureUs = 0;
        sFullMask = 0xFu;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) sDistant[s] = false;
    }

    void setLevel(int level) {
        sLevel = level;
        sParams = MpQuality::paramsFor(level);
    }

    // Level 3's ranking and "are remote puppets on screen": camera distance of every drawn remote puppet.
    bool rankPuppets() {
        float d2[MP_MAX_PLAYERS];
        bool shown = false;
        const TVec3f cam = MR::getCamPos();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            d2[s] = -1.0f;
            MpPuppetBase* p = MpPuppetsInt::puppet(s);
            if (p == nullptr || p->isSelf() || !p->isDrawable()) {
                continue;
            }
            const float dx = p->mPosition.x - cam.x, dy = p->mPosition.y - cam.y, dz = p->mPosition.z - cam.z;
            d2[s] = dx * dx + dy * dy + dz * dz;
            shown = shown || !p->isCulled();
        }
        sFullMask = MpQuality::nearestTwoMask(d2, MP_MAX_PLAYERS);
        return shown;
    }

    void perfTurn(uint64_t nowUs, bool drew, uint32_t intervalUs, bool shown) {
        if (!mp_perflog_env()) {
            return;
        }
        uint32_t z[3];
        mp_perf_take(z);
        z[0] = z[0] > sInjectedUs ? z[0] - sInjectedUs : 0;   // (SMG_MP_TEST_SLOW_MS slept inside the timed hook: not ours)
        sInjectedUs = 0;
        PerfWindow& w = sPerf;
        if (w.startUs == 0) {
            w = PerfWindow();
            w.startUs = nowUs;
            sPerfFrames.reset();
            return;   // (the first turn's counts may hold anything since the last window)
        }
        w.turns++;
        w.totalUs += z[0];
        w.calcUs += z[1];
        w.drawUs += z[2];
        if (z[0] > w.totalMax) w.totalMax = z[0];
        if (z[2] > w.drawMax) w.drawMax = z[2];
        if (drew) {
            w.pictures++;
            w.shownPictures += shown ? 1 : 0;
            if (intervalUs != 0) sPerfFrames.add(intervalUs);
        }
        const uint64_t span = nowUs - w.startUs;
        if (span < 10000000u || w.turns == 0) {
            return;
        }
        const double t = (double)w.turns, pics = w.pictures ? (double)w.pictures : 1.0;
        uint32_t hBusy = 0, hWakes = 0, hMax = 0;
        mp_core_helper_take(&hBusy, &hWakes, &hMax);   // (the session thread on core 2; the first window starts its measurement)
        // MP3 nocap: the queued sends the helper put on the wire (STATE 30 Hz + POINTER <= 15 Hz while B is held): a rate near 30/s and
        // a small wait say the send pacing holds (with or without the fourth core); "refused" counts sends the full ring turned away.
        uint32_t sSent = 0, sRefused = 0, sLagSum = 0, sLagMax = 0;
        mp_core_send_take(&sSent, &sRefused, &sLagSum, &sLagMax);
        mp_log("perf %.1f s: mp %.3f/%.3f ms a turn (puppet calc %.3f), puppet draw %.3f/%.3f ms a picture, frame p95 %.1f ms "
               "(max %.1f, target %.1f, %u pictures, puppets on screen in %u), quality level %d, helper %.2f ms/s (%u wakes, max %.2f ms), "
               "sends %.1f/s (refused %u, wait avg %.2f max %.2f ms)",
               span / 1e6, w.totalUs / t / 1000.0, w.totalMax / 1000.0, w.calcUs / t / 1000.0, w.drawUs / pics / 1000.0,
               w.drawMax / 1000.0, sPerfFrames.percentileUs(95) / 1000.0, sPerfFrames.maxUs() / 1000.0, sGov.targetUs() / 1000.0,
               (unsigned)w.pictures, (unsigned)w.shownPictures, sLevel, hBusy / (span / 1e6) / 1000.0, (unsigned)hWakes, hMax / 1000.0,
               sSent / (span / 1e6), (unsigned)sRefused, sSent ? sLagSum / (double)sSent / 1000.0 : 0.0, sLagMax / 1000.0);
        w = PerfWindow();
        w.startUs = nowUs;
        sPerfFrames.reset();
    }
}  // namespace

namespace MpQuality {
    void turn() {
        const uint64_t now = (uint64_t)(port_host_time_ns() / 1000);
        if (sLastTurnUs != 0 && now - sLastTurnUs > 60000000u) {
            // a minute without a session scene (between sessions): a new start
            sGov.reset();
            setLevel(0);
            resetScene();
            sTargetTurns = 0;
        }
        sLastTurnUs = now;
        if (sTargetTurns == 0) {
            sGov.setMaxLevel(mp_quality_auto_max());   // (optfix: 0 by default: the governor measures but never lowers anything)
            sGov.setTargetUs(mp_frame_target_us());   // (two getenv: once every ~3 s)
            sTargetTurns = 180;
        }
        sTargetTurns--;

        const bool shown = rankPuppets();
        const bool drew = turnDraws();

        int belowLevel = 0;
        const int slowMs = mp_test_slow_ms(&belowLevel);   // (desktop test switch; 0 on the console)
        if (slowMs > 0 && drew && sLevel < belowLevel) {
            const uint64_t s0 = mp_prof_now_us();
            mp_test_sleep_ms(slowMs);
            sInjectedUs += (uint32_t)(mp_prof_now_us() - s0);
        }

        uint32_t interval = 0;
        if (drew) {
            if (sLastPictureUs != 0) {
                const uint64_t d = now - sLastPictureUs;
                interval = d > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)d;
                const bool changed = sGov.onPicture(now, interval, shown, mp_frame_work_us());
                const int forced = mp_quality_forced();
                if (forced >= 0) {
                    if (!sForcedLogged || sLevel != forced) {
                        sForcedLogged = true;
                        setLevel(forced);
                        mp_log("quality level %d (fixed by SMG_MP_QUALITY)", forced);
                    }
                } else if (changed) {
                    setLevel(sGov.level());
                    mp_log("quality level %d (frame p95 %.1f ms)", sLevel, sGov.lastP95Us() / 1000.0);
                }
            }
            sLastPictureUs = now;
        }
        perfTurn(now, drew, interval, shown);
    }

    void endScene() {
        resetScene();
    }

    int level() { return sLevel; }
    const LevelParams& params() { return sParams; }
    bool shadows() { return sParams.shadows; }

    bool halfRate(int slot, float d2) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS) {
            return false;
        }
        if (MpSpectate::watchedSlot() == slot) {   // MP3 spectate: the watched puppet is the nearest, whatever the picture says
            sDistant[slot] = false;
            return false;
        }
        sDistant[slot] = distant(sDistant[slot], d2, sParams);
        if (sDistant[slot]) {
            return true;
        }
        return sParams.nearestTwo && (sFullMask & (1u << slot)) == 0;
    }
}  // namespace MpQuality
