// Per-stage presets (stagepreset25, GXV_STAGE_PRESETS): the table, its text format, and the pure function that turns (stage name, scenario) into the
// preset to apply. Header only and free of the renderer, so a host test builds it (tests/vgx_stagepreset_test.cpp, tools/test_vgx_stagepreset.sh).
// The back end (vgx_backend_gl.cpp, StagePresetPoll) reads the override file, calls Resolve and applies the result; agents/stagepreset25/REPORT.txt has
// the design. No heap, no %zu, bounded buffers, strtol / strtod only (the console's C library is old).
//
// TEXT FORMAT (the compiled-in table, kBuiltinText below, and the optional file ux0:data/smg/stage-presets.txt, which is read after it and wins):
//   # comment                          (also after a line's tokens)
//   @class <name> key=value ...        a class: a named bundle of keys; `class=<other>` inside it inherits (8 deep); a later definition replaces
//   <Stage>[:<scenario>] key=value ... a stage row (the game's internal stage name, e.g. IceVolcanoGalaxy:2); no scenario = every scenario; a later row of the
//                                      same stage and scenario replaces the earlier one
//   * key=value ...                    the default for a stage with no row (and for the title / menus)
// KEYS of a row or a class:
//   class=<name>     start from that class (a row without it starts from the default's class; with it, from this one)
//   start= floor= ceil=   the rungs: a rung number (0..6; 4 = 848x480, 6 = 960x544 in the wide ladder) or a width (848, 848x480, 960, 960x544)
//   size=fixed|down|auto  fixed: no automatic change of size in the stage (floor = ceil = start); down: it may step down in play, never up again; auto: the
//                         controller's own rules between floor and ceil (may hunt: for measuring, not for shipping)
//   fx=free|down|hold     the controller's effect steps (the shed ladder): free as it decides; down: given up when needed, never taken back until the stage
//                         ends; hold: none either way (the preset is the whole answer)
//   edge=off|on      cuts=0|1      the present's edge pass off / the half-rate captures engaged from the first picture
//   ladder=two|full  two (default): only the floor and the ceiling are used, no rung between (the owner's two sizes); full: every rung between
//   emergency=<rung> the only way under the floor constant kShipFloorRung (default: none); floor and start are then limited by it instead
//   any other key=<number>  a stage-settable switch (port/stage_knob.h kDesc: its alias or its GXV_ / PETARI_ name), e.g. indcap_half=2 pb_decor=3500;
//                         resolved at apply time (an unknown or non-settable key is ignored with one log line)
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../include/port/stage_knob.h"
#include "../../include/port/cpulod.h"  // (cpulod26: `cpulod=N` in a preset line)
#include "../../include/port/across.h"  // (across26: `across=N` in a preset line)

