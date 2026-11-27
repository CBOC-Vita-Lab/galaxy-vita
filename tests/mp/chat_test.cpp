// Galaxy 3.0 multiplayer: host tests for messages between players (owner: chat).
//  A. port/mp_chat.h: the CHAT codec (fixed phrase form, text bounds, canonical), UTF-8 and control-character cleaning (invalid
//     bytes, overlongs, surrogates, C0/C1, bidi / zero-width, private use, astral -> '?', spaces), the 40-character / 120-byte
//     bounds, the token bucket (1 per 2 s, bursts of 3; the host's slack), the host's verdicts.
//  B. Game/MP/MpChatLogic.hpp: the grid and its moves, the quick ring (most used, stable places), mute (forgotten when the player
//     leaves), what a muted player's message does, the shown text (missing glyphs -> '?') and the balloon's line break.
//  C. The session layer (mp_core.cpp) with three loopback consoles: phrases relayed; typed text relayed in private and public rooms
//     (owner, 11 Nov), dropped by the host when it turned typing off (a modified guest sending it anyway); the host's rate limit; malformed CHATs count as bad messages; a console refuses to send what the
//     room does not take.
// Build and run: tests/mp/chat_build.sh  (g++ -fsanitize=address,undefined; the command is in the script)
#include "core_test_util.h"
#include "Game/MP/MpChatLogic.hpp"

#include <string>

static int g_checks = 0;
#define CK(c) do { ++g_checks; CHECK(c); } while (0)

static std::string Clean(const std::string& in) {
    char out[MP_CHAT_TEXT_BYTES + 1];
    const size_t n = mp_chat_clean(in.data(), in.size(), out, sizeof out);
    CK(n == strlen(out));
    return std::string(out, n);
}
static bool Ok(const std::string& s) { return mp_chat_text_ok(s.data(), s.size()); }

static std::vector<uint8_t> Text(const std::string& s) {
    std::vector<uint8_t> v = {MP_CHAT_TEXT, (uint8_t)s.size()};
    v.insert(v.end(), s.begin(), s.end());
    return v;
}
static bool Read(const std::vector<uint8_t>& v, MpChatMsg* m) {
    MpReader r(v.data(), v.size());
    return mp_read_chat(r, m);
}

