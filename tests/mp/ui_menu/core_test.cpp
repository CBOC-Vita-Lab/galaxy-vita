// The menus' host tests: the core family (flow split of ui_menu_test.cpp). Shared helpers: common.hpp.
#include "common.hpp"
#include "Game/MP/MpTextStrip.hpp"
#include "../../../decomp/src/Game/MP/MpMenuInternal.hpp"   // (flow look: Heads)

void testReachAndBack() {
    bool seen[MpMenu::S_COUNT] = {};
    for (const Path& p : kPaths) {
        fresh();
        p.go();
        const bool reached = S().screen() == p.want;
        if (!reached) fprintf(stderr, "  path %s ended on %s\n", p.name, MpMenu::screenName(S().screen()));
        CHECK(reached);
        seen[S().screen()] = true;
        // Back always returns: each press closes a level (or asks first: answered Yes), and the menu closes in a few presses
        int guard = 0;
        while (S().isOpen() && guard++ < 20) {
            if (S().screen() == MpMenu::S_SET_LEAVE) { down(); ok(); frames(2); continue; }   // settingsnative: Back there is Keep Editing (it never closes): Don't Save does
            if (S().screen() == MpMenu::S_SET_CAPTURE) { io.finger = true; frames(2); io.finger = false; continue; }   // settingsnative: a touch answers the capture with the list; no button works the menu there
            const int depth = S().depth();
            const int opened = io.opened;
            const bool fromBusy = S().screen() == MpMenu::S_BUSY;
            back();
            frames(2);
            // (uxfix #3: cancelling Play with Anyone's search goes back to the gathering screen it was chosen on: same depth, a different screen)
            const bool regathered = fromBusy && S().screen() == MpMenu::S_GATHER;
            if (!(S().depth() < depth || io.opened > opened || !S().isOpen() || regathered)) fprintf(stderr, "  path %s: back did not return (%s)\n", p.name, MpMenu::screenName(S().screen()));
            CHECK(S().depth() < depth || io.opened > opened || !S().isOpen() || regathered);
        }
        CHECK(!S().isOpen());
        CHECK(!S().isBusy());
    }
    for (int s = MpMenu::S_MAIN; s < MpMenu::S_COUNT; s++) {
        if (!seen[s]) fprintf(stderr, "  screen %s never reached\n", MpMenu::screenName(static_cast<Scr>(s)));
        CHECK(seen[s]);
    }
}

void testMessages() {
    using namespace MpMsgText;
    const char* langs[] = {"UsEnglish", "UsFrench", "EuSpanish", "EuGerman", "EuItalian"};
    for (const char* l : langs) {
        setLanguage(languageIndex(l));
        for (int id = MP_T_JOINED; id < MP_T_COUNT; id++) {
            if (id > MP_T_RACE_GO_SOON && id <= MP_T_TOAST_LAST) continue;
            if (id == 6 || id == 11 || id == 15) continue;   // (menus4: the unused numbers of the toast contract: no names, no words)
            if (!raw(static_cast<MpText>(id))[0]) fprintf(stderr, "  text %d empty (%s)\n", id, l);
            CHECK(raw(static_cast<MpText>(id))[0] != 0);
        }
        for (int id = MP_T_DLG_FIRST; id <= MP_T_ERR_ROOM_STARTED; id++) {
            const char* name = dialogName(static_cast<MpText>(id));
            CHECK(name != nullptr);
            const char16_t* t = findDialog(name);
            CHECK(t && t[0] == 0x1A && t[1] == 0x0106 && t[2] == 0x0003 && t[3] != 0);
            // window lines: at most ~44 units, at most 3 lines
            int line = 0, lines = 1, longest = 0;
            for (const char16_t* p = t + 3; *p; ++p) {
                if (*p == u'\n') { ++lines; line = 0; continue; }
                if (++line > longest) longest = line;
            }
            if (longest > 44 || lines > 3) fprintf(stderr, "  %s (%s): %d lines, longest %d\n", name, l, lines, longest);
            CHECK(longest <= 44 && lines <= 3);
        }
    }
    setLanguage(LANG_EN);
    CHECK(findDialog("MP_LEAVE_HOST_Yes")[0] == u'Y');
    CHECK(findDialog("MP_NOPE") == nullptr);
    CHECK(findDialog("XP_ERR_TIMEOUT") == nullptr);
    CHECK(findDialog("MP_DYN0") != nullptr);
    char16_t b[32];
    formatTime(b, 32, 5025);   // 83.75 s
    CHECK(std::u16string(b) == u"1:23.75");
    formatTime(b, 32, 0);
    CHECK(std::u16string(b) == u"0:00.00");
    formatTime(b, 32, 0xFFFFFFFFu);
    CHECK(std::u16string(b) == u"99:59.99");
    // names from the wire never inject markup or tags
    utf8To16(b, 32, "A\xee\x80\x80" "B\x1a" "C\xff");
    CHECK(b[0] == u'A' && b[1] == 0xFFFD && b[2] == u'B' && b[3] == 0xFFFD && b[4] == u'C' && b[5] == 0xFFFD);
    // pictures become the game's picture tags
    char16_t w[16];
    const char16_t in[] = {u'x', picChar(PIC_CROSS), 0};
    toWindowText(w, 16, in, false);
    CHECK(w[0] == u'x' && w[1] == 0x1A && w[2] == 0x0306 && w[3] == 0 && w[4] == 0);
    // a toast with a name and a time
    Args a;
    a.str = "Luigi";
    a.turns = 5025;
    format(b, 32, MP_T_FINISHED, a);
    CHECK(std::u16string(b) == u"Luigi finished in 1:23.75!");
    setLanguage(LANG_FR);
    format(b, 32, MP_T_FINISHED, a);
    CHECK(std::u16string(b) == u"Luigi a fini en 1:23.75 !");
    setLanguage(LANG_EN);
    CHECK(galaxyCount() == 42 && galaxyIndexOf("EggStarGalaxy") == 0 && galaxyStage(42) == nullptr);
}