namespace vgx::stagepreset {

// THE floor: no preset starts or floors below this rung unless the row names `emergency=<rung>` (the owner's ruling of 11 Nov: 960x544 and 848x480 only).
constexpr int kShipFloorRung = 4;
constexpr int kTopRung = 6;
constexpr int kMsaa2Rung = 7;         // q30aa: 960x544 2x MSAA (only in Quality mode when GXV_Q30_MSAA=1 and hardware FBO_MSAA is available)
constexpr int kNone = -100;           // an unset integer field of a row
constexpr int kMaxKvRow = 16, kMaxKvSpec = 28, kMaxClasses = 24, kMaxRows = 160, kNameLen = 40, kKeyLen = 32, kLine = 256;  // (14 Nov c160: rows 12 -> 16 keys: the base class with its far-cut pins + a tool file's reproj keys)

struct Kv {
    char key[kKeyLen];
    float val;
};
// What one class or row says (kNone / empty = nothing said).
struct Delta {
    char name[kNameLen];  // the class's name / the row's stage ("*" the default)
    int scenario;         // rows: -1 = any
    char cls[kNameLen];   // class= (the base)
    char hz[kNameLen];    // hz= (the class to use at zoom >= 1.4; in a class: the next lighter class)
    int start, floorR, ceil, size, fx, edge, cuts, emergency, two;
    int mode;             // modes26: 0 = a 60-mode row (the default: untagged), 30 = a 30-mode row (`mode=30`); rows only
    int nkv;
    Kv kv[kMaxKvRow];
};
enum Size { kSizeAuto = 0, kSizeFixed = 1, kSizeDown = 2 };
enum Fx { kFxFree = 0, kFxDown = 1, kFxHold = 2 };

struct Table {
    int nClasses = 0, nRows = 0;
    Delta classes[kMaxClasses];
    Delta rows[kMaxRows];
};
struct Diag {                // what the parser found wrong (bounded)
    int lines = 0, rows = 0, classes = 0, errors = 0, dropped = 0;
    int nmsg = 0;
    char msg[6][112];
    void Note(int line, const char* what, const char* detail) {
        ++errors;
        if (nmsg < 6) std::snprintf(msg[nmsg++], sizeof msg[0], "line %d: %.60s%s%.36s", line, what, detail && *detail ? ": " : "", detail ? detail : "");
    }
};

// The preset of one stage, resolved.
struct Spec {
    char cls[kNameLen];
    int zoomed;           // 1: the high-zoom class was taken
    int start, floorR, ceil, size, fx, edge, cuts, emergency, two;
    int nkv;
    Kv kv[kMaxKvSpec];
    unsigned flags;       // kClamped...: what the resolution had to correct
    int matched;          // 0 the default, 1 the stage, 2 the stage and scenario
    // modes26: the picture mode this spec is for (60 or 30) and, in the 30 mode, what the stage's 60 row asked for that was left out (keys; "frame" = the row's rungs / edge / cuts / fx)
    int pictureMode;
    int perf896;          // gpu896: 1 = the Performance rule below made this spec a 960x544 start that may step down once to 896x512 (GPU-limited only)
    int nskip;
    char skip[kMaxKvSpec + 1][kKeyLen];
    // q30-26: the keys the 30 mode kept but at a lower level than the 60 row asked ("alias=asked>kept"; stage_knob.h Desc::bMin / bMax)
    int nclamp;
    char clamp[kMaxKvSpec + 1][kKeyLen + 16];
};
enum Flag : unsigned { kClampedFloor = 1, kOrderFixed = 2, kKvFull = 4, kClassMissing = 8, kStartFixed = 16 };

inline void ClearDelta(Delta& d) {
    std::memset(&d, 0, sizeof d);
    d.scenario = -1;
    d.start = d.floorR = d.ceil = d.size = d.fx = d.edge = d.cuts = d.emergency = d.two = kNone;
}

// ---- parsing ------------------------------------------------------------------------------------------------------------------------------
inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }
// a rung from "4" / "848" / "848x480" / "960x544"
inline bool ParseRung(const char* v, int* out) {
    if (!v || !*v) return false;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    if (end == v) return false;
    if (n >= 0 && n <= 8 && (*end == 0)) { *out = static_cast<int>(n); return true; }
    if (*end == 'x' || *end == 'X') {  // the height must be a number too
        char* e2 = nullptr;
        (void)std::strtol(end + 1, &e2, 10);
        if (e2 == end + 1 || *e2 != 0) return false;
    } else if (*end != 0) {
        return false;
    }
    static const int widths[7] = {608, 664, 720, 784, 848, 896, 960};
    for (int i = 0; i < 7; ++i)
        if (n == widths[i]) { *out = i; return true; }
    return false;
}
inline bool ParseNum(const char* v, float* out) {
    if (!v || !*v) return false;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end == v || *end != 0) return false;
    if (!(d > -1e9 && d < 1e9)) return false;
    *out = static_cast<float>(d);
    return true;
}
inline bool Eq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
    return *a == 0 && *b == 0;
}
inline void CopyName(char* dst, size_t n, const char* src, size_t len) {
    if (len >= n) len = n - 1;
    std::memcpy(dst, src, len);
    dst[len] = 0;
}
inline void AddKv(Delta& d, const char* key, float val) {
    for (int i = 0; i < d.nkv; ++i)
        if (std::strcmp(d.kv[i].key, key) == 0) { d.kv[i].val = val; return; }
    if (d.nkv < kMaxKvRow) {
        CopyName(d.kv[d.nkv].key, kKeyLen, key, std::strlen(key));
        d.kv[d.nkv].val = val;
        ++d.nkv;
    }
}
// One key=value of a line into `d`. False with `why` when it is wrong (the rest of the line goes on).
inline bool SetKey(Delta& d, const char* key, const char* val, const char** why) {
    *why = nullptr;
    if (!std::strcmp(key, "class")) {
        if (!*val) { *why = "empty class"; return false; }
        CopyName(d.cls, kNameLen, val, std::strlen(val));
        return true;
    }
    if (!std::strcmp(key, "mode")) {   // modes26: which picture mode a stage row is for
        if (!std::strcmp(val, "60")) d.mode = 0;
        else if (!std::strcmp(val, "30")) d.mode = 30;
        else { *why = "mode is 60 or 30"; return false; }
        return true;
    }
    if (!std::strcmp(key, "hz")) {
        if (!*val) { *why = "empty class"; return false; }
        CopyName(d.hz, kNameLen, val, std::strlen(val));
        return true;
    }
    if (!std::strcmp(key, "start") || !std::strcmp(key, "floor") || !std::strcmp(key, "ceil")) {
        int r;
        if (!ParseRung(val, &r)) { *why = "not a rung or a size"; return false; }
        (key[0] == 's' ? d.start : key[0] == 'f' ? d.floorR : d.ceil) = r;
        return true;
    }
    if (!std::strcmp(key, "size")) {
        if (Eq(val, "fixed")) d.size = kSizeFixed;
        else if (Eq(val, "down")) d.size = kSizeDown;
        else if (Eq(val, "auto")) d.size = kSizeAuto;
        else { *why = "size is fixed, down or auto"; return false; }
        return true;
    }
    if (!std::strcmp(key, "fx")) {
        if (Eq(val, "free")) d.fx = kFxFree;
        else if (Eq(val, "down")) d.fx = kFxDown;
        else if (Eq(val, "hold")) d.fx = kFxHold;
        else { *why = "fx is free, down or hold"; return false; }
        return true;
    }
    if (!std::strcmp(key, "edge")) {
        if (Eq(val, "off")) d.edge = 1;
        else if (Eq(val, "on")) d.edge = 0;
        else { *why = "edge is on or off"; return false; }
        return true;
    }
    if (!std::strcmp(key, "cuts")) {
        if (!std::strcmp(val, "1") || Eq(val, "on")) d.cuts = 1;
        else if (!std::strcmp(val, "0") || Eq(val, "off")) d.cuts = 0;
        else { *why = "cuts is 0 or 1"; return false; }
        return true;
    }
    if (!std::strcmp(key, "ladder")) {
        if (Eq(val, "two")) d.two = 1;
        else if (Eq(val, "full")) d.two = 0;
        else { *why = "ladder is two or full"; return false; }
        return true;
    }
    if (!std::strcmp(key, "emergency")) {
        int r;
        if (Eq(val, "none")) { d.emergency = kNone; return true; }
        if (!ParseRung(val, &r)) { *why = "not a rung or a size"; return false; }
        d.emergency = r;
        return true;
    }
    float f;
    if (!ParseNum(val, &f)) { *why = "not a number"; return false; }
    if (d.nkv >= kMaxKvRow) {
        bool have = false;
        for (int i = 0; i < d.nkv; ++i) have = have || !std::strcmp(d.kv[i].key, key);
        if (!have) { *why = "too many keys on one line"; return false; }
    }
    AddKv(d, key, f);
    return true;
}
// Parses `text` (len bytes, need not end with a newline or a NUL) into `t`, adding to what it holds (a later class or row of the same name replaces the
// earlier one). Returns the number of lines accepted; Diag says what was wrong. Never writes outside the table, never reads past len.
inline int ParseText(Table& t, const char* text, size_t len, Diag& dg) {
    int accepted = 0, lineNo = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t e = pos;
        while (e < len && text[e] != '\n') ++e;
        ++lineNo;
        ++dg.lines;
        char line[kLine];
        const size_t n = e - pos;
        bool tooLong = n >= sizeof line;
        if (!tooLong) { std::memcpy(line, text + pos, n); line[n] = 0; }
        pos = e + 1;
        if (tooLong) { dg.Note(lineNo, "line longer than 255 bytes ignored", ""); ++dg.dropped; continue; }
        char* hash = std::strchr(line, '#');
        if (hash) *hash = 0;
        char* p = line;
        while (IsSpace(*p)) ++p;
        if (!*p) continue;
        // first token: the head
        char* tok = p;
        while (*p && !IsSpace(*p)) ++p;
        char head[kNameLen * 2];
        if (static_cast<size_t>(p - tok) >= sizeof head) { dg.Note(lineNo, "name too long", ""); ++dg.dropped; continue; }
        CopyName(head, sizeof head, tok, static_cast<size_t>(p - tok));
        const bool isClass = !std::strcmp(head, "@class");
        Delta d;
        ClearDelta(d);
        if (isClass) {
            while (IsSpace(*p)) ++p;
            char* cn = p;
            while (*p && !IsSpace(*p)) ++p;
            if (p == cn || std::memchr(cn, '=', static_cast<size_t>(p - cn))) { dg.Note(lineNo, "@class needs a name", ""); ++dg.dropped; continue; }
            if (static_cast<size_t>(p - cn) >= static_cast<size_t>(kNameLen)) { dg.Note(lineNo, "class name too long", ""); ++dg.dropped; continue; }
            CopyName(d.name, kNameLen, cn, static_cast<size_t>(p - cn));
        } else {
            if (std::strchr(head, '=')) { dg.Note(lineNo, "a line starts with a stage name, @class or *", head); ++dg.dropped; continue; }
            char* colon = std::strchr(head, ':');
            if (colon) {
                *colon = 0;
                char* end = nullptr;
                const long sc = std::strtol(colon + 1, &end, 10);
                if (end == colon + 1 || *end != 0 || sc < 0 || sc > 99) { dg.Note(lineNo, "bad scenario number", colon + 1); ++dg.dropped; continue; }
                d.scenario = static_cast<int>(sc);
            }
            if (!head[0] || std::strlen(head) >= static_cast<size_t>(kNameLen)) { dg.Note(lineNo, "bad stage name", head); ++dg.dropped; continue; }
            CopyName(d.name, kNameLen, head, std::strlen(head));
        }
        // the key=value tokens
        for (;;) {
            while (IsSpace(*p)) ++p;
            if (!*p) break;
            char* k = p;
            while (*p && !IsSpace(*p)) ++p;
            char saved = *p;
            *p = 0;
            char* eq = std::strchr(k, '=');
            if (!eq || eq == k) {
                dg.Note(lineNo, "not key=value", k);
            } else if (static_cast<size_t>(eq - k) >= static_cast<size_t>(kKeyLen)) {
                dg.Note(lineNo, "key too long", k);
            } else {
                *eq = 0;
                const char* why = nullptr;
                if (!SetKey(d, k, eq + 1, &why)) dg.Note(lineNo, why ? why : "bad value", k);
            }
            if (!saved) break;
            p = p + 1;
        }
        // store (a later line of the same name replaces)
        if (isClass) {
            int at = -1;
            for (int i = 0; i < t.nClasses; ++i)
                if (!std::strcmp(t.classes[i].name, d.name)) at = i;
            if (at < 0 && t.nClasses < kMaxClasses) at = t.nClasses++;
            if (at < 0) { dg.Note(lineNo, "too many classes", d.name); ++dg.dropped; continue; }
            t.classes[at] = d;
            ++dg.classes;
        } else {
            int at = -1;
            for (int i = 0; i < t.nRows; ++i)
                if (!std::strcmp(t.rows[i].name, d.name) && t.rows[i].scenario == d.scenario && t.rows[i].mode == d.mode) at = i;
            if (at < 0 && t.nRows < kMaxRows) at = t.nRows++;
            if (at < 0) { dg.Note(lineNo, "too many stage rows", d.name); ++dg.dropped; continue; }
            t.rows[at] = d;
            ++dg.rows;
        }
        ++accepted;
    }
    return accepted;
}

