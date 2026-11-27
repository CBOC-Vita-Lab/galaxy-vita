// Host tests for the in-play presentation (owner: ingame): decomp/include/Game/MP/MpPresentLogic.hpp, the strip's arrivals and
// departures and the life counter's own head (MpPartyLogic.hpp), the default name (platform/src/mp/mp_config.cpp mp_default_name).
//   - which events toast (the rule: a toast only for what the player cannot see), and the template match that catches the menu
//     sequence's own "<name> left." / "<name> changed the rules: ..." in all five languages;
//   - the name tag's state machine (join, back on screen, character change, the balloon, the HUD hidden, no puppet);
//   - the HUD rectangles and the edge heads' clamping at 16:9 and 4:3, every direction, hostile input;
//   - the watch camera's pull-in;
//   - the default name rule.
//   tests/mp/present_build.sh
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpPartyLogic.hpp"
#include "Game/MP/MpPresentLogic.hpp"
#include "mp_config.h"

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace MpPresent;
namespace txt = MpMsgText;

// ---- toasts
static void testToastRule() {
    // not shown: what the picture shows
    for (uint16_t id : {MP_T_JOINED, MP_T_LEFT, MP_T_GOT_STAR, MP_T_STAMP_0, MP_T_STAMP_3, MP_T_PLAYER_DOWN, MP_T_BUBBLED, MP_T_POPPED,
                        MP_T_ALL_CAUGHT})
        CHECK(!toastShown(id));
    for (uint16_t id : {0x0302, 0x0303, 0x0309}) CHECK(!toastShown(id));
    // shown: what nothing on screen says
    for (uint16_t id : {MP_T_HOST_CHOOSES, MP_T_HOST_MIGRATED, MP_T_YOU_ARE_HOST, MP_T_OUT_OF_LIVES, MP_T_SKIP_VOTE,
                        MP_T_EVERYONE_LEFT, MP_T_WARP_FAILED, MP_T_WARPED_TO_YOU, MP_T_HOST_BROWSING, MP_T_HOST_BROWSING_STAR,
                        MP_T_HOST_DATA_BAD, MP_T_FINISHED, MP_T_NO_SETTINGS})
        CHECK(toastShown(id));
    for (uint16_t id : {0x0301, 0x0304, 0x0305, 0x0306, 0x0307, 0x0308, 0x030B}) CHECK(toastShown(id));
}

static void testTemplates() {
    for (int lang = 0; lang < txt::LANG_COUNT; lang++) {
        txt::setLanguage(lang);
        for (const char* name : {"Alex", "Luigi", "A", "Ana María", "名前"}) {
            txt::Args a;
            a.str = name;
            char16_t t[96];
            txt::format(t, 96, MP_T_LEFT, a);
            CHECK(matchesTemplate(t, txt::raw(MP_T_LEFT)));
            CHECK(!matchesTemplate(t, txt::raw(MP_T_HOST_MIGRATED)));
            const txt::Rules r{name, 0, 1, true};
            txt::formatRulesToast(t, 96, r, true);
            CHECK(matchesTemplate(t, txt::raw(MP_T_RULES_CHANGED)));
            txt::formatRulesToast(t, 96, r, false);   // (the late joiner's rules toast stays)
            CHECK(!matchesTemplate(t, txt::raw(MP_T_RULES_CHANGED)) && !matchesTemplate(t, txt::raw(MP_T_LEFT)));
            // kept toasts never match a dropped template
            a.str2 = "Mario";
            txt::format(t, 96, MP_T_HOST_MIGRATED, a);
            CHECK(!matchesTemplate(t, txt::raw(MP_T_LEFT)) || lang != 0);
            txt::format(t, 96, MP_T_RECONNECTING, txt::Args());
            for (MpText id : {MP_T_LEFT, MP_T_JOINED, MP_T_RULES_CHANGED, MP_T_GOT_STAR, MP_T_BUBBLED, MP_T_POPPED, MP_T_PLAYER_DOWN})
                CHECK(!matchesTemplate(t, txt::raw(id)));
            txt::format(t, 96, MP_T_UPDATE_AVAILABLE, txt::Args());
            for (MpText id : {MP_T_LEFT, MP_T_JOINED, MP_T_RULES_CHANGED}) CHECK(!matchesTemplate(t, txt::raw(id)));
        }
    }
    txt::setLanguage(0);
    // the matcher itself: a wildcard is one or more characters; hostile input
    CHECK(matchesTemplate(u"Bob left.", u"\x01 left."));
    CHECK(!matchesTemplate(u" left.", u"\x01 left."));
    CHECK(!matchesTemplate(u"Bob left!", u"\x01 left."));
    CHECK(matchesTemplate(u"a left. left.", u"\x01 left."));
    CHECK(matchesTemplate(u"x: y z", u"\x01: \x02"));
    CHECK(!matchesTemplate(u"x:", u"\x01: \x02"));
    CHECK(!matchesTemplate(nullptr, u"\x01") && !matchesTemplate(u"a", nullptr) && !matchesTemplate(u"a", u""));
    std::u16string longText(5000, u'a');
    CHECK(!matchesTemplate(longText.c_str(), u"\x01b"));
}

