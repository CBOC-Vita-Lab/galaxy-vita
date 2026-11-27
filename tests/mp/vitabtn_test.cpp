// Galaxy 3.0 multiplayer: the controls are the Vita's (MpControls.hpp). Host test, no game needed.
//  1. the matcher itself (what counts as a Wii control, what does not),
//  2. every multiplayer text of every id in all five languages: no Wii control word, no Wii-only picture, every picture is one
//     the handheld UI covers (the list is compared with platform/src/port/handheld_ui.cpp),
//  3. every row of the action table has an input: each Wii read the game code makes comes out of vita/runtime/input.cpp (parsed),
//  4. every guide picture the menus use answers a touch; rows, heads and guide buttons of a composed page are touch targets.
// Build + run (from the repo root): tests/mp/vitabtn_build.sh
#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "Game/MP/MpControls.hpp"
#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpPanelPlan.hpp"
#include "vita_words.h"

static int gFail = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFail; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKM(c, ...) do { ++gChecks; if (!(c)) { ++gFail; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static std::string Root() { const char* r = getenv("VB_ROOT"); return r ? r : "."; }
static std::string Slurp(const std::string& rel) {
    std::ifstream f(Root() + "/" + rel);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string U8(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) o += c < 0x80 ? static_cast<char>(c < 0x20 && c != '\n' ? '?' : c) : '~';
    return o;
}

static void testMatcher() {
    using vitawords::WhyWii;
    // wrong: each of these names a Wii control
    for (const char16_t* bad : {u"Press A to start.", u"Press (A) to start", u"Hold B to point", u"Appuyez sur B.", u"Pulsa el botón A.", u"Drücke die Taste Z.",
                                u"Premi il tasto C.", u"Hold Z", u"Press + to open the menu", u"Press 1 and 2", u"Use the Nunchuk.", u"Wii Remote", u"Shake the controller",
                                u"Point at the screen.", u"Remote", u"Tap [B] to go back", u"the C button"})
        CHECKM(!WhyWii(bad).empty(), "matcher misses: %s", U8(bad).c_str());
    // fine: these do not
    for (const char16_t* good : {u"Star Race", u"Cosmic Mario", u"¡\x01 está en una burbuja!", u"A \x01 no le quedan vidas.", u"1st", u"2nd", u"Room Code (6 letters)",
                                 u"L or R", u"Hold  + a direction: quick message.", u"Show your pointer.", u"Players: 2", u"203.0.113.7", u"No Limit",
                                 u"Remove", u"Ready?", u"Choose a room to join.", u"Votre code ami : \x01", u"Tieni  o tocca"})
        CHECKM(WhyWii(good).empty(), "matcher flags: %s -> %s", U8(good).c_str(), WhyWii(good).c_str());
    // the hand-picked pictures: a Wii controller picture is refused, the Vita-covered ones are fine
    CHECK(!vitawords::WhyPic(u"").empty());
    CHECK(!vitawords::WhyPic(u"").empty());
    CHECK(!vitawords::WhyPic(u"").empty());   // (an unknown picture)
    CHECK(vitawords::WhyPic(u"       ").empty());
}

static void testTexts() {
    int n = 0;
    for (int lang = 0; lang < MpMsgText::LANG_COUNT; lang++) {
        MpMsgText::setLanguage(lang);
        for (int id = 1; id < MP_T_COUNT; id++) {
            const char16_t* t = MpMsgText::raw(static_cast<MpText>(id));
            if (!t || !t[0]) continue;
            ++n;
            std::u16string s(t);
            // (a place's number is not a button: "1." "2." are the German rows, "1°" the Italian)
            if (id >= MP_T_PLACE_1 && id <= MP_T_PLACE_4) continue;
            const std::string why = vitawords::WhyWii(s), pic = vitawords::WhyPic(s);
            CHECKM(why.empty(), "lang %d id %d: %s: %s", lang, id, why.c_str(), U8(s).c_str());
            CHECKM(pic.empty(), "lang %d id %d: %s: %s", lang, id, pic.c_str(), U8(s).c_str());
        }
    }
    fprintf(stderr, "  texts checked: %d\n", n);
    CHECK(n > 5 * 250);   // (flow, words pass: the retired screens' texts went: 1470 now)
    MpMsgText::setLanguage(0);
}

// platform/src/port/handheld_ui.cpp kCells: {glyph, picture} lines; glyph = font picture + 1
static std::set<int> HandheldPics() {
    const std::string src = Slurp("platform/src/port/handheld_ui.cpp");
    const size_t a = src.find("const Cell kCells[]");
    const size_t b = src.find("};", a);
    std::set<int> pics;
    if (a == std::string::npos || b == std::string::npos) return pics;
    const std::string block = src.substr(a, b - a);
    static const std::regex re("\\{([0-9]+), kGlyph");
    for (std::sregex_iterator it(block.begin(), block.end(), re), end; it != end; ++it) pics.insert(std::stoi((*it)[1]) - 1);
    return pics;
}

static void testPicturesAndInputs() {
    // the pictures the table and the texts rely on are the ones the handheld UI replaces
    const std::set<int> hh = HandheldPics();
    CHECK(!hh.empty());
    std::set<int> mine(std::begin(MpControls::kReplacedPics), std::end(MpControls::kReplacedPics));
    CHECKM(hh == mine, "MpControls::kReplacedPics differs from handheld_ui.cpp kCells (%zu vs %zu)", mine.size(), hh.size());
    // the Wii reads that come out of the Vita's input layer
    const std::string in = Slurp("vita/runtime/input.cpp");
    CHECK(!in.empty());
    std::set<unsigned> wii;
    static const std::regex pair("\\{SCE_CTRL_[A-Z]+, (0x[0-9A-Fa-f]+)\\}");
    for (std::sregex_iterator it(in.begin(), in.end(), pair), end; it != end; ++it) wii.insert(static_cast<unsigned>(std::stoul((*it)[1], nullptr, 16)));
    const bool swing = in.find("pad->accX = pad->accY = shake") != std::string::npos && in.find("SCE_CTRL_SQUARE") != std::string::npos;
    const bool pointer = in.find("pad->pointerValid = pointerValid") != std::string::npos && in.find("pad->touched") != std::string::npos;
    CHECK(swing);
    CHECK(pointer);
    unsigned covered = 0;
    for (const MpControls::Row& r : MpControls::kRows) {
        CHECKM(r.what && r.what[0], "row %d has no description", r.id);
        CHECKM((r.vita && r.vita[0]) || (r.touch && r.touch[0]), "row %d (%s) has no input at all", r.id, r.what);
        if (r.vita && r.vita[0]) CHECKM(vitawords::WhyWii(std::u16string(r.vita, r.vita + strlen(r.vita))).empty(), "row %d vita text: %s", r.id, r.vita);
        if (r.touch && r.touch[0]) CHECKM(vitawords::WhyWii(std::u16string(r.touch, r.touch + strlen(r.touch))).empty(), "row %d touch text: %s", r.id, r.touch);
        // every Wii read has a Vita source
        for (unsigned bit = 1; bit < 0x40000; bit <<= 1) {
            if (!(r.reads & bit)) continue;
            bool ok;
            if (bit == MpControls::W_SWING) ok = swing;
            else if (bit == MpControls::W_POINTER) ok = pointer;
            else ok = wii.count(bit) != 0;
            CHECKM(ok, "row %d (%s): Wii read 0x%x has no Vita input in vita/runtime/input.cpp", r.id, r.what, bit);
            covered |= bit;
        }
        // its picture is one the handheld UI draws a Vita button over
        if (r.pic >= 0) CHECKM(mine.count(r.pic) != 0, "row %d picture %d", r.id, r.pic);
        CHECKM(static_cast<int>(r.id) == static_cast<int>(&r - MpControls::kRows), "row %d out of order", r.id);
    }
    CHECK(MpControls::kRowCount == MpControls::A_COUNT);
    // the menus' guide buttons: every picture they pass to button() answers a touch
    // (flow split: the shared core and every screen family's file)
    std::string seq;
    for (const char* f : {"MpMenuSequence", "MpMenuMain", "MpMenuGather", "MpMenuChars", "MpMenuSession", "MpMenuFiles", "MpMenuFriends",
                          "MpMenuConnTest", "MpMenuRules"})
        seq += Slurp((std::string("decomp/src/Game/MP/") + f + ".cpp").c_str());
    static const std::regex btn("button\\(txt::PIC_([A-Z]+),");
    std::set<std::string> names;
    for (std::sregex_iterator it(seq.begin(), seq.end(), btn), end; it != end; ++it) names.insert((*it)[1]);
    CHECK(names.size() >= 4);
    for (const std::string& n : names) {
        const int pic = n == "CROSS" ? MpMsgText::PIC_CROSS : n == "CIRCLE" ? MpMsgText::PIC_CIRCLE : n == "TRIANGLE" ? MpMsgText::PIC_TRIANGLE : n == "SQUARE" ? MpMsgText::PIC_SQUARE : n == "START" ? MpMsgText::PIC_START : -1;
        CHECKM(pic >= 0 && MpControls::guideInput(pic) != MpControls::G_NONE, "guide picture %s has no touch input", n.c_str());
    }
    // the pictures the panel draws itself (t.Picture(23, ...)) are covered, too
    const std::string pl = Slurp("decomp/src/Game/MP/MpPanelPlan.cpp");
    static const std::regex pic("t\\.Picture\\(([0-9]+),");
    for (std::sregex_iterator it(pl.begin(), pl.end(), pic), end; it != end; ++it) {
        const int p = std::stoi((*it)[1]);
        CHECKM(mine.count(p) != 0, "MpPanelPlan.cpp draws picture %d", p);
    }
}

static float Adv(void*, uint16_t) { return 14.0f; }
static bool Has(void*, uint16_t) { return true; }

static void testTouchTargets() {
    using namespace MpPanel;
    FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = Has;
    static Model m;
    static Plan plan;
    static Scene scene;
    for (float width : {810.6667f, 608.0f}) {
        Screen scr;
        scr.width = width;
        // a list with four rows and the four guide buttons
        clear(&m);
        m.kind = KIND_LIST;
        copy(m.title, 48, u"Friends");
        copy(m.subtitle, 64, u"Your friends");
        m.rowCount = 4;
        for (int i = 0; i < 4; i++) copy(m.rows[i].label, kLabel, u"Somebody");
        m.cursor = 1;
        const int pics[4] = {0, 1, 2, 5};
        for (int i = 0; i < 4; i++) { m.buttons[i].pic = static_cast<uint8_t>(pics[i]); copy(m.buttons[i].label, 20, u"Go"); }
        m.buttonCount = 4;
        beginScene(&scene);
        begin(&plan);
        compose(m, scr, fm, 0.25f, &scene, &plan);
        CHECK(!plan.overflow);
        CHECKM(plan.hitCount == 4, "list rows give %d touch targets", plan.hitCount);
        for (int i = 0; i < plan.hitCount; i++) {
            const Plan::Hit& h = plan.hits[i];
            CHECKM(h.y1 - h.y0 >= 36.0f && h.x1 - h.x0 > 200.0f, "row %d target %.0f x %.0f", i, h.x1 - h.x0, h.y1 - h.y0);
            CHECK(hitCell(plan, (h.x0 + h.x1) * 0.5f, (h.y0 + h.y1) * 0.5f) == i);
            CHECK(h.x0 >= 0 && h.x1 <= scr.width && h.y0 >= 0 && h.y1 <= scr.height);
            if (i) CHECK(h.y0 >= plan.hits[i - 1].y1 - 3.0f);   // (the rows do not overlap)
        }
        CHECKM(plan.guideHitCount == 4, "guide gives %d touch targets", plan.guideHitCount);
        for (int i = 0; i < plan.guideHitCount; i++) {
            const Plan::GuideHit& h = plan.guideHits[i];
            CHECK(h.pic == pics[i]);
            CHECK(h.y1 - h.y0 >= 36.0f);
            CHECK(hitGuide(plan, (h.x0 + h.x1) * 0.5f, (h.y0 + h.y1) * 0.5f) == pics[i]);
            CHECK(h.x0 >= 0 && h.x1 <= scr.width && h.y0 >= 0 && h.y1 <= scr.height);
            if (i) CHECK(h.x0 >= plan.guideHits[i - 1].x1 - 1.0f);
        }
        CHECK(hitGuide(plan, scr.width * 0.5f, 5.0f) < 0);
        // character select: six heads
        clear(&m);
        m.kind = KIND_CHARS;
        copy(m.title, 48, u"Character");
        m.rowCount = 6;
        for (int i = 0; i < 6; i++) { copy(m.rows[i].label, kLabel, u"Mario"); m.rows[i].icon = static_cast<uint8_t>(ICON_MARIO + i); m.rows[i].owner = -1; }
        m.cursor = 0;
        m.buttonCount = 1;
        m.buttons[0].pic = 0;
        copy(m.buttons[0].label, 20, u"Select");
        beginScene(&scene);
        begin(&plan);
        compose(m, scr, fm, 0.25f, &scene, &plan);
        CHECKM(plan.hitCount == 6, "heads give %d touch targets", plan.hitCount);
        for (int i = 0; i < plan.hitCount; i++) {
            const Plan::Hit& h = plan.hits[i];
            CHECK(hitCell(plan, (h.x0 + h.x1) * 0.5f, (h.y0 + h.y1) * 0.5f) == i);
            CHECK(h.x1 - h.x0 > 100.0f && h.y1 - h.y0 > 100.0f);
            CHECK(h.x0 >= 0 && h.x1 <= scr.width && h.y0 >= 0 && h.y1 <= scr.height);
        }
        // (heads of one row side by side, the second row under the first: no target covers another's middle)
        for (int i = 0; i < plan.hitCount; i++)
            for (int j = 0; j < plan.hitCount; j++) {
                if (i == j) continue;
                const Plan::Hit& a = plan.hits[i];
                const Plan::Hit& b = plan.hits[j];
                CHECKM(hitCell(plan, (b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f) == j, "head target %d hides %d", i, j);
                (void)a;
            }
    }
}

int main() {
    testMatcher();
    testTexts();
    testPicturesAndInputs();
    testTouchTargets();
    fprintf(stderr, "%d checks, %d failures\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
