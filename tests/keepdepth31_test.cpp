// keepdepth31 (17 Nov 2026): host proof of port/keepdepth.h, the rules of GXV_HRW_KEEPDEPTH and GXV_HRW_BOXDEPTH.
//   1. Decide: which command of a pair's first picture may be left out (the game's last depth clear, only when nothing after it touches depth),
//      on position lists shaped like the game's pictures (PlanFrame's positions: two a command, the command then its clear).
//   2. StartDead: whether the picture after the pair overwrites its depth before reading it (else the back end clears it for it).
//   3. BoxDepthWeight: the window depth as the depth code (round trip, 24- and 16-bit depth), the key's values.
//   4. A ray-cast scene of a second picture: background, the player, a rail and a post in front of him; he moves, the camera stays. The box drawn
//      plainly (today: the background moves with him) against the box keyed on the full-size depth, compared with the true redraw.
// Built once as shipped (must PASS) and once per fault (-DKEEPDEPTH_MUTANT=1..11, each must FAIL): tools/test_keepdepth31.sh.
#include "port/keepdepth.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace kd = port::keepdepth;
namespace h41 = port::hrw41;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// ---- 1, 2: position lists
struct Pic {
    std::vector<uint8_t> ev;
    size_t Cmd(uint8_t atCommand, uint8_t afterIt = 0) { ev.push_back(atCommand); ev.push_back(afterIt); return ev.size() - 2; }
};
// The game's shapes (the flags PlanFrame gives each position).
constexpr uint8_t kClearEfb = kd::kFull | kd::kWrite;               // MainLoopFramework::clearEfb: colour + alpha + depth, ALWAYS
constexpr uint8_t k3D = kd::kRead | kd::kWrite;                       // an opaque model: LEQUAL + write
constexpr uint8_t kXlu = kd::kRead;                                   // a translucent model: LEQUAL, no write
constexpr uint8_t kZClear = kd::kClear | kd::kFull | kd::kWrite;      // MR::clearZBuffer
constexpr uint8_t k2D = 0;                                            // a layout pane: no depth test
static Pic GamePicture() {
    Pic p;
    p.Cmd(kClearEfb);
    for (int i = 0; i < 40; ++i) p.Cmd(k3D);
    for (int i = 0; i < 10; ++i) p.Cmd(kXlu);
    p.Cmd(0);  // an image effect's colour copy (no depth)
    p.Cmd(kXlu);
    return p;
}

// emufix31: GXV_HRW_KEEPDEPTH_2D (Decide's allowWrites) and the census's Survey. The Observatory (emu31c, Vita3K): 899 of 899 first pictures refused as
// "depth written after it"; the 2D's depth-test-ALWAYS writes are passed over with the switch on, anything that reads, overwrites all of the depth or
// is a copy's clear still refuses.
constexpr uint8_t k2DWrite = kd::kWrite | kd::kDrawn;                 // a 2D draw: depth test ALWAYS, depth write (part of the picture)
static void TestDecideWrites() {
    {
        Pic p = GamePicture();
        const size_t at = p.Cmd(kZClear);
        p.Cmd(k2D);
        p.Cmd(k2DWrite);
        p.Cmd(k2D);
        const size_t w = p.Cmd(k2DWrite);
        p.Cmd(kd::kWarpRead);
        const kd::Decision off = kd::Decide(p.ev.data(), p.ev.size());
        Check(off.why == kd::Why::WriteAfter && off.by == w, "switch off: a 2D depth write after the clear refuses, and names the LAST such draw");
        const kd::Decision on = kd::Decide(p.ev.data(), p.ev.size(), true);
        Check(on.why == kd::Why::Kept && on.at == at && on.writes == 2, "GXV_HRW_KEEPDEPTH_2D=1: the 2D draws' partial writes are passed over (2 counted)");
        const kd::After a = kd::Survey(p.ev.data(), p.ev.size());
        Check(a.clear == at && a.reads == 0 && a.writes == 2 && a.drawWrites == 2 && a.fulls == 0, "the survey: after the clear, 2 draw writes, no read, no overwrite");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        const size_t r = p.Cmd(kXlu);  // a 2D model that tests depth, before the write
        p.Cmd(k2DWrite);
        p.Cmd(kd::kWarpRead);
        Check(kd::Decide(p.ev.data(), p.ev.size(), true).why == kd::Why::ReadAfter, "with the switch on, a depth test after the clear still refuses (it would see the 3D's depth)");
        const kd::Decision off = kd::Decide(p.ev.data(), p.ev.size());
        const kd::After a = kd::Survey(p.ev.data(), p.ev.size());
        Check(off.why == kd::Why::WriteAfter && off.by != r && a.reads == 1 && a.writes == 1, "switch off: refused by the write; the survey also sees the read before it");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(k2DWrite);
        p.Cmd(kClearEfb | kd::kDrawn);  // a covering quad after the clear (writes colour and all of the depth)
        Check(kd::Decide(p.ev.data(), p.ev.size(), true).why == kd::Why::WriteAfter, "with the switch on, a whole overwrite after the clear refuses");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(k2DWrite);
        p.Cmd(0, kd::kWrite);  // a copy whose clear writes part of the depth (not a draw)
        p.Cmd(kd::kWarpRead);
        Check(kd::Decide(p.ev.data(), p.ev.size(), true).why == kd::Why::WriteAfter, "with the switch on, a copy's partial depth clear still refuses");
    }
    {
        Pic p = GamePicture();
        p.Cmd(k2DWrite);
        p.Cmd(kd::kWarpRead);
        const kd::After a = kd::Survey(p.ev.data(), p.ev.size());
        Check(kd::Decide(p.ev.data(), p.ev.size(), true).why == kd::Why::ReadAfter && a.clear == SIZE_MAX, "no clear at all: nothing to keep, the survey finds none");
    }
}