// ---- resolution (pure) --------------------------------------------------------------------------------------------------------------------
inline void SpecKv(Spec& s, const char* key, float v) {
    for (int i = 0; i < s.nkv; ++i)
        if (!std::strcmp(s.kv[i].key, key)) { s.kv[i].val = v; return; }
    if (s.nkv < kMaxKvSpec) {
        CopyName(s.kv[s.nkv].key, kKeyLen, key, std::strlen(key));
        s.kv[s.nkv].val = v;
        ++s.nkv;
    } else {
        s.flags |= kKvFull;
    }
}
inline void ApplyDelta(Spec& s, const Delta& d) {
    if (d.start != kNone) s.start = d.start;
    if (d.floorR != kNone) s.floorR = d.floorR;
    if (d.ceil != kNone) s.ceil = d.ceil;
    if (d.size != kNone) s.size = d.size;
    if (d.fx != kNone) s.fx = d.fx;
    if (d.edge != kNone) s.edge = d.edge;
    if (d.cuts != kNone) s.cuts = d.cuts;
    if (d.emergency != kNone) s.emergency = d.emergency;
    if (d.two != kNone) s.two = d.two;
    for (int i = 0; i < d.nkv; ++i) SpecKv(s, d.kv[i].key, d.kv[i].val);
}
inline const Delta* FindClass(const Table& t, const char* name) {
    for (int i = t.nClasses - 1; i >= 0; --i)
        if (!std::strcmp(t.classes[i].name, name)) return &t.classes[i];
    return nullptr;
}
// the class and what it inherits (the base first)
inline void ApplyClass(const Table& t, Spec& s, const char* name, int depth) {
    const Delta* c = FindClass(t, name);
    if (!c) { s.flags |= kClassMissing; return; }
    if (c->cls[0] && depth < 8) ApplyClass(t, s, c->cls, depth + 1);
    ApplyDelta(s, *c);
}
inline void InitSpec(Spec& s) {
    std::memset(&s, 0, sizeof s);
    s.start = s.floorR = s.ceil = kTopRung;  // (an empty preset is 960x544, fixed by nothing: the corrections below)
    s.size = kSizeDown;
    s.fx = kFxDown;
    s.edge = s.cuts = 0;
    s.emergency = kNone;
    s.two = 1;
}
// q30aa (GXV_Q30_MSAA, default 0): when enabled in Quality (30 fps), QualityFrame starts at rung 7 (960x544 2x MSAA) with floor rung 6 (960x544 1x) and size=auto so Quality drops MSAA before ever dropping below 30 fps.
inline bool& QualityMsaa() { static bool on = false; return on; }
// The floor constant and the order of the rungs (see Resolve).
inline void Correct(Spec& s) {
    // the floor constant
    const int highest = (s.pictureMode == 30 && QualityMsaa()) ? kMsaa2Rung : kTopRung;
    const int lowest = s.emergency != kNone ? std::max(0, std::min(s.emergency, kTopRung)) : kShipFloorRung;
    auto lift = [&](int& r) { if (r < lowest) { r = lowest; s.flags |= kClampedFloor; } if (r > highest) r = highest; };
    lift(s.start);
    lift(s.floorR);
    lift(s.ceil);
    if (s.size == kSizeFixed) {
        if (s.floorR != s.start || s.ceil != s.start) s.flags |= kOrderFixed;
        s.floorR = s.ceil = s.start;
    } else {
        if (s.ceil < s.floorR) { s.ceil = s.floorR; s.flags |= kOrderFixed; }
        if (s.start < s.floorR) { s.start = s.floorR; s.flags |= kStartFixed; }
        if (s.start > s.ceil) { s.start = s.ceil; s.flags |= kStartFixed; }
    }
    if (s.size == kSizeDown && s.fx < kFxDown) s.fx = kFxDown;
}


// (ResolveRows: the 60 rows) The preset of stage `name`, scenario `scenario` (-1 unknown). `matched`: 2 a row of the stage and scenario, 1 a row of the stage, 0 the default.
// Corrections (flags): nothing goes under kShipFloorRung without an `emergency` rung (then under that one); floor <= start <= ceil; size=fixed makes the three
// equal to start; size=down needs fx >= down (a size that never comes back up with effects that do would hunt on the effects).
// `forceClass` (GXV_STAGE_PRESETS_CLASS, for tours): every stage gets that class and nothing else: its row and the default row are not read.
inline void ResolveRows(const Table& t, const char* name, int scenario, Spec& s, const char* forceClass, int zoomTenths) {
    InitSpec(s);
    const Delta* row = nullptr;
    int matched = 0;
    for (int i = 0; i < t.nRows && !(forceClass && *forceClass); ++i) {
        const Delta& r = t.rows[i];
        if (r.mode != 0) continue;   // (modes26: a `mode=30` row is not a 60 row)
        if (std::strcmp(r.name, name ? name : "")) continue;
        if (r.scenario == scenario && scenario >= 0) { row = &r; matched = 2; break; }
        if (r.scenario < 0 && matched < 1) { row = &r; matched = 1; }
    }
    const Delta* dflt = nullptr;
    for (int i = t.nRows - 1; i >= 0 && !(forceClass && *forceClass); --i)
        if (t.rows[i].mode == 0 && !std::strcmp(t.rows[i].name, "*")) { dflt = &t.rows[i]; break; }
    const char* cls = forceClass && *forceClass ? forceClass : (row && row->cls[0] ? row->cls : (dflt && dflt->cls[0] ? dflt->cls : "normal"));
    // zoom (the owner's rule: the tests run at zoom 1.0, normal play is 1.5 where the load is lower): at zoom >= 1.4 the row's hz=<class> is used, else the
    // chosen class's own hz= (its next lighter class). A forced class (tours) is never lightened: a tour at 1.5 measures that class.
    if (zoomTenths >= 14 && !(forceClass && *forceClass)) {
        const char* hz = row && row->hz[0] ? row->hz : nullptr;
        for (const char* c = cls; !hz && c && *c;) {
            const Delta* cd = FindClass(t, c);
            if (!cd) break;
            if (cd->hz[0]) hz = cd->hz;
            else c = cd->cls[0] ? cd->cls : nullptr;
        }
        if (hz && std::strcmp(hz, "none") != 0 && FindClass(t, hz)) { cls = hz; s.zoomed = 1; }
    }
    ApplyClass(t, s, cls, 0);
    if (!row && dflt) ApplyDelta(s, *dflt);
    if (row) ApplyDelta(s, *row);
    CopyName(s.cls, kNameLen, cls, std::strlen(cls));
    s.matched = matched;
    Correct(s);
}

