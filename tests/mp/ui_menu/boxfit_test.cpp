// menus5 item 2: the long lines of the word table, each measured in the box that really draws it, in all five languages. The mechanical
// check (share/qa/mechanical-checks-1112.md) counted every line over 44 characters; most of those lines are not in a 44-character box.
// Each id below was traced to its box (13 Nov); the limit is that box's own:
//   TOAST   MpUi::toast / MpOverlay::toastText: one line, a 96-unit buffer; the longest player name (15 characters) goes in first.
//   HINT    a hint under a title in the game's look (GameLookFromTitle keeps it only when it is 44 characters or fewer: longer is dropped).
//   DIALOG  the game's message window (RawWindow, Dlg::Ok / YesNo): up to 3 lines of 44 characters.
//   LINE    the game's bottom message (Model::line, InfConfirmU: 480 units): wrapped at spaces, two lines at most. The line holds about 36
//           letters (share/playtest/shots2/falls_question.png: "how should they come back?", 26 letters, is 425 of the band's 660 pixels);
//           34 is used, so a wider-lettered language still has room.
//   STATUS  a list window's one status line (Model::status, size 24, drawn up to the window's width less 80): composed for real and the
//           words must not be squeezed under the project's own floor (17 units high, the composer's textClipped).
#include "common.hpp"
#include "../../../decomp/src/Game/MP/MpMenuInternal.hpp"

namespace {
enum Box { TOAST, HINT, DIALOG, LINE, STATUS };
struct Probe { MpText id; Box box; const char* who; };
const Probe kProbes[] = {
    {MP_T_STAR_NOT_SAVED, TOAST, "MpFlow star not saved"},
    {MP_T_OUT_OF_LIVES, TOAST, "MpBubble"},
    {MP_T_UPDATE_AVAILABLE, TOAST, "update notice"},
    {MP_T_DESC_SLOT_LEAVE, HINT, "Which File? (leave)"},
    {MP_T_DESC_UNBLOCK, HINT, "blocked list (unused today)"},
    {MP_T_CT_L_SEE, LINE, "connection test"},
    {MP_T_CT_L_FAR_MAYBE, LINE, "connection test"},
    {MP_T_CT_L_FAR_NO, LINE, "connection test"},
    {MP_T_CT_L_UPDATE, LINE, "connection test"},
    {MP_T_CT_L_CLOCK, LINE, "connection test"},
    {MP_T_CT_L_UPDATE, STATUS, "friends gate"},
    {MP_T_CT_L_CLOCK, STATUS, "friends gate"},
    {MP_T_SVC_BUSY, STATUS, "friends gate"},
    {MP_T_NO_BLOCKED, STATUS, "blocked list"},
    {MP_T_KS_KEEP_NO_SPACE, DIALOG, "make a copy"},
    {MP_T_KS_KS_NOTICE, DIALOG, "update notice"},
    {MP_T_KS_BK_ROLLED, DIALOG, "restore a copy"},
    {MP_T_KS_BK_BAD, DIALOG, "restore a copy"},
    {MP_T_KS_BK_NEWER, DIALOG, "restore a copy"},
    {MP_T_KS_BK_NO_SPACE, DIALOG, "restore a copy"},
};

int WrapLines(const std::u16string& t, size_t width) {   // greedy, at spaces; a break already in the text stays
    int lines = 0;
    size_t from = 0;
    while (from <= t.size()) {
        size_t nl = t.find(u'\n', from);
        const std::u16string para = t.substr(from, nl == std::u16string::npos ? std::u16string::npos : nl - from);
        size_t cur = 0;
        ++lines;
        size_t p = 0;
        while (p < para.size()) {
            size_t e = para.find(u' ', p);
            if (e == std::u16string::npos) e = para.size();
            const size_t w = e - p;
            if (cur == 0) cur = w;
            else if (cur + 1 + w <= width) cur += 1 + w;
            else ++lines, cur = w;
            p = e + 1;
        }
        if (nl == std::u16string::npos) break;
        from = nl + 1;
    }
    return lines;
}
size_t Widest(const std::u16string& t) {
    size_t worst = 0, run = 0;
    for (char16_t c : t) {
        if (c == u'\n') run = 0;
        else if (++run > worst) worst = run;
    }
    return worst;
}
}  // namespace