// halo32: DecideReader. The game's 2D tail has one LEQUAL + write draw (the HUD markers) after the clear; the clear is then left out and that draw gets a
// depth-only pre-pass of its own geometry. Any second depth event refuses (a write before the reader would be wiped by the pre-pass).
constexpr uint8_t kHudRead = kd::kRead | kd::kWrite | kd::kDrawn;     // the HUD markers' draw: LEQUAL + write
static void TestReader() {
    {
        Pic p = GamePicture();
        const size_t at = p.Cmd(kZClear);
        p.Cmd(k2D);
        const size_t rd = p.Cmd(kHudRead);
        p.Cmd(k2D);
        p.Cmd(kd::kWarpRead);
        const kd::ReaderDecision d = kd::DecideReader(p.ev.data(), p.ev.size());
        Check(kd::Decide(p.ev.data(), p.ev.size(), true).why == kd::Why::ReadAfter, "the HUD reader refuses the plain rule (it would see the 3D's depth)");
        Check(d.ok && d.at == at && d.reader == rd, "one HUD reader after the clear: the clear is left out and the reader found");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(kHudRead);
        p.Cmd(kHudRead);
        Check(!kd::DecideReader(p.ev.data(), p.ev.size()).ok, "two readers refuse (the second would see the first's writes wiped, or the 3D's depth)");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(k2DWrite);
        p.Cmd(kHudRead);
        Check(!kd::DecideReader(p.ev.data(), p.ev.size()).ok, "a depth write before the reader refuses (the pre-pass would wipe it)");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(kHudRead);
        p.Cmd(0, kd::kWrite);
        Check(!kd::DecideReader(p.ev.data(), p.ev.size()).ok, "a copy's depth clear after the reader refuses");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(kHudRead | kd::kFull);
        Check(!kd::DecideReader(p.ev.data(), p.ev.size()).ok, "a reader that overwrites all of the depth is no reader");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(kd::kRead);  // not a draw (no kDrawn): a copy's depth read
        Check(!kd::DecideReader(p.ev.data(), p.ev.size()).ok, "a reader that is not a draw refuses");
    }
    {
        Pic p = GamePicture();
        p.Cmd(k2D);
        p.Cmd(kHudRead);
        Check(!kd::DecideReader(p.ev.data(), p.ev.size()).ok, "no clear: nothing to leave out");
    }
}