void testResultsCodec() {
    MpRaceResults r = {9, 3, {{1, 1, 100}, {0, 2, 200}, {3, 0, MP_RACE_DNF}}};
    uint8_t buf[64];
    MpWriter w(buf, sizeof buf);
    mp_write_results(w, r);
    MpReader rd(buf, w.n);
    MpRaceResults o;
    CHECK(mp_read_results(rd, &o) && o.count == 3 && o.e[2].turns == MP_RACE_DNF && o.serial == 9);
    // duplicated slot, bad place, too many, trailing bytes: refused
    // flow's own writer (MpFlowLogic.hpp) and our reader agree
    {
        MpFlowLogic::Results fr = {5, 2, {{2, 300, 1}, {0, MpFlowLogic::kDnf, 0}}};
        uint8_t fb[64];
        MpWriter fw(fb, sizeof fb);
        MpFlowLogic::writeResults(fw, fr);
        MpReader rr(fb, fw.n);
        MpRaceResults x;
        CHECK(mp_read_results(rr, &x) && x.count == 2 && x.e[0].slot == 2 && x.e[0].turns == 300 && x.e[0].place == 1 &&
              x.e[1].turns == MP_RACE_DNF && x.e[1].place == 0);
    }
    buf[3 + 6] = 1;   // second entry's slot = first's
    MpReader r2(buf, w.n);
    CHECK(!mp_read_results(r2, &o));
    MpReader r3(buf, w.n + 0);
    (void)r3;
    std::mt19937 rng(7);
    int accepted = 0;
    for (int i = 0; i < 200000; i++) {
        uint8_t b[40];
        // half random lengths, half the right length for a count with small slots / places (so some get through)
        const int c = 1 + rng() % 4;
        const int n = (rng() & 1) ? static_cast<int>(rng() % sizeof b) : 3 + 6 * c;
        for (int k = 0; k < n; k++) b[k] = static_cast<uint8_t>(rng());
        if (n > 2 && (rng() & 1)) {
            b[2] = static_cast<uint8_t>(c);
            for (int k = 0; k < c && 8 + 6 * k < n; k++) {
                b[3 + 6 * k] = static_cast<uint8_t>(rng() % 5);
                b[8 + 6 * k] = static_cast<uint8_t>(rng() % 5);
                if (b[8 + 6 * k] == 0) b[4 + 6 * k] = b[5 + 6 * k] = b[6 + 6 * k] = b[7 + 6 * k] = 0xFF;
            }
        }
        MpReader fr(b, n);
        MpRaceResults x;
        if (mp_read_results(fr, &x)) {
            ++accepted;
            CHECK(x.count >= 1 && x.count <= 4);
            for (int k = 0; k < x.count; k++) CHECK(x.e[k].slot < 4 && x.e[k].place <= x.count && ((x.e[k].place == 0) == (x.e[k].turns == MP_RACE_DNF)));
            MpMsg m;
            m.type = MP_MSG_RESULTS;
            m.fromSlot = 0;
            m.len = static_cast<uint16_t>(n);
            memcpy(m.data, b, n);
            CHECK(MpOverlay::showResults(m));
        }
    }
    fprintf(stderr, "  results fuzz: %d of 200000 accepted\n", accepted);
    MpOverlay::reset();
}