void testWordsRealBoxes() {
    using namespace MpMsgText;
    MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static MpPanel::Model model;
    static MpPanel::Plan plan;
    static MpPanel::Scene scene;
    int measured = 0;
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        for (const Probe& p : kProbes) {
            Args a;
            a.str = "Wwwwwwwwwwwwwww";   // (15 letters: the widest name)
            a.str2 = "Wwwwwwwwwwwwwww";
            char16_t buf[160];
            format(buf, 160, p.id, a);
            const std::u16string t = buf;
            CHECK(!t.empty());
            bool ok = true;
            switch (p.box) {
            case TOAST: ok = t.size() <= 95 && t.find(u'\n') == std::u16string::npos; break;
            case HINT: ok = t.size() <= 44 && t.find(u'\n') == std::u16string::npos; break;
            case DIALOG: ok = Widest(t) <= 44 && WrapLines(t, 44) <= 3; break;
            case LINE: ok = WrapLines(t, 34) <= 2; break;
            case STATUS: {
                for (float width : {810.6667f, 608.0f}) {
                    MpPanel::Screen scr;
                    scr.width = width;
                    model = MpPanel::Model();
                    model.kind = MpPanel::KIND_LIST;
                    const std::u16string title = u"Friends";
                    for (size_t i = 0; i < title.size(); i++) model.title[i] = title[i];
                    for (size_t i = 0; i < t.size() && i < 79; i++) model.status[i] = t[i];
                    model.buttonCount = 1;
                    MpMenu::ModelButton(&model, MpMsgText::PIC_CIRCLE, MP_T_BACK);
                    MpMenu::GameLookFromTitle(&model);
                    MpPanel::beginScene(&scene);
                    MpPanel::begin(&plan);
                    MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
                    if (plan.textClipped || plan.overflow) ok = false;
                    for (int i = plan.hintRuns; i < plan.runCount; i++)
                        if (plan.runs[i].font == MpPanel::FONT_MESSAGE && 33.0f * plan.runs[i].scaleY < 17.0f) ok = false;
                }
                break;
            }
            }
            if (!ok) fprintf(stderr, "  words: id %d (%s) lang %d does not fit its %s box: %zu characters, widest line %zu\n", static_cast<int>(p.id), p.who, lang,
                             p.box == TOAST ? "toast" : p.box == HINT ? "hint" : p.box == DIALOG ? "dialog" : p.box == LINE ? "line" : "status", t.size(), Widest(t));
            CHECK(ok);
            ++measured;
        }
    }
    setLanguage(LANG_EN);
    fprintf(stderr, "  words in their real boxes: %d measures (%zu ids x 5 languages)\n", measured, sizeof kProbes / sizeof kProbes[0]);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Native Settings (v3, 15 Nov): every new id measured in the box that draws it, five languages.