static void TestDecide() {
    {
        Pic p = GamePicture();
        const size_t at = p.Cmd(kZClear);
        for (int i = 0; i < 12; ++i) p.Cmd(k2D);
        p.Cmd(kd::kWarpRead);  // the display copy, read by the warp
        const kd::Decision d = kd::Decide(p.ev.data(), p.ev.size());
        Check(d.why == kd::Why::Kept && d.at == at, "the game's picture: its last depth clear is left out (the 2D after it tests no depth)");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(k2D);
        p.Cmd(kXlu);  // a 2D model that tests depth after the clear: it would see the 3D's depth
        p.Cmd(k2D);
        p.Cmd(kd::kWarpRead);
        Check(kd::Decide(p.ev.data(), p.ev.size()).why == kd::Why::ReadAfter, "a depth-tested draw after the clear refuses");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(k2D);
        p.Cmd(0, kd::kWrite);  // a copy whose clear writes part of the depth
        p.Cmd(kd::kWarpRead);
        Check(kd::Decide(p.ev.data(), p.ev.size()).why == kd::Why::WriteAfter, "a partial depth write after the clear refuses");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear, kd::kRead);  // an effect run deferred to just after the clear's own command reads depth (its read lands on the clear's second position)
        p.Cmd(k2D);
        Check(kd::Decide(p.ev.data(), p.ev.size()).why == kd::Why::ReadAfter, "a read at the clear's own second position (after it) refuses");
    }
    {
        Pic p;  // GameScene::drawMirror: a clear inside the 3D, the 3D goes on, then the real end
        p.Cmd(kClearEfb);
        p.Cmd(k3D);
        p.Cmd(kZClear);
        p.Cmd(k3D);
        p.Cmd(kXlu);
        const size_t last = p.Cmd(kZClear);
        p.Cmd(k2D);
        p.Cmd(kd::kWarpRead);
        const kd::Decision d = kd::Decide(p.ev.data(), p.ev.size());
        Check(d.why == kd::Why::Kept && d.at == last, "a mirror's clear inside the 3D: only the LAST clear is left out");
    }
    {
        Pic p;
        p.Cmd(kClearEfb);
        p.Cmd(k3D);
        p.Cmd(kClearEfb);  // a covering quad that also writes colour after the 3D (a fill): not the clear
        p.Cmd(k2D);
        Check(kd::Decide(p.ev.data(), p.ev.size()).why == kd::Why::WriteAfter, "a covering quad that writes colour is never left out");
    }
    {
        Pic p = GamePicture();
        p.Cmd(k2D);
        p.Cmd(kd::kWarpRead);
        Check(kd::Decide(p.ev.data(), p.ev.size()).why == kd::Why::ReadAfter, "a picture without the clear: nothing to leave out");
        Check(kd::Decide(nullptr, 0).why == kd::Why::NoClear, "an empty picture: nothing");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(k2D);
        p.Cmd(kd::kRead);  // the present's depth-gated edge pass (GXV_EDGE_DEPTH) reads the depth at the display copy
        Check(kd::Decide(p.ev.data(), p.ev.size()).why == kd::Why::ReadAfter, "the present's edge pass reading depth refuses");
    }
    {
        Pic p = GamePicture();
        p.Cmd(kZClear);
        p.Cmd(0, kZClear & ~kd::kClear);  // a copy whose clear overwrites all of the depth after ours: refused (a full write)
        Check(kd::Decide(p.ev.data(), p.ev.size()).why == kd::Why::WriteAfter, "a whole-depth copy clear after the clear refuses");
    }
    TestDecideWrites();
}

static void TestStartDead() {
    Pic a;
    a.Cmd(kClearEfb);
    a.Cmd(k3D);
    Check(kd::StartDead(a.ev.data(), a.ev.size()), "the game's picture begins with clearEfb: the depth before it is never read");
    Pic b;
    b.Cmd(k3D);
    b.Cmd(kClearEfb);
    Check(!kd::StartDead(b.ev.data(), b.ev.size()), "a picture that tests depth before overwriting it: not dead (the back end clears it first)");
    Pic c;
    c.Cmd(kd::kRead, kd::kFull);  // a depth copy that clears after reading
    Check(!kd::StartDead(c.ev.data(), c.ev.size()), "a copy that reads the depth and then clears it: not dead");
    Pic d;
    d.Cmd(kd::kWrite);
    d.Cmd(k2D);
    d.Cmd(kClearEfb);
    Check(kd::StartDead(d.ev.data(), d.ev.size()), "a partial write before the overwrite reads nothing: dead");
    Pic e;
    e.Cmd(kd::kWarpRead);
    e.Cmd(kClearEfb);
    Check(!kd::StartDead(e.ev.data(), e.ev.size()), "the warp's own read counts as a read here");
    Check(!kd::StartDead(nullptr, 0), "an empty picture: not dead (the depth lives on)");
}

