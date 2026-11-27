// Galaxy 3.0 multiplayer: drop-in puppets, pure logic (owner: dropin; no game headers, host tested in tests/mp/dropin_test.cpp).
//
// A player who joins while the others are already in a scene must be drawn with his own character at once. The engine cannot
// take a new actor mid-scene: the movement / calcAnim / draw category lists and the draw-buffer actor lists are sized exactly
// at the end of the scene's init (NameObjCategoryList::allocateBuffer, DrawBufferGroup::allocateActorListBuffer) and
// MR::Vector::push_back has no bound. So every puppet a scene may need is made at its init: one per present remote player,
// plus (drop-in allowed, a free seat) one "spare" per character nobody uses yet. Characters are unique in a session (core's
// assignLook), so one spare per character is enough, and a joiner (or a LOOK change) is bound to the puppet of his character.
// When there is none (no free seat at init, memory guard, archive missing), the player stays hidden until the next scene:
// never a stand-in of somebody else's character.
#pragma once

#include <stdint.h>

#ifndef MP_DROPIN_SLOTS
#define MP_DROPIN_SLOTS 4      // = MP_MAX_PLAYERS
#endif
#ifndef MP_DROPIN_LOOKS
#define MP_DROPIN_LOOKS 6      // = MP_LOOK_COUNT
#endif

namespace MpDropIn {
    // ---- which spares a scene makes ------------------------------------------------------------------------------------------
    struct SceneRoster {
        bool present[MP_DROPIN_SLOTS];
        uint8_t look[MP_DROPIN_SLOTS];   // >= MP_DROPIN_LOOKS: treated as look 0 (Mario), as the puppets do
        int localSlot;
        bool dropInAllowed;              // MpSettings.flags bit0
        int maxPlayers;                  // MpSettings.maxPlayers (2..4)
    };

    static inline int clampLook(uint8_t l) { return l < MP_DROPIN_LOOKS ? (int)l : 0; }

    static inline int presentCount(const SceneRoster& r) {
        int n = 0;
        for (int s = 0; s < MP_DROPIN_SLOTS; s++) n += r.present[s] ? 1 : 0;
        return n;
    }

    // memfix: free seats = what the room can still take (0 when drop-in is off or the room is full).
    static inline int freeSeats(const SceneRoster& r) {
        int maxP = r.maxPlayers;
        if (maxP > MP_DROPIN_SLOTS) maxP = MP_DROPIN_SLOTS;
        if (!r.dropInAllowed) return 0;
        const int n = maxP - presentCount(r);
        return n > 0 ? n : 0;
    }

    // Bit c set = make a spare of character c: drop-in allowed, a free seat, nobody present (us included) has c. memfix: at most as
    // many spares as there are free seats (a late joiner takes one: more would only cost scene memory), the first characters nobody
    // has in character order (Mario, Luigi, Toad: the lobby's default picks come first). A joiner whose character has no spare is shown by the existing fallbacks (a see-through stand-in of its family, else hidden until the next
    // scene) and its name tag / party strip entry.
    static inline uint32_t spareLooks(const SceneRoster& r) {
        const int seats = freeSeats(r);
        if (seats == 0) return 0;
        uint32_t taken = 0;
        for (int s = 0; s < MP_DROPIN_SLOTS; s++)
            if (r.present[s]) taken |= 1u << clampLook(r.look[s]);
        const uint32_t cand = ((1u << MP_DROPIN_LOOKS) - 1u) & ~taken;
        uint32_t out = 0;
        int n = 0;
        for (int c = 0; c < MP_DROPIN_LOOKS && n < seats; c++)
            if (cand & (1u << c)) {
                out |= 1u << c;
                ++n;
            }
        return out;
    }

    // integ (B1): the characters whose model archives the scene must REQUEST in its archive-request phase: everybody present (us
    // included) and the spares' characters. An archive can not be mounted later from the scene init thread (initScene), so a spare
    // whose archive was not requested here is not made at all (the late joiner of that character stays hidden until the next scene).
    static inline uint32_t archiveLooks(const SceneRoster& r) {
        uint32_t m = spareLooks(r);
        for (int s = 0; s < MP_DROPIN_SLOTS; s++)
            if (r.present[s]) m |= 1u << clampLook(r.look[s]);
        return m;
    }