// ---- modes26: the two picture modes -------------------------------------------------------------------------------------------------------
// Performance (60): the stage's row as it always was (rows tagged `mode=30` are not read). Quality (30): the stage's 60 row is read, then every KEY of it that is a sacrifice
// (stage_knob.h Desc::ex) and its frame (rungs, edge pass, half-rate captures, effect steps) are left out and named in Spec::skip; the exact keys stay. The frame is 960x544 fixed with
// the edge pass on and the effect steps held. Then the stage's `mode=30` row (else the `*` one, `mode=30`) is applied as written: the (mostly empty) full-quality row set.
constexpr int kPerformance = 60, kQuality = 30;
inline void AddSkip(Spec& s, const char* what) {
    if (s.nskip < kMaxKvSpec + 1) { CopyName(s.skip[s.nskip], kKeyLen, what, std::strlen(what)); ++s.nskip; }
}
inline void QualityFrame(Spec& s) {
    if (QualityMsaa()) {
        s.start = kMsaa2Rung;
        s.floorR = kTopRung;
        s.ceil = kMsaa2Rung;
        s.size = kSizeAuto;
    } else {
        s.start = s.floorR = s.ceil = kTopRung;
        s.size = kSizeFixed;
    }
    s.fx = kFxHold;
    s.edge = 0;
    s.cuts = 0;
    s.emergency = kNone;
    s.two = 1;
}
inline void AddClamp(Spec& s, const char* key, float asked, float kept) {
    if (s.nclamp < kMaxKvSpec + 1) { std::snprintf(s.clamp[s.nclamp], sizeof s.clamp[0], "%s=%g>%g", key, static_cast<double>(asked), static_cast<double>(kept)); ++s.nclamp; }
}
// q30-26: does a key of the spec (by switch name) have a value of at least `atLeast`?
inline bool HasSwitchAtLeast(const Spec& s, const char* name, float atLeast) {
    for (int i = 0; i < s.nkv; ++i) {
        const port::stageknob::Desc* d = port::stageknob::FindDesc(s.kv[i].key);
        if (d && !std::strcmp(d->name, name) && s.kv[i].val >= atLeast) return true;
    }
    return false;
}
// The 30 mode's filter over the 60 row's keys (q30-26: three classes). exact ('x', 'p' up to exactMax): kept as it is. barely noticeable ('b'): kept, the value clamped into [bMin, bMax]
// (named in Spec::clamp when it changed). sacrifice ('s', and a 'p' value above exactMax): left out and named in Spec::skip.
inline void FilterForQuality(Spec& s) {
    int keep = 0;
    for (int i = 0; i < s.nkv; ++i) {
        const port::stageknob::Desc* d = port::stageknob::FindDesc(s.kv[i].key);
        if (d) {   // (an unknown key stays: ValidateKv refuses it with its usual message)
            float v = s.kv[i].val;
            const int r = port::stageknob::QualityValue(*d, s.kv[i].val, &v);
            if (r == 0) { AddSkip(s, s.kv[i].key); continue; }
            if (r == 2) { AddClamp(s, s.kv[i].key, s.kv[i].val, v); s.kv[i].val = v; }
        }
        if (keep != i) s.kv[keep] = s.kv[i];
        ++keep;
    }
    s.nkv = keep;
    // the silhouette gate on the game's collision rays alone can miss Mario behind foliage (ledger: "visible risk"): in Quality it is kept only with the occlusion query's confirmation (silh_occ >= 1,
    // or the across umbrella, which brings both)
    for (int i = 0; i < s.nkv; ++i) {
        const port::stageknob::Desc* d = port::stageknob::FindDesc(s.kv[i].key);
        if (!d || std::strcmp(d->name, "GXV_SILH_GATE") != 0 || s.kv[i].val != 1.0f) continue;
        if (HasSwitchAtLeast(s, "GXV_SILH_OCC", 1.0f) || HasSwitchAtLeast(s, "GXV_ACROSS", 1.0f)) break;
        AddSkip(s, "silh_gate(no silh_occ)");
        for (int k = i; k + 1 < s.nkv; ++k) s.kv[k] = s.kv[k + 1];
        --s.nkv;
        break;
    }
    Spec q = s;
    QualityFrame(q);
    if (q.start != s.start || q.floorR != s.floorR || q.ceil != s.ceil || q.size != s.size || q.fx != s.fx || q.edge != s.edge || q.cuts != s.cuts || q.emergency != s.emergency) AddSkip(s, "frame");
    QualityFrame(s);
}
// The 30 rows on top: the stage's `mode=30` row (a scenario row first), else the `*` one with `mode=30`. A row's class= is applied first. Everything such a row says is applied.
inline void ApplyQualityRows(const Table& t, const char* name, int scenario, Spec& s) {
    const Delta* row = nullptr;
    int matched = 0;
    for (int i = 0; i < t.nRows; ++i) {
        const Delta& r = t.rows[i];
        if (r.mode != 30 || std::strcmp(r.name, name ? name : "")) continue;
        if (r.scenario == scenario && scenario >= 0) { row = &r; matched = 2; break; }
        if (r.scenario < 0 && matched < 1) { row = &r; matched = 1; }
    }
    const Delta* dflt = nullptr;
    for (int i = t.nRows - 1; i >= 0; --i)
        if (t.rows[i].mode == 30 && !std::strcmp(t.rows[i].name, "*")) { dflt = &t.rows[i]; break; }
    const char* cls = row && row->cls[0] ? row->cls : (!row && dflt && dflt->cls[0] ? dflt->cls : nullptr);
    if (cls) ApplyClass(t, s, cls, 0);
    if (!row && dflt) ApplyDelta(s, *dflt);
    if (row) ApplyDelta(s, *row);
    if (row || dflt) s.matched = row ? matched : 0;
}
// ---- gpu896 (21 Nov, the owner's policy: "any time the gpu is the issue, dropping graphics to 896x512 in performance is fine and then dropping to 30fps") --------------------
// Performance (60) only (Resolve calls it only there; Quality's frame is QualityFrame, 960x544 fixed, whatever the 60 row says). A stage whose 60 spec is the plain "960x544, fixed, effect
// steps one way" frame (every stage on the built-in base class) becomes: start 960x544, floor 848x480 (rung 4), the controller's Performance ladder (cheaper picture, 896x512, 848x480, edge smoothing off; down
// for a GPU-limited stage only, and back up with room; the next load starts at 960x544), and hud_native=1 so the 2D layer is drawn at the screen's own size once the 3D is
// at 896x512 (at 960x544 the back end's own test, "panel-size rung", leaves it off). Left as they are: a row that says anything else (emergency / lastresort 848, size=down or auto already,
// another start, fx=hold, a floor below the top). GPU-limited gating is the controller's (GXV_AUTOQ_GPUGATE and the GPU-bound tests of its size step-down), not this table's.
// Switch: PerfGpu896() (set by the back end from GXV_PERF_GPU896; false = every spec exactly as before).
inline bool& PerfGpu896() { static bool on = false; return on; }
constexpr int kMidRung = 5;   // 896x512 on the wide ladder
inline void ApplyPerfGpu896(Spec& s) {
    if (s.start != kTopRung || s.floorR != kTopRung || s.ceil != kTopRung || s.size != kSizeFixed || s.fx != kFxDown || s.emergency != kNone) return;
    s.floorR = kShipFloorRung;   // 848x480: the owner's floor (nothing under it without emergency=)
    s.size = kSizeAuto;          // the controller's Performance ladder moves in both ways (Controller::LadderStep, StagePreset::ladder), GPU-limited only
    s.fx = kFxFree;
    s.two = 0;                   // full ladder: 960 -> 896 -> 848
    s.perf896 = 1;
    bool have = false;
    for (int i = 0; i < s.nkv; ++i) have = have || !std::strcmp(s.kv[i].key, "hud_native");
    if (!have) SpecKv(s, "hud_native", 1.0f);
}
// The preset of stage `name`, scenario `scenario` (-1 unknown) for picture mode `pictureMode` (60, the default, or 30). See ResolveRows for the rest.
inline void Resolve(const Table& t, const char* name, int scenario, Spec& s, const char* forceClass = nullptr, int zoomTenths = 10, int pictureMode = kPerformance) {
    ResolveRows(t, name, scenario, s, forceClass, zoomTenths);
    s.pictureMode = pictureMode == kQuality ? kQuality : kPerformance;
    if (s.pictureMode != kQuality) { if (PerfGpu896() && !(forceClass && *forceClass)) ApplyPerfGpu896(s); return; }
    FilterForQuality(s);
    ApplyQualityRows(t, name, scenario, s);
    QualityFrame(s);   // (a 30 row may add switches, not a smaller picture, the edge pass off or the effect steps back)
    if (std::strcmp(s.cls, "none") == 0) CopyName(s.cls, kNameLen, "mode30", 6);
    Correct(s);
}