//   BAND    the game's bottom message (a pill's or row's sentence, the leave question): two lines of 34 letters at most (no line over 44).
//   PILL    a big button: "Label: Value" (Row::label 40, Row::value 28). The button's text box is 296 units; a longer text goes on two lines
//           at the space nearest the middle (MpPanelPlan.cpp, KIND_BUTTONS), so each half must stay within kPillHalf letters (the real
//           fonts: tools/mp/panel_preview, share/settings-native-preview).
//   ROW     a value row of a list: the name, and the value right-aligned: together kRowChars letters at most (checked in the pictures).
//   GUIDE   a guide-band button (Model::buttons, 20 units, formatRaw stops at 19): 19 letters at most.
//   DIALOG  the game's message window: 3 lines of 44, with the longest button and the longest move in the two arguments.
//   TOAST   one line, the 96-unit buffer.
// ---------------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr size_t kPillHalf = 22;   // letters of one line of a pill (a 304-unit text box at the button's font; 2 lines allowed)
constexpr size_t kRowChars = 56;   // name + value of a list row
constexpr size_t kGuideChars = 19;
const MpText kSetBand[] = {MP_T_SET_BAND, MP_T_SET_D_PICTURE_SMOOTH, MP_T_SET_D_PICTURE_DETAILED, MP_T_SET_D_PICTURE_SMOOTH_NEW, MP_T_SET_D_PICTURE_DETAILED_NEW, MP_T_SET_D_CAMERA, MP_T_SET_D_ZOOM, MP_T_SET_D_RSTICK_FREE, MP_T_SET_D_RSTICK_CAMERA, MP_T_SET_D_RSTICK_POINTER, MP_T_SET_D_CAM_SPEED,
    MP_T_SET_D_INVERT_ON, MP_T_SET_D_INVERT_OFF, MP_T_SET_D_INVERT_X, MP_T_SET_D_INVERT_Y, MP_T_SET_D_INVERT_BOTH, MP_T_SET_D_INVERT_CLASSIC_X, MP_T_SET_D_LANG_NAMED, MP_T_SET_D_HOLD, MP_T_SET_D_BUTTONS_NEXT, MP_T_SET_D_SESSION_MENU, MP_T_SET_D_REACH, MP_T_SET_D_RESET, MP_T_SET_D_SESSION, MP_T_SET_D_NOT_RACE,
    MP_T_SET_D_RESTART_NEXT, MP_T_SET_D_MOTION_ON, MP_T_SET_D_MOTION_OFF, MP_T_SET_D_TILT_CONSOLE, MP_T_SET_D_TILT_STICK, MP_T_SET_D_POINTER_STEADY, MP_T_SET_D_POINTER_QUICK,
    MP_T_SET_D_REAR_ON, MP_T_SET_D_REAR_OFF, MP_T_SET_D_TAPJUMP_ON, MP_T_SET_D_TAPJUMP_OFF, MP_T_SET_D_CONTROLS, MP_T_SET_D_MORE, MP_T_SET_D_TILT_POWER, MP_T_SET_D_BUTTONS,
    MP_T_SET_D_CAPTURE_TIMEOUT, MP_T_SET_TITLE_BTN_CONSOLE, MP_T_SET_TITLE_BTN_PAD, MP_T_SET_D_TEST_FILE, MP_T_SET_LEAVE_Q,
    MP_T_SET_MD_JUMP, MP_T_SET_MD_STAR_BITS, MP_T_SET_MD_SPIN, MP_T_SET_MD_SPIN2, MP_T_SET_MD_CROUCH, MP_T_SET_MD_CAMERA, MP_T_SET_MD_PAUSE, MP_T_SET_MD_AIM,
    MP_T_SET_MD_CAM_UP, MP_T_SET_MD_CAM_DOWN, MP_T_SET_MD_CAM_LEFT, MP_T_SET_MD_CAM_RIGHT, MP_T_SET_D_ASSIST_ON, MP_T_SET_D_ASSIST_OFF, MP_T_SET_D_RESTART_ASK, MP_T_SET_RESTART_WAIT, MP_T_SET_RESTART_STUCK, MP_T_SET_RESTART_STUCK_LATER};
const MpText kSetToasts[] = {MP_T_SET_TOAST_SAVED, MP_T_SET_TOAST_SAVED_LATER, MP_T_SET_TOAST_RECOVERED, MP_T_SET_TOAST_UNREADABLE, MP_T_SET_TOAST_HOLD};
// names a pill (or a row) can carry on its own
const MpText kSetLabels[] = {MP_T_SET_ENTRY, MP_T_SET_L_PICTURE, MP_T_SET_L_CAMERA, MP_T_SET_L_ZOOM, MP_T_SET_L_RSTICK, MP_T_SET_L_CAM_SPEED, MP_T_SET_L_INVERT, MP_T_SET_L_LANGUAGE, MP_T_SET_L_CONTROLS,
    MP_T_SET_L_MORE, MP_T_SET_L_MOTION, MP_T_SET_L_REACH, MP_T_SET_L_RESET, MP_T_SET_L_TILT, MP_T_SET_L_TILT_POWER, MP_T_SET_L_POINTER, MP_T_SET_L_REAR, MP_T_SET_L_TAPJUMP,
    MP_T_SET_L_BUTTONS, MP_T_SET_SAVE, MP_T_SET_SAVE_RESTART, MP_T_SET_DONT_SAVE, MP_T_SET_KEEP_EDITING, MP_T_SET_L_ASSIST, MP_T_SET_START_NOW, MP_T_SET_LATER};