// ---- A ----------------------------------------------------------------------------------------------------------------------
static void TestClean() {
    CK(Clean("Hello!") == "Hello!");
    CK(Clean("  spaced   out  ") == "spaced out");
    CK(Clean("tab\there\nnew\r\nline") == "tab here new line");
    CK(Clean(std::string("ctl\x01\x07\x1b[31mred\x7f", 13)) == "ctl[31mred");
    CK(Clean("\xc2\x85" "C1\xc2\x9f") == "C1");                                     // C1 controls (U+0085, U+009F)
    CK(Clean("caf\xc3\xa9 \xc2\xa1hola! \xc2\xbfqu\xc3\xa9?") == "caf\xc3\xa9 \xc2\xa1hola! \xc2\xbfqu\xc3\xa9?");   // é ¡ ¿ kept
    CK(Clean("bad\xff\xfe" "bytes\xc0\xaf" "x\xe0\x80\xaf") == "badbytesx");       // invalid, overlong '/', overlong 3-byte
    CK(Clean("sur\xed\xa0\x80rogate") == "surrogate");                           // CESU surrogate
    CK(Clean("trunc\xe3\x81") == "trunc");                                         // cut in the middle of a character
    CK(Clean("emoji \xf0\x9f\x98\x80!") == "emoji ?!");                            // astral -> '?'
    CK(Clean("pic\xee\x80\x80" "ture") == "pic?ture");                             // private use (the panels' picture markup) -> '?'
    CK(Clean("rl\xe2\x80\xaeo zero\xe2\x80\x8bwidth bom\xef\xbb\xbf") == "rlo zerowidth bom");   // bidi override, ZWSP, BOM
    CK(Clean("line\xe2\x80\xa8sep") == "line sep");
    CK(Clean("\xe3\x80\x80wide\xe3\x80\x80space") == "wide space");               // ideographic space
    CK(Clean("") == "" && Clean("   \t\n ") == "" && Clean("\x01\x02") == "");
    CK(Clean(std::string("nul\0after", 9)) == "nul");
    // ingame: 24 characters at most (counting the spaces: one balloon line), never a split character, never more than 72 bytes
    CK(MP_CHAT_TEXT_CHARS == 24 && MP_CHAT_TEXT_BYTES == 72);
    std::string longAscii(100, 'a');
    CK(Clean(longAscii) == std::string(24, 'a'));
    std::string kana;
    for (int i = 0; i < 60; i++) kana += "\xe3\x81\x82";   // あ x 60
    const std::string k = Clean(kana);
    CK(k.size() == 72 && Ok(k));
    std::string words;
    for (int i = 0; i < 30; i++) words += "ab ";
    const std::string w = Clean(words);
    CK(w.size() == 23 && w.back() == 'b' && Ok(w));   // (24 characters with the spaces, no trailing space: 23)
    // a small output buffer never overflows and never splits a character
    char tiny[5];
    CK(mp_chat_clean("\xc3\xa9\xc3\xa9\xc3\xa9", 6, tiny, sizeof tiny) == 4 && !strcmp(tiny, "\xc3\xa9\xc3\xa9"));
    // smallfix: at most 2 combining marks in a row; a message with more is refused on receipt (cleaning would change it)
    {
        const std::string m3 = "e\xcc\x81\xcc\x82\xcc\x83";   // e + 3 marks
        CK(Clean(m3) == "e\xcc\x81\xcc\x82" && Ok("e\xcc\x81\xcc\x82") && !Ok(m3));
        std::string smear = "a";
        for (int i = 0; i < 23; i++) smear += "\xcc\x80";
        CK(Clean(smear) == "a\xcc\x80\xcc\x80" && !Ok(smear));
        CK(Clean("a\xcc\x80\xcc\x80 b\xcc\x80\xcc\x80") == "a\xcc\x80\xcc\x80 b\xcc\x80\xcc\x80");   // the run restarts after a letter
    }
    // what the cleaner gives is accepted, and only that
    CK(Ok("Hello!") && Ok("caf\xc3\xa9") && !Ok(" lead") && !Ok("trail ") && !Ok("dou  ble") && !Ok("") && !Ok("a\nb") && !Ok("\xff"));
    CK(!Ok(std::string(25, 'x')) && Ok(std::string(24, 'x')) && !Ok(std::string(41, 'x')));   // (a longer one is refused whole on receipt)
    // the game's own message escape (0x1A: picture / colour / size tags), line breaks, bidi and zero-width tricks: never accepted
    CK(!Ok("a\x1a" "b") && !Ok("a\rb") && !Ok("a\xe2\x80\xae" "b") && !Ok("a\xe2\x80\x8b" "b") && !Ok("a\xee\x80\x80" "b") && !Ok("a\xe2\x80\xa8" "b"));
    CK(Clean("pic\x1a\x06\x03" "ture") == "picture" || Clean("pic\x1a\x06\x03" "ture").find('\x1a') == std::string::npos);
    // every byte string: cleaning is idempotent and its output is accepted
    uint32_t seed = 7;
    for (int it = 0; it < 200000; it++) {
        char in[64];
        const int n = 1 + (int)(seed % 63);
        for (int i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; in[i] = (char)(seed >> 16); }
        char a[MP_CHAT_TEXT_BYTES + 1], b[MP_CHAT_TEXT_BYTES + 1];
        const size_t na = mp_chat_clean(in, (size_t)n, a, sizeof a);
        const size_t nb = mp_chat_clean(a, na, b, sizeof b);
        if (na != nb || memcmp(a, b, na) != 0) { CK(!"clean not idempotent"); break; }
        if (na > 0 && !mp_chat_text_ok(a, na)) { CK(!"clean output rejected"); break; }
        if (na > MP_CHAT_TEXT_BYTES) { CK(!"too long"); break; }
    }
    printf("clean: ok\n");
}