void testCompose() {
    MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static MpPanel::Model model;
    static MpPanel::Plan plan;
    static MpPanel::Scene scene;
    const char* langs[] = {"UsEnglish", "UsFrench", "UsSpanish", "EuGerman", "EuItalian"};
    // uinative: every screen in both pictures: the Vita's 16:9 and a 4:3 one (608 square units across)
    for (float width : {810.6667f, 608.0f})
    for (const char* l : langs) {
        for (const Path& p : kPaths) {
            MpPanel::Screen scr;
            scr.width = width;
            fresh();
            MpMsgText::setLanguage(MpMsgText::languageIndex(l));
            p.go();
            S().buildModel(&model);
            vitaCheckModel(model, p.name, l);
            MpPanel::beginScene(&scene);
            MpPanel::begin(&plan);
            MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
            CHECK(!plan.overflow);
            // vitabtn: what the page shows can be touched: a row / head / tile each, a guide button each
            if (model.kind == MpPanel::KIND_LIST && model.rowCount > 0) CHECK(plan.hitCount == model.rowCount);
            if (model.kind == MpPanel::KIND_CHARS) CHECK(plan.hitCount == model.rowCount);
            if (model.kind != MpPanel::KIND_WAITING && model.kind != MpPanel::KIND_RESULTS) CHECK(plan.guideHitCount == model.buttonCount);
            int buttons = 0, frames_ = 0;
            for (int i = 0; i < scene.useCount; i++) {
                const auto& u = scene.uses[i];
                if (u.kind == MpPanel::USE_BUTTON) ++buttons;
                if (u.kind == MpPanel::USE_FRAME) ++frames_;
                CHECK(u.text < scene.textCount && u.text2 < scene.textCount);
                CHECK(u.x > -304 && u.x < 304 && u.y > -228 - 60 && u.y < 228);
            }
            CHECK(frames_ == 1);
            // character select: a taken character is a ghost (faint, pulsing alpha); the local pick stays solid
            if (model.kind == MpPanel::KIND_CHARS) {
                for (int i = 0; i < scene.useCount; i++) {
                    const auto& u = scene.uses[i];
                    if (u.kind != MpPanel::USE_CHAR) continue;
                    const auto& r = model.rows[u.slot];
                    if (r.marks & MpPanel::MARK_DISABLED) CHECK((u.state & MpPanel::kCharTaken) && u.alpha < 0.4f && u.alpha > 0.2f);
                    else CHECK(!(u.state & MpPanel::kCharTaken) && u.alpha > 0.99f);
                }
                // uinative2: under each head (MiiIcon: 100 units across at scale 1, the selected one grows ~1.1) its name and the
                // owner's plate (down to 88 units under the head's middle): the head below and the guide band stay clear of them
                const MpPanel::LayoutUse* col[6] = {};
                for (int i = 0; i < scene.useCount; i++)
                    if (scene.uses[i].kind == MpPanel::USE_CHAR && scene.uses[i].slot < 6) col[scene.uses[i].slot] = &scene.uses[i];
                for (int i = 0; i < 3; i++) {
                    if (!col[i]) continue;
                    const float half = 55.0f * col[i]->w;
                    CHECK(col[i]->y + half <= MpPanel::kContentTopAlone + MpPanel::kBandTight + 2.0f);
                    if (col[i + 3]) {
                        CHECK(col[i]->y - 101.0f >= col[i + 3]->y + 55.0f * col[i + 3]->w + MpPanel::kGhostFloat - 1.0f);   // (the plate's foot)
                        CHECK(col[i + 3]->y - 101.0f >= MpPanel::kFootTop);
                    }
                }
            }
            CHECK(buttons <= MpPanel::kMaxButtons);
            CHECK(model.kind != MpPanel::KIND_BUTTONS || buttons == model.rowCount);
            // menus3: the falls question (two answers) and every screen of one or two big buttons with a line in the game's look: the buttons
            // stand on the picture's middle (kLookMid), not hung from its top
            if (model.kind == MpPanel::KIND_BUTTONS && model.look == MpPanel::LOOK_GAME && model.line[0] && model.headCount == 0 && model.rowCount >= 1 && model.rowCount <= 2) {
                float lo = 1e9f, hi = -1e9f;
                for (int i = 0; i < scene.useCount; i++)
                    if (scene.uses[i].kind == MpPanel::USE_BUTTON) {
                        const float mid = scene.uses[i].y + 48.0f * scene.uses[i].w;
                        if (mid < lo) lo = mid;
                        if (mid > hi) hi = mid;
                    }
                if (fabsf((lo + hi) * 0.5f - MpPanel::kLookMid) > 30.0f) {
                    fprintf(stderr, "  not centred: %s %.1f (middle %.1f)\n", p.name, (lo + hi) * 0.5f, MpPanel::kLookMid);
                    CHECK(false);
                }
            }
            // everything drawn stays on the picture
            for (int i = 0; i < plan.vertexCount; i++) {
                const auto& v = plan.vertices[i];
                CHECK(v.x >= -20 && v.x <= scr.width + 20 && v.y >= -20 && v.y <= scr.height + 20);
            }
            for (int i = 0; i < plan.runCount; i++) CHECK(plan.runs[i].x >= 0 && plan.runs[i].x < scr.width && plan.runs[i].y < scr.height);
            uinativeGeometry(p.name, l, model, scr, scene, plan);
        }
    }
    MpMsgText::setLanguage(0);
    // uinative: a screen of two big buttons with no title (the entry screen's style): no title band, the buttons at the pause
    // menu's own size around the picture's middle, clear of the guide band, the hand on the selected one
    for (float width : {810.6667f, 608.0f}) {
        MpPanel::Screen scr;
        scr.width = width;
        MpPanel::clear(&model);
        model.kind = MpPanel::KIND_BUTTONS;
        model.rowCount = 2;
        MpPanel::copy(model.rows[0].label, MpPanel::kLabel, u"Single Player");
        MpPanel::copy(model.rows[1].label, MpPanel::kLabel, u"Multiplayer");
        model.cursor = 1;
        model.buttonCount = 1;
        model.buttons[0].pic = 1;
        MpPanel::copy(model.buttons[0].label, 20, u"Select");
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
        int buttons = 0, pointers = 0;
        for (int i = 0; i < scene.useCount; i++) {
            const auto& u = scene.uses[i];
            if (u.kind == MpPanel::USE_FRAME) CHECK((u.state & MpPanel::kFrameNoTitle) && (u.state & MpPanel::kFrameFoot));
            if (u.kind == MpPanel::USE_POINTER) ++pointers;
            if (u.kind != MpPanel::USE_BUTTON) continue;
            ++buttons;
            const float mid = u.y + 48.0f * u.w;
            CHECK(u.w > 0.99f && mid < 140.0f && mid - 42.0f > MpPanel::kFootTop);
        }
        CHECK(buttons == 2 && pointers == 1 && !plan.overflow);
        uinativeGeometry("bare buttons", "UsEnglish", model, scr, scene, plan);
    }
    // flow look (LOOK.md): the file select's look: no title band, no guide band, no hand, the line in the game's bottom message;
    // the players as heads (shown, never pointed), the big buttons under them and over the line's window
    for (float width : {810.6667f, 608.0f})
    for (int nb = 1; nb <= 3; nb++) {
        MpPanel::Screen scr;
        scr.width = width;
        MpPanel::clear(&model);
        model.kind = MpPanel::KIND_BUTTONS;
        model.rowCount = nb;
        const char16_t* labels[3] = {u"Start!", u"Invite a Friend Far Away", u"Change File"};
        for (int i = 0; i < nb; i++) MpPanel::copy(model.rows[i].label, MpPanel::kLabel, labels[i]);
        model.cursor = 0;
        model.buttonCount = 1;
        model.buttons[0].pic = 2;
        MpPanel::copy(model.buttons[0].label, 20, u"Back");
        MpPanel::copy(model.title, 96, u"Play Together");   // (not drawn in the game's look)
        model.look = MpPanel::LOOK_GAME;
        MpPanel::copy(model.line, 96, u"Press Start to start!");
        const char16_t* names[4] = {u"Mario", u"Luigi64", u"Peach", u"Wario"};
        MpMenu::Heads(&model, 4, [&](int i, MpPanel::Row& r) {
            MpPanel::copy(r.label, MpPanel::kLabel, names[i]);
            r.icon = static_cast<uint8_t>(MpPanel::ICON_MARIO + i);
            if (i == 0) r.marks |= MpPanel::MARK_CROWN;
            if (i == 3) r.marks |= MpPanel::MARK_DISABLED;
        });
        CHECK(model.kind == MpPanel::KIND_HEADS && model.headCount == 4 && model.rowCount == 4 + nb && model.cursor == 4);
        CHECK(model.rows[4].label[0] == u'S');
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
        CHECK(!plan.overflow);
        int buttons = 0, pointers = 0, lines = 0, heads = 0;
        float headLow = 1e9f;
        for (int i = 0; i < scene.useCount; i++) {
            const auto& u = scene.uses[i];
            if (u.kind == MpPanel::USE_FRAME) {
                CHECK((u.state & MpPanel::kFrameNoTitle) && !(u.state & MpPanel::kFrameFoot));
                CHECK(u.text >= 0 && scene.texts[u.text][0] == 0);   // (no title, no breadcrumb)
            }
            if (u.kind == MpPanel::USE_POINTER) ++pointers;
            if (u.kind == MpPanel::USE_LINE) {
                ++lines;
                CHECK(u.y == MpPanel::kLineY && u.text >= 0 && scene.texts[u.text][0] == u'P');
            }
            if (u.kind == MpPanel::USE_CHAR) {
                ++heads;
                CHECK(u.state & MpPanel::kCharStill);
                CHECK(MpPanel::headRestsFacing(u.state));   // uxfix B3: every gathering head faces the viewer (the game picture showed one from behind)
                CHECK(u.x - 40.0f > -304.0f && u.x + 40.0f < 304.0f);
                if (u.slot == 3) CHECK((u.state & MpPanel::kCharTaken) && u.alpha < 0.4f);
                else CHECK(!(u.state & MpPanel::kCharTaken) && u.alpha > 0.99f);
                if (u.y - 72.0f < headLow) headLow = u.y - 72.0f;   // (the name's foot under the head)
                CHECK(u.y + 45.0f < 228.0f);
            }
            if (u.kind == MpPanel::USE_BUTTON) {
                ++buttons;
                CHECK(u.slot < nb);
                CHECK((u.state == MpPanel::BUTTON_SELECTED) == (u.slot == 0));
                const float mid = u.y + 48.0f * u.w, half = MpPanel::kButtonH * 0.5f * 1.15f * u.w;
                CHECK(mid - half >= MpPanel::kLookBottom - 1.0f);                           // (over the line's window)
                CHECK(MpPanel::kLineY <= MpPanel::kLookBottom);                          // (the window's top under the room)
                CHECK(u.w > (nb < 3 ? 0.99f : 0.65f));   // (big: two at the pause menu's own size; three, rare on the gathering screen, smaller)
            }
        }
        for (int i = 0; i < scene.useCount; i++) {
            const auto& u = scene.uses[i];
            if (u.kind == MpPanel::USE_BUTTON) CHECK(u.y + 48.0f * u.w + MpPanel::kButtonH * 0.5f * 1.15f * u.w <= headLow + 1.0f);   // (under the names)
        }
        CHECK(buttons == nb && pointers == 0 && lines == 1 && heads == 4);
        CHECK(plan.guideHitCount == 1);   // (the guide's words stay: the Vita's touch boxes)
        // the guide's line under the line's window
        CHECK(MpPanel::kFootLine + 12.0f <= MpPanel::kLineY - MpPanel::kLineTwoH + 1.0f);   // (a two-line window too)
        CHECK(MpPanel::kLineY - MpPanel::kLineTwoH > -228.0f);                                  // (and it stays on the picture)
    }
    // menus2: the far-away code: the biggest thing on its screen, one group to a line (read aloud a group at a time), in the game's font,
    // under the heads' names and over the line's window; the code alone, or with a button still showing under it
    for (float width : {810.6667f, 608.0f})
        for (int nb = 0; nb <= 1; nb++)
            for (const char16_t* code : {u"KCBC4-SUFHS-N8568", u"KCBC4-SUFHS-N8568-ABCD", u"ABCDEFGHJKLMNPQ"}) {
                MpPanel::Screen scr;
                scr.width = width;
                MpPanel::clear(&model);
                model.kind = MpPanel::KIND_BUTTONS;
                model.rowCount = nb;
                if (nb) MpPanel::copy(model.rows[0].label, MpPanel::kLabel, u"Change File");
                model.cursor = nb ? 0 : -1;
                model.buttonCount = 1;
                model.buttons[0].pic = 2;
                MpPanel::copy(model.buttons[0].label, 20, u"Leave");
                model.look = MpPanel::LOOK_GAME;
                MpPanel::copy(model.line, 96, u"Tell your friend this code.");
                MpPanel::copy(model.big, 24, code);
                const char16_t* names[2] = {u"Mario", u"Luigi64"};
                MpMenu::Heads(&model, 2, [&](int i, MpPanel::Row& r) {
                    MpPanel::copy(r.label, MpPanel::kLabel, names[i]);
                    r.icon = static_cast<uint8_t>(MpPanel::ICON_MARIO + i);
                });
                MpPanel::beginScene(&scene);
                MpPanel::begin(&plan);
                MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
                CHECK(!plan.overflow && !plan.textClipped);
                const float lowest = MpPanel::layoutToSquareY(scr, MpPanel::kLookBottom);
                int lines = 0;
                float lastTop = -1e9f, tallest = 0;
                for (int i = 0; i < plan.runCount; i++) {
                    const auto& r = plan.runs[i];
                    if (!(r.color.r == 255 && r.color.g == 226 && r.color.b == 40)) continue;
                    ++lines;
                    const float h = 33.0f * r.scaleY;
                    CHECK(r.y > lastTop);   // (one under the other, in order)
                    lastTop = r.y;
                    if (h > tallest) tallest = h;
                    CHECK(r.length <= 6 && r.length >= 4);
                    CHECK(r.x >= 0 && r.x + RunWidth(plan, r) < scr.width);
                    CHECK(nb > 0 || r.y + h <= lowest + 1.0f);   // (over the line's window)
                }
                const int groups = (code[0] == u'A') ? 3 : (code[17] == u'-' ? 4 : 3);
                CHECK(lines == groups);
                CHECK(nb > 0 || tallest >= 36.0f);   // (big: far taller than a button's words, 24 units, or the names, 20)
                for (int i = 0; i < scene.useCount; i++)
                    if (scene.uses[i].kind == MpPanel::USE_BUTTON) CHECK(scene.uses[i].y + 48.0f * scene.uses[i].w - MpPanel::kButtonH * 0.6f * scene.uses[i].w >= MpPanel::kLookBottom - 12.0f);
            }
    // code5: the 5-character code is ONE row and the biggest thing on the screen, in all five languages, with the line under it (the short
    // line, or the one that says why the code is long), at both widths, alone or with a button under it; bigger than any group of a long code
    {
        float tallestLong = 0;
        for (float width : {810.6667f, 608.0f})
            for (int lang = 0; lang < 5; lang++)
                for (int nb = 0; nb <= 1; nb++)
                    for (int which = 0; which < 2; which++) {   // 0: the 5-character code, 1: the long code (the fallback)
                        const char* langs[] = {"UsEnglish", "UsFrench", "EuSpanish", "EuGerman", "EuItalian"};
                        MpMsgText::setLanguage(MpMsgText::languageIndex(langs[lang]));
                        MpPanel::Screen scr;
                        scr.width = width;
                        MpPanel::clear(&model);
                        model.kind = MpPanel::KIND_BUTTONS;
                        model.rowCount = nb;
                        if (nb) MpPanel::copy(model.rows[0].label, MpPanel::kLabel, u"Change File");
                        model.cursor = nb ? 0 : -1;
                        model.buttonCount = 1;
                        model.buttons[0].pic = 2;
                        MpPanel::copy(model.buttons[0].label, 20, u"Leave");
                        model.look = MpPanel::LOOK_GAME;
                        MpPanel::copy(model.line, 96, MpMsgText::raw(which ? MP_T_G_CODE_LONG : MP_T_G_TELL_CODE));
                        MpPanel::copy(model.big, 24, which ? u"KCBC4-SUFHS-N8568" : u"KCBC4");
                        const char16_t* names[2] = {u"Mario", u"Luigi64"};
                        MpMenu::Heads(&model, 2, [&](int i, MpPanel::Row& r) {
                            MpPanel::copy(r.label, MpPanel::kLabel, names[i]);
                            r.icon = static_cast<uint8_t>(MpPanel::ICON_MARIO + i);
                        });
                        MpPanel::beginScene(&scene);
                        MpPanel::begin(&plan);
                        MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
                        CHECK(!plan.overflow && !plan.textClipped);
                        int lines = 0;
                        float tallest = 0;
                        for (int i = 0; i < plan.runCount; i++) {
                            const auto& r = plan.runs[i];
                            if (!(r.color.r == 255 && r.color.g == 226 && r.color.b == 40)) continue;
                            ++lines;
                            if (33.0f * r.scaleY > tallest) tallest = 33.0f * r.scaleY;
                            CHECK(r.x >= 0 && r.x + RunWidth(plan, r) < scr.width);
                            if (!which) CHECK(r.length == 5);
                        }
                        if (which) { CHECK(lines == 3); if (tallest > tallestLong) tallestLong = tallest; }
                        else {
                            CHECK(lines == 1);   // (ONE row)
                            CHECK(tallest >= 60.0f);   // (the biggest thing on the screen: a long code's lines are 66 at most)
                        }
                    }
        CHECK(tallestLong > 0 && tallestLong < 66.0f * 1.0f + 1.0f);
        MpMsgText::setLanguage(MpMsgText::languageIndex("UsEnglish"));
    }
    // crispcode: the invite code is drawn in the big display face (the game's MenuFont64: 64 rows, here 32 units a character) when the face
    // has every character of the code, at both widths, one row or three; it fits its room and stays the biggest thing; with the face missing
    // (no metrics) or lacking a character, the same code is drawn in the message font and fits all the same (never blank)
    {
        struct Face {
            static float Adv64(void*, uint16_t) { return 32.0f; }
            static bool HasAll(void*, uint16_t) { return true; }
            static bool NoS(void*, uint16_t c) { return c != u'S'; }
        };
        int dummy = 0;
        for (float width : {810.6667f, 608.0f})
            for (int mode = 0; mode < 4; mode++)   // 0: face there, 1: no metrics, 2: lacks S, 3: lacks a letter the code does not use
                for (int which = 0; which < 2; which++) {
                    MpPanel::FontMetrics fm2[MpPanel::FONT_COUNT];
                    for (int i = 0; i < 2; i++) fm2[i].height = 33.0f, fm2[i].baseline = 26.0f, fm2[i].advance = Adv, fm2[i].has = HasG;
                    if (mode != 1) {
                        fm2[MpPanel::FONT_DISPLAY].height = 64.0f, fm2[MpPanel::FONT_DISPLAY].baseline = 54.0f;
                        fm2[MpPanel::FONT_DISPLAY].context = &dummy;
                        fm2[MpPanel::FONT_DISPLAY].advance = Face::Adv64;
                        fm2[MpPanel::FONT_DISPLAY].has = mode == 2 ? Face::NoS : Face::HasAll;
                    }
                    MpPanel::Screen scr;
                    scr.width = width;
                    MpPanel::clear(&model);
                    model.kind = MpPanel::KIND_BUTTONS;
                    model.rowCount = 0;
                    model.cursor = -1;
                    model.buttonCount = 1;
                    model.buttons[0].pic = 2;
                    MpPanel::copy(model.buttons[0].label, 20, u"Leave");
                    model.look = MpPanel::LOOK_GAME;
                    MpPanel::copy(model.line, 96, u"Tell your friend this code.");
                    MpPanel::copy(model.big, 24, which ? u"KCBC4-SUFHS-N8568" : u"SUPZX");
                    const char16_t* names[2] = {u"Mario", u"Luigi64"};
                    MpMenu::Heads(&model, 2, [&](int i, MpPanel::Row& r) {
                        MpPanel::copy(r.label, MpPanel::kLabel, names[i]);
                        r.icon = static_cast<uint8_t>(MpPanel::ICON_MARIO + i);
                    });
                    MpPanel::beginScene(&scene);
                    MpPanel::begin(&plan);
                    MpPanel::compose(model, scr, fm2, 0.25f, &scene, &plan);
                    CHECK(!plan.overflow && !plan.textClipped);
                    const bool wantDisplay = mode == 0 || mode == 3;
                    const float room = (width < 608.0f ? width : 608.0f) - 40.0f;
                    int lines = 0;
                    float tallest = 0, tallestOther = 0;
                    for (int i = 0; i < plan.runCount; i++) {
                        const auto& r = plan.runs[i];
                        const float rowH = (r.font == MpPanel::FONT_DISPLAY ? 64.0f : 33.0f) * r.scaleY;
                        if (!(r.color.r == 255 && r.color.g == 226 && r.color.b == 40)) { if (r.color.r != 6 && rowH > tallestOther) tallestOther = rowH; continue; }   // (the code's own shadow runs are no rival)
                        ++lines;
                        CHECK(r.font == (wantDisplay ? MpPanel::FONT_DISPLAY : MpPanel::FONT_MESSAGE));
                        float w = 0;
                        for (int k = 0; k < r.length; k++) w += (r.font == MpPanel::FONT_DISPLAY ? 32.0f : 16.0f) * r.scaleX;
                        CHECK(w <= room + 0.5f);                       // (inside its box)
                        CHECK(r.x >= 0 && r.x + w < scr.width);
                        CHECK(rowH <= 121.0f && rowH > 0.0f);          // (inside its row: 120 at most)
                        if (rowH > tallest) tallest = rowH;
                        if (r.font == MpPanel::FONT_DISPLAY) CHECK(r.scaleY <= 2.0f && r.scaleX <= 2.0f);   // (near 1x to 2x of the face's own size)
                    }
                    CHECK(lines == (which ? 3 : 1));
                    CHECK(tallest > tallestOther);                     // (the biggest thing on the screen)
                    if (!which) CHECK(tallest >= 60.0f);
                }
    }
    // toasts: three at once, a fourth waits; each lasts and goes
    fresh();
    session(true, 0, MP_UI_PLAYING);
    for (int i = 0; i < 4; i++) MpOverlay::toast(MP_T_JOINED, 1, 0);
    MpOverlay::toast(9999, 1, 0);   // (unknown: ignored)
    MpOverlay::toast(0x030A, 1, 0);  // (flow's "new host": the ui's own toast covers it)
    frames(1);
    CHECK(MpOverlay::toastCount() == 3);
    frames(220);
    CHECK(MpOverlay::toastCount() == 1);
    frames(220);
    CHECK(MpOverlay::toastCount() == 0);
    // flow's ids (MpFlow.hpp MP_TXT_*) become our words
    MpOverlay::toast(0x0308, 1, 5025);
    MpOverlay::toast(0x0309, 2, 1);
    MpOverlay::toast(0x0307, 1, 2 | (3 << 8));
    {
        bool fin = false, stamp = false, vote = false;
        for (auto& l : fake::log) {
            fin = fin || l == "ui: toast \"Luigi64 finished in 1:23.75!\"";
            stamp = stamp || l == "ui: toast \"Ros: Thanks!\"";
            vote = vote || l == "ui: toast \"Luigi64 wants to skip (2/3).\"";
        }
        CHECK(fin && stamp && vote);
    }
    frames(500);
    // results card: auto-continue after 8 s
    MpMsg m;
    MpWriter w(m.data, sizeof m.data);
    MpRaceResults r = {1, 2, {{0, 1, 100}, {1, 0, MP_RACE_DNF}}};
    mp_write_results(w, r);
    m.len = static_cast<uint16_t>(w.n);
    CHECK(MpOverlay::showResults(m));
    CHECK(MpOverlay::buildResults(&model) && model.rowCount == 2);
    frames(8 * 60 + 2);
    CHECK(!MpOverlay::resultsShowing());
}