const MpText kSetValues[] = {MP_T_SET_V_SMOOTH, MP_T_SET_V_DETAILED, MP_T_SET_V_SYSTEM_LANG, MP_T_SET_V_CONSOLE, MP_T_SET_V_LEFT_STICK, MP_T_SET_V_FREE_CAM, MP_T_SET_V_CLASSIC_CAM, MP_T_SET_V_CAMERA, MP_T_SET_V_POINTER,
    MP_T_SET_V_INVERT_X, MP_T_SET_V_INVERT_Y, MP_T_SET_V_INVERT_BOTH, MP_T_SET_V_NORMAL, MP_T_SET_V_NONE, MP_T_SET_V_STEADY, MP_T_SET_V_QUICK, MP_T_ON, MP_T_OFF, MP_T_SET_LANG_ENGLISH, MP_T_SET_LANG_FRENCH, MP_T_SET_LANG_SPANISH,
    MP_T_SET_LANG_GERMAN, MP_T_SET_LANG_ITALIAN};
const MpText kSetGuides[] = {MP_T_SET_SAVE, MP_T_SET_DONT_SAVE, MP_T_SET_KEEP_EDITING, MP_T_SET_RESET};
struct Pair { MpText label, value; };
// the pills that carry a value ("Label: Value"): the Settings screen's Picture pill
const Pair kSetPills[] = {{MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH}, {MP_T_SET_L_PICTURE, MP_T_SET_V_DETAILED}};
// the value rows (a list row: the name, the value to its right): Camera, Controls, More
const Pair kSetRows[] = {{MP_T_SET_L_ZOOM, MP_T_NONE}, {MP_T_SET_L_RSTICK, MP_T_SET_V_FREE_CAM}, {MP_T_SET_L_RSTICK, MP_T_SET_V_CLASSIC_CAM}, {MP_T_SET_L_RSTICK, MP_T_SET_V_CAMERA}, {MP_T_SET_L_RSTICK, MP_T_SET_V_POINTER}, {MP_T_SET_L_CAM_SPEED, MP_T_NONE}, {MP_T_SET_L_INVERT, MP_T_SET_V_INVERT_X}, {MP_T_SET_L_INVERT, MP_T_SET_V_INVERT_Y}, {MP_T_SET_L_INVERT, MP_T_SET_V_INVERT_BOTH}, {MP_T_SET_L_INVERT, MP_T_ON},
    {MP_T_SET_L_TILT, MP_T_SET_V_CONSOLE}, {MP_T_SET_L_TILT, MP_T_SET_V_LEFT_STICK}, {MP_T_SET_L_TILT_POWER, MP_T_SET_V_NORMAL}, {MP_T_SET_L_POINTER, MP_T_SET_V_STEADY},
    {MP_T_SET_L_POINTER, MP_T_SET_V_QUICK}, {MP_T_SET_L_REAR, MP_T_ON}, {MP_T_SET_L_TAPJUMP, MP_T_OFF}, {MP_T_SET_L_LANGUAGE, MP_T_SET_V_SYSTEM_LANG},
    {MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_ITALIAN}, {MP_T_SET_L_MOTION, MP_T_ON}, {MP_T_SET_L_REACH, MP_T_ON}, {MP_T_SET_L_ASSIST, MP_T_OFF}, {MP_T_SET_L_ASSIST, MP_T_ON}};
const MpText kSetMoves[] = {MP_T_SET_M_JUMP, MP_T_SET_M_STAR_BITS, MP_T_SET_M_SPIN, MP_T_SET_M_SPIN2, MP_T_SET_M_CROUCH, MP_T_SET_M_CAMERA, MP_T_SET_M_PAUSE, MP_T_SET_M_AIM,
    MP_T_SET_M_CAM_UP, MP_T_SET_M_CAM_DOWN, MP_T_SET_M_CAM_LEFT, MP_T_SET_M_CAM_RIGHT};
const MpText kSetButtons[] = {MP_T_SET_B_R, MP_T_SET_B_L2, MP_T_SET_B_R2, MP_T_SET_B_L3, MP_T_SET_B_R3, MP_T_SET_B_UP, MP_T_SET_B_DOWN, MP_T_SET_B_LEFT, MP_T_SET_B_RIGHT,
    MP_T_SET_B_CROSS, MP_T_SET_B_CIRCLE, MP_T_SET_B_SQUARE, MP_T_SET_B_TRIANGLE, MP_T_SET_B_L, MP_T_SET_B_SELECT, MP_T_SET_B_START};