// ---- validation of the settable keys (pure) ------------------------------------------------------------------------------------------------
// The keys of a spec that a preset may really set: unknown / non-settable / out-of-range ones are left out and counted. out[] gets {switch name, value}.
struct Applied { const char* name; float val; };
inline int ValidateKv(const Spec& s, Applied* out, int cap, char* bad, size_t badCap) {
    int n = 0, nbad = 0;
    if (bad && badCap) bad[0] = 0;
    for (int i = 0; i < s.nkv; ++i) {
        const port::stageknob::Desc* d = port::stageknob::FindDesc(s.kv[i].key);
        const char* why = nullptr;
        if (!d) why = "not stage-settable";
        else if (s.kv[i].val < d->lo || s.kv[i].val > d->hi) why = "out of range";
        if (why) {
            ++nbad;
            if (bad && badCap) {
                const size_t used = std::strlen(bad);
                if (used + 4 < badCap) std::snprintf(bad + used, badCap - used, "%s%s (%s)", used ? ", " : "", s.kv[i].key, why);
            }
            continue;
        }
        static const bool allowOff = std::getenv("GXV_STAGE_PRESETS_ALLOW_OFF") && std::getenv("GXV_STAGE_PRESETS_ALLOW_OFF")[0] == '1';
        bool clamped = false;
        const float v = port::stageknob::ClampOff(*d, s.kv[i].val, allowOff, &clamped);
        if (clamped && bad && badCap) {
            const size_t used = std::strlen(bad);
            if (used + 4 < badCap) std::snprintf(bad + used, badCap - used, "%s%s (off clamped to the weakest reduced form)", used ? ", " : "", s.kv[i].key);
        }
        if (n < cap) { out[n].name = d->name; out[n].val = v; ++n; }
    }
    // cpulod26: `cpulod=N` stands for the members of the umbrella (port/cpulod.h) that the line does not name itself
    for (int i = 0; i < s.nkv; ++i) {
        const port::stageknob::Desc* d = port::stageknob::FindDesc(s.kv[i].key);
        if (!d || std::strcmp(d->name, "GXV_CPULOD") != 0 || s.kv[i].val < d->lo || s.kv[i].val > d->hi) continue;
        port::cpulod::Expand(static_cast<int>(s.kv[i].val),
            [&](const char* member) {
                for (int k = 0; k < s.nkv; ++k) {
                    const port::stageknob::Desc* e = port::stageknob::FindDesc(s.kv[k].key);
                    if (e && std::strcmp(e->name, member) == 0) return true;
                }
                return false;
            },
            [&](const char* member, float val) {
                if (n < cap && port::stageknob::FindDesc(member)) { out[n].name = member; out[n].val = val; ++n; }
            }, s.pictureMode == 30);   // (q30-26: the 30 mode's q members only)
    }
    // across26: `across=N` stands for the members of the umbrella (port/across.h) that the line does not name itself (only the stage-settable ones: SILH_GATE, SILH_OCC, INDCAP_SKIP)
    for (int i = 0; i < s.nkv; ++i) {
        const port::stageknob::Desc* d = port::stageknob::FindDesc(s.kv[i].key);
        if (!d || std::strcmp(d->name, "GXV_ACROSS") != 0 || s.kv[i].val < d->lo || s.kv[i].val > d->hi) continue;
        port::across::Expand(static_cast<int>(s.kv[i].val),
            [&](const char* member) {
                for (int k = 0; k < s.nkv; ++k) {
                    const port::stageknob::Desc* e = port::stageknob::FindDesc(s.kv[k].key);
                    if (e && std::strcmp(e->name, member) == 0) return true;
                }
                return false;
            },
            [&](const char* member, float val) {
                if (n < cap && port::stageknob::FindDesc(member)) { out[n].name = member; out[n].val = val; ++n; }
            }, s.pictureMode == 30);   // (q30-26: the 30 mode's q members only)
    }
    return n;
}

