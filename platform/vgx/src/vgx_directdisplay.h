// GXV_DIRECT_DISPLAY (direct24): the decision, the buffer rotation and the census of showing the EFB's own colour buffer as the display
// picture, with no separate "present" scene. No GL, no SDK: everything the machine does to the world goes through a small API class, so
// tests/vgx_directdisplay_test.cpp drives it against a fake (the real one is in vgx_backend_gl.cpp, "namespace dd").
//
// The idea (see REPORT.txt of direct24): at 960x544 (rung 6, 1x) the EFB colour buffer is exactly the display picture, in the order the display
// wants (row 0 = the picture's top row). The EFB's colour buffers are made displayable (4096-aligned linear A8B8G8R8 storage, each with its own
// sync object: vitaGL patch 0030), the picture's last EFB scene is begun with that buffer's sync object, and the buffer is queued to the display
// as it is. The EFB then goes on with another buffer (the next picture starts with a whole clear, so it needs nothing of the old one) and the
// queued one stays out of every use for `kFlightPictures` pictures: the display holds it, and a scene begun on a buffer whose release depends on an
// entry not queued yet would wait for it forever.
//
// A picture is shown this way only when every test passes; every doubt is today's present (reason counted). Once a scene fails to begin (vitaGL's
// scene-begin failure counter grows) the whole thing is off for good.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vgx::dd {

enum Reason : int {
    kDirect = 0,      // shown directly
    kSwitchOff,       // GXV_DIRECT_DISPLAY=0 (never counted: the machine is not asked)
    kNoApi,           // vitaGL without patch 0030
    kSelfOff,         // a scene failed to begin, or a buffer could not be made displayable: off for the rest of the run
    kSize,            // the render size is not the screen's, or the copy is not the whole EFB at 1:1
    kMsaa,            // multisampled EFB (rungs 7-8)
    kHud,             // GXV_HUD_NATIVE's tail is being drawn into the display
    kClear,           // the picture's copy clears the EFB in part (a rectangle or some planes): the successor could not hold the colour that stays
    kCapture,         // a capture (screenshot, verification) reads the EFB after the present
    kSharpen,         // the present would sharpen / smooth edges / draw the characters' edge smoothing: more than a copy
    kNotDisplayable,  // the EFB's colour buffer is not a display texture (made before the switch, or no memory for one)
    kNoSuccessor,     // no buffer to go on with
    kRefused,         // the host or vitaGL did not take the flip (vglSetNextFlip)
    kHost,            // the host's present is not allowed / is not a plain whole-screen picture
    kNoPlan,          // the copy does not clear and the frame planning (GXV_DEPTHPLAN / GXV_EFBSWAP) is off: nobody can say the next picture overwrites the colour whole
    kLive,            // the last pictures after a flip began with the old colour live (read or partly kept): flipping them costs a draw-back each, so it waits (back-off)
    kHrw,             // (dd24, GXV_DD_DYN) a 60 Assist pair is armed, about to arm, or ended less than kHrwLead presents ago: the present is the old way
    kCount
};
inline const char* ReasonName(int r) {
    static const char* const n[kCount] = {"direct", "off", "no-api", "self-off", "size", "msaa", "hud", "clear", "capture", "sharpen",
                                          "not-displayable", "no-successor", "refused", "host", "no-plan", "live", "hrw"};
    return r >= 0 && r < kCount ? n[r] : "?";
}

