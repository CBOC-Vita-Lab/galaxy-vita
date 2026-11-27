#include "Game/System/WPadAcceleration.hpp"
#include "Game/System/WPad.hpp"
#include "Game/Util/MathUtil.hpp"
#include <revolution/wpad.h>

void WPadAcceleration_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
}

namespace {
    bool sSelectDominantRotation;

    inline f32 squareDistance(const TVec3f& rA, const TVec3f& rB) {
        return JMathInlineVEC::PSVECSquareDistance(&rA, &rB);
    }
}

#ifdef TARGET_PC
// GXV_HOTPROF_WPAD (hotprof25 round 2; exact): a remote held still (always so on a handheld unless a shake is being simulated) has the
// same sample in all 128 history slots. update() then sums 128 equal vectors, compares 127 of them (updateRotate) and adds 31 zero
// distances (updateAccAverage), four objects a turn: 60 k instructions a picture in the console profile. Here the length of the run of
// equal samples at the head of the history is kept per object (bit-equal, finite); while it covers the whole history the three results
// are known: the mean is the one computed when the run reached the history's length (the same 128 additions of the same value), the
// rotation outputs are what updateRotate's "still" exit leaves, and the average distance is +0 (31 times (v - v)^2 = +0, over 32).
// 0 off, 1 on, 2 verify (the loops run; their results compared with the known ones: "wpad-still:" line, mismatches must be 0).
#include <cstring>
extern "C" int port_hotprof_wpad_mode(void) __attribute__((weak));
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
namespace {
    struct PortStill {
        const WPadAcceleration* obj;
        s32 run;       // samples at the head of the history bit-equal to the latest (0: latest is not finite, or unknown)
        bool haveAvg;  // avg holds update()'s mean for a history of 128 samples equal to the latest
        TVec3f avg;
    };
    PortStill sPortStill[8];
    u32 sPortStillTurns, sPortStillHits, sPortStillBad;

    PortStill* portStillOf(const WPadAcceleration* obj, bool reset) {
        PortStill* free = nullptr;
        for (PortStill& e : sPortStill) {
            if (e.obj == obj) {
                free = &e;
                break;
            }
            if (e.obj == nullptr && free == nullptr) {
                free = &e;
            }
        }
        if (free != nullptr && (free->obj != obj || reset)) {
            free->obj = obj;
            free->run = 0;
            free->haveAvg = false;
        }
        return free;
    }
    inline bool portFinite(const TVec3f& v) {  // (from the bits: no NaN, no infinity)
        u32 w[3];
        std::memcpy(w, &v, sizeof(w));
        return (w[0] & 0x7F800000u) != 0x7F800000u && (w[1] & 0x7F800000u) != 0x7F800000u && (w[2] & 0x7F800000u) != 0x7F800000u;
    }
    // One sample was written to the history: `prev` the latest before it (nullptr: there was none), `now` the new latest.
    inline void portStillPush(PortStill* st, const TVec3f* prev, const TVec3f& now) {
        if (st == nullptr) {
            return;
        }
        if (!portFinite(now)) {
            st->run = 0;
            st->haveAvg = false;
        } else if (prev != nullptr && st->run > 0 && std::memcmp(prev, &now, sizeof(TVec3f)) == 0) {
            if (st->run < 128) {
                st->run++;
            }
        } else {
#if PORT_WPAD_NEG  // (tools/test_hotprof_wpad.sh's negative control: a different sample does not end the run)
            if (st->run < 128) {
                st->run++;
            }
#else
            st->run = 1;
            st->haveAvg = false;
#endif
        }
    }
}
#endif

WPadAcceleration::WPadAcceleration(const WPad* pPad, u32 device)
    : mPad(pPad), _4(device), _8(), _C(0.15f), _10(0, 0, 0), _1C(), _20(true), _624(-1), _628(), _62C(0, 0, 0), _638(0.0f, 0.0f, 0.0f), _644(128),
      _648(), _64C() {
    KPADSetAccParam(mPad->mChannel, _8, _C);
#ifdef TARGET_PC
    portStillOf(this, true);  // (an object built where another one lived starts with nothing known)
#endif
}

bool WPadAcceleration::getAcceleration(TVec3f* pOut) const {
    if (_628 <= 0) {
        pOut->zero();
        return false;
    }

    pOut->set(mHistory[_624]);
    return true;
}

