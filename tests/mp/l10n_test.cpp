// Galaxy 3.0 multiplayer: German / Italian texts (decomp/src/Game/MP/MpMessagesExtra.cpp) against the English table:
// every id has a translation, the markup matches the English row exactly, the lines fit the windows, the characters are in the game's font.
// Build: g++ -std=c++20 -fshort-wchar -Idecomp/include -Iplatform/include tests/mp/l10n_test.cpp decomp/src/Game/MP/MpMessages.cpp decomp/src/Game/MP/MpMessagesExtra.cpp
#include "Game/MP/MpMessages.hpp"

#include <stdio.h>
#include <string>

static int sFail = 0, sWarn = 0;
#define CHECK(c) do { if (!(c)) { ++sFail; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string U8(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) {
        if (c < 0x80) o += (c < 0x20 && c != '\n') ? '?' : static_cast<char>(c);
        else if (c < 0x800) o += static_cast<char>(0xC0 | (c >> 6)), o += static_cast<char>(0x80 | (c & 0x3F));
        else o += "?";
    }
    return o;
}

// Markup (\x01-\x04, pictures U+E000..) in order of appearance.
static std::u16string Markup(const char16_t* t) {
    std::u16string m;
    for (; *t; ++t)
        if ((*t >= 1 && *t <= 4) || MpMsgText::isPicChar(*t)) m += *t;
    return m;
}

// The characters the game's message font (MessageFont26) has and the two languages use. No capital sharp s (not in the font).
static bool Allowed(char16_t c, int lang) {
    if (c == u'\n' || (c >= 0x20 && c <= 0x7E) || (c >= 1 && c <= 4) || MpMsgText::isPicChar(c)) return true;
    if (lang == MpMsgText::LANG_DE) return std::u16string(u"ÄÖÜäöüß").find(c) != std::u16string::npos;
    if (lang == MpMsgText::LANG_IT) return std::u16string(u"ÀÈÉÌÒÙàèéìòù°").find(c) != std::u16string::npos;
    return false;
}

// Widest line with names counted as 8 and numbers as 2, and the number of lines.
static void Measure(const char16_t* t, int& lines, int& longest) {
    int line = 0;
    lines = 1, longest = 0;
    for (; *t; ++t) {
        if (*t == u'\n') { ++lines; line = 0; continue; }
        line += (*t == 1 || *t == 2 || *t == 4) ? 8 : (*t == 3 ? 2 : 1);
        if (line > longest) longest = line;
    }
}

static bool IsValueId(int id) {
    switch (id) {
    case MP_T_OFF: case MP_T_ON: case MP_T_CODE_ONLY: case MP_T_VIS_FRIENDS: case MP_T_ANYONE: case MP_T_FALLS_BUBBLES: case MP_T_FALLS_LIVES:
    case MP_T_SAME_WIFI: case MP_T_NEARBY:   // flow: values (Room Settings is gone; kept for the old rows still in the table)
        return true;
    default:
        return false;
    }
}

int main() {
    using namespace MpMsgText;
    int checked = 0;
    for (int lang : {LANG_DE, LANG_IT}) {
        const char* ln = lang == LANG_DE ? "de" : "it";
        for (int id = MP_T_JOINED; id < MP_T_COUNT; id++) {
            setLanguage(LANG_EN);
            const char16_t* en = raw(static_cast<MpText>(id));
            if (!en[0]) continue;   // (a gap in the toast numbers, or a marker)
            ++checked;
            const char16_t* t = extraText(id, lang);
            if (!t || !t[0]) { fprintf(stderr, "  %s id %d: missing (English: %s)\n", ln, id, U8(en).c_str()); CHECK(false); continue; }
            const std::u16string s(t);
            const std::string shown = U8(s);
            // raw() serves it
            setLanguage(lang);
            CHECK(std::u16string(raw(static_cast<MpText>(id))) == s);
            // markup identical to English
            if (Markup(en) != Markup(t)) { fprintf(stderr, "  %s id %d: markup differs: %s\n", ln, id, shown.c_str()); CHECK(false); }
            // characters
            for (char16_t c : s)
                if (!Allowed(c, lang)) { fprintf(stderr, "  %s id %d: character U+%04X not allowed: %s\n", ln, id, c, shown.c_str()); CHECK(false); break; }
            CHECK(s.back() != u' ' || id == MP_T_CHAT_FROM || id == MP_T_YOUR_CODE || id == MP_T_LAST_PLAYED   /* (prefixes: a date or a list follows) */);   // (flow: a prefix)
            CHECK(s.find(u"  ") == std::u16string::npos);
            // limits
            int lines, longest;
            Measure(t, lines, longest);
            const char* name = dialogName(static_cast<MpText>(id));
            if (name) {   // window: 3 lines, 44 units hard, 40 aimed
                if (longest > 44 || lines > 3) { fprintf(stderr, "  %s %s: %d lines, longest %d\n", ln, name, lines, longest); CHECK(false); }
                else if (longest > 40) { fprintf(stderr, "  note: %s %s: longest line %d (aim 40)\n", ln, name, longest); ++sWarn; }
            } else if (id <= MP_T_TOAST_LAST) {   // toast: the 96-unit buffer with 15-character names
                Args a;
                a.str = "Toadsworth12345";
                a.str2 = "Luigi64Luigi64";
                a.num = 99;
                a.turns = 5025;
                char16_t out[96];
                const int n = format(out, 96, static_cast<MpText>(id), a);
                if (n >= 95) { fprintf(stderr, "  %s toast %d: %d units\n", ln, id, n); CHECK(false); }
            } else {
                const int total = longest;
                int enLines, enLongest;
                Measure(en, enLines, enLongest);
                if (IsValueId(id)) { if (total > 27) { fprintf(stderr, "  %s value %d: %d units (28 buffer): %s\n", ln, id, total, shown.c_str()); CHECK(false); } }
                else if (total > 79) { fprintf(stderr, "  %s text %d: %d units (80 buffer): %s\n", ln, id, total, shown.c_str()); CHECK(false); }
                else if (enLongest <= 24 && total > 34) { fprintf(stderr, "  note: %s label %d: %d units vs English %d: %s\n", ln, id, total, enLongest, shown.c_str()); ++sWarn; }
            }
        }
    }
    // the Yes / No of the confirm windows
    for (int lang : {LANG_DE, LANG_IT}) {
        setLanguage(lang);
        const char16_t* y = findDialog("MP_LEAVE_HOST_Yes");
        const char16_t* n = findDialog("MP_LEAVE_HOST_No");
        CHECK(y && y[0] && n && n[0]);
        CHECK(lang != LANG_DE || (std::u16string(y) == u"Ja" && std::u16string(n) == u"Nein"));
    }
    // language names of the game's prefixes; unknown ids fall back to English; Dutch has no texts: English
    CHECK(languageIndex("EuGerman") == LANG_DE && languageIndex("EuItalian") == LANG_IT && languageIndex("EuDutch") == LANG_EN);
    CHECK(extraText(0, LANG_DE) == nullptr && extraText(MP_T_COUNT, LANG_IT) == nullptr && extraText(-1, LANG_IT) == nullptr);
    CHECK(extraText(MP_T_JOINED, LANG_FR) == nullptr);
    setLanguage(LANG_DE);
    CHECK(std::u16string(raw(MP_T_BACK)) == u"Zurück");
    setLanguage(99);
    CHECK(language() == LANG_EN);
    printf("l10n: %d ids x 2 languages, %d notes, %d failures\n", checked, sWarn, sFail);
    return sFail ? 1 : 0;
}
