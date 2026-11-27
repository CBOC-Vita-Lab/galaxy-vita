// menus4 item 3: the words in the game's voice, five languages (the player's words: a Friend Code, not an "online account" or a sign-in;
// no possessive that breaks in other languages; one idea per line; every line fits)
#include "common.hpp"

namespace {
std::string Lower8(const char16_t* t) {
    std::string o;
    for (; *t; ++t) o += *t < 128 ? static_cast<char>(*t >= 'A' && *t <= 'Z' ? *t + 32 : *t) : '?';
    return o;
}
bool Has(const char16_t* t, const char* part) { return Lower8(t).find(part) != std::string::npos; }
}  // namespace

void testWordsGameVoice() {
    using namespace MpMsgText;
    static const MpText kKs[] = {MP_T_KS_KEEP_H_MAKE, MP_T_KS_KEEP_H_SHARE, MP_T_KS_KEEP_MADE, MP_T_KS_MADE_SHARE, MP_T_KS_BK_ACCOUNT, MP_T_KS_ROW_ACCOUNT,
                                 MP_T_KS_H_ACCOUNT, MP_T_KS_D_ACCOUNT_DONE, MP_T_KS_D_ACCOUNT_SAME, MP_T_KS_HOWTO_SAFE, MP_T_KS_ROW_SET_FLOW, MP_T_KS_ROW_SET_COOP};
    static const char* const kBannedTerms[] = {"online account", "account online", "compte en ligne", "cuenta en l", "online-konto", "sign-in", "sign in", "sign-up",
                                               "connexion en ligne", "inicio de sesi", "anmeldung", "accesso online", "remembered", "team file names"};
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        // the account is the Friend Code to the player; nothing about signing in or up
        for (MpText id : kKs)
            for (const char* b : kBannedTerms) {
                if (Has(raw(id), b)) fprintf(stderr, "  lang %d id %d says '%s'\n", lang, id, b);
                CHECK(!Has(raw(id), b));
            }
        // "Look for Friends Nearby": friends, not consoles
        CHECK(!Has(raw(MP_T_SCAN_NEARBY), "console") && !Has(raw(MP_T_SCAN_NEARBY), "konsole") && !Has(raw(MP_T_SCAN_NEARBY), "consolas"));
        // one idea in the Messages hint: one line, no controls named
        CHECK(std::u16string(raw(MP_T_DESC_MESSAGES)).find(u'\n') == std::u16string::npos && std::u16string(raw(MP_T_DESC_MESSAGES)).size() <= 44);
        // the head start is a plain label with its number, no "s late" sentence
        CHECK(Has(raw(MP_T_HEAD_START), "\x03") || std::u16string(raw(MP_T_HEAD_START)).find(u'\x03') != std::u16string::npos);
        // lines that must fit the bottom message / a window
        for (MpText id : {MP_T_DESC_BLOCKED, MP_T_CT_WAIT_FAR, MP_T_SVC_OFFLINE, MP_T_KS_CANT_READ_HINT, MP_T_KS_SP_CANT_READ_HINT, MP_T_KS_SLOTS_CANT_READ, MP_T_KS_KEEP_H_BACK,
                          MP_T_KS_KEEP_H_MAKE, MP_T_KS_KEEP_H_SHARE, MP_T_KS_H_ACCOUNT}) {
            const std::u16string t = raw(id);
            if (t.size() > 44) fprintf(stderr, "  lang %d id %d: %zu characters\n", lang, id, t.size());
            CHECK(!t.empty() && t.size() <= 44);
        }
        // the dialogs and cards: 3 lines of 44 at most
        for (MpText id : {MP_T_DLG_POWER_SAVE, MP_T_DLG_DATA_MISMATCH, MP_T_DLG_CT_OFFER, MP_T_KS_HOWTO_SAFE, MP_T_KS_KEEP_MADE, MP_T_KS_MADE_SHARE, MP_T_KS_BK_ACCOUNT}) {
            int lines = 1, run = 0, worst = 0;
            for (const char16_t* p = raw(id); *p; ++p) {
                if (*p == u'\n') { ++lines; run = 0; continue; }
                if (++run > worst) worst = run;
            }
            if (worst > 44 || lines > 3) fprintf(stderr, "  lang %d id %d: %d lines, %d wide\n", lang, id, lines, worst);
            CHECK(worst <= 44 && lines <= 3);
        }
        // the Power Save tip: no jargon sentence of "Power Save Mode" (the setting is named once, the way the Settings app shows it)
        CHECK(!Has(raw(MP_T_DLG_POWER_SAVE), "save mode") && !Has(raw(MP_T_DLG_POWER_SAVE), "mode économie") && !Has(raw(MP_T_DLG_POWER_SAVE), "modo de ahorro"));
        // the rules toast puts the name in brackets: no "'s", "de/di/von + name" that breaks for a name like Ana, Ros or Ines
        for (const char* host : {"Ana", "Ros", "Ines"}) {
            Rules r{host, 0, 0, true};
            char16_t t[96];
            formatRulesToast(t, 96, r, false);
            const std::string a = Lower8(t);
            CHECK(a.find("'s") == std::string::npos && a.find(" de ") == std::string::npos && a.find(" di ") == std::string::npos && a.find(" von ") == std::string::npos);
            CHECK(a.find(std::string("(") + Lower8(reinterpret_cast<const char16_t*>(std::u16string(host, host + strlen(host)).c_str())) + ")") != std::string::npos);
        }
    }
    setLanguage(LANG_EN);
    // the ids that carried no words are gone (their numbers stay free)
    CHECK(std::u16string(raw(static_cast<MpText>(6))).empty() && std::u16string(raw(static_cast<MpText>(11))).empty() && std::u16string(raw(static_cast<MpText>(15))).empty());
}

