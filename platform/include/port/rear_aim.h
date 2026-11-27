// Rear touch aiming, the pure rules (crash33, 14 Nov; host test: tests/rear_aim.cpp, tools/test_rear_aim.sh). No SDK types.
//
// The rear pad works like a laptop's track pad: a finger sliding on it moves the pointer by how far it slides. The hands that hold
// the console rest on the same pad, and the report of 14 Nov (build c106, "the star bit pointer was moving everywhere") showed that
// the earlier rules (GXV_REAR_STRICT=0 keeps them) let those hands move the pointer:
//   1. a contact took the pointer once it was 40 panel units (2 % of the pad, about 3 mm) from where it LANDED, however long that
//      took: every resting finger creeps that far within seconds of play (thumbs working the sticks rock the whole hand), so every
//      resting finger armed sooner or later;
//   2. once armed, the contact kept the pointer until it lifted, and every movement over 1.5 units moved the pointer (100 units of
//      a shifting grip = 18 % of the picture), and the pointer stayed on screen for as long as that finger stayed down;
//   3. when the armed contact lifted, the next contact that was 40 units from its own landing place (any other resting finger,
//      measured since it landed minutes ago) took the pointer at once;
//   4. a palm or two merging fingertips change the pad's contact id: a new contact, a new landing place, a new arm;
//   5. nothing looked at WHERE a contact was (the outer bands are where the holding fingers lie) or at HOW MANY there were.
// The strict rules (default):
//   A. grip bands: a contact that lands in the outer GXV_REAR_GRIP_PCT (25) % of the pad's width on either side, or in the top or
//      bottom 8 % of its height, never takes the pointer (a contact that landed in the middle may slide out there);
//   B. one deliberate slide: a contact arms only by sliding 48 units (2.5 % of the pad) within 0.35 s; a contact that has not, has
//      its start moved to where it is now, so slow creep never adds up;
//   C. one finger: while two or more contacts of the middle are in motion (not resting: moved 12 units within the last 0.5 s, or
//      newly landed) nothing moves and the pointer is let go; the one that stays has to slide again;
//   D. let go at rest: an armed contact that has not moved the pointer for 1.5 s lets it go (a finger that aimed and then
//      rests does not drift the pointer with the hand's wobble); the pointer's 3 s on screen count from its last MOVEMENT;
//   E. no jumps: a step of more than 300 units in one sample is the pad swapping or merging contacts, never a finger: it
//      is not applied;
//   F. no ghosts: before a contact has the pointer, a step of more than 150 units in one sample sets its start to where it is, and
//      a contact that has been on the pad for under 0.03 s cannot take the pointer, nor one whose distance came from one hop (the
//      largest single step may be at most 60 % of the slide).
#pragma once

#include <cmath>
#include <cstring>