// ---- 3, 4: the box key
constexpr double kE = -1.00025, kF = -200.025;  // the game's projection (near 100): zn = f / dist - e
static double Window(double dist) { return 0.5 * (kF / dist - kE) + 0.5; }
static double Quant(double w, int bits) { const double s = std::ldexp(1.0, bits) - 1.0; return std::floor(w * s + 0.5) / s; }

static void TestWindowCode() {
    double worst = 0.0, worst24 = 0.0, worst16 = 0.0;
    for (double d = 101.0; d < 7.0e5; d *= 1.07) {
        const double c = h41::DepthCode(d);
        worst = std::max(worst, std::fabs(kd::WindowCode(Window(d), kE, kF) - c));
        if (d < 2.0e4) {  // (where the player and what stands near him can be)
            worst24 = std::max(worst24, std::fabs(kd::WindowCode(Quant(Window(d), 24), kE, kF) - c));
            worst16 = std::max(worst16, std::fabs(kd::WindowCode(Quant(Window(d), 16), kE, kF) - c));
        }
    }
    const h41::Key k = h41::MakeKey(600.0, 70.0);
    std::printf("window depth -> code: worst error %.2e (float), %.2e (24-bit depth), %.2e (16-bit) codes; the key's tolerance at 600 units: %.4f codes\n",
                worst, worst24, worst16, static_cast<double>(k.tol));
    Check(worst < 1e-6, "window depth -> code is the snapshot's code (exact arithmetic)");
    Check(worst24 < 0.02 * k.tol && worst16 < 0.25 * k.tol, "24- and 16-bit depth: the code is far inside the key's tolerance");
    const double atHim = kd::BoxDepthWeight(k, Window(600.0), Window(3000.0), kE, kF);
    const double behind = kd::BoxDepthWeight(k, Window(3000.0), Window(3000.0), kE, kF);
    const double inFront = kd::BoxDepthWeight(k, Window(600.0), Window(400.0), kE, kF);  // his pixel would land on a post 200 units in front of him
    const double near = kd::BoxDepthWeight(k, Window(650.0), Window(3000.0), kE, kF);     // inside the tolerance
    const double edge = kd::BoxDepthWeight(k, Window(600.0 + 70.0 * 1.25), Window(3000.0), kE, kF);
    std::printf("weights: his depth %.3f, background %.3f, his pixel onto a post in front %.3f, 50 units off %.3f, 87 units off %.3f\n", atHim, behind, inFront,
                near, edge);
    Check(atHim == 1.0 && behind == 0.0 && inFront == 0.0 && near == 1.0 && edge > 0.0 && edge < 1.0,
          "the key: 1 at his depth, 0 behind him and where something stands in front of him, soft between");
}

// The scene: a W x H picture. Background (a far wall, a checker), the player (a box of rows, his colour varies by row so a shift shows), a rail (two
// rows) and a post (two columns) in front of him. A = the first picture; in B he has moved by (mx, my) pixels at his own distance; the camera has not
// moved, so the scene's own warp is the identity (a true redraw of everything else is A itself).
namespace scene {
constexpr int W = 96, H = 72;
constexpr int P0x = 40, P1x = 56, P0y = 24, P1y = 44;  // the player in A: [P0x, P1x) x [P0y, P1y)
constexpr int RailY0 = 33, RailY1 = 35;                // the rail's rows, all columns
constexpr int PostX0 = 50, PostX1 = 52, PostY0 = 8, PostY1 = 60;  // the post: two columns, rows [PostY0, PostY1)
constexpr double DistBg = 3000.0, DistHim = 600.0, DistRail = 400.0, DistPost = 450.0;
struct Px { int r, g, b; double dist; int what; };  // what: 0 background, 1 player, 2 rail, 3 post
Px Bg(int x, int y) { const int c = ((x / 3 + y / 3) & 1) ? 200 : 40; return {c, c, c, DistBg, 0}; }
bool Him(int x, int y, int mx, int my) { return x >= P0x + mx && x < P1x + mx && y >= P0y + my && y < P1y + my; }
Px Picture(int x, int y, int mx, int my) {  // (mx, my): his offset; the nearest thing wins
    Px p = Bg(x, y);
    if (Him(x, y, mx, my)) p = {220, 20 + 5 * (y - P0y - my), 10, DistHim, 1};
    if (x >= PostX0 && x < PostX1 && y >= PostY0 && y < PostY1 && DistPost < p.dist) p = {20, 200, 30, DistPost, 3};
    if (y >= RailY0 && y < RailY1 && DistRail < p.dist) p = {30, 60, 220, DistRail, 2};
    return p;
}
int Err(const Px& a, int r, int g, int b) { return std::abs(a.r - r) + std::abs(a.g - g) + std::abs(a.b - b); }
}  // namespace scene

