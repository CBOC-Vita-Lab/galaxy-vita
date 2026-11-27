// Galaxy 3.0 multiplayer: puppet level of detail by distance, game side (owner: lod2). See MpLod.hpp and MpLodLogic.hpp.
#include "Game/MP/MpLod.hpp"
#include "Game/MP/MpFormation.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpQuality.hpp"
#include "Game/MP/MpSpectate.hpp"   // MP3 spectate: the watched puppet counts as the nearest
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "port/mp_prof.h"
#include <stdlib.h>

namespace {
    MpLod::Config makeCfg() {
        MpLod::Config c;
        const char* e = getenv("SMG_MP_LOD2_TRIMPX");
        const int px = e != nullptr && *e != 0 ? atoi(e) : 0;
        if (px > 0 && px < 400) {
            c.trimPxIn = (float)px;
            c.trimPxOut = (float)px * 1.15f;
        }
        return c;
    }
    MpLod::Config sCfg = makeCfg();
    MpLod::Slot sSlot[MpLod::SLOTS];
    int sNearest = -1;
    uint32_t sCounts[MpLod::C_COUNTS];
    uint32_t sCheckEqual, sCheckDiff, sCheckBits;
    float sCheckMax;
    uint32_t sTurns;
    uint64_t sWindowStartUs;

    int envInt(const char* name, int def) {
        const char* e = getenv(name);
        return e != nullptr && *e != 0 ? atoi(e) : def;
    }
    struct Switches {
        bool master, shadow, rate, tiny, tags, onePass, shadowPx, trim;
        int check;
    };
    const Switches& sw() {
        static const Switches s = [] {
            Switches v;
            v.master = envInt("SMG_MP_LOD2", 1) != 0;
            v.shadow = v.master && envInt("SMG_MP_LOD2_SHADOW", 1) != 0;
            v.rate = v.master && envInt("SMG_MP_LOD2_RATE", 1) != 0;
            v.tiny = v.master && envInt("SMG_MP_LOD2_TINY", 1) != 0;
            v.tags = v.master && envInt("SMG_MP_LOD2_TAGS", 1) != 0;
            v.onePass = v.master && envInt("SMG_MP_LOD2_ONEPASS", 1) != 0;
            v.shadowPx = v.shadow && envInt("SMG_MP_LOD2_SHADOWPX", 1) != 0;
            v.trim = v.master && envInt("SMG_MP_LOD2_TRIM", 0) != 0;   // (off: not invisible)
            v.check = v.master ? envInt("SMG_MP_LOD2_CHECK", 0) : 0;
            return v;
        }();
        return s;
    }

    void resetSlots() {
        for (int s = 0; s < MpLod::SLOTS; s++) {
            sSlot[s].period = 1;
            sSlot[s].phase = 0;
            sSlot[s].shadow = MpLod::SH_VOLUME;
            sSlot[s].tiny = false;
            sSlot[s].trim = false;
            sSlot[s].special = false;
        }
        sNearest = -1;
    }

    void logWindow(uint64_t nowUs) {
        if (sWindowStartUs == 0) {
            sWindowStartUs = nowUs;
            return;
        }
        if (nowUs - sWindowStartUs < 10000000u || sTurns == 0) {
            return;
        }
        const uint32_t* c = sCounts;
        mp_log("lod2 %.1f s, %u turns: joints calc %u, skipped by rate %u / off screen %u / tiny %u; passes one %u two %u; light skipped %u; "
               "tags steady %u active %u; heads steady %u active %u; check equal %u differ %u (bitwise %u, max %.3g)",
               (nowUs - sWindowStartUs) / 1e6, (unsigned)sTurns, c[MpLod::C_CALC], c[MpLod::C_SKIP_RATE], c[MpLod::C_SKIP_CULL],
               c[MpLod::C_SKIP_TINY], c[MpLod::C_ONE_PASS], c[MpLod::C_TWO_PASS], c[MpLod::C_LIGHT_SKIP], c[MpLod::C_TAG_STEADY],
               c[MpLod::C_TAG_ACTIVE], c[MpLod::C_HEAD_STEADY], c[MpLod::C_HEAD_ACTIVE], (unsigned)sCheckEqual, (unsigned)sCheckDiff,
               (unsigned)sCheckBits, (double)sCheckMax);
        for (uint32_t& v : sCounts) v = 0;
        sTurns = 0;
        sWindowStartUs = nowUs;
    }
}  // namespace

extern "C" int64_t port_host_time_ns(void);

