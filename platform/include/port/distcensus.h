// far30 (13 Nov 2026) GXV_DISTCENSUS=1 (default 0): the distance census, the pure parts (no game types; host-tested by tests/distcensus.cpp).
//
// What the game does, by DISTANCE FROM THE CAMERA and by kind of thing, counts only (no clock anywhere: valid in the emulator).
//   bands (metres, 100 game units = 1 m): 0-10, 10-25, 25-50, 50-100, 100-200, 200+
//   classes: map (collision actors: planets, platforms, terrain pieces; plain map objects), chars (Mario, enemies, NPCs, animals, rides), plants (flower / bush
//            groups, decoration, decoration with effects), items (coins, star bits, 1-ups, switches, morph items), particles, shadows, other
//   per band and class, per game turn (logic, posed, particles, shadows) or per DRAWN picture (draws, indices):
//     logic      LiveActor::movement entered (the actor's logic ran; an actor the far-logic or alod cuts left out is not counted)
//     posed      the joints computed (LiveActor::calcAnim at the point where calcAnmMtx runs: in a calc list or in the drawn turn's flush)
//     draws      shape draw calls the game hands the drawers (the shapes of every model in the clipping lists that is not clipped, hidden or stamped "not drawn"
//                by a cut; the player's too). A model's shapes all count: a hidden single material is not known here.
//     indices    triangle indices of those shapes: for each strip / fan of n vertices in a shape's display lists 3 * (n - 2) (the list's own, before the renderer's welding)
//     particles  live particles of the stage's effect system at the start of its calc (each is simulated once a turn), by the emitter's origin
//     shadows    shadow controllers updated (cast) this turn, by the actor's position (their draws / indices are NOT counted)
//   screen size (draws only): the projected radius of the actor's clipping sphere in pixels of a 544-line picture: < 8, 8-32, 32-128, over 128
//   Distance is that of the actor's position (clipping sphere centre; particles: the emitter's origin) to the CAMERA, not to the player.
//   NOT counted: models outside the clipping lists (sky, water surfaces, 2D layouts), the draws of particles and shadows, non-LiveActor objects' logic,
//   anything on a turn where there is no camera or no player (star select, loading).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace port {
namespace distcensus {

constexpr int kBands = 6;
constexpr float kBandEdge[kBands - 1] = {1000.0f, 2500.0f, 5000.0f, 10000.0f, 20000.0f};  // game units
inline int BandOf(float d2) {
    for (int i = 0; i < kBands - 1; ++i)
        if (d2 <= kBandEdge[i] * kBandEdge[i]) return i;
    return kBands - 1;
}
inline const char* BandName(int b) {
    static const char* const n[kBands] = {"0-10", "10-25", "25-50", "50-100", "100-200", "200+"};
    return b >= 0 && b < kBands ? n[b] : "?";
}
// the band's edges in metres for the line ("lo-hi m"); the last one has no hi
inline int BandLoM(int b) { return b <= 0 ? 0 : static_cast<int>(kBandEdge[b - 1] / 100.0f); }

enum Class : uint8_t { kMap = 0, kChar, kPlant, kItem, kPtcl, kShadow, kOther, kClasses };
inline const char* ClassName(int c) {
    static const char* const n[kClasses] = {"map", "chars", "plants", "items", "particles", "shadows", "other"};
    return c >= 0 && c < kClasses ? n[c] : "?";
}
// the class of an actor's sensors (hit sensor types, decomp/include/Game/Util/ActorSensorUtil.hpp ATYPE_*): characters first, then items, then map objects.
inline Class ClassOfSensorType(uint32_t t) {
    if (t <= 0x12) return kChar;                  // player, puppeteer, NPCs, animals, rides, springs, jet turtle
    if (t >= 0x13 && t <= 0x44) return kChar;     // enemies and bosses
    if (t >= 0x4A && t <= 0x50) return kItem;     // coin, red coin, star piece, 1-up, coin throw, switch, morph item
    if (t >= 0x45 && t <= 0x5F) return kMap;     // map objects
    if (t >= 0x60 && t <= 0x77) return kItem;     // binders, rush objects: things the player grabs
    return kOther;
}
inline Class ClassOfSensorTypes(const uint32_t* types, int n) {
    bool ch = false, item = false, map = false;
    for (int i = 0; i < n; ++i) {
        const Class c = ClassOfSensorType(types[i]);
        ch = ch || c == kChar;
        item = item || c == kItem;
        map = map || c == kMap;
    }
    return ch ? kChar : item ? kItem : map ? kMap : kOther;
}
// from port::cliprange::Kind (plant 0, collision 1, sensor 2, decor 3, decor+fx 4, nomodel 5); `sensor` is the class of its sensors (kSens only)
inline Class ClassOfKind(int kind, Class sensor, bool player) {
    if (player) return kChar;
    switch (kind) {
        case 0: return kPlant;
        case 1: return kMap;
        case 2: return sensor;
        case 3: return kPlant;
        case 4: return kPlant;
        default: return kOther;
    }
}

// screen size classes of the projected bounding radius in pixels
constexpr int kSizes = 4;
inline int SizeOf(float radiusPx) { return radiusPx < 8.0f ? 0 : radiusPx < 32.0f ? 1 : radiusPx < 128.0f ? 2 : 3; }
inline const char* SizeName(int s) {
    static const char* const n[kSizes] = {"<8px", "8-32px", "32-128px", ">128px"};
    return s >= 0 && s < kSizes ? n[s] : "?";
}
// radius in pixels of a 544-line picture: the game projects 456 lines (cliprange.h); a sphere that contains the eye counts as big
inline float RadiusPx(float radius, float dist, float tanHalf, float screenLines = 544.0f) {
    if (!(dist > radius) || !(tanHalf > 0.0f)) return 1.0e9f;
    return radius * (screenLines * 0.5f) / (dist * tanHalf);
}

// triangle indices of a display list of strips / fans: stride bytes a vertex, big-endian counts (the game's lists). The list ends at the first other command.
// *verts gets the vertices. 0x98 = GX_TRIANGLESTRIP, 0xA0 = GX_TRIANGLEFAN.
inline uint32_t DlIndices(const uint8_t* dl, uint32_t size, uint32_t stride, uint32_t* verts) {
    uint32_t idx = 0, v = 0, i = 0;
    while (i + 3 <= size) {
        const uint8_t cmd = dl[i];
        if (cmd != 0x98 && cmd != 0xA0) break;
        const uint32_t n = (static_cast<uint32_t>(dl[i + 1]) << 8) | dl[i + 2];
        i += 3;
        if (static_cast<uint64_t>(i) + static_cast<uint64_t>(stride) * n > size) break;
        i += stride * n;
        v += n;
        if (n >= 3) idx += 3 * (n - 2);
    }
    if (verts) *verts = v;
    return idx;
}

struct Cell {
    uint64_t logic = 0, posed = 0, draws = 0, indices = 0, particles = 0;
};
struct SizeCell {
    uint64_t draws = 0, indices = 0;
};
struct Table {
    Cell cell[kBands][kClasses];
    SizeCell size[kBands][kSizes];
    uint64_t turns = 0, pictures = 0;   // game turns, drawn pictures
    void Clear() { *this = Table(); }
    void Logic(int b, Class c) { ++cell[b][c].logic; }
    void Posed(int b, Class c) { ++cell[b][c].posed; }
    void Particles(int b, uint32_t n) { cell[b][kPtcl].particles += n; }
    void Shadow(int b) { ++cell[b][kShadow].logic; }
    void Draw(int b, Class c, int sz, uint32_t draws, uint32_t indices) {
        cell[b][c].draws += draws;
        cell[b][c].indices += indices;
        size[b][sz].draws += draws;
        size[b][sz].indices += indices;
    }
    // the end of a turn: `turn` (this turn's tallies) joins this one. The draws and indices only when the turn is drawn.
    void AddTurn(const Table& turn, bool drawn) {
        ++turns;
        if (drawn) ++pictures;
        for (int b = 0; b < kBands; ++b) {
            for (int c = 0; c < kClasses; ++c) {
                const Cell& s = turn.cell[b][c];
                Cell& d = cell[b][c];
                d.logic += s.logic;
                d.posed += s.posed;
                d.particles += s.particles;
                if (drawn) {
                    d.draws += s.draws;
                    d.indices += s.indices;
                }
            }
            if (drawn)
                for (int s = 0; s < kSizes; ++s) {
                    size[b][s].draws += turn.size[b][s].draws;
                    size[b][s].indices += turn.size[b][s].indices;
                }
        }
    }
    // another table's totals added (a window into the stage's)
    void AddTable(const Table& o) {
        turns += o.turns;
        pictures += o.pictures;
        for (int b = 0; b < kBands; ++b) {
            for (int c = 0; c < kClasses; ++c) {
                cell[b][c].logic += o.cell[b][c].logic;
                cell[b][c].posed += o.cell[b][c].posed;
                cell[b][c].draws += o.cell[b][c].draws;
                cell[b][c].indices += o.cell[b][c].indices;
                cell[b][c].particles += o.cell[b][c].particles;
            }
            for (int s = 0; s < kSizes; ++s) {
                size[b][s].draws += o.size[b][s].draws;
                size[b][s].indices += o.size[b][s].indices;
            }
        }
    }
    // one band's sums over the classes
    Cell BandSum(int b) const {
        Cell t;
        for (int c = 0; c < kClasses; ++c) {
            t.logic += cell[b][c].logic;
            t.posed += cell[b][c].posed;
            t.draws += cell[b][c].draws;
            t.indices += cell[b][c].indices;
            t.particles += cell[b][c].particles;
        }
        return t;
    }
};

// A table of the indices of a model data's shapes (the game's glue fills it once a model data: key = its address). Open addressing, no deletion.
struct ModelIdx {
    static constexpr int kCap = 1024;
    uintptr_t key[kCap] = {};
    uint32_t shapes[kCap] = {};
    uint32_t indices[kCap] = {};
    int used = 0;
    bool Find(uintptr_t k, uint32_t* sh, uint32_t* ix) const {
        uint32_t i = static_cast<uint32_t>((k >> 4) * 2654435761u) % kCap;
        for (int p = 0; p < kCap; ++p, i = (i + 1) % kCap) {
            if (key[i] == k) { *sh = shapes[i]; *ix = indices[i]; return true; }
            if (key[i] == 0) return false;
        }
        return false;
    }
    void Put(uintptr_t k, uint32_t sh, uint32_t ix) {
        if (used >= kCap - 8) return;   // full: not cached (the caller recomputes)
        uint32_t i = static_cast<uint32_t>((k >> 4) * 2654435761u) % kCap;
        for (int p = 0; p < kCap; ++p, i = (i + 1) % kCap) {
            if (key[i] == k) { shapes[i] = sh; indices[i] = ix; return; }
            if (key[i] == 0) { key[i] = k; shapes[i] = sh; indices[i] = ix; ++used; return; }
        }
    }
};

// ---- the lines -------------------------------------------------------------------------------------------------------------------------------
// dist-win: <stage> turns T pictures P secs S
// dist: <stage> band <lo>-<hi> m: logic N, posed N, draws N, indices N, particles N | by class map L/P/D/I/Pt chars ... | by screen size <8px D/I 8-32px D/I 32-128px D/I >128px D/I
//   <stage> is the stage name, with "(stage)" appended on the line written at the stage's end (whole-stage means). logic, posed, particles (shadows count under logic of
//   the shadows class) are means per game turn; draws and indices means per drawn picture. In the by-class part: L logic, P posed, D draws, I indices, Pt particles.
inline int FormatWin(char* buf, size_t n, const char* stage, const Table& t, double secs) {
    return snprintf(buf, n, "dist-win: %s turns %u pictures %u secs %.1f", stage, static_cast<unsigned>(t.turns), static_cast<unsigned>(t.pictures), secs);
}
inline int FormatBand(char* buf, size_t n, const char* stage, int b, const Table& t) {
    const double tn = t.turns ? static_cast<double>(t.turns) : 1.0, pn = t.pictures ? static_cast<double>(t.pictures) : 1.0;
    const Cell s = t.BandSum(b);
    int o;
    if (b < kBands - 1)
        o = snprintf(buf, n, "dist: %s band %d-%d m:", stage, BandLoM(b), static_cast<int>(kBandEdge[b] / 100.0f));
    else
        o = snprintf(buf, n, "dist: %s band %d-inf m:", stage, BandLoM(b));
    if (static_cast<size_t>(o) < n)
        o += snprintf(buf + o, n - static_cast<size_t>(o), " logic %.1f, posed %.1f, draws %.1f, indices %.0f, particles %.1f | by class", static_cast<double>(s.logic) / tn,
                      static_cast<double>(s.posed) / tn, static_cast<double>(s.draws) / pn, static_cast<double>(s.indices) / pn, static_cast<double>(s.particles) / tn);
    for (int c = 0; c < kClasses && static_cast<size_t>(o) < n; ++c) {
        const Cell& x = t.cell[b][c];
        o += snprintf(buf + o, n - static_cast<size_t>(o), " %s %.1f/%.1f/%.1f/%.0f/%.1f", ClassName(c), static_cast<double>(x.logic) / tn, static_cast<double>(x.posed) / tn,
                      static_cast<double>(x.draws) / pn, static_cast<double>(x.indices) / pn, static_cast<double>(x.particles) / tn);
    }
    if (static_cast<size_t>(o) < n) o += snprintf(buf + o, n - static_cast<size_t>(o), " | by screen size");
    for (int z = 0; z < kSizes && static_cast<size_t>(o) < n; ++z) {
        o += snprintf(buf + o, n - static_cast<size_t>(o), " %s %.1f/%.0f", SizeName(z), static_cast<double>(t.size[b][z].draws) / pn, static_cast<double>(t.size[b][z].indices) / pn);
    }
    return o;
}

}  // namespace distcensus
}  // namespace port