    // ---- binding, once per turn -----------------------------------------------------------------------------------------------
    // pool[c]: the slot the scene's puppet of character c is bound to (-1 spare/unbound), or -2 when the scene has no puppet of c.
    // bound[s]: the character of the puppet bound to remote slot s, or -1. The local slot is never touched (self puppet).
    struct Binding {
        int8_t pool[MP_DROPIN_LOOKS];
        int8_t bound[MP_DROPIN_SLOTS];
        bool standIn[MP_DROPIN_SLOTS];   // bound[s] is a see-through stand-in of the same family, not the player's character
    };
    enum Action : uint8_t { ACT_NONE = 0, ACT_UNBIND = 1, ACT_BIND = 2, ACT_REBIND = 3, ACT_HIDDEN = 4 };
    struct SlotStep {
        uint8_t action;
        int8_t oldChar;    // UNBIND / REBIND: the character given back to the pool
        int8_t newChar;    // BIND / REBIND: the character taken; HIDDEN: the character that has no puppet
    };
    // Updates b in place and says per slot what changed. Unbinding runs for every slot before any binding, so a character freed
    // by one slot this turn can be taken by another the same turn.
    static inline void step(const SceneRoster& r, Binding& b, SlotStep out[MP_DROPIN_SLOTS]) {
        for (int s = 0; s < MP_DROPIN_SLOTS; s++) {
            out[s].action = ACT_NONE;
            out[s].oldChar = -1;
            out[s].newChar = -1;
        }
        for (int s = 0; s < MP_DROPIN_SLOTS; s++) {
            if (s == r.localSlot) continue;
            const int have = b.bound[s];
            if (have < 0) continue;
            bool keep = r.present[s];
            const int want = clampLook(r.look[s]);
            if (keep && b.standIn[s] && want < MP_DROPIN_LOOKS && b.pool[want] == -1) {
                keep = false;   // the real character's puppet is free now: trade the stand-in for it
            }
            if (keep && want != have) {
                // a LOOK change applies at the next scene (as the player's own self puppet does), unless another present
                // player now has that character: then this one gives it up (never two of a character)
                for (int o = 0; o < MP_DROPIN_SLOTS; o++)
                    if (o != s && r.present[o] && clampLook(r.look[o]) == have) keep = false;
            }
            if (!keep) {
                if (have < MP_DROPIN_LOOKS && b.pool[have] == s) b.pool[have] = -1;
                b.bound[s] = -1;
                b.standIn[s] = false;
                out[s].action = ACT_UNBIND;
                out[s].oldChar = (int8_t)have;
            }
        }
        for (int s = 0; s < MP_DROPIN_SLOTS; s++) {
            if (s == r.localSlot || !r.present[s] || b.bound[s] >= 0) continue;
            const int want = clampLook(r.look[s]);
            if (b.pool[want] == -1) {
                b.pool[want] = (int8_t)s;
                b.bound[s] = (int8_t)want;
                b.standIn[s] = false;
                out[s].action = out[s].action == ACT_UNBIND ? ACT_REBIND : ACT_BIND;
                out[s].newChar = (int8_t)want;
            } else if (out[s].action == ACT_NONE || out[s].action == ACT_UNBIND) {
                // no puppet of that character in this scene (or, roster glitch, another slot holds it): hidden, never a stand-in
                out[s].action = out[s].action == ACT_UNBIND ? ACT_UNBIND : ACT_HIDDEN;
                out[s].newChar = (int8_t)want;
            }
        }
    }

    // ---- stand-in: no puppet of the player's character in this scene (the memory guard said no, the room was full at init)
    // A spare of the same family nobody has, drawn see-through, until the real one is free or the next scene: Mario <-> Cosmic
    // Mario, Luigi <-> Cosmic Luigi. Toads get none (the engine draws Toad: no see-through), they stay hidden.
    static inline int family(int c) {
        switch (c) {
        case 0: case 3: return 0;   // Mario, Cosmic Mario
        case 1: case 5: return 1;   // Luigi, Cosmic Luigi
        default: return 2;          // Toad, Yellow Toad
        }
    }
    static inline int standInFor(const SceneRoster& r, const Binding& b, int want) {
        if (want < 0 || want >= MP_DROPIN_LOOKS || family(want) == 2) return -1;
        for (int c = 0; c < MP_DROPIN_LOOKS; c++) {
            if (c == want || family(c) != family(want) || b.pool[c] != -1) continue;
            bool used = false;
            for (int s = 0; s < MP_DROPIN_SLOTS; s++) used = used || (r.present[s] && clampLook(r.look[s]) == c);
            if (!used) return c;
        }
        return -1;
    }
    static inline void bindStandIn(Binding& b, int slot, int c) {
        b.pool[c] = (int8_t)slot;
        b.bound[slot] = (int8_t)c;
        b.standIn[slot] = true;
    }