static void TestCodec() {
    MpChatMsg m;
    for (int id = 0; id < 256; id++) {
        std::vector<uint8_t> v = {MP_CHAT_PHRASE, (uint8_t)id};
        CK(Read(v, &m) == (id < MP_CHAT_PHRASES));
    }
    CK(!Read({MP_CHAT_PHRASE}, &m) && !Read({MP_CHAT_PHRASE, 1, 0}, &m) && !Read({}, &m) && !Read({2, 1}, &m) && !Read({0xFF, 1}, &m));
    CK(Read(Text("Hi there"), &m) && m.kind == MP_CHAT_TEXT && m.len == 8 && !strcmp(m.text, "Hi there"));
    CK(!Read({MP_CHAT_TEXT, 0}, &m));                                    // empty
    std::vector<uint8_t> v = Text("Hi");
    v[1] = 3;
    CK(!Read(v, &m));                                                    // length past the end
    v = Text("Hi");
    v.push_back('!');
    CK(!Read(v, &m));                                                    // extra byte
    CK(!Read(Text("bad\x01" "ctl"), &m) && !Read(Text(" lead"), &m) && !Read(Text("a\xff"), &m) && !Read(Text(std::string(41, 'x')), &m));
    v = {MP_CHAT_TEXT, 121};
    v.insert(v.end(), 121, 'a');
    CK(!Read(v, &m));
    // write -> read round trip, canonical
    MpChatMsg w;
    CK(mp_chat_make_text(&w, "  Hello \x01 world  ", 18) && !strcmp(w.text, "Hello world") && w.len == 11);
    uint8_t buf[MP_CHAT_MAX_PAYLOAD];
    MpWriter wr(buf, sizeof buf);
    mp_write_chat(wr, w);
    CK(wr.ok() && wr.n == 13);
    MpReader rd(buf, wr.n);
    CK(mp_read_chat(rd, &m) && m.len == 11 && !memcmp(m.text, "Hello world", 11));
    CK(!mp_chat_make_text(&w, "\x01\x02  ", 4));
    // the largest text fits the core's maximum
    std::string big;
    for (int i = 0; i < 40; i++) big += "\xe2\x82\xac";   // € x 40: cut to 24 = 72 bytes
    CK(mp_chat_make_text(&w, big.data(), big.size()) && w.len == 72);
    MpWriter wb(buf, sizeof buf);
    mp_write_chat(wb, w);
    CK(wb.ok() && wb.n == MP_CHAT_MAX_PAYLOAD);
    // settings flags
    MpSettings s;
    memset(&s, 0, sizeof s);
    // ingame: typed messages in every room (the old off / public bits no longer decide it)
    s.flags = 7;
    CK(mp_chat_typed_allowed(&s));
    s.flags = 7 | MP_SET_TEXT_OFF;
    CK(!mp_chat_typed_allowed(&s));
    s.flags = 7 | MP_SET_PUBLIC;   // (owner, 11 Nov: typed messages in public games too)
    CK(mp_chat_typed_allowed(&s) && !mp_chat_typed_allowed(nullptr));
    // wire (owner, 11-12 Nov): a room this console knows is public (Quick Match, Play with Anyone) takes typed text like any other; only the
    // host's off switch (and no settings at all) refuses
    s.flags = 7;
    CK(mp_chat_typed_allowed_here(&s, false) && mp_chat_typed_allowed_here(&s, true));
    s.flags = 7 | MP_SET_PUBLIC;
    CK(mp_chat_typed_allowed_here(&s, false) && mp_chat_typed_allowed_here(&s, true) && !mp_chat_typed_allowed_here(nullptr, false) && !mp_chat_typed_allowed_here(nullptr, true));
    s.flags = 7 | MP_SET_TEXT_OFF;
    CK(!mp_chat_typed_allowed_here(&s, false) && !mp_chat_typed_allowed_here(&s, true));
    printf("codec: ok\n");
}

static void TestBucket() {
    MpChatBucket b;
    mp_chat_bucket_reset(&b);
    uint64_t t = 50000;
    // a burst of 3, then one every 2 s
    CK(mp_chat_bucket_take(&b, t, 2000, 3) && mp_chat_bucket_take(&b, t, 2000, 3) && mp_chat_bucket_take(&b, t, 2000, 3));
    CK(!mp_chat_bucket_take(&b, t, 2000, 3) && !mp_chat_bucket_take(&b, t + 1999, 2000, 3));
    CK(mp_chat_bucket_take(&b, t + 2000, 2000, 3) && !mp_chat_bucket_take(&b, t + 3000, 2000, 3));
    CK(mp_chat_bucket_take(&b, t + 4000, 2000, 3));
    // never more than the burst after a long quiet time; the clock going back gives no credit
    CK(mp_chat_bucket_ready(&b, t + 100000, 2000, 3));
    int n = 0;
    while (mp_chat_bucket_take(&b, t + 100000, 2000, 3)) n++;
    CK(n == 3);
    CK(!mp_chat_bucket_take(&b, t + 50000, 2000, 3) && !mp_chat_bucket_take(&b, t + 51999, 2000, 3));
    // sustained: a sender pressing every frame for a minute sends 3 + 30
    mp_chat_bucket_reset(&b);
    n = 0;
    for (uint64_t ms = 0; ms < 60000; ms += 16) n += mp_chat_bucket_take(&b, 200000 + ms, MP_CHAT_SEND_PERIOD_MS, MP_CHAT_SEND_BURST);
    CK(n >= 32 && n <= 33);
    // the host lets a legitimate sender through even when jitter bunches its messages up (sent 2 s apart, received 1.6 s apart)
    MpChatBucket snd, host;
    mp_chat_bucket_reset(&snd);
    mp_chat_bucket_reset(&host);
    int sent = 0, relayed = 0;
    for (int i = 0; i < 200; i++) {
        const uint64_t ts = 300000 + (uint64_t)i * (i < 3 ? 0 : 2000), tr = ts + ((i % 2) ? 400 : 0);
        if (mp_chat_bucket_take(&snd, ts, MP_CHAT_SEND_PERIOD_MS, MP_CHAT_SEND_BURST)) {
            sent++;
            relayed += mp_chat_bucket_take(&host, tr, MP_CHAT_RELAY_PERIOD_MS, MP_CHAT_RELAY_BURST);
        }
    }
    CK(sent > 150 && relayed == sent);
    printf("bucket: ok\n");
}