// ---- the compiled-in table ----------------------------------------------------------------------------------------------------------------
// presets26 (12 Nov): rebuilt from console measurements; the owner's picture is 960x544 FIXED. No class below starts, floors or ceilings under rung 6 except
// `lastresort` (848x480), which NO row uses: a stage that is still in the 40s after its row is named by the owner and given that class in the file. Every key
// here is one a console run measured or verified (tests/vgx_stagepreset_test.cpp keeps the allow-list); keys no console run has priced (light volumes, shadow clip, compositor, far-speck cuts, the far-draw governor, fold level 2) are in agents/presets26/stage-presets-1112.txt, for the file.
//   base        every stage: 960 fixed, effect steps one way, clip chain skipped when empty (exact)
//   gpu-cap     base + the unread Indirect capture left out (a scene removed: Dark Matter Plant, Ghostly, Dreadnought, Dusty Dune); never at a stale-read stage
//   gpu-light   base + half-rate captures and clip chain (the U set)
//   gpu-glow    gpu-light + bloom fold 1 (the two Bowser reactors)      gpu-warp   gpu-light + ind_flat=1 (Freezeflame; never 2)
//   gpu-heavy / cpu / cpu-gov: today the same as gpu-cap / base / gpu-light; the file adds their unmeasured keys. dl_static / the hot-spot switches / shadow
//   stencil / skip-maxstep / stable shadow are not stage-settable: the global list in agents/presets26/REPORT.txt. hsr_order is in no class (ties 4 measured slower).
//   lastresort  gpu-light at 848 fixed (needs the upscaler, global), edge pass off, effects held
// No hz= anywhere: the same class at zoom 1.0 and 1.5 (a lighter class at 1.5 is unmeasured). A row's own keys win over its class.
inline constexpr char kBuiltinText[] =
    "# no preset at all: the global switches and today's controller (a row may say class=none)\n"
    "@class none\n"
    "# EVERY stage: 960x544 fixed; effect steps one way only (fx=down); clip chain skipped when empty (exact, console). The full file adds colshadow_clip=1\n"
    "# (14 Nov, owner: the hub's far-cut step across the game, 'if it's not too noticeable'): cpulod=3 for the UPDATE RATES only (far animation / material lists / poses\n"
    "# less often, from 9-18 m out); what is DRAWN stays at today's level 2 (model detail pb_lod, plants pb_decor, emitters pb_ptcl, fur, shadow reach, particle and\n"
    "# star-bit thinning): those keys are sacrifices, so the Quality (30) mode leaves them all out and keeps its own q level\n"
    "@class base start=6 floor=6 ceil=6 size=fixed fx=down clip_skipempty=1 cpulod=3 pb_lod=0.45 pb_decor=4000 pb_ptcl=0.4 pb_fur=2000 pb_shadow=1800 ptcl_thin=0.35 ptcl_thin_d0=2000 ptcl_thin_d1=5000 piecethin_dist=4500\n"
    "# base + the unread Indirect capture left out: only where a scene was removed on the console (NOT at the stale-read stages)\n"
    "@class gpu-cap class=base indcap_skip=1\n"
    "# base + half-rate captures / clip chain (U set)\n"
    "@class gpu-light class=base indcap_half=2 indcap_camgate=1 clip_half=2 cuts=1\n"
    "# gpu-light (bloom_fold=1 removed in w25f so Final Bowser / Star Reactor inherit mode_keys.h's GXV_BLOOM_FOLD=2)\n"
    "@class gpu-glow class=gpu-light\n"
    "# gpu-light + simple blended indirect warps read directly (ind_flat=1; never 2)\n"
    "@class gpu-warp class=gpu-light ind_flat=1\n"
    "# = gpu-cap in this file (the full file adds lightvol=3: exact, console-unmeasured; hsr_order is in no class: ties 4 measured slower)\n"
    "@class gpu-heavy class=gpu-cap\n"
    "# = base in this file: par_dl, dl_static and the hot-spot switches are GLOBAL (entry.cpp); the far-speck cuts (<= 1 fps) are in the full file only\n"
    "@class cpu class=base\n"
    "# = gpu-light in this file (the full file adds the far cuts and cpu_gov=5, host-proven only)\n"
    "@class cpu-gov class=gpu-light\n"
    "# 848x480 fixed + upscaler (global, entry.cpp): NO stage uses it; the owner names a stage that is still in the 40s\n"
    "@class lastresort class=gpu-light start=4 floor=4 ceil=4 size=fixed fx=hold edge=off\n"
    "\n"
    "# default row: every stage with no row of its own (and the title / menus) gets the base class only\n"
    "* class=base\n"
    "# Comet Observatory and its domes (14 Nov, console c157 2990a/b): cpulod=3 (the strongest look-alike far cuts) = hitch lines 82 -> 47, pad to flip ~98 -> ~88 ms, pair 29.9 -> 29.1 ms\n"
    "# (the hub keeps the WHOLE level 3 as measured: its own keys put back level 3's drawn cuts over the base class's level-2 values)\n"
    "AstroGalaxy class=base cpulod=3 pb_lod=0.3 pb_decor=3000 pb_ptcl=0.3 pb_fur=1500 pb_shadow=1500 ptcl_thin=0.2 ptcl_thin_d0=1500 ptcl_thin_d1=4000 piecethin_dist=3500\n"
    "AstroDome class=base cpulod=3 pb_lod=0.3 pb_decor=3000 pb_ptcl=0.3 pb_fur=1500 pb_shadow=1500 ptcl_thin=0.2 ptcl_thin_d0=1500 ptcl_thin_d1=4000 piecethin_dist=3500\n"
    "# Sling Pod: game 22.3 / FE 17.8 / GPU 15.7; 31.4 fps c54. CPU class (globals do the work)\n"
    "CocoonExGalaxy class=cpu\n"
    "# Bubble Breeze: game 22.2 / FE 19.4 / GPU 18.8; 33.0 fps c54\n"
    "CubeBubbleExLv1Galaxy class=cpu\n"
    "# Gusty Garden: K 42.6, E 43.5-45.5; stale-read stage: no indcap_skip\n"
    "CosmosGardenGalaxy class=cpu\n"
    "# Dusty Dune: E 46.5 (scenes 5 -> 4, busy -1.3 ms): indcap_skip on\n"
    "SandClockGalaxy class=cpu indcap_skip=1\n"
    "# Snow Cap: game 17.3 (update 10.1), GPU 14.2; c54 only (class by estimate)\n"
    "SnowCapsuleGalaxy class=cpu\n"
    "# Honeyhive 1: K 34.4-35.2, E 35.5-37.8, EU 37.6-40.8 (U set + global stencil) | full file adds: far cuts + cpu_gov=5 (host-proven only)\n"
    "HoneyBeeKingdomGalaxy:1 class=cpu-gov\n"
    "# Honeyhive 2: 55.9 fps c54, GPU busy 16.9: class by estimate\n"
    "HoneyBeeKingdomGalaxy:2 class=gpu-light\n"
    "# Beach Bowl: K 36.8-37.6, E 38.4, EU 40.5 (global stencil); stale-read stage: no indcap_skip | w25f: Observatory level-3 draw cuts (Performance-only; stripped in Quality)\n"
    "HeavenlyBeachGalaxy class=gpu-light pb_lod=0.3 pb_decor=3000 pb_ptcl=0.3 pb_fur=1500 pb_shadow=1500 ptcl_thin=0.2 piecethin_dist=3500\n"
    "# Star Reactor: K 39.4, EU 42.2-42.4; stale-read stage: no indcap_skip | inherits mode_keys.h GXV_BLOOM_FOLD=2\n"
    "KoopaBattleVs1Galaxy class=gpu-glow\n"
    "# Galaxy Reactor (Final Bowser): inherits mode_keys.h GXV_BLOOM_FOLD=2 | w25f: Observatory level-3 draw cuts (Performance-only; stripped in Quality)\n"
    "KoopaBattleVs3Galaxy class=gpu-glow pb_lod=0.3 pb_decor=3000 pb_ptcl=0.3 pb_fur=1500 pb_shadow=1500 ptcl_thin=0.2 piecethin_dist=3500\n"
    "# Freezeflame: K 37.8; ind_flat=1 -3.3 ms (23.7 ms, 42.2 fps); +clearq+silh_gate+stencil = same 23.7); stale-read stage: no indcap_skip\n"
    "IceVolcanoGalaxy class=gpu-warp\n"
    "# Dreadnought: K 33.6, E 35.4 (ties 0 = 42.3 inexact, ties 4 = 33.4 vs 30.7 control SLOWER: no hsr key) | full file adds: lightvol=3, colshadow_clip=1 (from base)\n"
    "# gov77: GPU-limited (plain 47.8 fps, GPU 19.3 ms, game 10.3 ms): edge smoothing starts off, ~1 ms, seam-free at the load | w25f: Observatory level-3 draw cuts (Performance-only)\n"
    "CannonFleetGalaxy class=gpu-heavy edge=off pb_lod=0.3 pb_decor=3000 pb_ptcl=0.3 pb_fur=1500 pb_shadow=1500 ptcl_thin=0.2 piecethin_dist=3500\n"
    "# Dark Matter Plant: K 35.5 -> E 48.3 (clip chain + capture skip)\n"
    "KoopaBattleVs2Galaxy class=gpu-cap\n"
    "# Ghostly: K 49.7 -> E 54.0-54.2; U set regressed (EU 51.0): no gpu-light\n"
    "PhantomGalaxy class=gpu-cap\n"
    "# Space Junk: K 48.8, E 49.8-50.0, EU 52.5 (stencil is global)\n"
    "StarDustGalaxy class=gpu-light\n"
    "# Sea Slide: K 45-46, E 46.1-46.3, EU 48.5 (stencil is global)\n"
    "OceanRingGalaxy class=gpu-light\n"
    "# Gold Leaf: K 49.9-52.0, EU 52.5-53.6; game thread pinned 16.6\n"
    "ReverseKingdomGalaxy class=gpu-light\n"
    "# Matter Splatter: 8 EFB scenes (17 in all), c54 35.1 | full file adds: gpu-cap (capture skip, by analogy with Dark Matter Plant)\n"
    "DarkRoomGalaxy class=base\n"
    "# Sand Spiral: 6 EFB scenes, c54 37.7 | full file adds: gpu-cap (capture skip, by analogy)\n"
    "TransformationExGalaxy class=base\n"
    "# Flipswitch: 7 EFB scenes (14 in all), TA 18.3 of 21.9, c54 45.8 | full file adds: gpu-cap (capture skip, by analogy)\n"
    "FlipPanelExGalaxy class=base\n"
    "# Deep Dark: K 46.3, E 45.9, EU 45.8; 848 + detail 3 was WORSE (43.5): nothing added\n"
    "OceanPhantomCaveGalaxy class=base\n"
    "# Toy Time: K 43.5, E 40.5 (cadence), later control 48.3; pacing: global SKIP_MAXSTEP\n"
    "FactoryGalaxy class=base\n"
    "# Melty Molten: w25f upgraded base -> gpu-light (indcap_half=2 indcap_camgate=1 clip_half=2 cuts=1) + Observatory level-3 draw cuts (Performance-only; stripped in Quality); stale-read stage: no indcap_skip\n"
    "HellProminenceGalaxy class=gpu-light pb_lod=0.3 pb_decor=3000 pb_ptcl=0.3 pb_fur=1500 pb_shadow=1500 ptcl_thin=0.2 piecethin_dist=3500\n"
    "# Good Egg: K 54.6, E 55.2, E2 55.7, EU 54.5\n"
    "EggStarGalaxy class=base\n"
    "# K 41.0, EU 41.2, 848 41.2, preset 41.4: no lever known\n"
    "SkullSharkGalaxy class=base\n"
    "# Buoy Base: c54 47.0 (GPU 3D 21.0); no switch measured\n"
    "OceanFloaterLandGalaxy class=base\n"
    "# Gateway: c54 48.5, pixel bound; the size would help most (last-resort candidate)\n"
    "HeavensDoorGalaxy class=base\n"
    "# Sweet Sweet: c54 50.0 (busy 18.1); no switch measured\n"
    "BeltConveyerExGalaxy class=base\n"
    "# Drip Drop: c54 53.7; no switch measured\n"
    "TearDropGalaxy class=base\n"
    "# Battlerock: 59.3-59.6\n"
    "BattleShipGalaxy class=base\n"
    "# Bigmouth: c54 59.8\n"
    "FishTunnelGalaxy class=base\n"
    "# Airship Armada: c54 59.8, K 55.5 (window shift)\n"
    "KoopaJrShipLv1Galaxy class=base\n"
    "# Lava Reactor: 59.5-59.9\n"
    "FloaterOtaKingGalaxy class=base\n"
    "# Honeyclimb: c54 59.9\n"
    "HoneyBeeExGalaxy class=base\n"
    "# Loopdeeloop: c54 59.9\n"
    "SurfingLv1Galaxy class=base\n"
    "# Rolling Green: c54 59.9\n"
    "TamakoroExLv1Galaxy class=base\n"
    "# Boo's Boneyard: c54 59.9\n"
    "TeresaMario2DGalaxy class=base\n"
    "# Hurry-Scurry: c54 60.0\n"
    "BreakDownPlanetGalaxy class=base\n"
    "# Robot Reactor: 59.6-59.9\n"
    "TriLegLv1Galaxy class=base\n";