// Native Settings (v3, 15 Nov): the words in the game's voice. One plain sentence each, ending like a sentence; labels are short names with no colon or
// full stop (the menu adds the value); the three leave pills are three different words; the windows ask or tell.
void testSettingsVoice() {
    using namespace MpMsgText;
    static const MpText kSentences[] = {MP_T_SET_D_PICTURE_SMOOTH, MP_T_SET_D_PICTURE_DETAILED, MP_T_SET_D_PICTURE_SMOOTH_NEW, MP_T_SET_D_PICTURE_DETAILED_NEW, MP_T_SET_D_CAMERA, MP_T_SET_D_ZOOM, MP_T_SET_D_RSTICK_CAMERA, MP_T_SET_D_RSTICK_POINTER,
        MP_T_SET_D_INVERT_ON, MP_T_SET_D_INVERT_OFF, MP_T_SET_D_LANG_OWN, MP_T_SET_D_LANG_NAMED, MP_T_SET_D_REACH, MP_T_SET_D_RESET, MP_T_SET_D_SESSION, MP_T_SET_D_NOT_RACE,
        MP_T_SET_D_RESTART_NEXT, MP_T_SET_D_HOLD, MP_T_SET_D_BUTTONS_NEXT, MP_T_SET_D_SESSION_MENU, MP_T_SET_D_MOTION_ON, MP_T_SET_D_MOTION_OFF, MP_T_SET_D_TILT_CONSOLE, MP_T_SET_D_TILT_STICK, MP_T_SET_D_POINTER_STEADY, MP_T_SET_D_POINTER_QUICK,
        MP_T_SET_D_REAR_ON, MP_T_SET_D_REAR_OFF, MP_T_SET_D_TAPJUMP_ON, MP_T_SET_D_TAPJUMP_OFF, MP_T_SET_D_CONTROLS, MP_T_SET_D_MORE, MP_T_SET_D_TILT_POWER, MP_T_SET_D_BUTTONS,
        MP_T_SET_D_CAPTURE, MP_T_SET_D_CAPTURE_TIMEOUT, MP_T_SET_D_TEST_FILE, MP_T_SET_TOAST_SAVED, MP_T_SET_TOAST_SAVED_LATER, MP_T_SET_TOAST_RECOVERED, MP_T_SET_TOAST_UNREADABLE,
        MP_T_SET_TOAST_HOLD, MP_T_SET_MD_JUMP, MP_T_SET_MD_STAR_BITS, MP_T_SET_MD_SPIN, MP_T_SET_MD_SPIN2, MP_T_SET_MD_CROUCH, MP_T_SET_MD_CAMERA, MP_T_SET_MD_PAUSE,
        MP_T_SET_MD_AIM, MP_T_SET_MD_CAM_UP, MP_T_SET_MD_CAM_DOWN, MP_T_SET_MD_CAM_LEFT, MP_T_SET_MD_CAM_RIGHT, MP_T_SET_D_ASSIST_ON, MP_T_SET_D_ASSIST_OFF, MP_T_SET_D_RESTART_ASK, MP_T_SET_RESTART_WAIT, MP_T_SET_RESTART_STUCK, MP_T_SET_RESTART_STUCK_LATER};
    static const MpText kNames[] = {MP_T_SET_ENTRY, MP_T_SET_L_PICTURE, MP_T_SET_L_CAMERA, MP_T_SET_L_ZOOM, MP_T_SET_L_RSTICK, MP_T_SET_L_INVERT, MP_T_SET_L_LANGUAGE, MP_T_SET_L_CONTROLS,
        MP_T_SET_L_MORE, MP_T_SET_L_MOTION, MP_T_SET_L_REACH, MP_T_SET_L_RESET, MP_T_SET_L_TILT, MP_T_SET_L_TILT_POWER, MP_T_SET_L_POINTER, MP_T_SET_L_REAR, MP_T_SET_L_TAPJUMP,
        MP_T_SET_L_BUTTONS, MP_T_SET_V_SMOOTH, MP_T_SET_V_DETAILED, MP_T_SET_V_SYSTEM_LANG, MP_T_SET_V_STEADY, MP_T_SET_V_QUICK, MP_T_SET_V_NONE, MP_T_SET_V_NORMAL, MP_T_SET_SAVE,
        MP_T_SET_SAVE_RESTART, MP_T_SET_DONT_SAVE, MP_T_SET_KEEP_EDITING, MP_T_SET_START_NOW, MP_T_SET_LATER, MP_T_SET_RESET, MP_T_SET_M_JUMP, MP_T_SET_M_STAR_BITS, MP_T_SET_M_SPIN, MP_T_SET_M_CROUCH,
        MP_T_SET_TITLE_BTN_CONSOLE, MP_T_SET_TITLE_BTN_PAD};
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        for (MpText id : kSentences) {
            const std::u16string t = raw(id);
            CHECK(!t.empty() && t.find(u'\n') == std::u16string::npos);
            CHECK(t.back() == u'.' || t.back() == u'?' || t.back() == u'!');
            CHECK(!Has(t.c_str(), "wii") && !Has(t.c_str(), "nintendo") && !Has(t.c_str(), "frames"));   // (no "frames a second": the owner's rule)
        }
        for (MpText id : kNames) {
            const std::u16string t = raw(id);
            CHECK(!t.empty() && t.find(u':') == std::u16string::npos && t.back() != u'.' && t.find(u'\n') == std::u16string::npos);
        }
        // the leave question's pills are four different words; the three that fit a guide button do
        CHECK(std::u16string(raw(MP_T_SET_SAVE)) != raw(MP_T_SET_DONT_SAVE) && std::u16string(raw(MP_T_SET_SAVE)) != raw(MP_T_SET_KEEP_EDITING) &&
              std::u16string(raw(MP_T_SET_DONT_SAVE)) != raw(MP_T_SET_KEEP_EDITING) && std::u16string(raw(MP_T_SET_SAVE)) != raw(MP_T_SET_SAVE_RESTART));
        // the conflict window names its two arguments, each on its own line, and asks a question
        const std::u16string c = raw(MP_T_SET_DLG_CONFLICT);
        CHECK(c.find(u'\x01') != std::u16string::npos && c.find(u'\x02') != std::u16string::npos && c.find(u'\x01') < c.find(u'\n') && c.find(u'\x02') > c.find(u'\n'));
        CHECK(c.find(u'?') != std::u16string::npos);
        for (MpText id : {MP_T_SET_DLG_RESET, MP_T_SET_DLG_RESET_ALL}) CHECK(std::u16string(raw(id)).back() == u'?');
        for (MpText id : {MP_T_SET_DLG_RESTART_FAIL, MP_T_SET_DLG_LOST, MP_T_SET_DLG_UNAVAILABLE}) CHECK(std::u16string(raw(id)).back() == u'.' || std::u16string(raw(id)).back() == u'!');
        for (MpText id : {MP_T_SET_DLG_RESTART_FAIL, MP_T_SET_DLG_CONFLICT, MP_T_SET_DLG_LOST, MP_T_SET_DLG_UNAVAILABLE, MP_T_SET_DLG_RESET, MP_T_SET_DLG_RESET_ALL})
            CHECK(dialogName(id) && !strncmp(dialogName(id), "MP_SET_", 7));
        // one Star Bits name per language: the move, the reach and the sentences about them use the same word
        static const char* const kStarBits[LANG_COUNT] = {"star bit", "morceaux d'", "fragmentos", "sternensplitter", "astroschegge"};
        for (MpText id : {MP_T_SET_M_STAR_BITS, MP_T_SET_L_REACH, MP_T_SET_MD_STAR_BITS, MP_T_SET_D_REACH}) CHECK(Has(raw(id), kStarBits[lang]));
        // the reset names the standard, not "as they were"; "Safe Saves", "Layout", "Game's Own" are gone
        CHECK(Has(raw(MP_T_SET_DLG_RESET), "standard") || Has(raw(MP_T_SET_DLG_RESET), "est?ndar"));
    }
    setLanguage(LANG_EN);
    // the English the owner chose, word for word
    CHECK(std::u16string(raw(MP_T_SET_LEAVE_Q)) == u"Save your changes?" && std::u16string(raw(MP_T_SET_TOAST_SAVED)) == u"Settings saved." &&
          std::u16string(raw(MP_T_SET_D_MOTION_ON)) == u"Less camera shake and fewer bright flashes." && std::u16string(raw(MP_T_SET_D_TEST_FILE)) == u"A test file on this console sets this one." &&
          std::u16string(raw(MP_T_SET_DONT_SAVE)) == u"Don't Save" && std::u16string(raw(MP_T_SET_KEEP_EDITING)) == u"Keep Editing" &&
          std::u16string(raw(MP_T_SET_V_SMOOTH)) == u"Smooth Motion" && std::u16string(raw(MP_T_SET_V_DETAILED)) == u"Full Detail" &&
          std::u16string(raw(MP_T_SET_V_SYSTEM_LANG)) == u"System Language" && std::u16string(raw(MP_T_SET_SAVE_RESTART)) == u"Save and Start Again" &&
          std::u16string(raw(MP_T_SET_TOAST_HOLD)) == u"Picture and language were set back to standard so the game could start." &&
          std::u16string(raw(MP_T_SET_D_BUTTONS_NEXT)) == u"New buttons start the next time the game starts." &&
          std::u16string(raw(MP_T_SET_D_CAPTURE_TIMEOUT)) == u"No button pressed. Choose one from the list." &&
          std::u16string(raw(MP_T_SET_DLG_RESET)) == u"Put back the standard buttons?" && std::u16string(raw(MP_T_SET_D_SESSION)) == u"Change this when you're not playing together." &&
          std::u16string(raw(MP_T_SET_D_NOT_RACE)) == u"Not during a race.");
    // the two lines with an argument carry it (the move to bind; the language in use)
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        CHECK(std::u16string(raw(MP_T_SET_D_CAPTURE)).find(u'\x01') != std::u16string::npos && std::u16string(raw(MP_T_SET_D_LANG_OWN)).find(u'\x01') != std::u16string::npos);
    }
    setLanguage(LANG_EN);
    // the Pointer sentences say what the stick does (the speed follows the push): not "one even speed"
    CHECK(!Has(raw(MP_T_SET_D_POINTER_STEADY), "even speed") && Has(raw(MP_T_SET_D_POINTER_STEADY), "push"));
    // the Star Bits move says it is also Back in menus
    CHECK(Has(raw(MP_T_SET_MD_STAR_BITS), "back"));
    fprintf(stderr, "settings voice: ok\n");
}