static void TestHostCheck() {
    MpSettings s;
    memset(&s, 0, sizeof s);
    s.flags = 7;
    MpChatBucket b;
    mp_chat_bucket_reset(&b);
    MpChatMsg m;
    const uint8_t phrase[2] = {MP_CHAT_PHRASE, 5};
    const std::vector<uint8_t> text = Text("hello");
    CK(mp_chat_host_check(phrase, 2, true, &s, false, &b, 1000, &m) == MP_CHAT_OK);
    CK(mp_chat_host_check(phrase, 2, false, &s, false, &b, 1000, &m) == MP_CHAT_BAD);   // never unreliable
    CK(mp_chat_host_check(text.data(), (uint16_t)text.size(), true, &s, false, &b, 1000, &m) == MP_CHAT_OK);
    CK(mp_chat_host_check(text.data(), (uint16_t)text.size(), true, &s, true, &b, 1000, &m) == MP_CHAT_OK);   // public room: typed text too (owner)
    // (phrases are never limited by the public mark: the first line above)
    s.flags = 7 | MP_SET_TEXT_OFF;
    CK(mp_chat_host_check(text.data(), (uint16_t)text.size(), true, &s, false, &b, 1000, &m) == MP_CHAT_TYPED_OFF);
    s.flags = 7 | MP_SET_PUBLIC;
    CK(mp_chat_typed_allowed(&s));   // (the public mark alone no longer stops typed text)
    CK(mp_chat_host_check(phrase, 2, true, &s, false, &b, 1000, &m) == MP_CHAT_OK);     // (the 4th of the burst)
    CK(mp_chat_host_check(phrase, 2, true, &s, false, &b, 1000, &m) == MP_CHAT_RATE);
    CK(mp_chat_host_check(text.data(), (uint16_t)text.size(), true, &s, false, &b, 1000 + MP_CHAT_RELAY_PERIOD_MS, &m) == MP_CHAT_OK);
    // a long or multi-line text from a modified console: refused whole
    const std::vector<uint8_t> longText = Text(std::string(30, 'x').c_str());
    CK(mp_chat_host_check(longText.data(), (uint16_t)longText.size(), true, &s, false, &b, 90000, &m) == MP_CHAT_BAD);
    const std::vector<uint8_t> twoLines = Text("one\ntwo");
    CK(mp_chat_host_check(twoLines.data(), (uint16_t)twoLines.size(), true, &s, false, &b, 90000, &m) == MP_CHAT_BAD);
    mp_chat_bucket_reset(&b);
    for (int i = 0; i < 4; i++) mp_chat_host_check(phrase, 2, true, &s, false, &b, 1000, &m);
    CK(mp_chat_host_check(phrase, 2, true, &s, false, &b, 1000 + MP_CHAT_RELAY_PERIOD_MS, &m) == MP_CHAT_OK);
    // tiny steps add up (no credit lost to rounding)
    mp_chat_bucket_reset(&b);
    for (int i = 0; i < 4; i++) mp_chat_bucket_take(&b, 5000, 1500, 4);
    for (uint64_t ms = 5001; ms < 5000 + 1500; ms++) CK(!mp_chat_bucket_take(&b, ms, 1500, 4));
    CK(mp_chat_bucket_take(&b, 6500, 1500, 4));
    printf("host check: ok\n");
}