inline void LoadBuiltin(Table& t, Diag& dg) { ParseText(t, kBuiltinText, sizeof kBuiltinText - 1, dg); }

// One line for the log: "stage-preset: <stage> class <c>: start <rung> floor <rung> ceil <rung> ..." (the back end adds what it applied).
inline int Describe(const Spec& s, const char* stage, int scenario, char* out, size_t cap) {
    char kvs[360] = "";
    for (int i = 0; i < s.nkv && std::strlen(kvs) + 40 < sizeof kvs; ++i) {
        const size_t used = std::strlen(kvs);
        std::snprintf(kvs + used, sizeof kvs - used, "%s%s=%g", used ? " " : "", s.kv[i].key, static_cast<double>(s.kv[i].val));
    }
    char sc[12] = "";
    if (scenario >= 0) std::snprintf(sc, sizeof sc, ":%d", scenario);
    return std::snprintf(out, cap, "stage-preset: %s%s class %s%s: start %d floor %d ceil %d size %s fx %s edge %s cuts %d ladder %s%s%s%s [row: %s]",
                         stage ? stage : "?", sc, s.cls, s.zoomed ? " (zoom >= 1.4: the lighter class)" : "", s.start, s.floorR, s.ceil, s.size == kSizeFixed ? "fixed" : (s.size == kSizeDown ? "down" : "auto"),
                         s.fx == kFxHold ? "hold" : (s.fx == kFxDown ? "down" : "free"), s.edge ? "off" : "on", s.cuts, s.two ? "two" : "full",
                         s.emergency != kNone ? " EMERGENCY-FLOOR" : "", kvs[0] ? " keys " : "", kvs,
                         s.matched == 2 ? "stage and scenario" : (s.matched == 1 ? "stage" : "default"));
}