static void TestScene() {
    using namespace scene;
    const h41::Key k = h41::MakeKey(DistHim, 70.0);
    struct Move { int mx, my; } moves[] = {{0, -6}, {4, -6}, {-5, 3}, {7, 0}};
    int plainBad = 0, keyedBad = 0, checked = 0, hidden = 0, keyedBad16 = 0, uncovered = 0, plainGhost = 0, keyedGhost = 0;
    for (const Move& m : moves) {
        // the box: his rectangle in A and in B, 8 px round
        const int bx0 = std::min(P0x, P0x + m.mx) - 8, bx1 = std::max(P1x, P1x + m.mx) + 8;
        const int by0 = std::min(P0y, P0y + m.my) - 8, by1 = std::max(P1y, P1y + m.my) + 8;
        for (int y = by0; y < by1; ++y)
            for (int x = bx0; x < bx1; ++x) {
                const Px truth = Picture(x, y, m.mx, m.my);
                const Px scn = Picture(x, y, 0, 0);                  // the scene's own warp (identity): A at the pixel
                const Px box = Picture(x - m.mx, y - m.my, 0, 0);    // the box's source: A where he was
                // a pixel of him in B whose source pixel was hidden in A (behind the rail or the post): no first picture has it (the fresh player
                // draws it, tested against the kept depth); not counted
                if (truth.what == 1 && box.what != 1) { ++hidden; continue; }
                for (int bits : {0, 16}) {
                    const double wBox = bits ? Quant(Window(box.dist), bits) : Window(box.dist);
                    const double wScene = bits ? Quant(Window(scn.dist), bits) : Window(scn.dist);
                    const double a = kd::BoxDepthWeight(k, wBox, wScene, kE, kF);
                    if (truth.what != 1 && scn.what == 1) {
                        // the strip he left: what was behind him is in no first picture. The box fills it from beside him (its own source); what must
                        // never show there is the old player (a ghost): the weight must be 1 and the box's source not him
                        if (!bits) { ++uncovered; plainGhost += box.what == 1; }
                        if (!bits) keyedGhost += !(a == 1.0 && box.what != 1);
                        else keyedBad16 += !(a == 1.0 && box.what != 1);
                        continue;
                    }
                    if (!bits) { ++checked; plainBad += Err(box, truth.r, truth.g, truth.b) > 6; }  // the box today: his move applied to every pixel in it
                    const int r = static_cast<int>(std::lround(a * box.r + (1 - a) * scn.r)), g = static_cast<int>(std::lround(a * box.g + (1 - a) * scn.g)),
                              b = static_cast<int>(std::lround(a * box.b + (1 - a) * scn.b));
                    const bool bad = Err(truth, r, g, b) > 6;
                    if (bits) keyedBad16 += bad;
                    else keyedBad += bad;
                }
            }
    }
    std::printf("scene: 4 moves; %d box pixels the first picture can give (%d of him hidden in it, %d uncovered behind him: not counted); wrong pixels: "
                "plain box %d, box keyed on the full-size depth %d (16-bit depth: %d wrong or ghost); old-player ghosts in the uncovered strip: plain %d, keyed %d\n",
                checked, hidden, uncovered, plainBad, keyedBad, keyedBad16, plainGhost, keyedGhost);
    Check(plainBad > 400, "the plain box moves the background, the rail and the post with him (the measurement shows the fault)");
    Check(keyedBad == 0 && keyedBad16 == 0, "the box keyed on the full-size depth: every pixel the first picture can give is the true redraw's");
    Check(uncovered > 50 && keyedGhost == 0, "the strip he left is filled from beside him, never with the old player");
}

int main() {
    TestDecide();
    TestReader();
    TestStartDead();
    TestWindowCode();
    TestScene();
    std::printf("keepdepth31 (mutant %d): %s\n", KEEPDEPTH_MUTANT, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