// ---- B ----------------------------------------------------------------------------------------------------------------------
static void TestLogic() {
    using namespace MpChatLogic;
    // the grid: every phrase once; moves
    bool seen[MP_CHAT_PHRASES] = {};
    for (uint8_t id : kGridOrder) { CK(id < MP_CHAT_PHRASES && !seen[id]); seen[id] = true; }
    CK(kGridOrder[0] == 0);   // "Over here!" first
    CK(gridMove(0, 1, 13) == 1 && gridMove(2, 1, 13) == 0 && gridMove(0, 3, 13) == 2 && gridMove(0, 0, 13) == 0);
    CK(gridMove(9, 2, 13) == 12 && gridMove(9, 2, 12) == 9 && gridMove(12, 0, 13) == 10 && gridMove(12, 2, 13) == 12 && gridMove(-1, 2, 13) == 0);
    for (int c = 0; c < 13; c++)
        for (int d = 0; d < 4; d++) { const int n = gridMove(c, d, 13); CK(n >= 0 && n < 13); }
    // the ring: three phrases (up, right, down) and, ingame, "Write a Message" on the left (kWriteDir); a phrase used more than the
    // least used of the three takes its place; the others stay put; the left is never a phrase
    Ring r;
    r.reset();
    CK(kWriteDir == 3 && r.slot[0] == 0 && r.slot[1] == 3 && r.slot[2] == 2);
    CK(r.used(0) == 0 && r.used(3) == 1);                     // (each starts with one use; Over here! and Let's go! now 2)
    CK(r.used(5) == -1);                                    // Help! once: not more than Wait for me! (1): stays out
    CK(r.used(5) == 2);                                     // twice: beats Wait for me!, takes the bottom
    CK(r.slot[2] == 5 && r.slot[0] == 0 && r.slot[1] == 3);
    CK(r.used(1) == -1 && r.indexOf(1) == -1);              // Thanks! (2): nobody below 2
    CK(r.used(1) == 2 && r.slot[2] == 1);                   // (3 uses: beats Help! with 2)
    for (int i = 0; i < 10; i++) CK(r.used(i % MP_CHAT_PHRASES) != kWriteDir);
    CK(r.used(-1) == -1 && r.used(MP_CHAT_PHRASES) == -1);
    for (int i = 0; i < 70000; i++) r.used(7);              // (counts saturate)
    CK(r.uses[7] == 0xFFFF && r.indexOf(7) >= 0);
    // ingame: "Hide messages" belongs to the player for the session: a reconnect into another slot stays hidden, a stranger in the
    // freed slot does not; unhiding works; the list never overflows
    {
        Mute h;
        h.reset();
        const char* names[4] = {"Me", "Troll", "Pal", nullptr};
        bool here[4] = {true, true, true, false};
        auto pres = [&](int s) { return here[s]; };
        auto name = [&](int s) { return names[s] ? names[s] : ""; };
        h.sync(pres, name);
        h.set(1, true, names[1]);
        h.sync(pres, name);
        CK(h.muted(1) && !h.muted(2));
        here[1] = false;                       // Troll drops...
        h.sync(pres, name);
        CK(!h.muted(1));
        names[1] = "Newbie"; here[1] = true;   // ...a stranger takes slot 1: not hidden
        h.sync(pres, name);
        CK(!h.muted(1));
        names[3] = "Troll"; here[3] = true;    // Troll comes back in slot 3: hidden again
        h.sync(pres, name);
        CK(h.muted(3) && !h.muted(1));
        h.set(3, false, names[3]);
        h.sync(pres, name);
        CK(!h.muted(3));
        char nm[16];
        for (int i = 0; i < 100; i++) { snprintf(nm, sizeof nm, "p%d", i); h.set(2, true, nm); }
        CK(h.keyCount == Mute::kKeys);
        h.set(-1, true, "x"); h.set(4, true, "x");   // (out of range: nothing)
        CK(h.keyCount == Mute::kKeys);
        h.reset();
        CK(h.keyCount == 0 && !h.muted(2));
    }
    // merge1: "Hide messages" by the core's stable player id: the hidden player's rename and rejoin (another slot, another name) keep
    // them hidden; another player with the same name (another id) is not hidden; two players of one name are told apart
    {
        Mute h;
        h.reset();
        const char* names[5] = {"Me", "Player", "Player", nullptr, nullptr};
        uint32_t sids[5] = {0, 0xA1, 0xB2, 0, 0};
        bool here[5] = {true, true, true, false, false};
        auto pres = [&](int s) { return here[s]; };
        auto name = [&](int s) { return names[s] ? names[s] : ""; };
        auto sid = [&](int s) { return sids[s]; };
        h.sync(pres, name, sid);
        h.set(1, true, names[1], sids[1]);          // hide the first "Player"
        h.sync(pres, name, sid);
        CK(h.muted(1) && !h.muted(2));              // (the second "Player" is another player)
        here[1] = false;                            // drops...
        h.sync(pres, name, sid);
        CK(!h.muted(1) && !h.muted(2));
        names[3] = "Renamed"; sids[3] = 0xA1; here[3] = true;   // ...rejoins in another slot under another name: hidden
        h.sync(pres, name, sid);
        CK(h.muted(3) && !h.muted(2));
        names[3] = "Renamed again";                 // a rename while present
        h.sync(pres, name, sid);
        CK(h.muted(3));
        names[4] = "Player"; sids[4] = 0xC3; here[4] = true;    // a stranger with the hidden player's old name: not hidden
        h.sync(pres, name, sid);
        CK(!h.muted(4) && h.muted(3) && !h.muted(2));
        h.set(3, false, names[3], sids[3]);          // unhide by id
        h.sync(pres, name, sid);
        CK(!h.muted(3) && h.keyCount == 0);
        // an unknown id (0: a peer without a commitment) falls back to the name key
        h.reset();
        sids[1] = 0; sids[2] = 0; names[2] = "Other"; here[1] = true;
        h.sync(pres, name, sid);
        h.set(1, true, names[1], 0);
        h.sync(pres, name, sid);
        CK(h.muted(1) && !h.muted(2) && h.keyCount == 1);
        CK(Mute::keyOf("x", 7) == 7 && Mute::keyOf("x") == Mute::keyOf("x", 0) && Mute::keyOf("x") != 7);
    }
    // mute: this console only, forgotten when the player leaves
    Mute mu;
    mu.reset();
    mu.set(2, true);
    mu.set(9, true);
    CK(mu.muted(2) && !mu.muted(1) && !mu.muted(9) && !mu.muted(-1));
    bool present[4] = {true, true, true, false};
    mu.forgetGone([&](int s) { return present[s]; });
    CK(mu.muted(2));
    present[2] = false;
    mu.forgetGone([&](int s) { return present[s]; });
    CK(!mu.muted(2));
    // ingame: a hidden player shows nothing (typed or quick); the balloon is the message: never a toast
    CK(decide(MP_CHAT_TEXT, true).toast == false && decide(MP_CHAT_TEXT, true).balloon == false);
    CK(decide(MP_CHAT_PHRASE, true).toast == false && decide(MP_CHAT_PHRASE, true).balloon == false);
    CK(!decide(MP_CHAT_TEXT, false).toast && decide(MP_CHAT_TEXT, false).balloon && decide(MP_CHAT_PHRASE, false).balloon);
    // shown text: UTF-16; a glyph the font lacks shows as nothing (never '?' or a box), no double / edge space left behind
    char16_t out[64];
    auto noE9 = [](char16_t c) { return c != 0xE9; };
    CK(toShown("caf\xc3\xa9 ok", 8, out, 64, noE9) == 6 && out[3] == u' ' && out[4] == u'o' && out[5] == u'k');
    CK(toShown("\xc3\xa9 x \xc3\xa9", 8, out, 64, noE9) == 1 && out[0] == u'x');
    {   // hostile bytes straight to the screen path (never through the cleaner): control, escape, private use, bidi, too long
        auto all = [](char16_t) { return true; };
        const std::string evil = std::string("a\x1a\x06\x03\nb\xee\x80\x80" "c\xe2\x80\xae" "d\xe2\x80\x8b" "e\xf0\x9f\x98\x80" "f") + std::string(60, 'z');
        const int n = toShown(evil.data(), evil.size(), out, 64, all);
        CK(n == MP_CHAT_TEXT_CHARS);
        for (int i = 0; i < n; i++) CK(out[i] >= 0x20 && out[i] != 0x1A && !(out[i] >= 0xE000 && out[i] <= 0xF8FF) && out[i] != 0x202E && out[i] != 0x200B && out[i] != u'?');
        CK(out[0] == u'a' && out[1] == u'b' && out[2] == u'c' && out[3] == u'd' && out[4] == u'e' && out[5] == u'f');
    }
    CK(toShown("abc", 3, out, 3, [](char16_t) { return true; }) == 2 && out[2] == 0);
    CK(toShown(nullptr, 0, out, 64, [](char16_t) { return true; }) == 0 && out[0] == 0);
    // the balloon's line break: at the space nearest the middle, or hard at the limit
    char16_t t1[64] = u"Wait for me at the big tree please";
    CK(wrapTwoLines(t1, 64, 20) == 2);
    int nl = 0;
    for (int i = 0; t1[i]; i++) if (t1[i] == u'\n') nl = i;
    CK(nl > 10 && nl < 24);
    char16_t t2[64] = u"Short one";
    CK(wrapTwoLines(t2, 64, 20) == 1);
    char16_t t3[64] = u"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    CK(wrapTwoLines(t3, 64, 20) == 2 && t3[20] == u'\n' && t3[40] == u'a' && t3[41] == 0);
    {   // 13 Nov (A3): a full typed message (24 characters) wrapped for the balloon (14 a line): never a line over 14, never a lost character
        const char16_t* cases[] = {u"WWWWWWWWWWWWWWWWWWWWWWWW", u"WWWWWWWWWWW WWWWWWWWWWW", u"WWWWWWWWWWWWWWWWWWWW WWW", u"a very long typed message", u"iiiiiiiiiiiiiiiiiiiiiiii",
                                   u"hello", u"Wait for me, wait!!"};
        for (const char16_t* c : cases) {
            char16_t t[64] = {};
            int n = 0;
            while (c[n]) { t[n] = c[n]; n++; }
            const int lines = wrapTwoLines(t, 64, 14);
            int run = 0, worst = 0, kept = 0, nl = 0;
            for (int i = 0; t[i]; i++) {
                if (t[i] == u'\n') { run = 0; nl++; continue; }
                kept++;
                if (++run > worst) worst = run;
            }
            CK(lines == (n <= 14 ? 1 : 2) && nl == lines - 1 && worst <= 14);
            CK(kept == n || kept == n - 1);   // (a space turned into the break is the only change)
        }
    }
    CK(balloonTurns(MP_CHAT_PHRASE, 5) == 210 && balloonTurns(MP_CHAT_TEXT, 40) == 360 && balloonTurns(MP_CHAT_TEXT, 3) < 210);
    printf("logic: ok\n");
}