namespace MpLod {
    bool enabled() { return sw().master && mp_active(); }
    bool shadowOn() { return sw().shadow; }
    bool rateOn() { return sw().rate; }
    bool tinyOn() { return sw().tiny; }
    bool tagsOn() { return sw().tags; }
    bool onePassOn() { return sw().onePass; }
    bool shadowPxOn() { return sw().shadowPx; }
    bool trimOn() { return sw().trim; }
    int checkMode() { return sw().check; }
    bool onePassOk(bool blendRunning, float d2) { return MpLod::onePassOk(blendRunning, d2, sCfg); }
    float cullMargin() { return sw().tiny ? sCfg.cullMargin : 1.0f; }

    void sceneInit() {
        resetSlots();
        for (uint32_t& v : sCounts) v = 0;
        sTurns = 0;
        sWindowStartUs = 0;
        if (sw().master) {
            mp_log("lod2 on shadow=%d (by pixels %d) rate=%d tiny=%d tags=%d onepass=%d trim=%d check=%d", sw().shadow, sw().shadowPx, sw().rate, sw().tiny, sw().tags, sw().onePass, sw().trim, sw().check);
        }
    }

    void endScene() { resetSlots(); }

    const Slot& slot(int s) {
        static const Slot neutral = {1, 0, SH_VOLUME, false, false, false};
        return s >= 0 && s < SLOTS ? sSlot[s] : neutral;
    }

    void count(Count c) {
        if (c >= 0 && c < C_COUNTS) sCounts[c]++;
    }

    void checkNote(bool equal, int bitDiffs, float maxAbs) {
        if (equal) {
            sCheckEqual++;
        } else {
            sCheckDiff++;
            sCheckBits += (uint32_t)bitDiffs;
            if (maxAbs > sCheckMax) sCheckMax = maxAbs;
            if (sCheckDiff <= 8) mp_log("lod2 check: one pass differs from two: %d floats differ, max abs diff %g", bitDiffs, (double)maxAbs);
        }
    }

    void turn() {
        if (!enabled()) {
            return;
        }
        sTurns++;
        const TVec3f cam = MR::getCamPos();
        const float fovy = MR::getFovy();
        const float screenH = (float)MR::getScreenHeight();
        float d2[SLOTS];
        int period[SLOTS];
        bool far[SLOTS];
        MpPuppetBase* pup[SLOTS];
        for (int s = 0; s < SLOTS; s++) {
            d2[s] = -1.0f;
            period[s] = 0;
            far[s] = false;
            pup[s] = nullptr;
            MpPuppetBase* p = s < MP_MAX_PLAYERS ? MpPuppetsInt::puppet(s) : nullptr;
            if (p == nullptr || p->isSelf() || !p->isDrawable()) {
                continue;
            }
            TVec3f c;
            p->lodCenter(&c);
            const float dx = c.x - cam.x, dy = c.y - cam.y, dz = c.z - cam.z;
            d2[s] = dx * dx + dy * dy + dz * dz;
            far[s] = p->isFar();
            pup[s] = p;
        }
        sNearest = pickNearest(sNearest, d2, SLOTS, sCfg);
        const int watched = MpSpectate::watchedSlot();
        const unsigned formation = MpFormation::placedMask();
        const int level = MpQuality::level();
        for (int s = 0; s < SLOTS; s++) {
            Slot& o = sSlot[s];
            if (pup[s] == nullptr) {
                o.period = 1;
                o.tiny = false;
                o.trim = false;
                o.special = false;
                continue;
            }
            const bool special = isSpecial(s, sNearest, watched, formation);
            o.special = special;
            const int band = bandFor(o.period - 1, d2[s], special, sCfg);
            o.period = (uint8_t)bandPeriod(band);
            period[s] = o.period;
            // tiny: the body's own height (190 units * the puppet's scale) from the distance; a special puppet never
            const float px = bodyPixels(190.0f * pup[s]->mScale.y, __builtin_sqrtf(d2[s]), fovy, screenH);
            o.tiny = !special && nextTiny(o.tiny, px, sCfg);
            o.shadow = sw().shadowPx ? nextShadowPx(o.shadow, refPixels(px, screenH, sCfg), level, special, far[s], sCfg, d2[s]) : nextShadow(o.shadow, d2[s], level, special, far[s], sCfg);
            o.trim = sw().trim && !special && nextTrim(o.trim, px, sCfg);   // (the nearest / watched / formation puppets keep their face)
        }
        uint8_t phase[SLOTS];
        assignPhases(period, SLOTS, phase);
        for (int s = 0; s < SLOTS; s++) {
            sSlot[s].phase = phase[s];
        }
        logWindow((uint64_t)(port_host_time_ns() / 1000));
    }
}  // namespace MpLod