const MpText kSetDialogs[] = {MP_T_SET_DLG_RESTART_FAIL, MP_T_SET_DLG_CONFLICT, MP_T_SET_DLG_LOST, MP_T_SET_DLG_UNAVAILABLE, MP_T_SET_DLG_RESET, MP_T_SET_DLG_RESET_ALL, MP_T_SET_DLG_RESTART_PAUSE};
std::u16string SetText(MpText id, const char* a = nullptr, const char* b = nullptr) {
    MpMsgText::Args args;
    args.str = a, args.str2 = b;
    char16_t buf[160];
    MpMsgText::format(buf, 160, id, args);
    return buf;
}
std::string Ascii16(const std::u16string& t) {   // (UTF-8 for the log)
    std::string o;
    for (char16_t c : t) {
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) o += static_cast<char>(0xC0 | (c >> 6)), o += static_cast<char>(0x80 | (c & 0x3F));
        else o += static_cast<char>(0xE0 | (c >> 12)), o += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), o += static_cast<char>(0x80 | (c & 0x3F));
    }
    return o;
}
// the pill's text as the composer sets it (label, ": ", value), cut in two at the space nearest the middle when it is long
size_t PillWorstHalf(const std::u16string& whole) {
    if (whole.size() <= kPillHalf) return whole.size();
    int best = -1;
    const int len = static_cast<int>(whole.size());
    for (int k = 1; k < len - 1; k++)
        if (whole[k] == u' ' && (best < 0 || (k > len / 2 ? k - len / 2 : len / 2 - k) < (best > len / 2 ? best - len / 2 : len / 2 - best))) best = k;
    if (best < 0) return whole.size();
    return std::max<size_t>(best, whole.size() - best - 1);
}
}  // namespace