// ---- name tags
static TagIn in(bool onScreen, bool hud = true, bool balloon = false, uint8_t look = 1, bool present = true, bool drawable = true) {
    TagIn i;
    i.present = present; i.drawable = drawable; i.onScreen = onScreen; i.hud = hud; i.balloon = balloon; i.look = look;
    return i;
}
static int turnsShown(TagState& s, const TagIn& i, int max = 400) {
    int n = 0;
    for (int k = 0; k < max; k++) if (s.step(i).tag > 0.0f) n++;
    return n;
}
static void testTagJoin() {
    TagState s;
    // joins on screen: shown ~2 s (plus the fade out), then gone while nothing happens
    const int n = turnsShown(s, in(true));
    CHECK(n >= kTagShowTurns && n <= kTagShowTurns + kFadeTurns + 1);
    CHECK(s.step(in(true)).tag == 0.0f);
    // a balloon brings it back for as long as it is up
    for (int k = 0; k < 300; k++) {
        const float a = s.step(in(true, true, true)).tag;
        CHECK(k < kFadeTurns || a == 1.0f);
    }
    int after = 0;
    for (int k = 0; k < 50; k++) if (s.step(in(true)).tag > 0.0f) after++;
    CHECK(after <= kFadeTurns);
    // a character change: again
    CHECK(turnsShown(s, in(true, true, false, 2), 200) >= kTagShowTurns);
    // off screen briefly (a camera swing): no new show; off for a while then back: shown again
    for (int k = 0; k < 10; k++) s.step(in(false, true, false, 2));
    CHECK(turnsShown(s, in(true, true, false, 2), 60) == 0);
    for (int k = 0; k < kTagOffMin + 5; k++) { const TagOut o = s.step(in(false, true, false, 2)); CHECK(o.tag == 0.0f); }
    CHECK(turnsShown(s, in(true, true, false, 2), 200) >= kTagShowTurns);
}
static void testTagHud() {
    TagState s;
    // joined during a cutscene: nothing while the HUD is hidden, and the 2 s are still owed when play resumes
    for (int k = 0; k < 300; k++) { const TagOut o = s.step(in(true, false)); CHECK(o.tag == 0.0f && o.edge == 0.0f); }
    CHECK(turnsShown(s, in(true), 400) >= kTagShowTurns);
    // a tag that is up goes at once when the HUD hides (star get, the fly-in), and off screen at once (never over nothing)
    TagState t;
    t.step(in(true)); for (int k = 0; k < 20; k++) t.step(in(true));
    CHECK(t.step(in(true)).tag == 1.0f);
    CHECK(t.step(in(true, false)).tag == 0.0f);
    TagState u;
    for (int k = 0; k < 20; k++) u.step(in(true));
    CHECK(u.step(in(false)).tag == 0.0f);
    // no puppet / not present: nothing at all
    TagState v;
    for (int k = 0; k < 20; k++) { const TagOut o = v.step(in(true, true, true, 1, true, false)); CHECK(o.tag == 0.0f && o.edge == 0.0f); }
    for (int k = 0; k < 20; k++) { const TagOut o = v.step(in(false, true, true, 1, false, true)); CHECK(o.tag == 0.0f && o.edge == 0.0f); }
}
static void testTagEdge() {
    TagState s;
    // off screen: the edge head fades in, no tag; back on screen: the edge head goes at once; HUD hidden: none
    float e = 0.0f;
    for (int k = 0; k < 20; k++) { const TagOut o = s.step(in(false)); CHECK(o.tag == 0.0f); e = o.edge; }
    CHECK(e == 1.0f);
    CHECK(s.step(in(true)).edge == 0.0f);
    for (int k = 0; k < 20; k++) s.step(in(false));
    CHECK(s.step(in(false, false)).edge == 0.0f);
    // leaving the session clears everything; a rejoin is a join (the name shows again)
    s.step(in(false, true, false, 1, false));
    CHECK(turnsShown(s, in(true), 200) >= kTagShowTurns);
}