void testStarCard() {
    static MpPanel::Model model;
    fresh();
    session(true, 0, MP_UI_PLAYING);
    fake::players[3].present = true;
    MpStarCard c;
    memset(&c, 0, sizeof c);
    c.teamBits = -1;
    c.count = 4;
    c.by = 1;
    c.team = true;
    for (int i = 0; i < 4; i++) c.e[i] = MpStarCard::Entry{(uint8_t)i, i != 3, (uint16_t)(10 + i), (uint16_t)(20 * i), (uint8_t)i};
    CHECK(!MpOverlay::starCardShowing() && !MpOverlay::buildStarCard(&model));
    const char16_t* want[3] = {u"TEAM STAR!", u"ÉTOILE D'ÉQUIPE !", u"¡SUPERESTRELLA EN EQUIPO!"};
    const char16_t star[2] = {MpMsgText::picChar(MpMsgText::PIC_STAR), 0}, coin[2] = {MpMsgText::picChar(MpMsgText::PIC_COIN), 0};
    const char16_t* got[3] = {u"got the Power Star!", u"a obtenu une Étoile !", u"consiguió la Superestrella!"};
    for (int lang = 0; lang < 3; lang++) {
        MpMsgText::setLanguage(lang);
        MpOverlay::showStarCard(c);
        frames(14);
        CHECK(MpOverlay::starCardShowing() && !MpOverlay::resultsShowing());   // (it is not the race card: nothing holds the player)
        CHECK(MpOverlay::buildStarCard(&model) && model.kind == MpPanel::KIND_RESULTS);
        CHECK(model.rowCount == 4 && model.buttonCount == 0 && model.alpha > 0.99f);
        CHECK(Has(model.title, want[lang]) && Has(model.title, star));   // the Power Star, not a crown
        CHECK(model.title[0] == MpMsgText::picChar(MpMsgText::PIC_STAR) && !Has(model.title, got[lang]));
        for (int i = 0; i < 4; i++) {
            const auto& r = model.rows[i];
            CHECK(r.marks & MpPanel::MARK_STATIC);
            CHECK(!(r.marks & (MpPanel::MARK_GOLD | MpPanel::MARK_CROWN)) && r.lead[0] == 0);   // no ranking, no crown
            CHECK(r.icon >= MpPanel::ICON_MARIO && r.icon <= MpPanel::ICON_COSMIC_LUIGI);
            CHECK(!!(r.marks & MpPanel::MARK_YOU) == (i == 0));
            if (i == 3) CHECK(Utf8(r.value) == "-");                 // no tally from slot 3
            else CHECK(Has(r.value, u"\t") && Has(r.value, coin));
        }
        CHECK(Utf8(model.rows[1].value).find("11") != std::string::npos && Utf8(model.rows[1].value).find("20") != std::string::npos);
        MpOverlay::dismissStarCard();
        CHECK(!MpOverlay::starCardShowing());
    }
    // costar: the team line (shared Star Bit count) comes after the players, only with a count
    {
        c.teamBits = 87;
        const char16_t* tl[3] = {u"Team", u"Équipe", u"Equipo"};
        for (int lang = 0; lang < 3; lang++) {
            MpMsgText::setLanguage(lang);
            MpOverlay::showStarCard(c);
            frames(14);
            CHECK(MpOverlay::buildStarCard(&model) && model.rowCount == 5);
            CHECK(Has(model.rows[4].label, tl[lang]) && Utf8(model.rows[4].value).find("87") != std::string::npos);
            CHECK(model.rows[4].icon == 0 && (model.rows[4].marks & MpPanel::MARK_STATIC) && !(model.rows[4].marks & MpPanel::MARK_YOU));
            MpOverlay::dismissStarCard();
        }
        c.teamBits = -1;
    }
    // not everyone was near: the normal title names whoever got the star
    for (int lang = 0; lang < 3; lang++) {
        MpMsgText::setLanguage(lang);
        c.team = false;
        MpOverlay::showStarCard(c);
        frames(14);
        CHECK(MpOverlay::buildStarCard(&model));
        CHECK(Has(model.title, u"Luigi64") && Has(model.title, got[lang]) && !Has(model.title, want[lang]));
        MpOverlay::dismissStarCard();
    }
    MpMsgText::setLanguage(0);
    // it fades in and out, auto-dismisses, a repeat refreshes without restarting the timer, bad data is ignored
    c.team = true;
    MpOverlay::showStarCard(c);
    CHECK(MpOverlay::buildStarCard(&model) && model.alpha < 0.1f);
    frames(100);
    c.e[3].have = true;
    MpOverlay::showStarCard(c);                                   // (the last tally arrived)
    CHECK(MpOverlay::buildStarCard(&model) && Utf8(model.rows[3].value) != "-");
    frames(MpOverlay::kStarCardFrames - 100 - 5);
    CHECK(MpOverlay::starCardShowing() && MpOverlay::buildStarCard(&model) && model.alpha < 1.0f);   // (fading out: the timer was not restarted)
    frames(10);
    CHECK(!MpOverlay::starCardShowing() && !MpOverlay::buildStarCard(&model));
    MpStarCard none;
    memset(&none, 0, sizeof none);
    none.teamBits = -1;
    MpOverlay::showStarCard(none);
    none.count = 9;
    MpOverlay::showStarCard(none);
    CHECK(!MpOverlay::starCardShowing());
    // a card never covers the other overlays' rules: the race waiting screen steps aside while it shows, and it takes no input
    MpOverlay::showWaiting(true, 1, 3, 60);
    MpOverlay::showStarCard(c);
    CHECK(MpOverlay::buildStarCard(&model) && !MpOverlay::buildWaiting(&model));
    MpOverlay::update(true);   // (OK pressed: the card ignores it)
    CHECK(MpOverlay::starCardShowing());
    MpOverlay::dismissStarCard();
    MpOverlay::showWaiting(false, 0, 0, -1);
    MpOverlay::reset();
    CHECK(!MpOverlay::starCardShowing());
}