bool WPadAcceleration::getPastAcceleration(TVec3f* pOut, s32 index) const {
    if (index >= _628) {
        pOut->zero();
        return false;
    }

    s32 slot = _624 - index;

    if (slot < 0) {
        slot += 128;
    }

    pOut->set(mHistory[slot]);
    return true;
}

bool WPadAcceleration::isStationary() const {
    return _20;
}

bool WPadAcceleration::isBalanced() const {
    return _1C < 0.018f;
}

void WPadAcceleration::update() {
#ifdef TARGET_PC
    const int portMode = port_hotprof_wpad_mode ? port_hotprof_wpad_mode() : 0;
    PortStill* const portSt = portMode != 0 ? portStillOf(this, false) : nullptr;
#endif
    s32 statusCount = mPad->getValidStatusCount();
    if (statusCount <= 0) {
        TVec3f previous;

        if (_624 == -1) {
            previous.set< f32 >(0.0f, 0.0f, 0.0f);
        } else {
            previous = mHistory[_624];
        }
#ifdef TARGET_PC
        const s32 portPrev = _624;
#endif

        _624++;

        if (_624 >= 128U) {
            _624 = 0;
        }

        mHistory[_624] = previous;
#ifdef TARGET_PC
        {
            const TVec3f portOld = previous;  // (the slot written may be the one read)
            portStillPush(portSt, portPrev == -1 ? nullptr : &portOld, mHistory[_624]);
        }
#endif

        if (_628 < 128U) {
            _628++;
        }
    }

    for (s32 i = statusCount - 1; i >= 0; i--) {
        KPADStatus* pStatus = mPad->getKPadStatus(i);

        if (pStatus == nullptr) {
            continue;
        }

        TVec3f acceleration;

        if (_4 == 0) {
            acceleration.x = -pStatus->acc.x;
            acceleration.y = pStatus->acc.z;
            acceleration.z = -pStatus->acc.y;
        } else {
            if (!MR::isDeviceFreeStyle(pStatus)) {
                continue;
            }

            acceleration.x = -pStatus->ex_status.fs.acc.x;
            acceleration.y = pStatus->ex_status.fs.acc.z;
            acceleration.z = -pStatus->ex_status.fs.acc.y;
        }

#ifdef TARGET_PC
        const bool portHad = _624 != -1;
        TVec3f portOld;
        if (portHad) {
            portOld = mHistory[_624];
        }
#endif
        _624++;

        if (_624 >= 128U) {
            _624 = 0;
        }

        mHistory[_624] = acceleration;
#ifdef TARGET_PC
        portStillPush(portSt, portHad ? &portOld : nullptr, mHistory[_624]);
#endif

        if (_628 < 128U) {
            _628++;
        }
    }

#ifdef TARGET_PC
    // (GXV_HOTPROF_WPAD) The whole history is one finite value and its mean is known: the loops below would make these values.
    const bool portKnown = portSt != nullptr && portSt->run >= 128 && _628 == 128 && portSt->haveAvg;
    if (portMode != 0 && (++sPortStillTurns & 0x3FFF) == 0 && port_log_summary) {
        port_log_summary("wpad-still: %u updates, %u known, %u mismatches (mode %d)", sPortStillTurns, sPortStillHits, sPortStillBad, portMode);
    }
    if (portKnown) {
        sPortStillHits++;
        if (portMode == 1) {
            _62C = portSt->avg;
            _638.zero();
            s32 count = _644;
            if (count >= _628) {
                count = _628;
            }
            if (count >= 3) {
                _648 = 0;
                _64C = 0;
            }
            _1C = 0.0f;
            updateIsStable();
            return;
        }
    }
#endif
    _62C.zero();

    for (s32 j = 0; j < _628; j++) {
        TVec3f acceleration;
        getPastAcceleration(&acceleration, j);
        _62C += acceleration;
    }

    if (_628 > 0) {
        _62C.scale(1.0f / _628);
    }

#ifdef TARGET_PC
    if (portSt != nullptr && portSt->run >= 128 && _628 == 128 && !portSt->haveAvg) {
        portSt->avg = _62C;  // (the mean of 128 samples equal to the latest: the same sum every turn until the run breaks)
        portSt->haveAvg = true;
    }
#endif
    updateRotate();
    updateAccAverage();
#ifdef TARGET_PC
    if (portKnown) {  // (verify: the loops ran; compare with what mode 1 would have left)
        const TVec3f zero(0.0f, 0.0f, 0.0f);
        const f32 zeroAvg = 0.0f;
        s32 count = _644;
        if (count >= _628) {
            count = _628;
        }
        if (std::memcmp(&_62C, &portSt->avg, sizeof(TVec3f)) != 0 || std::memcmp(&_638, &zero, sizeof(TVec3f)) != 0 ||
            std::memcmp(&_1C, &zeroAvg, sizeof(f32)) != 0 || (count >= 3 && (_648 != 0 || _64C != 0))) {
            if (sPortStillBad++ < 8 && port_log_summary) {
                port_log_summary("wpad-still: MISMATCH (mean %g %g %g, rotate %g %g %g / %d %d, average %g)", _62C.x, _62C.y, _62C.z, _638.x,
                                 _638.y, _638.z, static_cast< int >(_648), static_cast< int >(_64C), _1C);
            }
        }
    }
#endif
    updateIsStable();
}