// ---- HUD rectangles and edge heads
static bool inside(const Rect& r, float x, float y, float rad) { return overlaps(r, x, y, rad); }
static void testEdges(float w, float h, int strip) {
    Rect hud[kHudRects];
    const int n = hudRects(w, h, strip, hud);
    CHECK(n == (strip > 0 ? 5 : 4));
    for (int i = 0; i < n; i++) CHECK(hud[i].x0 >= -1.0f && hud[i].x1 <= w + 1.0f && hud[i].y0 >= -1.0f && hud[i].y1 <= h + 1.0f && hud[i].x0 < hud[i].x1);
    // the four corners are covered (the counters sit there)
    CHECK(inside(hud[0], w * 0.08f, h * 0.1f, 1.0f));     // Star counter, top left
    CHECK(inside(hud[1], w * 0.88f, h * 0.18f, 1.0f));    // life meter, top right
    CHECK(inside(hud[2], w * 0.08f, h * 0.88f, 1.0f));    // life counter, bottom left
    CHECK(inside(hud[3], w * 0.9f, h * 0.82f, 1.0f));     // coins / Star Bits, bottom right
    const float rad = 25.0f, margin = 30.0f;
    int checked = 0;
    for (int a = 0; a < 360; a += 3) {
        for (int behind = 0; behind < 2; behind++) {
            const float ang = a * 3.14159265f / 180.0f;
            const float sx = w * 0.5f + cosf(ang) * w * 2.0f, sy = h * 0.5f + sinf(ang) * h * 2.0f;
            const EdgePlace p = edgePlace(sx, sy, behind != 0, w, h, margin, rad, hud, n);
            ++checked;
            CHECK(p.x >= margin - 0.5f && p.x <= w - margin + 0.5f && p.y >= margin - 0.5f && p.y <= h - margin + 0.5f);
            for (int i = 0; i < n; i++) CHECK(!inside(hud[i], p.x, p.y, rad));
            CHECK(std::fabs(p.ax * p.ax + p.ay * p.ay - 1.0f) < 1e-3f);
            // on the edge (inset): one coordinate at the inset border
            const bool onEdge = std::fabs(p.x - margin) < 0.6f || std::fabs(p.x - (w - margin)) < 0.6f || std::fabs(p.y - margin) < 0.6f ||
                                std::fabs(p.y - (h - margin)) < 0.6f;
            CHECK(onEdge);
            if (!behind) {   // it points toward the player
                const float dx = sx - p.x, dy = sy - p.y;
                CHECK(dx * p.ax + dy * p.ay > 0.0f);
            }
        }
    }
    CHECK(checked == 240);
    // straight left, middle height: no HUD there, so exactly the left edge at the player's height
    const EdgePlace l = edgePlace(-500.0f, h * 0.5f, false, w, h, margin, rad, hud, n);
    CHECK(std::fabs(l.x - margin) < 0.01f && std::fabs(l.y - h * 0.5f) < 0.01f && l.ax < -0.99f);
    // toward the top left corner: pushed off the Star counter, still near that corner
    const EdgePlace c = edgePlace(-1000.0f, -1000.0f, false, w, h, margin, rad, hud, n);
    CHECK(!inside(hud[0], c.x, c.y, rad) && (c.x < w * 0.5f && c.y < h * 0.5f));
    // hostile input
    const float bad[] = {NAN, INFINITY, -INFINITY, 1e38f};
    for (float b : bad) {
        const EdgePlace p = edgePlace(b, b, false, w, h, margin, rad, hud, n);
        CHECK(MpPresent::finitef(p.x) && MpPresent::finitef(p.y) && MpPresent::finitef(p.ax) && MpPresent::finitef(p.ay));
        for (int i = 0; i < n; i++) CHECK(!inside(hud[i], p.x, p.y, rad));
    }
    const EdgePlace z = edgePlace(w * 0.5f, h * 0.5f, true, w, h, margin, rad, hud, n);   // right behind the camera
    CHECK(MpPresent::finitef(z.x) && MpPresent::finitef(z.y));
}

