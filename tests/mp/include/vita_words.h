// Galaxy 3.0 multiplayer: "nothing on the Vita names a Wii control" (tests/mp/vitabtn_test.cpp, tests/mp/ui_menu_test.cpp).
// The matcher is deliberately narrow: a Wii control is named when a text
//  - contains a Wii word (Nunchuk, Wii, Wiimote, Remote, Wii Remote),
//  - says "shake" / "shaking" / "point at" / "pointing at" / "pointer at" ("point" alone is fine: "show your pointer"),
//  - names the A, B, C or Z button, or "1" / "2" / "+" / "-" as a button: a single capital letter or digit or sign that is next to
//    "press / hold / button" (in any of the five languages), or alone in brackets, "(A)", "[B]" (not a count: "(2)"), or followed by the word "button".
// Plain uses stay clear: Spanish "A <name>", "1st", "2 players", "Star Race", "L or R" (the Vita has L and R), "Hold <glyph> + a direction".
#pragma once

#include <string.h>

#include <string>
#include <vector>

#include "Game/MP/MpControls.hpp"
#include "Game/MP/MpMessages.hpp"

namespace vitawords {

inline std::string Ascii(const std::u16string& s) {
    std::string o;
    for (char16_t c : s) o += c < 0x80 ? (c < 0x20 ? ' ' : static_cast<char>(c)) : '~';   // (a non-ASCII letter stays a letter: '~' below)
    return o;
}

inline std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

inline bool IsWordChar(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '~'; }

struct Tok { std::string text; size_t at; };

inline std::vector<Tok> Tokens(const std::string& s) {
    std::vector<Tok> t;
    size_t i = 0;
    while (i < s.size()) {
        if (IsWordChar(static_cast<unsigned char>(s[i]))) {
            size_t j = i;
            while (j < s.size() && IsWordChar(static_cast<unsigned char>(s[j]))) ++j;
            t.push_back({s.substr(i, j - i), i});
            i = j;
        } else if (s[i] == '+' || s[i] == '-') {
            t.push_back({std::string(1, s[i]), i});
            ++i;
        } else ++i;
    }
    return t;
}

// The words that introduce a button in the five languages (lower case, accents dropped to '~').
inline bool Introduces(const std::string& w) {
    static const char* const k[] = {"press", "hold", "button", "buttons", "pressing", "bouton", "appuyez", "maintenez", "gardez", "boton", "pulsa", "presiona",
                                    "manten", "taste", "druecke", "dr~cke", "halte", "tasto", "premi", "tieni", "premuto", "mant~n", "bot~n", "pr~ssez"};
    for (const char* x : k) if (w == x) return true;
    return false;
}

// Why the text names a Wii control ("" when it does not). `text` is the message with its markup (\x01.. and pictures) left in.
inline std::string WhyWii(const std::u16string& raw) {
    std::u16string clean;
    for (char16_t c : raw) {
        if (c >= 1 && c <= 4) clean += u"Name";
        else if (MpMsgText::isPicChar(c)) clean += u" GLYPH ";   // (a picture is a glyph, not a word: judged by the picture check)
        else clean += c;
    }
    const std::string s = Ascii(clean);
    const std::string low = Lower(s);
    for (const char* w : {"nunchuk", "wiimote", "wii", "remote"}) {
        const std::vector<Tok> t = Tokens(low);
        for (const Tok& k : t) if (k.text == w) return std::string("Wii word: ") + w;
    }
    for (const char* ph : {"shake", "shaking", "point at", "pointing at", "pointer at"}) {
        size_t at = low.find(ph);
        while (at != std::string::npos) {
            const bool startOk = at == 0 || !IsWordChar(static_cast<unsigned char>(low[at - 1]));
            const size_t end = at + strlen(ph);
            const bool endOk = end >= low.size() || !IsWordChar(static_cast<unsigned char>(low[end]));
            if (startOk && endOk) return std::string("Wii wording: ") + ph;
            at = low.find(ph, at + 1);
        }
    }
    const std::vector<Tok> t = Tokens(s);
    for (size_t i = 0; i < t.size(); i++) {
        const std::string& w = t[i].text;
        const bool single = (w.size() == 1 && (w[0] == 'A' || w[0] == 'B' || w[0] == 'C' || w[0] == 'Z' || w[0] == '1' || w[0] == '2' || w[0] == '+' || w[0] == '-'));
        if (!single) continue;
        if ((w[0] == '+' || w[0] == '-') && ((t[i].at > 0 && IsWordChar(static_cast<unsigned char>(s[t[i].at - 1]))) ||
                                              (t[i].at + 1 < s.size() && IsWordChar(static_cast<unsigned char>(s[t[i].at + 1])))))
            continue;   // ("D-pad", "A-B", "+5": part of a word)
        // introduced by "press / hold / button..." up to three words before ("Appuyez sur B", "Pulsa el botón A"), a glyph ends the look back
        bool before = false;
        for (size_t k = i; k > 0 && k + 3 > i; k--) {
            if (t[k - 1].text == "GLYPH") break;
            if (Introduces(Lower(t[k - 1].text))) { before = true; break; }
        }
        const bool after = i + 1 < t.size() && Introduces(Lower(t[i + 1].text));
        const char open = t[i].at > 0 ? s[t[i].at - 1] : 0, close = t[i].at + 1 < s.size() ? s[t[i].at + 1] : 0;
        const bool bracket = (open == '(' && close == ')') || (open == '[' && close == ']');
        if (before || after || (bracket && !(w[0] >= '0' && w[0] <= '9'))) return "names a button: " + w;
    }
    return "";
}

// Pictures of the picture font: every one a text shows must be a plain picture or one the handheld UI draws a Vita button over.
inline std::string WhyPic(const std::u16string& raw) {
    for (char16_t c : raw) {
        if (!MpMsgText::isPicChar(c)) continue;
        const int pic = c - 0xE000;
        bool ok = false;
        for (int p : MpControls::kReplacedPics) ok = ok || p == pic;
        for (int p : MpControls::kPlainPics) ok = ok || p == pic;
        if (!ok) return "picture " + std::to_string(pic) + " is not one the handheld UI covers";
        // a Wii-only picture (the remote, the Nunchuk) is never a multiplayer glyph
        if (pic == 3 || pic == 4 || pic == 6) return "picture " + std::to_string(pic) + " is a Wii controller";
    }
    return "";
}

}  // namespace vitawords