// sg12 (21 Nov 2026): the HRW guard. c163 found the lives-counter flicker (and the Game Over strip, the map's Back button) only under the pairs of
// GXV_HRW=5: a flipped EFB buffer came back into use while the display still held it (the pair's first scenes do not carry the buffer's sync object).
// 60 Assist is read once at start-up (GXV_HRW is 5 with it, 0 without: settings_launch.cpp), so the guard is per run: the direct display never runs while
// GXV_HRW is not 0 (every picture of the run may be one of a pair, and the pairs' buffers are the ones that are reused). It is applied where the switch is read
// (Mode in vgx_backend_gl.cpp, the two reads in vgx_draw.cpp), so nothing of the machine (display textures, spares, the covering-rectangle mark) is made
// either. GXV_DIRECT_DISPLAY_HRW=1 (default 0, a diagnostic) lifts the guard for a console A/B of the flicker.
// requested: GXV_DIRECT_DISPLAY; hrw: GXV_HRW; lift: GXV_DIRECT_DISPLAY_HRW.
// dd24: GXV_DD_DYN=1 (default 0) lifts the guard for the whole run and replaces it by a per-picture decision (Inputs::hrwHold, Decide, the game thread's hold and its drain
// handshake in vgx_galaxy.cpp): the direct display runs while no pair is armed or about to arm. `dyn` is that switch.
inline long EffectiveMode(long requested, long hrw, long lift = 0, long dyn = 0) {
    if (requested != 1) return requested;
    return hrw != 0 && lift != 1 && dyn != 1 ? 0 : requested;
}
constexpr uint64_t kHrwLead = 4; // (dd24) presents after the last pair picture before the direct display may flip again
// The pure rule of the hold: a picture may be flipped only when no pair is held by the game thread (engaged or wanted) and the last pair picture is kHrwLead presents back.
inline bool HrwHold(bool gameHold, bool pairPicture, uint64_t presents, uint64_t lastPairPresent) {
    return gameHold || pairPicture || (lastPairPresent != 0 && presents < lastPairPresent + kHrwLead);
}

// ddfix25 (night60: with GXV_DD_DYN=1 Honeyhive hit vitaGL's 16 display textures (VGL_DD_MAX) at picture ~2300, free video memory went 40 -> 8 MiB, vitaGL's allocator then waited ~4-8 s a picture for
// its collector, and the machine went off for good). Under GXV_DD_DYN no new display texture is made while 12 or more are live, less than 16 MiB of video memory is free, or the last refusal is
// less than kDynCooldown presents back: the picture is shown the old way (kNoSuccessor) or the texture copy is the ordinary one. A refusal by vitaGL under GXV_DD_DYN starts the same cool-down
// instead of switching the machine off. Pure rule, tested by tests/vgx_directdisplay_test.cpp.
constexpr unsigned kDynMaxTextures = 12;
constexpr uint64_t kDynVramMin = 16ull << 20;
constexpr uint64_t kDynCooldown = 120;
inline bool DynMakeDenied(unsigned liveDisplayTextures, uint64_t freeVram, uint64_t presents, uint64_t coolUntil) {
    return presents < coolUntil || liveDisplayTextures >= kDynMaxTextures || freeVram < kDynVramMin;
}

// What the decision looks at (the back end fills it, the host adds its own part).
struct Inputs {
    int mode = 0;                 // GXV_DIRECT_DISPLAY (1 = flip; 0 and 2 never flip)
    bool api = false;             // vitaGL's patch 0030 is there
    bool selfOff = false;
    bool keepRt = false;          // GXV_KEEP_RT (the emulator): render targets are never given up, no rotation
    int x = 0, y = 0, w = 0, h = 0;   // the copy rectangle, in render pixels
    int rtW = 0, rtH = 0;         // the EFB's size
    int screenW = 0, screenH = 0; // the display
    int dx0 = 0, dx1 = 0;         // the display rectangle (x range)
    int samples = 1;
    bool hudEngaged = false;
    bool clearWhole = false;      // the copy clears colour and alpha of the whole EFB (the successor then needs nothing of the old colour)
    bool clearEnable = false;     // the copy clears at all (not elided)
    bool planOn = false;          // the frame planning that can prove the next picture overwrites the colour whole is on (kNoPlan otherwise)
    bool haveCopy = false;        // the copy's command was there (statistics only)
    int clearColor = 0, clearAlpha = 0, clearDepth = 0, clearElided = 0; // (statistics only: the state behind kClear)
    bool capture = false;
    float sharpen = 0.0f;
    bool edgeAA = false;
    bool chara = false;
    bool displayable = false;     // the EFB's colour buffer is a display texture
    bool phaseZero = true;        // the host's whole present (not the HUD tail's two halves)
    bool hostOk = true;
    bool hrwHold = false;         // (dd24) HrwHold(): a pair is held / drawn / too recent
};
inline Reason Decide(const Inputs& i) {
    if (i.mode != 1) return kSwitchOff;
    if (!i.api) return kNoApi;
    if (i.selfOff) return kSelfOff;
    if (i.keepRt) return kNotDisplayable;
    if (!i.hostOk) return kHost;
    if (i.hrwHold) return kHrw;
    if (i.hudEngaged || !i.phaseZero) return kHud;
    if (i.samples > 1) return kMsaa;
    if (i.capture) return kCapture;
    if (i.x != 0 || i.y != 0 || i.w != i.rtW || i.h != i.rtH || i.w != i.screenW || i.h != i.screenH || i.dx0 != 0 || i.dx1 != i.screenW) return kSize;
    if (i.sharpen >= 0.05f || i.edgeAA || i.chara) return kSharpen;
    if (i.clearEnable && !i.clearWhole) return kClear;      // a partial clear: the old colour partly stays
    if (!i.clearEnable && !i.planOn) return kNoPlan;        // no clear: the next picture's start must be known to overwrite whole (r3)
    if (!i.displayable) return kNotDisplayable;
    return kDirect;
}