// ---- C ----------------------------------------------------------------------------------------------------------------------
static std::vector<MpCore*> Session(MpLoopHub& hub, int n, uint32_t seed, uint8_t flags, uint8_t visibility) {
    std::vector<MpCore*> all;
    const char* names[4] = {"Host", "Gina", "Gus", "Toad"};
    for (int i = 0; i < n; i++) all.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], seed + (uint32_t)i * 7919u));
    MpHostParams hp = HostParams();
    hp.settings.flags = flags;
    hp.visibility = visibility;
    CK(all[0]->uiHost(&hp));
    CK(WaitPhase(all, all[0], MP_UI_LOBBY, 2000));
    for (int i = 1; i < n; i++) {
        CK(ScanJoin(all, all[i]));
        CK(WaitPhase(all, all[i], MP_UI_LOBBY, 8000));
    }
    CK(all[0]->uiStart());
    Step(all, 300);
    return all;
}
static void Free(std::vector<MpCore*>& all) {
    for (MpCore*& c : all) { delete c; c = 0; }
}
static MpSettings Set(MpCore* c) {
    MpSettings s;
    c->settings(&s);
    return s;
}
static const MpSettings* PS(MpCore* c) {
    static MpSettings s;
    c->settings(&s);
    return &s;
}
static int Chats(MpCore* c, int* texts = nullptr, int from = -1) {
    MpMsg m;
    int n = 0, t = 0;
    while (c->poll(&m)) {
        if (m.type != MP_MSG_CHAT || (from >= 0 && m.fromSlot != from)) continue;
        MpReader r(m.data, m.len);
        MpChatMsg cm;
        CK(mp_read_chat(r, &cm));
        n++;
        if (cm.kind == MP_CHAT_TEXT) t++;
    }
    if (texts) *texts = t;
    return n;
}
static bool SendText(MpCore* c, const char* s) {
    MpChatMsg m;
    if (!mp_chat_make_text(&m, s, strlen(s))) return false;
    uint8_t buf[MP_CHAT_MAX_PAYLOAD];
    MpWriter w(buf, sizeof buf);
    mp_write_chat(w, m);
    return c->send(MP_MSG_CHAT, buf, (uint16_t)w.n, true);
}
static bool SendPhrase(MpCore* c, uint8_t id) {
    const uint8_t b[2] = {MP_CHAT_PHRASE, id};
    return c->send(MP_MSG_CHAT, b, 2, true);
}
// what a modified console does: the wire bytes whatever its own checks say (sendTo the host skips the core's CHAT check)
static bool ForceText(MpCore* c, int hostSlot, const char* s) {
    std::vector<uint8_t> v = Text(s);
    return c->sendTo(hostSlot, MP_MSG_CHAT, v.data(), (uint16_t)v.size(), true);
}