void WPadAcceleration::updateRotate() {
    _638.zero();
    s32 count = _644;

    if (count >= _628) {
        count = _628;
    }

    if (count < 3) {
        return;
    }

    TVec3f latest;
    getPastAcceleration(&latest, 0);
    f32 zMagnitude = 0.0f;
    f32 xMagnitude = 0.0f;
    _648 = 0;
    _64C = 0;
#ifdef TARGET_PC
    // A remote held still (always so on a handheld unless a shake is being simulated) has the same sample all through its
    // history: every difference below is then a zero vector, every cross product a zero vector, the sum stays +0 (adding -0 to
    // +0 gives +0) and nothing is found. Checking that the samples equal the latest one costs a fraction of the 128 cross
    // products. A NaN never compares equal, so such a history still takes the loop as before.
    {
        bool still = true;
        for (s32 i = 1; i < count && still; i++) {
            TVec3f past;
            getPastAcceleration(&past, i);
            still = past.x == latest.x && past.y == latest.y && past.z == latest.z;
        }
        if (still) {
            return;
        }
    }
#endif
    bool foundZ = false;
    bool foundX = false;

    for (s32 i = 2; i < count; i++) {
        TVec3f previous;
        TVec3f current;
        TVec3f cross;
        getPastAcceleration(&previous, i - 1);
        getPastAcceleration(&current, i);
        cross.cross(previous - latest, current - latest);
        _638 += cross;

        if (i < 20) {
            continue;
        }

        if (!foundZ && MR::abs(_638.z) > 6.0f) {
            foundZ = true;
            _648 = _638.z > 0.0f ? 1 : -1;
            zMagnitude = MR::abs(_638.z);
        }

        if (!foundX && MR::abs(_638.x) > 6.0f) {
            foundX = true;
            _64C = _638.x > 0.0f ? 1 : -1;
            xMagnitude = MR::abs(_638.z);
        }
    }

    if (::sSelectDominantRotation) {
        if (zMagnitude < xMagnitude) {
            _648 = 0;
        }

        if (xMagnitude < zMagnitude) {
            _64C = 0;
        }
    }
}

void WPadAcceleration::updateAccAverage() {
    f32 total = 0.0f;
    s32 count = 0;
    TVec3f previous(0.0f, 0.0f, 0.0f);

    if (getPastAcceleration(&previous, 0)) {
        count++;
    }

    for (s32 i = 1; i < 32; i++) {
        TVec3f current;

        if (!getPastAcceleration(&current, i)) {
            break;
        }

        total += ::squareDistance(current, previous);
        previous = current;
        count++;
    }

    if (count > 0) {
        _1C = total / count;
    } else {
        _1C = 0.0f;
    }
}

void WPadAcceleration::updateIsStable() {
    TVec3f difference = mHistory[0] - _10;
    _20 = true;

    if (MR::abs(difference.x) >= 0.30f || MR::abs(difference.y) >= 0.30f || MR::abs(difference.z) >= 0.30f) {
        _10 = mHistory[0];
        _20 = false;
    }
}