// The state behind a fallback, for the census line (the last picture that took it).
inline int Describe(char* out, size_t cap, Reason r, const Inputs& i) {
    switch (r) {
    case kClear:
        return std::snprintf(out, cap, "copy%s clear enable %d color %d alpha %d depth %d elided %d, whole %d, rect %d,%d %dx%d of EFB %dx%d", i.haveCopy ? "" : " (no command)", i.clearEnable ? 1 : 0,
                             i.clearColor, i.clearAlpha, i.clearDepth, i.clearElided, i.clearWhole ? 1 : 0, i.x, i.y, i.w, i.h, i.rtW, i.rtH);
    case kNoPlan:
        return std::snprintf(out, cap, "copy clear enable %d, frame planning %d", i.clearEnable ? 1 : 0, i.planOn ? 1 : 0);
    case kSize:
        return std::snprintf(out, cap, "copy rect %d,%d %dx%d, EFB %dx%d, screen %dx%d, display x %d..%d", i.x, i.y, i.w, i.h, i.rtW, i.rtH, i.screenW, i.screenH, i.dx0, i.dx1);
    case kSharpen:
        return std::snprintf(out, cap, "sharpen %.2f edgeAA %d chara %d", static_cast<double>(i.sharpen), i.edgeAA ? 1 : 0, i.chara ? 1 : 0);
    case kMsaa:
        return std::snprintf(out, cap, "samples %d", i.samples);
    case kHud:
        return std::snprintf(out, cap, "hud engaged %d, phase zero %d", i.hudEngaged ? 1 : 0, i.phaseZero ? 1 : 0);
    case kCapture:
        return std::snprintf(out, cap, "capture %d", i.capture ? 1 : 0);
    case kNotDisplayable:
        return std::snprintf(out, cap, "displayable %d keepRt %d", i.displayable ? 1 : 0, i.keepRt ? 1 : 0);
    case kHrw:
        return std::snprintf(out, cap, "a 60 Assist pair is held, drawn or less than %d presents back", static_cast<int>(kHrwLead));
    case kLive:
        return std::snprintf(out, cap, "the last flips' next pictures began with live colour or alpha (planning %d)", i.planOn ? 1 : 0);
    default:
        return std::snprintf(out, cap, "-");
    }
}
struct Census {
    uint64_t pictures = 0, direct = 0, flips = 0, reason[kCount] = {};
    uint64_t startsDead = 0, startsRestored = 0; // after a flip: the next picture's start found the colour dead (nothing done) / live (drawn back from the picture shown)
    uint64_t lastLog = 0;
    uint64_t switches = 0;                       // (dd24) pictures whose way (direct / old present) differs from the picture before
    int lastDirect = -1;
    uint64_t dynReturned = 0, dynHeld = 0;       // (pace25) display textures given back to the spares instead of becoming a texture's storage (AdoptEfbBuffer) / display textures a plain texture holds now
    uint64_t dynDenied = 0, dynRefused = 0;      // (ddfix25) display textures not made under GXV_DD_DYN (count / video memory / cool-down); refusals by vitaGL that started a cool-down instead of the end
    Inputs last[kCount];                         // the inputs of the last picture that took each fallback
    void Note(Reason r, const Inputs* in = nullptr) {
        ++pictures;
        { const int d = r == kDirect ? 1 : 0; if (lastDirect >= 0 && lastDirect != d) ++switches; lastDirect = d; }
        if (r == kDirect) ++direct;
        else {
            ++reason[r];
            if (in) last[r] = *in;
        }
    }
    bool LineDue(uint64_t every) const { return every != 0 && pictures >= lastLog + every; }
    // The basic-log line, every `every` pictures (0: never); returns the text length, 0 when not due.
    int Line(char* out, size_t cap, uint64_t every, int mode, bool selfOff, uint64_t refusedByVitaGL, bool dyn = false) {
        if (every == 0 || pictures < lastLog + every) return 0;
        lastLog = pictures;
        int n = std::snprintf(out, cap, "direct-display: GXV_DIRECT_DISPLAY=%d%s: %llu pictures, %llu shown directly (flips %llu, vitaGL refused %llu; next-picture starts: colour dead %llu, drawn back %llu); fallbacks:", mode,
                              selfOff ? " (SELF-DISABLED)" : "", static_cast<unsigned long long>(pictures), static_cast<unsigned long long>(direct),
                              static_cast<unsigned long long>(flips), static_cast<unsigned long long>(refusedByVitaGL), static_cast<unsigned long long>(startsDead),
                              static_cast<unsigned long long>(startsRestored));
        if (dyn && n > 0 && n < static_cast<int>(cap)) // (dd24) the verify figures of GXV_DD_DYN: the share shown directly, the way changes, the pictures held back for a pair
            n += std::snprintf(out + n, cap - static_cast<size_t>(n), " DD_DYN: %.1f %% direct, mode switches %llu, held for 60 Assist %llu, no new display texture %llu, vitaGL refusals ridden out %llu, display textures returned to the spares (not adopted) %llu, held by plain textures now %llu;", pictures ? 100.0 * static_cast<double>(direct) / static_cast<double>(pictures) : 0.0,
                               static_cast<unsigned long long>(switches), static_cast<unsigned long long>(reason[kHrw]), static_cast<unsigned long long>(dynDenied), static_cast<unsigned long long>(dynRefused), static_cast<unsigned long long>(dynReturned), static_cast<unsigned long long>(dynHeld));
        bool any = false;
        int top = -1;
        for (int r = kSwitchOff; r < kCount && n > 0 && n < static_cast<int>(cap); ++r)
            if (reason[r]) {
                n += std::snprintf(out + n, cap - static_cast<size_t>(n), " %s %llu,", ReasonName(r), static_cast<unsigned long long>(reason[r]));
                any = true;
                if (top < 0 || reason[r] > reason[top]) top = r;
            }
        if (!any && n > 0 && n < static_cast<int>(cap)) n += std::snprintf(out + n, cap - static_cast<size_t>(n), " none");
        if (top >= 0 && n > 0 && n < static_cast<int>(cap)) {
            n += std::snprintf(out + n, cap - static_cast<size_t>(n), " top: %s [", ReasonName(top));
            if (n > 0 && n < static_cast<int>(cap)) n += Describe(out + n, cap - static_cast<size_t>(n), static_cast<Reason>(top), last[top]);
            if (n > 0 && n < static_cast<int>(cap)) n += std::snprintf(out + n, cap - static_cast<size_t>(n), "]");
        }
        return n;
    }
};