static void TestCorePrivate() {
    {   // touches: a core that nobody configured starts with typed messages off (quick phrases are not gated by it)
        MpLoopHub h0;
        h0.setClock(VNow, 0);
        MpCore* fresh = MakeCore(h0, 9, "Fresh", 99);
        const MpSettings d = Set(fresh);
        CK((d.flags & MP_SET_TEXT_OFF) != 0 && !mp_chat_typed_allowed(&d));
        delete fresh;
    }
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = Session(hub, 3, 21, 7, 0);
    MpCore *h = all[0], *g1 = all[1], *g2 = all[2];
    CK(Phase(g1) == MP_UI_PLAYING && Phase(g2) == MP_UI_PLAYING);
    { const MpSettings s0 = Set(h); CK(mp_chat_typed_allowed(&s0)); }
    // a phrase and a text from guest 1 reach the host and guest 2
    CK(SendPhrase(g1, 7));
    Step(all, 100);
    CK(SendText(g1, "See you at the top!"));
    Step(all, 300);
    int t = 0;
    CK(Chats(h, &t, 1) == 2 && t == 1);
    CK(Chats(g2, &t, 1) == 2 && t == 1);
    CK(Chats(g1) == 0);   // (never echoed back)
    // the host's rate limit: a guest that floods (a modified one: the game would not) gets 4 through, then one per 1.5 s
    for (int i = 0; i < 12; i++) CK(SendPhrase(g2, (uint8_t)(i % MP_CHAT_PHRASES)));
    Step(all, 300);
    const int got = Chats(g1, nullptr, 2);
    CK(got == MP_CHAT_RELAY_BURST);
    CK(Chats(h, nullptr, 2) == got);
    CK(LogCount("chat drop slot=2 kind=phrase reason=rate") >= 1);
    Step(all, 3100);
    for (int i = 0; i < 3; i++) CK(SendPhrase(g2, 1));
    Step(all, 300);
    CK(Chats(g1, nullptr, 2) == 2);
    // malformed CHATs are bad messages (and dropped)
    const int badBefore = LogCount("bad message");
    const uint8_t junk[3] = {MP_CHAT_PHRASE, 99, 0};
    CK(!g1->send(MP_MSG_CHAT, junk, 3, true));   // (the console's own check)
    CK(g1->sendTo(0, MP_MSG_CHAT, junk, 2, true));
    Step(all, 200);
    CK(Chats(h, nullptr, 1) == 0 && Chats(g2, nullptr, 1) == 0);
    (void)badBefore;
    // the host turns typing off: texts stop (the guest's own console refuses them; a forced one is dropped by the host)
    MpSettings s = Set(h);
    s.flags |= MP_SET_TEXT_OFF;
    CK(h->updateSettings(&s));
    Step(all, 3000);
    CK(!mp_chat_typed_allowed(PS(g1)));
    CK(!SendText(g1, "still there?"));
    CK(ForceText(g1, 0, "still there?"));
    CK(SendPhrase(g1, 3));
    Step(all, 300);
    CK(Chats(g2, &t, 1) == 1 && t == 0);
    CK(Chats(h, &t, 1) == 1 && t == 0);
    CK(LogCount("chat drop slot=1 kind=text reason=typed-off") == 1);
    Free(all);
    printf("core private room: ok\n");
}