// ---- the watch camera's pull-in
static void testPull() {
    CHECK(pullStep(1.0f, 2.0f, 800.0f) == 1.0f);
    const float p = pullStep(1.0f, 0.5f, 800.0f);
    CHECK(std::fabs(p - (0.5f - kPullMargin / 800.0f)) < 1e-5f);   // at once, in front of the wall
    CHECK(pullStep(p, 0.5f, 800.0f) == p);
    CHECK(pullStep(1.0f, 0.01f, 800.0f) == kPullMin);
    float q = p;
    int turns = 0;
    while (q < 1.0f && turns < 1000) { const float n = pullStep(q, 2.0f, 800.0f); CHECK(n - q <= kPullRelease + 1e-6f); q = n; turns++; }
    CHECK(q == 1.0f && turns > 5);
    CHECK(pullStep(NAN, NAN, NAN) == 1.0f && pullStep(0.5f, 2.0f, 0.0f) == 1.0f && MpPresent::finitef(pullStep(NAN, 0.3f, 500.0f)));
}

// ---- the strip: arrivals and departures; the life counter's head
static void testStrip() {
    using namespace MpPartyLogic;
    for (int l = 0; l < 6; l++) CHECK(counterNeedsHead((uint8_t)l) == (l >= 2));
    PartyInput in{};
    in.localSlot = 0;
    in.presentMask = 0x3;
    for (int s = 0; s < 4; s++) { in.look[s] = (uint8_t)s; in.flash[s] = -1.0f; }
    in.look[0] = 3;   // Cosmic Mario: the counter gets his own head
    MpPanel::Strip st;
    StripMotion mo;
    mo.reset();
    makeStrip(in, 1.0f, &st);
    CHECK(st.localLook == 3);
    mo.apply(in, &st);
    CHECK(st.count == 1 && st.m[0].enter == 1.0f && st.m[0].pos == 0.0f);   // the first roster is already in
    // slot 2 joins: slides in over kSlideTurns, after slot 1
    in.presentMask = 0x7;
    float last = -1.0f;
    for (int k = 0; k < kSlideTurns; k++) {
        makeStrip(in, 1.0f, &st);
        mo.apply(in, &st);
        CHECK(st.count == 2 && st.m[1].look == 2 && st.m[1].enter > last);
        last = st.m[1].enter;
    }
    CHECK(last == 1.0f && st.m[1].pos == 1.0f);
    // slot 1 leaves: kept with its look while it slides out, the other glides into its place
    in.presentMask = 0x5;
    float pos = 2.0f;
    for (int k = 0; k < kSlideTurns - 1; k++) {
        makeStrip(in, 1.0f, &st);
        mo.apply(in, &st);
        CHECK(st.count == 2 && st.m[0].look == 1 && st.m[0].enter < 1.0f && st.m[1].pos <= pos);
        pos = st.m[1].pos;
    }
    makeStrip(in, 1.0f, &st);
    mo.apply(in, &st);
    CHECK(st.count == 1 && st.m[0].look == 2 && st.m[0].pos == 0.0f);
}