constexpr int kFlightPictures = 2; // default; GXV_DIRECT_DISPLAY_FLIGHT=2..4 (1 would put a buffer back to use at the very next picture: its scene would wait, on the GPU, for the picture just queued to reach the screen)

// The rotation. Api (real: vgx_backend_gl.cpp; fake: the test):
//   unsigned TakeSuccessor();                  a displayable buffer of the EFB's size, not in use anywhere (a spare, or a new one); 0 = none
//   void GiveBack(unsigned id);                an unused successor goes back to the spares
//   void Rotate(unsigned oldEfb, unsigned next);   the EFB goes on with `next` (attached, state invalid)
//   bool Fits(unsigned id);                    the prepared successor still has the EFB's size
//   void Discard(unsigned id);                 it has not: let it go
//   void Release(unsigned id);                 a buffer that has left the display's hands: spares (or deleted when the size changed)
//   uint32_t BeginFailures();                  vitaGL's count of scenes that failed to begin
template <class Api>
class Machine {
public:
    explicit Machine(Api& api, int flight = kFlightPictures) : a_(api), flight_(flight < 2 ? 2 : flight > 4 ? 4 : flight) {}
    Census census;
    int backoffLen = 120; // pictures that would flip are presented the old way after four live starts in a row (GXV_DIRECT_DISPLAY_BACKOFF)
    bool selfOff = false;
    unsigned Next() const { return next_; }
    size_t InFlight() const { return flights_.size(); }
    const std::vector<std::pair<unsigned, uint64_t>>& Flights() const { return flights_; }