    // ---- arrival (a puppet bound mid-scene, at its first appearance) --------------------------------------------------------
    enum { ARRIVE_TURNS = 24 };   // 0.4 s
    static const float kArriveBehind = 110.0f;   // units behind the target (Mario is ~160 tall, his body radius 50)
    static const float kArriveAbove = 60.0f;
    // left = turns still to go (ARRIVE_TURNS .. 1). Grows from 35 % with an ease-out and a small overshoot, ends at exactly 1.
    static inline float arrivalScale(int left) {
        if (left <= 0) return 1.0f;
        if (left > ARRIVE_TURNS) left = ARRIVE_TURNS;
        const float t = 1.0f - (float)left / (float)ARRIVE_TURNS;   // 0 .. (1 - 1/24)
        const float u = 1.0f - t;
        const float ease = 1.0f - u * u * u;                          // ease-out cubic
        const float overshoot = 0.12f * ease * u * 4.0f;              // a little bulge in the middle, none at the ends
        return 0.35f + 0.65f * ease + overshoot;
    }
    // Opacity 0..255 over the first two thirds of the arrival, then opaque.
    static inline uint8_t arrivalAlpha(int left) {
        if (left <= 0) return 255;
        if (left > ARRIVE_TURNS) left = ARRIVE_TURNS;
        const int done = ARRIVE_TURNS - left;                 // 0 .. 23
        const int ramp = (ARRIVE_TURNS * 2) / 3;              // 16 turns
        if (done >= ramp) return 255;
        const int a = 40 + (215 * done) / ramp;
        return (uint8_t)(a > 255 ? 255 : a);
    }

    // ---- where the joiner arrives: a little behind the target player and above him (he drops onto the ground there), never
    // inside him. pos: the target's position; q: his rotation quaternion x,y,z,w as on the wire (int16 * 32767).
    // False when the input is not usable (non-finite, zero quaternion): then no warp.
    static inline bool arrivalPoint(const float pos[3], const int16_t q16[4], float out[3]) {
        float q[4];
        float n = 0.0f;
        for (int i = 0; i < 4; i++) {
            q[i] = (float)q16[i] / 32767.0f;
            n += q[i] * q[i];
        }
        if (!(n > 0.25f && n < 4.0f)) return false;
        const float inv = 1.0f / __builtin_sqrtf(n);
        for (int i = 0; i < 4; i++) q[i] *= inv;
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        // columns of the base matrix (MpPoseMath::mtxFromQuat): 1 = up, 2 = front
        const float up[3] = {2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w)};
        const float front[3] = {2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y)};
        for (int i = 0; i < 3; i++) {
            if (!(pos[i] > -1e6f && pos[i] < 1e6f)) return false;
            out[i] = pos[i] - front[i] * kArriveBehind + up[i] * kArriveAbove;
        }
        return true;
    }

    // ---- memory guard for the spares ---------------------------------------------------------------------------------------
    // A spare's archive goes to the scene heap (MEM1), not to the file cache: the file cache is the tight one on the Vita
    // (EggStar 1: 1.57 MB of 17.04 MB left in single player), the scene heap keeps >= 8.7 MB in every galaxy measured (perf
    // agent, armhf build with the Vita's heaps) and is freed with the scene. A spare is made only when the scene heap keeps
    // kKeepNapa after it (the rest of the scene's init and its play allocate after the puppets: see "puppet heap" lines).
    static const uint32_t kKeepNapa = 0x300000;     // 3 MiB
    static inline bool spareFits(uint32_t napaFree, uint32_t cost) {
        return napaFree >= kKeepNapa && napaFree - kKeepNapa >= cost;
    }
    // fix2: the scene's NameObjHolder has a FIXED capacity (0x1300) and its push_back is unchecked: a puppet (or a 3D head) is made only
    // while at least kObjSlotsKeep slots are left (slotsLeft < 0: unknown, e.g. a host test). Below it: the puppet is hidden / no spare /
    // the character select keeps its 2D heads.
    static const int kObjSlotsKeep = 64;
    static inline bool objSlotsOk(int slotsLeft) { return slotsLeft < 0 || slotsLeft >= kObjSlotsKeep; }
}  // namespace MpDropIn