void testSettingsBoxes() {
    using namespace MpMsgText;
    int measured = 0;
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        for (MpText id : kSetBand) {
            const std::u16string t = SetText(id);
            const bool ok = !t.empty() && t.find(u'\n') == std::u16string::npos && t.size() <= 66 && WrapLines(t, 34) <= 2;   // (two lines of 34 letters; no line over 44)
            if (!ok) fprintf(stderr, "  settings: band id %d lang %d: %zu characters, %d lines: %s\n", static_cast<int>(id), lang, t.size(), WrapLines(t, 34), Ascii16(t).c_str());
            CHECK(ok);
            ++measured;
        }
        // the two bands with an argument: the capture line with the longest move, the language line with the longest language name
        {
            std::string longestMove;
            for (MpText id : kSetMoves) { const std::string m = Ascii16(SetText(id)); if (m.size() > longestMove.size()) longestMove = m; }
            for (const std::u16string& t : {SetText(MP_T_SET_D_CAPTURE, longestMove.c_str()), SetText(MP_T_SET_D_LANG_OWN, "Fran\xc3\xa7" "ais"), SetText(MP_T_SET_D_LANG_OWN, "Italiano")}) {
                const bool ok = !t.empty() && t.size() <= 66 && WrapLines(t, 34) <= 2;
                if (!ok) fprintf(stderr, "  settings: band with an argument, lang %d: %zu characters, %d lines: %s\n", lang, t.size(), WrapLines(t, 34), Ascii16(t).c_str());
                CHECK(ok);
                ++measured;
            }
        }
        for (MpText id : kSetToasts) {
            const std::u16string t = SetText(id);
            const bool ok = !t.empty() && t.size() <= 75 && t.find(u'\n') == std::u16string::npos;
            if (!ok) fprintf(stderr, "  settings: toast id %d lang %d: %zu characters: %s\n", static_cast<int>(id), lang, t.size(), Ascii16(t).c_str());
            CHECK(ok);
            ++measured;
        }
        for (MpText id : kSetLabels) {
            const std::u16string t = SetText(id);
            const bool pill = id == MP_T_SET_ENTRY || id == MP_T_SET_L_PICTURE || id == MP_T_SET_L_CAMERA || id == MP_T_SET_L_CONTROLS || id == MP_T_SET_L_MORE || id == MP_T_SET_L_BUTTONS ||
                           id == MP_T_SET_SAVE || id == MP_T_SET_SAVE_RESTART || id == MP_T_SET_DONT_SAVE || id == MP_T_SET_KEEP_EDITING || id == MP_T_SET_START_NOW || id == MP_T_SET_LATER;   // (the rest are names of list rows: 39)
            const bool ok = !t.empty() && t.size() <= 39 && t.find(u'\n') == std::u16string::npos && (!pill || PillWorstHalf(t) <= kPillHalf);
            if (!ok) fprintf(stderr, "  settings: label id %d lang %d too long: %s\n", static_cast<int>(id), lang, Ascii16(t).c_str());
            CHECK(ok);
            ++measured;
        }
        for (MpText id : kSetGuides) {
            const std::u16string t = SetText(id);
            const bool ok = !t.empty() && t.size() <= kGuideChars;
            if (!ok) fprintf(stderr, "  settings: guide word id %d lang %d: %zu characters (19 at most): %s\n", static_cast<int>(id), lang, t.size(), Ascii16(t).c_str());
            CHECK(ok);
            ++measured;
        }
        for (MpText id : kSetValues) {
            const std::u16string t = SetText(id);
            const bool ok = !t.empty() && t.size() <= 27 && t.find(u'\n') == std::u16string::npos;
            if (!ok) fprintf(stderr, "  settings: value id %d lang %d too long: %s\n", static_cast<int>(id), lang, Ascii16(t).c_str());
            CHECK(ok);
            ++measured;
        }
        for (const Pair& p : kSetPills) {
            const std::u16string whole = SetText(p.label) + u": " + SetText(p.value);
            const bool ok = whole.size() <= 39 && PillWorstHalf(whole) <= kPillHalf;
            if (!ok) fprintf(stderr, "  settings: pill lang %d does not fit: %s (half %zu)\n", lang, Ascii16(whole).c_str(), PillWorstHalf(whole));
            CHECK(ok);
            ++measured;
        }
        for (const Pair& p : kSetRows) {   // (the value may be a number: "10", "2.0", "5 (Normal)": 12 letters is the worst)
            const std::u16string name = SetText(p.label);
            const std::u16string value = p.value == MP_T_NONE ? std::u16string(u"2.0") : SetText(p.value);
            const std::u16string worst = p.label == MP_T_SET_L_TILT_POWER ? u"10 (" + value + u")" : value;
            const bool ok = name.size() <= 39 && worst.size() <= 27 && name.size() + worst.size() <= kRowChars;
            if (!ok) fprintf(stderr, "  settings: row lang %d does not fit: %s | %s\n", lang, Ascii16(name).c_str(), Ascii16(worst).c_str());
            CHECK(ok);
            ++measured;
        }
        size_t worstMove = 0, worstButton = 0;
        std::string moveName, buttonName;
        for (MpText id : kSetMoves) {
            const std::u16string t = SetText(id);
            CHECK(!t.empty() && t.size() <= 39);
            if (t.size() > worstMove) worstMove = t.size(), moveName = Ascii16(t);
            ++measured;
        }
        for (MpText id : kSetButtons) {
            const std::u16string t = SetText(id);
            CHECK(!t.empty() && t.size() <= 27);
            if (t.size() > worstButton) worstButton = t.size(), buttonName = Ascii16(t);
            ++measured;
        }
        // a move row: its name and the button beside it
        CHECK(worstMove + worstButton <= kRowChars + 8);
        const std::string bArg = buttonName, mArg = moveName;
        for (MpText id : kSetDialogs) {
            const std::u16string t = SetText(id, bArg.c_str(), mArg.c_str());
            const bool ok = !t.empty() && Widest(t) <= 44 && WrapLines(t, 44) <= 3;
            if (!ok) fprintf(stderr, "  settings: dialog id %d lang %d: widest %zu, %d lines: %s\n", static_cast<int>(id), lang, Widest(t), WrapLines(t, 44), Ascii16(t).c_str());
            CHECK(ok);
            ++measured;
        }
    }
    setLanguage(LANG_EN);
    fprintf(stderr, "  settings texts in their boxes: %d measures\n", measured);
}