namespace port {
namespace rear {

struct Contact { int id; float x, y; };
struct Rect { float x0, y0, x1, y1; }; // the pad's coordinate range
struct Params {
    float arm, armQuick, noise;
    float gripX, gripY;   // fractions of the pad's width / height (0 = no band)
    float armWindow;      // seconds (0 = no limit: the earlier rule)
    float idle;           // seconds (0 = never let go at rest)
    float jump;           // panel units a sample (0 = no check)
    bool single, accumulate, lifeOnMoveOnly;
    float minAge;         // seconds a contact must have been down before it can take the pointer (0 = at once)
    float spread;         // the largest single step of the arming slide, as a share of its length, at most this (0 = no check)
};
inline Params LegacyParams() { return {40.0f, 0.25f, 1.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, true, false, 0.0f, 0.0f}; }
inline Params StrictParams(float gripPct = 25.0f) {
    if (!(gripPct >= 0.0f)) gripPct = 0.0f;
    if (gripPct > 45.0f) gripPct = 45.0f;
    return {48.0f, 0.35f, 1.5f, gripPct / 100.0f, 0.08f, 0.35f, 1.5f, 300.0f, true, true, true, 0.03f, 0.6f};
}
constexpr float kRestMove = 12.0f;   // rule C: a contact that moved this far ...
constexpr float kRestSeconds = 0.5f; // ... within this long is in motion

struct Stats { unsigned arms, gripLandings, multiSamples, creeps, idleDrops, jumps, samples; };

class Aim {
public:
    void Configure(const Params& p) { p_ = p; }
    const Params& params() const { return p_; }
    void Reset() { id_ = -1; count_ = 0; lastX_ = lastY_ = velX_ = velY_ = idle_ = 0; std::memset(seen_, 0, sizeof seen_); }
    // The contacts on the pad now never take the pointer (after an input reset); ones put down later can.
    void Block(const Contact* c, int n) {
        Reset();
        for (int i = 0; i < n && i < kMax; i++) seen_[count_++] = {c[i].id, c[i].x, c[i].y, 0, c[i].x, c[i].y, 0, true, false, c[i].x, c[i].y, 0};
    }
    bool aiming() const { return id_ >= 0; }
    const Stats& stats() const { return st_; }
    void ClearStats() { st_ = Stats{}; }
    // One sample. True when a contact holds the pointer; *dx, *dy are then the movement to apply (panel units, smoothed), *show says
    // whether this sample keeps the pointer on screen, *fresh that the contact took the pointer in this sample.
    bool Step(const Contact* c, int n, float dt, const Rect& r, float* dx, float* dy, bool* show, bool* fresh) {
        *dx = *dy = 0;
        *show = *fresh = false;
        if (n > kMax) n = kMax;
        if (n <= 0) { id_ = -1; count_ = 0; return false; }
        ++st_.samples;
        // forget contacts that have lifted, note new ones where they landed
        Seen kept[kMax];
        for (int i = 0; i < n; i++) {
            Seen s{c[i].id, c[i].x, c[i].y, 0, c[i].x, c[i].y, 0, false, InGrip(c[i].x, c[i].y, r), c[i].x, c[i].y, 0};
            bool known = false;
            for (int k = 0; k < count_; k++) {
                if (seen_[k].id == c[i].id) { s = seen_[k]; s.age += dt; s.quiet += dt; known = true; break; }
            }
            if (!known && s.grip) ++st_.gripLandings;
            const float mx = c[i].x - s.rx, my = c[i].y - s.ry;
            if (mx * mx + my * my >= kRestMove * kRestMove) { s.rx = c[i].x; s.ry = c[i].y; s.quiet = 0; }
            kept[i] = s;
        }
        std::memcpy(seen_, kept, sizeof(Seen) * static_cast<size_t>(n));
        count_ = n;
        if (p_.single) { // rule C
            int moving = 0;
            for (int i = 0; i < n; i++) if (!seen_[i].grip && !seen_[i].barred && seen_[i].quiet < kRestSeconds) ++moving;
            if (moving >= 2) {
                ++st_.multiSamples;
                Drop();
                for (int i = 0; i < n; i++) Anchor(seen_[i], c[i]);
                return false;
            }
        }
        int ai = -1;
        for (int i = 0; i < n; i++) if (c[i].id == id_) ai = i;
        if (ai >= 0 && p_.idle > 0 && idle_ >= p_.idle) { // rule D
            ++st_.idleDrops;
            Drop();
            Anchor(seen_[ai], c[ai]);
            ai = -1;
        }
        if (ai < 0) {
            id_ = -1;
            for (int i = 0; i < n && ai < 0; i++) {
                Seen& s = seen_[i];
                const float sx = c[i].x - s.px, sy = c[i].y - s.py; // this sample's step
                s.px = c[i].x; s.py = c[i].y;
                if (s.barred || s.grip) continue;
                // rule F (a contact nobody made: the owner's report of 14 Nov, "I didn't have fingers on the back"): a contact that
                // moves half of kJump in one sample before it has the pointer is the pad's own noise or a swapped id, not a slide:
                // its start is set to where it is; and a contact takes the pointer only after minAge on the pad (a blip cannot).
                if (p_.jump > 0 && (std::fabs(sx) > p_.jump * 0.5f || std::fabs(sy) > p_.jump * 0.5f)) { ++st_.jumps; Anchor(s, c[i]); continue; }
                const float step2 = sx * sx + sy * sy;
                if (step2 > s.maxStep) s.maxStep = step2;
                const float ax = c[i].x - s.x0, ay = c[i].y - s.y0;
                // (a slide is made of several samples: a contact that got its distance in one step, a position that hopped, waits for more)
                const bool spreadOk = p_.spread <= 0 || s.maxStep <= p_.spread * p_.spread * (ax * ax + ay * ay);
                if (ax * ax + ay * ay >= p_.arm * p_.arm && s.age >= p_.minAge && spreadOk) {
                    ai = i;
                    id_ = c[i].id;
                    const bool quick = s.age <= p_.armQuick; // the start of a quick slide is not lost
                    lastX_ = quick ? s.x0 : c[i].x;
                    lastY_ = quick ? s.y0 : c[i].y;
                    velX_ = velY_ = idle_ = 0;
                    *fresh = true;
                    ++st_.arms;
                } else if (p_.armWindow > 0 && s.age > p_.armWindow) { // rule B: creep does not add up
                    if (ax * ax + ay * ay >= 1.0f) ++st_.creeps;
                    Anchor(s, c[i]);
                }
            }
            if (ai < 0) return false;
        }
        float mx = c[ai].x - lastX_, my = c[ai].y - lastY_;
        if (p_.jump > 0 && !*fresh && (std::fabs(mx) > p_.jump || std::fabs(my) > p_.jump)) { // rule E
            ++st_.jumps;
            lastX_ = c[ai].x; lastY_ = c[ai].y;
            mx = my = 0;
            velX_ = velY_ = 0;
        }
        // A movement below the pad's noise is kept until it adds up to more (the reference only moves on with the pointer), so a
        // slow, careful slide still moves the pointer.
        if (!p_.accumulate) { lastX_ = c[ai].x; lastY_ = c[ai].y; }
        if (std::fabs(mx) >= p_.noise) lastX_ = c[ai].x; else mx = 0;
        if (std::fabs(my) >= p_.noise) lastY_ = c[ai].y; else my = 0;
        if (mx != 0 || my != 0) idle_ = 0; else idle_ += dt;
        velX_ = velX_ * 0.5f + mx * 0.5f; // a short running average takes the pad's jitter out
        velY_ = velY_ * 0.5f + my * 0.5f;
        if (std::fabs(velX_) < 0.01f) velX_ = 0;
        if (std::fabs(velY_) < 0.01f) velY_ = 0;
        *dx = velX_;
        *dy = velY_;
        *show = !p_.lifeOnMoveOnly || *fresh || velX_ != 0 || velY_ != 0;
        return true;
    }

private:
    enum { kMax = 8 };
    struct Seen { int id; float x0, y0, age, rx, ry, quiet; bool barred, grip; float px, py, maxStep; };
    bool InGrip(float x, float y, const Rect& r) const {
        const float w = r.x1 - r.x0, h = r.y1 - r.y0;
        if (!(w > 0) || !(h > 0)) return false;
        const float fx = (x - r.x0) / w, fy = (y - r.y0) / h;
        return fx < p_.gripX || fx > 1.0f - p_.gripX || fy < p_.gripY || fy > 1.0f - p_.gripY;
    }
    static void Anchor(Seen& s, const Contact& c) { s.x0 = c.x; s.y0 = c.y; s.age = 0; s.maxStep = 0; }
    // (age restarts with the start point: the 0.35 s of rule B and minAge both count from it)
    void Drop() { id_ = -1; velX_ = velY_ = idle_ = 0; }
    Params p_ = StrictParams();
    Seen seen_[kMax] = {};
    int count_ = 0, id_ = -1;
    float lastX_ = 0, lastY_ = 0, velX_ = 0, velY_ = 0, idle_ = 0;
    Stats st_ = {};
};

} // namespace rear
} // namespace port