static void TestCorePublic() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    // a room open to anyone (Quick Match hosts with visibility 2): the host marks it public. Owner, 11 Nov: typed messages in every kind
    // of game, public ones included (the rate limits and the text cleaner apply everywhere; the host's "off" still stops them)
    std::vector<MpCore*> all = Session(hub, 3, 33, 7, 2);
    MpCore *h = all[0], *g1 = all[1], *g2 = all[2];
    CK((Set(h).flags & MP_SET_PUBLIC) && (Set(g1).flags & MP_SET_PUBLIC) && (Set(g2).flags & MP_SET_PUBLIC));
    CK(mp_chat_typed_allowed(PS(g1)));
    CK(SendText(g1, "hello stranger"));
    CK(SendPhrase(g1, 11));
    Step(all, 400);
    int t = 0;
    CK(Chats(h, &t, 1) == 2 && t == 1);
    CK(Chats(g2, &t, 1) == 2 && t == 1);
    // the host's "off" (a settings change) stops them in a public room too
    MpSettings s = Set(h);
    s.flags = (uint8_t)(s.flags | MP_SET_TEXT_OFF);
    CK(h->updateSettings(&s));
    Step(all, 3000);
    CK(!SendText(g1, "and now?"));
    CK(ForceText(g1, 0, "and now?"));   // (a modified guest: the host drops it)
    Step(all, 300);
    CK(Chats(g2, &t, 1) == 0);
    Free(all);
    printf("core public room: ok\n");
}

int main(int argc, char** argv) {
    mp_log_set_sink(LogSink, true);
    g_echo = argc > 1 && !strcmp(argv[1], "-v");
    TestClean();
    TestCodec();
    TestBucket();
    TestHostCheck();
    TestLogic();
    TestCorePrivate();
    TestCorePublic();
    if (g_fail) {
        printf("FAILED: %d of %d checks\n", g_fail, g_checks);
        return 1;
    }
    printf("all chat tests passed (%d checks)\n", g_checks);
    return 0;
}