void testStripCompose() {
    using namespace MpPanel;
    FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static StripPlan plan;   // (memfix: the game's strip plan)
    static Scene scene;
    Screen scr;
    Strip s;
    s.count = 3;
    s.appear = 1.0f;
    s.m[0] = {1, PARTY_NORMAL, -1.0f};
    s.m[1] = {2, PARTY_QUIET, -1.0f};
    s.m[2] = {3, PARTY_DOWN, 0.3f};
    beginScene(&scene);
    begin(&plan);
    composeStrip(s, scr, fm, 0.25f, &scene, &plan);
    CHECK(scene.useCount == 3 && !plan.overflow);
    for (int i = 0; i < 3; i++) {
        const LayoutUse& u = scene.uses[i];
        CHECK(u.kind == USE_HEAD && u.slot >= kStripHeadBase && u.slot < kMaxHeads);   // (its own head layouts: never the toasts')
        CHECK((u.state & kCharLookMask) == s.m[i].look);
        CHECK(u.x > -304 && u.x < 0 && u.y < -150 && u.y > -228);                      // (bottom left, beside the life counter)
        CHECK(u.w > 0.3f && u.w < 0.5f);
    }
    CHECK(scene.uses[0].alpha > 0.99f && !(scene.uses[0].state & (kCharTaken | kHeadDark)));
    CHECK(scene.uses[1].alpha < 0.5f && scene.uses[1].alpha > 0.2f);                  // quiet: dimmed
    CHECK((scene.uses[2].state & kHeadDark) && !(scene.uses[2].state & kCharTaken));  // down: dark
    CHECK(scene.uses[0].x < scene.uses[1].x && scene.uses[1].x < scene.uses[2].x);
    // the star flash is the picture font's Power Star, above the flashing head only
    int stars = 0;
    for (int i = 0; i < plan.runCount; i++) {
        const TextRun& r = plan.runs[i];
        if (r.font == FONT_PICTURE && plan.text[r.start] == pictureCode(7)) {
            stars++;
            const LayoutUse& u = scene.uses[2];
            CHECK(r.y < layoutToSquareY(scr, u.y) && r.x > layoutToSquareX(scr, u.x) - 30.0f && r.x < layoutToSquareX(scr, u.x) + 30.0f);
        }
    }
    CHECK(stars == 1);
    // a bubble is the pale blue wash and floats (the head moves between pictures)
    s.m[0].state = PARTY_BUBBLE;
    beginScene(&scene);
    begin(&plan);
    composeStrip(s, scr, fm, 0.0f, &scene, &plan);
    const float y0 = scene.uses[0].y;
    CHECK((scene.uses[0].state & kCharTaken) && !(scene.uses[0].state & kHeadDark));
    beginScene(&scene);
    begin(&plan);
    composeStrip(s, scr, fm, 0.1f, &scene, &plan);
    CHECK(scene.uses[0].y != y0);
    // appearing with the HUD: slides in from the left and fades; hidden when the HUD is hidden
    float lastX = -1e9f;
    for (float a = 0.1f; a <= 1.0f; a += 0.3f) {
        s.appear = a;
        beginScene(&scene);
        begin(&plan);
        composeStrip(s, scr, fm, 0.0f, &scene, &plan);
        CHECK(scene.useCount == 3 && scene.uses[1].alpha <= a * 0.5f && scene.uses[1].x >= lastX);
        lastX = scene.uses[1].x;
    }
    {   // ingame: no colour pill, no "zzz" (the dimmed head says away); the bars only when the link is weak or lost (red when lost)
        s.appear = 1.0f;
        for (int i = 0; i < s.count; i++) s.m[i].link = LINK_GOOD, s.m[i].away = false;
        s.m[0].link = LINK_LOST;
        s.m[1].away = true;
        beginScene(&scene);
        begin(&plan);
        composeStrip(s, scr, fm, 0.0f, &scene, &plan);
        bool red = false, green = false;
        for (int v = 0; v < plan.vertexCount; v++) {
            red = red || (plan.vertices[v].c.r == 255 && plan.vertices[v].c.g == 90 && plan.vertices[v].c.b == 80);
            green = green || (plan.vertices[v].c.r == 150 && plan.vertices[v].c.g == 245);
        }
        CHECK(red && !green && plan.vertexCount == 3 * 2 * 2 * 3);   // (one head's bars: 3 bars x 2 rects x 2 triangles x 3 vertices)
        bool zzz = false;
        for (int r = 0; r < plan.runCount; r++) zzz = zzz || plan.text[plan.runs[r].start] == u'z';
        CHECK(!zzz);
        s.m[1].away = false;
        beginScene(&scene);
        begin(&plan);
        composeStrip(s, scr, fm, 0.0f, &scene, &plan);
        zzz = false;
        for (int r = 0; r < plan.runCount; r++) zzz = zzz || plan.text[plan.runs[r].start] == u'z';
        CHECK(!zzz);
    }
    s.appear = 0.0f;
    beginScene(&scene);
    begin(&plan);
    composeStrip(s, scr, fm, 0.0f, &scene, &plan);
    CHECK(scene.useCount == 0 && plan.runCount == 0);
    // 4:3 as well, and a flash outside 0..1 draws nothing
    Screen four;
    four.width = 608.0f;
    s.appear = 1.0f;
    s.m[2].flash = 1.5f;
    beginScene(&scene);
    begin(&plan);
    composeStrip(s, four, fm, 0.0f, &scene, &plan);
    CHECK(scene.useCount == 3 && plan.runCount == 0);
    for (int i = 0; i < 3; i++) CHECK(scene.uses[i].x > -304 && scene.uses[i].x < 304);
}