// ---- the default name
static void testDefaultName() {
    char out[MP_NAME_BYTES + 1];
    // 13 Nov: the default name is the character's, never the console's user name
    mp_default_name(1, out);
    CHECK(!strcmp(out, "Luigi"));
    mp_default_name(2, out);
    CHECK(!strcmp(out, "Toad"));
    mp_default_name(5, out);
    CHECK(!strcmp(out, "Cosmic Luigi"));
    mp_default_name(99, out);
    CHECK(!strcmp(out, "Mario"));
    for (int l = 0; l < MP_LOOK_COUNT; l++) {
        mp_default_name(l, out);
        CHECK(strcmp(out, "Player") != 0 && strlen(out) <= MP_NAME_BYTES && out[0]);
    }
    // a first run (no mp.cfg) never reads "Player" and is the character's name
    mp_config_set_dir("/tmp/present_test_no_cfg_dir");
    mp_config_load();
    CHECK(!strcmp(mp_config()->name, "Mario") && !mp_config()->nameTyped);
    // the name follows the character until the player types one; a typed name is kept (also through a save and a load)
    {
        MpConfig c = *mp_config();
        c.look = 1;
        mp_config_set(&c);
        CHECK(!strcmp(mp_config()->name, "Luigi") && !mp_config()->nameTyped);
        c.look = 2;   // (mp_ui_set_profile with a new name does exactly this: name + nameTyped)
        snprintf(c.name, sizeof c.name, "Ana");
        c.nameTyped = 1;
        mp_config_set(&c);
        CHECK(!strcmp(mp_config()->name, "Ana") && mp_config()->nameTyped && mp_config()->look == 2);
        c = *mp_config();
        c.look = 0;   // a change of character with a typed name: the name stays
        mp_config_set(&c);
        CHECK(!strcmp(mp_config()->name, "Ana") && mp_config()->look == 0);
        CHECK(mp_config_save());
        mp_config_load();
        CHECK(!strcmp(mp_config()->name, "Ana") && mp_config()->nameTyped);
        // an old file (no name_typed key): the console's user name, "Player" or a character's name were never typed; another name was
        FILE* f = fopen("/tmp/present_test_no_cfg_dir/mp.cfg", "w");
        if (f) {
            fputs("version = 1\nname = Player\nlook = 1\n", f);
            fclose(f);
            mp_config_load();
            CHECK(!strcmp(mp_config()->name, "Luigi") && !mp_config()->nameTyped);
            f = fopen("/tmp/present_test_no_cfg_dir/mp.cfg", "w");
            fputs("version = 1\nname = Rosie\nlook = 1\n", f);
            fclose(f);
            mp_config_load();
            CHECK(!strcmp(mp_config()->name, "Rosie") && mp_config()->nameTyped);
        } else CHECK(false);
    }
}

// ---- the star row timing
static void testStarRow() {
    CHECK(starRowAlpha(0, true) == 0.0f && starRowAlpha(kStarRowFrom - 1, true) == 0.0f);
    float last = 0.0f;
    for (int a = kStarRowFrom; a < kStarRowFrom + kStarRowFade; a++) { const float v = starRowAlpha(a, true); CHECK(v > last && v <= 1.0f); last = v; }
    CHECK(starRowAlpha(kStarRowFrom + kStarRowFade, true) == 1.0f && starRowAlpha(100000, true) == 1.0f);
    CHECK(starRowAlpha(kStarRowFrom + 50, false) == 0.0f && starRowAlpha(-5, true) == 0.0f);   // the demo ended: gone at once
}

// ---- the life counter shows exactly one head per character; the strip's gap
static void testCounterPane() {
    using namespace MpPartyLogic;
    CHECK(counterPane(0) == COUNTER_MARIO);
    CHECK(counterPane(1) == COUNTER_LUIGI);       // the game's own Luigi picture, not Mario plus a head
    for (int l = 2; l < 6; l++) CHECK(counterPane(l) == COUNTER_OWN_HEAD);   // Toad, Cosmic Mario, Yellow Toad, Cosmic Luigi
    CHECK(counterPane(-1) == COUNTER_MARIO && counterPane(99) == COUNTER_MARIO);
    for (int l = 0; l < 6; l++) CHECK((counterPane(l) == COUNTER_OWN_HEAD) == counterNeedsHead((uint8_t)l));
    // the strip's first head starts clear of the counter's "x 9": its left edge at least 30 layout units right of the counter's
    // number (which ends about at x = -205 in the 608-unit layout)
    CHECK(MpPanel::kStripX - MpPanel::kStripScale * 50.0f >= -205.0f + 30.0f);
    CHECK(kStripFirstX == MpPanel::kStripX);
}

int main() {
    testCounterPane();
    testStarRow();
    testToastRule();
    testTemplates();
    testTagJoin();
    testTagHud();
    testTagEdge();
    testEdges(810.6667f, 456.0f, 3);   // 16:9 (the Vita's 960 x 544 is 16:9 too)
    testEdges(810.6667f, 456.0f, 1);
    testEdges(608.0f, 456.0f, 2);      // 4:3
    testEdges(608.0f, 456.0f, 0);
    testPull();
    testStrip();
    testDefaultName();
    printf("present tests: %d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