// Native Settings, the owner's hard rule (14 / 15 Nov): EVERY pill, EVERY row and EVERY value has its own plain description, shown in the band while it is
// selected, and it says what the CURRENT value means (so it changes with the value). Every move in the button list has one too. This table is the contract
// with the menu: a pill / row / value / move with no description in any of the five languages fails here.
void testSettingsDescriptions() {
    using namespace MpMsgText;
    struct D { MpText label, value, desc; };   // value MP_T_NONE: the row has no named value of its own (Controls, More, Camera, Change Buttons, Zoom, Tilt Strength, Star Bit Reach, Put Everything Back)
    static const D kPills[] = {
        {MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH, MP_T_SET_D_PICTURE_SMOOTH}, {MP_T_SET_L_PICTURE, MP_T_SET_V_DETAILED, MP_T_SET_D_PICTURE_DETAILED},
        {MP_T_SET_L_CAMERA, MP_T_NONE, MP_T_SET_D_CAMERA}, {MP_T_SET_L_CONTROLS, MP_T_NONE, MP_T_SET_D_CONTROLS}, {MP_T_SET_L_MORE, MP_T_NONE, MP_T_SET_D_MORE},
        {MP_T_SET_L_ZOOM, MP_T_NONE, MP_T_SET_D_ZOOM},
        {MP_T_SET_L_RSTICK, MP_T_SET_V_CAMERA, MP_T_SET_D_RSTICK_CAMERA}, {MP_T_SET_L_RSTICK, MP_T_SET_V_POINTER, MP_T_SET_D_RSTICK_POINTER},
        {MP_T_SET_L_INVERT, MP_T_ON, MP_T_SET_D_INVERT_ON}, {MP_T_SET_L_INVERT, MP_T_OFF, MP_T_SET_D_INVERT_OFF},
        {MP_T_SET_L_LANGUAGE, MP_T_SET_V_SYSTEM_LANG, MP_T_SET_D_LANG_OWN},
        {MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_ENGLISH, MP_T_SET_D_LANG_NAMED}, {MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_FRENCH, MP_T_SET_D_LANG_NAMED},
        {MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_SPANISH, MP_T_SET_D_LANG_NAMED}, {MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_GERMAN, MP_T_SET_D_LANG_NAMED},
        {MP_T_SET_L_LANGUAGE, MP_T_SET_LANG_ITALIAN, MP_T_SET_D_LANG_NAMED},
        {MP_T_SET_L_MOTION, MP_T_ON, MP_T_SET_D_MOTION_ON}, {MP_T_SET_L_MOTION, MP_T_OFF, MP_T_SET_D_MOTION_OFF},
        {MP_T_SET_L_REACH, MP_T_NONE, MP_T_SET_D_REACH}, {MP_T_SET_L_RESET, MP_T_NONE, MP_T_SET_D_RESET},
        {MP_T_SET_L_TILT, MP_T_SET_V_CONSOLE, MP_T_SET_D_TILT_CONSOLE}, {MP_T_SET_L_TILT, MP_T_SET_V_LEFT_STICK, MP_T_SET_D_TILT_STICK},
        {MP_T_SET_L_TILT_POWER, MP_T_SET_V_NORMAL, MP_T_SET_D_TILT_POWER},
        {MP_T_SET_L_POINTER, MP_T_SET_V_STEADY, MP_T_SET_D_POINTER_STEADY}, {MP_T_SET_L_POINTER, MP_T_SET_V_QUICK, MP_T_SET_D_POINTER_QUICK},
        {MP_T_SET_L_REAR, MP_T_ON, MP_T_SET_D_REAR_ON}, {MP_T_SET_L_REAR, MP_T_OFF, MP_T_SET_D_REAR_OFF},
        {MP_T_SET_L_TAPJUMP, MP_T_ON, MP_T_SET_D_TAPJUMP_ON}, {MP_T_SET_L_TAPJUMP, MP_T_OFF, MP_T_SET_D_TAPJUMP_OFF},
        {MP_T_SET_L_BUTTONS, MP_T_NONE, MP_T_SET_D_BUTTONS},
        // the grey-out and override lines belong to every row that can be greyed
        {MP_T_SET_L_PICTURE, MP_T_NONE, MP_T_SET_D_SESSION}, {MP_T_SET_L_REACH, MP_T_NONE, MP_T_SET_D_NOT_RACE}, {MP_T_SET_L_PICTURE, MP_T_NONE, MP_T_SET_D_TEST_FILE},
        {MP_T_SET_L_PICTURE, MP_T_NONE, MP_T_SET_D_RESTART_NEXT}, {MP_T_SET_L_PICTURE, MP_T_NONE, MP_T_SET_D_HOLD}, {MP_T_SET_L_LANGUAGE, MP_T_NONE, MP_T_SET_D_HOLD},
        {MP_T_SET_L_BUTTONS, MP_T_NONE, MP_T_SET_D_BUTTONS_NEXT}, {MP_T_SET_L_BUTTONS, MP_T_NONE, MP_T_SET_D_CAPTURE}, {MP_T_SET_L_BUTTONS, MP_T_NONE, MP_T_SET_D_CAPTURE_TIMEOUT},
        {MP_T_SET_ENTRY, MP_T_NONE, MP_T_SET_D_SESSION_MENU}};   // (the Settings pill of the session menu and of the entry screen)
    static const D kMoves[] = {
        {MP_T_SET_M_JUMP, MP_T_NONE, MP_T_SET_MD_JUMP}, {MP_T_SET_M_STAR_BITS, MP_T_NONE, MP_T_SET_MD_STAR_BITS}, {MP_T_SET_M_SPIN, MP_T_NONE, MP_T_SET_MD_SPIN},
        {MP_T_SET_M_SPIN2, MP_T_NONE, MP_T_SET_MD_SPIN2}, {MP_T_SET_M_CROUCH, MP_T_NONE, MP_T_SET_MD_CROUCH}, {MP_T_SET_M_CAMERA, MP_T_NONE, MP_T_SET_MD_CAMERA},
        {MP_T_SET_M_PAUSE, MP_T_NONE, MP_T_SET_MD_PAUSE}, {MP_T_SET_M_AIM, MP_T_NONE, MP_T_SET_MD_AIM}, {MP_T_SET_M_CAM_UP, MP_T_NONE, MP_T_SET_MD_CAM_UP},
        {MP_T_SET_M_CAM_DOWN, MP_T_NONE, MP_T_SET_MD_CAM_DOWN}, {MP_T_SET_M_CAM_LEFT, MP_T_NONE, MP_T_SET_MD_CAM_LEFT}, {MP_T_SET_M_CAM_RIGHT, MP_T_NONE, MP_T_SET_MD_CAM_RIGHT}};
    // coverage: every pill / row label, every value, every move name that the menu shows is in a table row
    static const MpText kLabels[] = {MP_T_SET_L_PICTURE, MP_T_SET_L_CAMERA, MP_T_SET_L_ZOOM, MP_T_SET_L_RSTICK, MP_T_SET_L_INVERT, MP_T_SET_L_LANGUAGE, MP_T_SET_L_CONTROLS,
                                     MP_T_SET_L_MORE, MP_T_SET_L_MOTION, MP_T_SET_L_REACH, MP_T_SET_L_RESET, MP_T_SET_L_TILT, MP_T_SET_L_TILT_POWER, MP_T_SET_L_POINTER,
                                     MP_T_SET_L_REAR, MP_T_SET_L_TAPJUMP, MP_T_SET_L_BUTTONS, MP_T_SET_ENTRY};
    static const MpText kValues[] = {MP_T_SET_V_SMOOTH, MP_T_SET_V_DETAILED, MP_T_SET_V_SYSTEM_LANG, MP_T_SET_V_CONSOLE, MP_T_SET_V_LEFT_STICK, MP_T_SET_V_CAMERA, MP_T_SET_V_POINTER,
                                     MP_T_SET_V_NORMAL, MP_T_SET_V_STEADY, MP_T_SET_V_QUICK, MP_T_SET_LANG_ENGLISH, MP_T_SET_LANG_FRENCH, MP_T_SET_LANG_SPANISH, MP_T_SET_LANG_GERMAN,
                                     MP_T_SET_LANG_ITALIAN};   // (On / Off are covered by the rows that carry them; "None" by the second Spin row)
    for (MpText l : kLabels) { bool f = false; for (const D& d : kPills) f |= d.label == l; CHECK(f); }
    for (MpText v : kValues) { bool f = false; for (const D& d : kPills) f |= d.value == v; CHECK(f); }
    for (int id = MP_T_SET_M_JUMP; id <= MP_T_SET_M_CAM_RIGHT; id++) { bool f = false; for (const D& d : kMoves) f |= d.label == id; CHECK(f); }
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        for (const D* tbl : {kPills, kMoves}) {
            const size_t n = tbl == kPills ? sizeof kPills / sizeof kPills[0] : sizeof kMoves / sizeof kMoves[0];
            for (size_t i = 0; i < n; i++) {
                const std::u16string t = raw(tbl[i].desc);
                if (t.empty()) fprintf(stderr, "  settings: no description for label id %d value id %d in language %d\n", tbl[i].label, tbl[i].value, lang);
                CHECK(!t.empty());
                CHECK(t != raw(tbl[i].label));   // (a description is not just the label again)
                CHECK(t.size() >= 16);           // (a sentence, not a word)
                // two values of one pill: two different descriptions (the text changes when the value changes)
                for (size_t k = 0; k < i; k++)
                    if (tbl[k].label == tbl[i].label && tbl[k].value != tbl[i].value && tbl[k].value != MP_T_NONE && tbl[i].value != MP_T_NONE && tbl[k].desc != tbl[i].desc)
                        CHECK(std::u16string(raw(tbl[k].desc)) != t);
            }
        }
    }
    setLanguage(LANG_EN);
    // the rows whose value changes the meaning have two distinct descriptions
    CHECK(MP_T_SET_D_PICTURE_SMOOTH != MP_T_SET_D_PICTURE_DETAILED && MP_T_SET_D_MOTION_ON != MP_T_SET_D_MOTION_OFF && MP_T_SET_D_INVERT_ON != MP_T_SET_D_INVERT_OFF &&
          MP_T_SET_D_TILT_CONSOLE != MP_T_SET_D_TILT_STICK && MP_T_SET_D_POINTER_STEADY != MP_T_SET_D_POINTER_QUICK && MP_T_SET_D_LANG_OWN != MP_T_SET_D_LANG_NAMED &&
          MP_T_SET_D_RSTICK_CAMERA != MP_T_SET_D_RSTICK_POINTER && MP_T_SET_D_REAR_ON != MP_T_SET_D_REAR_OFF && MP_T_SET_D_TAPJUMP_ON != MP_T_SET_D_TAPJUMP_OFF);
    // modeux32: Picture names both modes and its rate; the changed form says the restart
    CHECK(Has(raw(MP_T_SET_D_PICTURE_SMOOTH), "60") && Has(raw(MP_T_SET_D_PICTURE_DETAILED), "30") && Has(raw(MP_T_SET_D_PICTURE_SMOOTH_NEW), "60") && Has(raw(MP_T_SET_D_PICTURE_DETAILED_NEW), "30"));
    CHECK(MP_T_SET_D_PICTURE_SMOOTH_NEW != MP_T_SET_D_PICTURE_SMOOTH);
    fprintf(stderr, "settings descriptions: ok\n");
}