// ---- policy26: the smooth-60 modes of a stage (GXV_SMOOTH60, port/smooth60.h) ---------------------------------------------------------------
// A stage's row is its 60 spec. Its two fallbacks are rows of their own when the table has them, named `<Stage>@848` and `<Stage>@30` (the stage name is any text up to
// the colon, so these never match a game stage), else derived: @848 = the 60 spec at rung 4 (848x480, fixed, edge pass off, the same switches); @30 = the class
// `s60-q30` alone (960x544, the quality put back: only the exact switches) when the file has it, else the 60 spec at rung 6 with its keys.
inline constexpr int kModes = 3;  // port::smooth60::k60 / k848 / k30 (gpugen26: kModesMid = 4 adds k896 = 3)
inline constexpr int kModesMid = 4;
inline float SpecGet(const Spec& s, const char* key, float dflt) {
    for (int i = 0; i < s.nkv; ++i)
        if (!std::strcmp(s.kv[i].key, key)) return s.kv[i].val;
    return dflt;
}
// deadfix27: the S60 ladder's keys (s60_start, s60_848, s60_30, s60_896, s60_hud) act only while GXV_SMOOTH60 is on; with it off they were applied and logged as "preset keys
// applied" and did nothing (console c86, mode-P run: "GXV_S60_848=1 GXV_S60_30=1" and no 848 step). They are taken out of the spec and named under "skipped for mode". Returns how many.
inline int DropInertS60(Spec& s) {
    int dropped = 0, w = 0;
    for (int i = 0; i < s.nkv; ++i) {
        if (!std::strncmp(s.kv[i].key, "s60_", 4)) { ++dropped; continue; }
        if (w != i) s.kv[w] = s.kv[i];
        ++w;
    }
    s.nkv = w;
    if (dropped && s.nskip < kMaxKvSpec + 1) { std::snprintf(s.skip[s.nskip], kKeyLen, "s60_*(GXV_SMOOTH60=0)"); ++s.nskip; }
    return dropped;
}
inline bool HasRow(const Table& t, const char* name, int scenario) {
    for (int i = 0; i < t.nRows; ++i)
        if (!std::strcmp(t.rows[i].name, name) && (t.rows[i].scenario == scenario || t.rows[i].scenario < 0)) return true;
    return false;
}
// gpugen26: mode 3 = port::smooth60::k896 (896x512, rung 5: the row `<Stage>@896`, else the 60 spec at rung 5 with the edge pass off). hudNative: the stage's 848 / 896 spec
// also gets `hud_native=1` unless the row names the key itself (GXV_SMOOTH60_HUD: the 2D layer is drawn at 960x544 over the upscaled picture, so the smaller 3D does not
// take the HUD's text and icons with it; the 960x544 modes are untouched).
inline void ResolveMode(const Table& t, const char* name, int scenario, int mode, Spec& s, int zoomTenths = 10, bool hudNative = false) {
    if (mode <= 0) { Resolve(t, name, scenario, s, nullptr, zoomTenths); return; }
    const int lowRung = mode == 3 ? 5 : 4;  // (rung 4 = 848x480, 5 = 896x512 on the wide ladder the Vita runs)
    auto hud = [&]() { if (hudNative && SpecGet(s, "hud_native", -1.0f) < 0.0f) SpecKv(s, "hud_native", 1.0f); };
    char alt[kNameLen + 8];
    std::snprintf(alt, sizeof alt, "%.*s@%s", kNameLen - 1, name ? name : "", mode == 1 ? "848" : mode == 3 ? "896" : "30");
    if (HasRow(t, alt, scenario)) {
        Resolve(t, alt, scenario, s, nullptr, zoomTenths);
        if (mode == 1 || mode == 3) { s.start = s.floorR = s.ceil = lowRung; s.size = kSizeFixed; hud(); }  // (an @848 / @896 row is that size whatever its text says)
        else { s.start = s.floorR = s.ceil = kTopRung; s.size = kSizeFixed; }                              // (an @30 row is 960x544)
        return;
    }
    if (mode == 1 || mode == 3) {
        Resolve(t, name, scenario, s, nullptr, zoomTenths);
        s.start = s.floorR = s.ceil = lowRung;
        s.size = kSizeFixed;
        s.edge = 1;
        hud();
        return;
    }
    if (FindClass(t, "s60-q30")) {
        Resolve(t, name, scenario, s, "s60-q30", zoomTenths);
    } else {
        Resolve(t, name, scenario, s, nullptr, zoomTenths);
    }
    s.start = s.floorR = s.ceil = kTopRung;
    s.size = kSizeFixed;
    s.edge = 0;
}

// modes26 / q30-26: the one line a stage load logs: "mode: 60|30, stage <name>, preset keys applied: <switch>=<value> ..., clamped for Quality: <alias>=<asked>><kept> ..., skipped for mode: <key> ...".
// Applied are the keys ValidateKv lets through (what ApplySpec sets); skipped are the 60 row's cuts the 30 mode left out ("frame" = its rungs, edge pass, captures, effect steps).
// `presetsOn` false (GXV_STAGE_PRESETS=0): nothing is applied, whatever the table says. Returns the length (snprintf's).
inline int FormatModeLine(const Spec& s, const char* stage, int scenario, bool presetsOn, char* out, size_t cap) {
    Applied ap[kMaxKvSpec + 24];
    char bad[160];
    const int n = presetsOn && std::strcmp(s.cls, "none") != 0 ? ValidateKv(s, ap, kMaxKvSpec + 24, bad, sizeof bad) : 0;
    char applied[700] = "", skipped[300] = "", clamped[200] = "";
    for (int i = 0; i < s.nclamp; ++i) {   // (q30-26)
        const size_t used = std::strlen(clamped);
        if (used + 52 >= sizeof clamped) { std::snprintf(clamped + used, sizeof clamped - used, "%s...", used ? " " : ""); break; }
        std::snprintf(clamped + used, sizeof clamped - used, "%s%s", used ? " " : "", s.clamp[i]);
    }
    for (int i = 0; i < n; ++i) {
        const size_t used = std::strlen(applied);
        if (used + 48 >= sizeof applied) { std::snprintf(applied + used, sizeof applied - used, "%s...", used ? " " : ""); break; }
        std::snprintf(applied + used, sizeof applied - used, "%s%s=%g", used ? " " : "", ap[i].name, static_cast<double>(ap[i].val));
    }
    for (int i = 0; i < s.nskip; ++i) {
        const size_t used = std::strlen(skipped);
        if (used + 40 >= sizeof skipped) { std::snprintf(skipped + used, sizeof skipped - used, "%s...", used ? " " : ""); break; }
        std::snprintf(skipped + used, sizeof skipped - used, "%s%s", used ? " " : "", s.skip[i]);
    }
    char sc[12] = "";
    if (scenario >= 0) std::snprintf(sc, sizeof sc, ":%d", scenario);
    return std::snprintf(out, cap, "mode: %d, stage %s%s, preset keys applied: %s, clamped for Quality: %s, skipped for mode: %s", s.pictureMode == kQuality ? 30 : 60, stage ? stage : "?", sc,
                         applied[0] ? applied : (presetsOn ? "none" : "none (GXV_STAGE_PRESETS=0)"), clamped[0] ? clamped : "none", skipped[0] ? skipped : "none");
}

}  // namespace vgx::stagepreset
