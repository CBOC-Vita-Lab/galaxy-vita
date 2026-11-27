// touches: the away state ("Be right back" / 30 s idle) and the quick-phrase set. Build and run:
//   g++ -std=c++20 -fshort-wchar -fsanitize=address,undefined -g -Idecomp/include -Iplatform/include tests/mp/away_test.cpp \
//       decomp/src/Game/MP/MpMessages.cpp decomp/src/Game/MP/MpMessagesExtra.cpp -o /tmp/away_test && /tmp/away_test
#include <stdio.h>
#include <string>

#include "Game/MP/MpAwayLogic.hpp"
#include "Game/MP/MpChatLogic.hpp"
#include "Game/MP/MpMessages.hpp"

static int gChecks = 0, gFails = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace MpAwayLogic;

static void testAway() {
    State s;
    for (int i = 0; i < kIdleTurns - 1; i++) CHECK(!s.step(false, true, false));
    CHECK(s.step(false, true, false) && s.away && !s.byPhrase);          // 30 s without input while playing: away
    CHECK(!s.step(true, true, false));                                   // a button: back
    // idle only counts while playing: a demo, a loading, a death do not make anyone away
    State d;
    for (int i = 0; i < 3 * kIdleTurns; i++) d.step(false, false, false);
    CHECK(!d.away);
    // input resets the count
    State r;
    for (int i = 0; i < kIdleTurns - 5; i++) r.step(false, true, false);
    r.step(true, true, false);
    for (int i = 0; i < kIdleTurns - 5; i++) r.step(false, true, false);
    CHECK(!r.away);
    // a menu counts as activity and never clears or sets away by itself
    State m;
    for (int i = 0; i < 2 * kIdleTurns; i++) m.step(false, true, true);
    CHECK(!m.away);
    // Be right back: away at once; the press that chose it (and a menu closing) does not undo it
    State b;
    b.brb();
    CHECK(b.away && b.byPhrase);
    CHECK(b.step(true, true, false));                                    // the same press
    for (int i = 0; i < 10; i++) b.step(true, true, true);               // the menu closing with B
    CHECK(b.away);
    for (int i = 0; i < kGraceTurns - 1; i++) CHECK(b.step(true, true, false));
    CHECK(b.away);
    for (int i = 0; i < 3; i++) b.step(true, true, false);
    CHECK(!b.away);                                                      // moved after the grace: back
    // away by phrase does not time out by itself
    State k;
    k.brb();
    for (int i = 0; i < 100000; i++) k.step(false, true, false);
    CHECK(k.away);
}

static void testPhrases() {
    using namespace MpMsgText;
    CHECK(MP_CHAT_PHRASES == 12 && MP_CHAT_PHRASE_BRB == 11);
    std::u16string en[MP_CHAT_PHRASES];
    for (int lang = 0; lang < LANG_COUNT; lang++) {
        setLanguage(lang);
        for (int i = 0; i < MP_CHAT_PHRASES; i++) {
            const char16_t* t = raw(static_cast<MpText>(MP_T_PHRASE_0 + i));
            std::u16string u(t);
            CHECK(!u.empty() && u.size() <= 20);                         // fits a tile and a balloon line
            if (lang == 0) en[i] = u;
        }
    }
    setLanguage(LANG_EN);
    // the grid shows every phrase once
    bool seen[MP_CHAT_PHRASES] = {};
    for (int i = 0; i < MP_CHAT_PHRASES; i++) {
        const int p = MpChatLogic::kGridOrder[i];
        CHECK(p >= 0 && p < MP_CHAT_PHRASES && !seen[p]);
        seen[p] = true;
    }
    // the set covers: greet, thanks, help, come here, wait, go, yes, no, sorry, be right back, which star, this one
    const char16_t* want[] = {u"Hello!", u"Thanks!", u"Help!", u"Over here!", u"Wait for me!", u"Let's go!", u"Yes!", u"No!", u"Sorry!",
                              u"Be right back!", u"Which star?", u"Let's do this one!"};
    for (const char16_t* w : want) {
        bool found = false;
        for (int i = 0; i < MP_CHAT_PHRASES; i++) found = found || en[i] == w;
        CHECK(found);
    }
    for (int lang = LANG_DE; lang < LANG_COUNT; lang++) {   // (German / Italian: every phrase differs from English)
        setLanguage(lang);
        for (int i = 0; i < MP_CHAT_PHRASES; i++) CHECK(std::u16string(raw(static_cast<MpText>(MP_T_PHRASE_0 + i))) != en[i] || i == 9 || i == 4);   // ("No!" / "Hello!"-like cognates may match)
    }
    setLanguage(LANG_EN);
    CHECK(en[MP_CHAT_PHRASE_BRB] == u"Be right back!");
    // the four old stamps keep their numbers (0..3: Over here, Thanks, Wait for me, Let's go)
    CHECK(en[0] == u"Over here!" && en[1] == u"Thanks!" && en[2] == u"Wait for me!" && en[3] == u"Let's go!");
}

int main() {
    testAway();
    testPhrases();
    fprintf(stderr, "%d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
