// touches: the host-rules card's logic and words. Build and run:
//   g++ -std=c++20 -fshort-wchar -fsanitize=address,undefined -g -Idecomp/include -Iplatform/include tests/mp/rules_test.cpp \
//       decomp/src/Game/MP/MpMessages.cpp decomp/src/Game/MP/MpMessagesExtra.cpp -o /tmp/rules_test && /tmp/rules_test
#include <stdio.h>
#include <string>

#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpRulesLogic.hpp"

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace MpRulesLogic;

static void testWatch() {
    const uint32_t k1 = hostKey("Luigi"), k2 = hostKey("Mario");
    CHECK(k1 != 0 && k1 != k2 && hostKey("") != 0 && hostKey(nullptr) != 0);
    const uint32_t s0 = sigOf(0, 0, true);
    Watch w;
    // first time in the lobby: the card, once
    CHECK(w.step(k1, s0, true, true, false) == CARD);
    for (int i = 0; i < 300; i++) CHECK(w.step(k1, s0, true, false, false) == NONE);   // (answered: no longer wanted)
    // the host changes a rule: a card once he stops, not before, and only once
    const uint32_t s1 = sigOf(0, 1, true);
    int cards = 0;
    for (int i = 0; i < 400; i++) cards += w.step(k1, s1, true, false, false) == CARD;
    CHECK(cards == 1);
    // a run of changes is one card
    Watch r;
    r.step(k1, s0, true, true, false);
    int n = 0;
    for (int i = 0; i < 200; i++) n += r.step(k1, i % 2 ? sigOf(1, 0, false) : sigOf(0, 1, false), true, false, false) != NONE;   // (flapping: never settles)
    CHECK(n == 0);
    for (int i = 0; i < 100; i++) n += r.step(k1, sigOf(0, 0, false), true, false, false) == CARD;
    CHECK(n == 1);
    // a late joiner (no lobby): the toast; a change in play: a toast
    Watch l;
    CHECK(l.step(k1, s0, false, true, false) == TOAST_JOIN);
    int t = 0;
    for (int i = 0; i < 200; i++) t += l.step(k1, s1, false, false, false) == TOAST_CHANGE;
    CHECK(t == 1);
    // already seen this host: nothing at the join; a change still tells
    Watch sn;
    CHECK(sn.step(k1, s0, true, false, false) == NONE);
    t = 0;
    for (int i = 0; i < 200; i++) t += sn.step(k1, s1, true, false, false) == CARD;
    CHECK(t == 1);
    // "don't show again": no join card, no join toast; a change is a toast, never a card
    Watch h;
    CHECK(h.step(k1, s0, true, true, true) == NONE);
    int c2 = 0, t2 = 0;
    for (int i = 0; i < 200; i++) {
        const Action a = h.step(k1, s1, true, false, true);
        c2 += a == CARD;
        t2 += a == TOAST_CHANGE;
    }
    CHECK(c2 == 0 && t2 == 1);
    // a new host after a migration: that host's own first-time card
    CHECK(w.step(k2, s0, true, true, false) == CARD);
}

static std::string Ascii(const char16_t* s) {
    std::string o;
    for (; *s; ++s) o += *s < 0x80 ? static_cast<char>(*s) : '?';
    return o;
}

static void testWords() {
    using namespace MpMsgText;
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        for (int mode = 0; mode < 2; mode++)
            for (int falls = 0; falls < 2; falls++)
                for (int bump = 0; bump < 2; bump++) {
                    Rules r{lang >= LANG_DE ? "Toadsworth12" : "Toadsworth123456", mode, falls, bump != 0};   // (16 characters, the longest name; DE / IT words are longer: 12)
                    setDynamicRules(0, r);
                    const char16_t* t = dynamicText(0);
                    CHECK(t[0] == 0x1A && t[1] == 0x0106 && t[2] == 0x0003);
                    int line = 0, lines = 1, longest = 0;
                    for (const char16_t* p = t + 3; *p; ++p) {
                        if (*p == u'\n') { ++lines; line = 0; continue; }
                        if (++line > longest) longest = line;
                    }
                    if (longest > 50 || lines != 2) fprintf(stderr, "  card lang=%d mode=%d: %d lines, longest %d\n", lang, mode, lines, longest);
                    CHECK(longest <= 50 && lines == 2);   // (uxfix #7: no bumping line, no "Show this next time?")   // the words rewrite (fdabace5) made the host line longer; 50 = the longest line with a 16-character name (the words themselves are checked to 40 by share/words/check_rewrite.py)
                    char16_t toast[96];
                    CHECK(formatRulesToast(toast, 96, r, false) < 90 && formatRulesToast(toast, 96, r, true) < 90);
                }
    }
    setLanguage(LANG_EN);
    Rules r{"Luigi", 0, 0, true};
    char16_t t[96];
    formatRulesToast(t, 96, r, false);
    CHECK(Ascii(t) == "Rules (Luigi): Play Together, bubbles.");
    r.falls = 1, r.bump = false;
    formatRulesToast(t, 96, r, true);
    CHECK(Ascii(t) == "Luigi changed the rules: Play Together, try again.");
    r.mode = 1;
    formatRulesToast(t, 96, r, false);
    CHECK(Ascii(t) == "Rules (Luigi): Star Race.");
    setDynamicRules(0, Rules{"Luigi", 0, 0, true});
    std::string d = Ascii(dynamicText(0) + 3);
    CHECK(d == "Let's explore together with Luigi!\nIf you fall, a bubble takes you to a friend.");
}

int main() {
    testWatch();
    testWords();
    fprintf(stderr, "%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