    // Before the picture is shown. Returns the buffer to flip (the EFB's colour) or 0 (today's present; the reason is in *why and counted by
    // Done when the picture was shown by the old way). `in.displayable` is filled here from `efbDisplayable`.
    unsigned Offer(Inputs in, unsigned efb, bool efbDisplayable, Reason* why) {
        ++pictures_;
        Age();
        if (in.mode == 1 && !selfOff && in.api) {
            const uint32_t f = a_.BeginFailures();
            if (!baseSet_) { base_ = f; baseSet_ = true; }
            if (f != base_) { // a scene failed to begin since the mode went on: off for good
                selfOff = true;
                lastWhy_ = kSelfOff;
                DropSuccessor();
            }
        }
        in.selfOff = selfOff;
        in.displayable = efbDisplayable;
        Reason r = Decide(in);
        if (r == kDirect && !in.clearWhole && backoff_ > 0) { --backoff_; r = kLive; } // (the last flips' next pictures needed the old colour back: wait)
        lastIn_ = in;
        if (r == kDirect) {
            // the successor is made now, before anything is flipped: when there is none the picture is shown the old way
            if (next_ != 0 && !a_.Fits(next_)) { // (the render size changed since it was prepared)
                a_.Discard(next_);
                next_ = 0;
            }
            if (next_ == 0) next_ = a_.TakeSuccessor();
            if (next_ == 0) r = kNoSuccessor;
        }
        efb_ = efb;
        offered_ = r == kDirect;
        lastWhy_ = r;
        if (why) *why = r;
        return offered_ ? efb : 0;
    }
    // After the picture was shown (flipped: the host queued `efb` to the display; else the old way, `why` = the reason: the offer's, or kRefused/kHost).
    void Done(bool flipped, Reason why) {
        if (flipped && offered_ && next_ != 0) {
            flights_.emplace_back(efb_, pictures_);
            const unsigned n = next_;
            next_ = 0;
            a_.Rotate(efb_, n);
            ++census.flips;
            census.Note(kDirect);
        } else {
            if (offered_ && why == kDirect) why = kRefused;
            census.Note(offered_ ? why : lastWhy_ != kDirect ? lastWhy_ : why, &lastIn_);
        }
        offered_ = false;
    }
    // The back end, at the start of the picture after a flip whose copy did not clear: the colour was dead there (the picture overwrites it whole before
    // anything reads it: nothing to do) or live (it was drawn back from the picture shown). Four live starts in a row = this game's pictures do not start
    // with a whole overwrite: flipping costs a draw-back each, so the next 120 pictures that would flip are presented the old way (kLive), then it tries again.
    void Settled(bool restored) {
        if (restored) {
            ++census.startsRestored;
            if (++liveStreak_ >= 4) { backoff_ = backoffLen; liveStreak_ = 0; }
        } else {
            ++census.startsDead;
            liveStreak_ = 0;
        }
    }
    // A change of the render size (or the end): the prepared successor has the wrong size
    void DropSuccessor() {
        if (next_) { a_.GiveBack(next_); next_ = 0; }
    }
    void FlushAll() { // (the flights go back at once: the display has been finished with, e.g. after sceGxmFinish)
        DropSuccessor();
        for (const auto& f : flights_) a_.Release(f.first);
        flights_.clear();
    }

private:
    void Age() {
        // A buffer is released flight_ (>= 2) pictures after the one that queued it. The release makes it a spare; it goes back to being the EFB at
        // the end of a later picture, whose own entry is queued by then (the entry whose display releases this buffer: a scene begun on a buffer whose
        // releasing entry is not queued yet would wait for ever). Its first scene then waits, on the GPU, for the entry queued after it to reach the screen:
        // with >= 2 pictures that has long happened.
        size_t keep = 0;
        for (size_t i = 0; i < flights_.size(); ++i) {
            if (pictures_ - flights_[i].second >= static_cast<uint64_t>(flight_)) a_.Release(flights_[i].first);
            else flights_[keep++] = flights_[i];
        }
        flights_.resize(keep);
    }
    Api& a_;
    int flight_;
    uint64_t pictures_ = 0;
    unsigned next_ = 0, efb_ = 0;
    bool offered_ = false, baseSet_ = false;
    uint32_t base_ = 0;
    int liveStreak_ = 0, backoff_ = 0;
    Inputs lastIn_;
    Reason lastWhy_ = kSwitchOff;
    std::vector<std::pair<unsigned, uint64_t>> flights_;
};

} // namespace vgx::dd