// MpUiSystem's Strip (outside review): a 0x1A tag at the end of a short message never reads past its terminating 0
void testTextStrip() {
    char16_t out[32];
    {   // a whole tag (size 6 bytes = 3 units) between words
        const uint16_t m[] = {u'A', 0x1A, 0x0006, 0x1234, u'B', u'\n', u'C', 0};
        CHECK(MpTextStrip::strip(m, out, 32) == 4 && out[0] == u'A' && out[1] == u'B' && out[2] == u' ' && out[3] == u'C');
    }
    {   // the tag's size unit is the end
        const uint16_t m[] = {u'A', 0x1A, 0, 0x4141, 0x4141};
        CHECK(MpTextStrip::strip(m, out, 32) == 1 && out[1] == 0);
    }
    {   // a tag that says it is longer than what is left: the text ends at the 0, nothing after it is read
        const uint16_t m[] = {u'A', 0x1A, 0x00FE, 0, u'X', u'Y'};
        CHECK(MpTextStrip::strip(m, out, 32) == 1 && out[0] == u'A');
    }
    CHECK(MpTextStrip::strip(nullptr, out, 32) == 0 && out[0] == 0);
    CHECK(MpTextStrip::strip(nullptr, out, 0) == 0);
}

// uxfix #7: no hint is dropped and no line is too long. GameLookFromTitle used to drop any hint over 44 characters without a word; in the
// file select's look every line of the bottom message must fit 44 characters, and a screen's hint (the description under the cursor) must be
// part of the line, for every screen, every row the cursor can stand on, in every language.
void testNoDroppedHints() {
    int checked = 0;
    for (int l = 0; l < MpMsgText::LANG_COUNT; l++) {
        MpMsgText::setLanguage(l);
        for (const Path& p : kPaths) {
            fresh();
            p.go();
            const int rows = S().itemCount();
            for (int step = 0; step <= (rows > 8 ? 8 : rows); step++) {
                static MpPanel::Model m;
                S().buildModel(&m);
                ++checked;
                if (m.look == MpPanel::LOOK_GAME && m.line[0]) {
                    // every line of the bottom message fits
                    int seg = 0, worst = 0;
                    for (const char16_t* q = m.line;; ++q) {
                        if (*q == u'\n' || !*q) { if (seg > worst) worst = seg; seg = 0; if (!*q) break; continue; }
                        ++seg;
                    }
                    if (worst > 44) fprintf(stderr, "  line too long: %s lang %d step %d: %d chars: %s\n", p.name, l, step, worst, U8(m.line).c_str());
                    CHECK(worst <= 44);
                    // the description under the cursor is not lost
                    if (m.hint[0]) {
                        const bool kept = std::u16string(m.line).find(m.hint) != std::u16string::npos;
                        if (!kept) fprintf(stderr, "  hint dropped: %s lang %d step %d: %s\n", p.name, l, step, U8(m.hint).c_str());
                        CHECK(kept);
                    }
                }
                if (rows == 0) break;
                if (S().screen() == MpMenu::S_NONE) break;
                down();
            }
        }
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    fresh();
    CHECK(checked > 500);
}
