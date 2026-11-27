// Galaxy 3.0 multiplayer (release3): host tests of the "newer version available" notice (platform/src/mp/mp_update.*) and of the
// bug-report scrubber and builder (platform/src/mp/mp_report.*). Build and run: tests/mp/release3_build.sh
#include "mp_report.h"
#include "mp_update.h"
#include "mp_config.h"
#include "mp_core.h"
#include "port/mp_core_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <sys/stat.h>

static int sFail = 0;
#define CHECK(c) do { if (!(c)) { ++sFail; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ_STR(a, b) do { std::string x_ = (a), y_ = (b); if (x_ != y_) { ++sFail; fprintf(stderr, "FAIL %s:%d\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, x_.c_str(), y_.c_str()); } } while (0)

static std::string Scrub(const std::string& in) {
    char out[4096];
    mp_scrub(in.data(), in.size(), out, sizeof out);
    return out;
}

static void testCompare() {
    CHECK(mp_version_compare("3.0.0", "3.0.0") == 0);
    CHECK(mp_version_compare("3.0.0", "3.0.1") == -1);
    CHECK(mp_version_compare("3.1.0", "3.0.9") == 1);
    CHECK(mp_version_compare("3.1", "3.1.0") == 0);                 // a missing number is 0
    CHECK(mp_version_compare("10.0.0", "9.9.9") == 1);              // numbers, not text
    CHECK(mp_version_compare("3.0.0-rc1", "3.0.0") == 0);           // the suffix is ignored for the order
    CHECK(mp_version_compare("3.0.0.1", "3.0.0") == 1);
    CHECK(mp_version_compare("", "3.0.0") == 2);
    CHECK(mp_version_compare(nullptr, "3.0.0") == 2);
    CHECK(mp_version_compare("3.x", "3.0.0") == 2);
    CHECK(mp_version_compare("3..0", "3.0.0") == 2);
    CHECK(mp_version_compare(".3", "3.0.0") == 2);
    CHECK(mp_version_compare("3.0.0.", "3.0.0") == 2);
    CHECK(mp_version_compare("12345.0", "3.0.0") == 2);             // at most 4 digits
    CHECK(mp_version_compare("1.2.3.4.5", "3.0.0") == 2);           // at most 4 numbers
    CHECK(mp_version_compare("3.0.0-", "3.0.0") == 2);
    CHECK(mp_version_compare("3.0.0-RC", "3.0.0") == 2);            // suffix: lower case only, as the service's VERSION_RE
    CHECK(mp_version_compare("3.0.0-aaaaaaaaaaaaa", "3.0.0") == 2); // suffix longer than 12
    CHECK(mp_version_compare("3.0.0 ", "3.0.0") == 2);
    CHECK(mp_version_compare("-1.0.0", "3.0.0") == 2);
}

static void testNoteAndOnce() {
    mp_update_reset();
    mp_update_set_current("3.0.0");
    CHECK(!mp_update_available());
    CHECK(!mp_update_take_notice());
    CHECK(!mp_update_note_latest("3.0.0"));          // same: nothing
    CHECK(!mp_update_note_latest("2.9.9"));          // older: nothing
    CHECK(!mp_update_note_latest("banana"));         // not a version
    CHECK(!mp_update_note_latest(""));
    CHECK(!mp_update_note_latest(nullptr));
    CHECK(!mp_update_available());
    CHECK(!mp_update_take_notice());
    CHECK(mp_update_note_latest("3.1.0"));           // newer
    CHECK(mp_update_available());
    CHECK_EQ_STR(mp_update_latest(), "3.1.0");
    CHECK(mp_update_take_notice());                  // the notice: once
    CHECK(!mp_update_take_notice());
    CHECK(mp_update_note_latest("3.2.0"));           // a still newer answer later: known, but the notice is not shown again this boot
    CHECK(mp_update_available());
    CHECK_EQ_STR(mp_update_latest(), "3.1.0");
    CHECK(!mp_update_take_notice());
    mp_update_reset();
    CHECK(!mp_update_available());
    CHECK_EQ_STR(mp_update_latest(), "");
}

static void testScan() {
    mp_update_reset();
    mp_update_set_current("3.0.0");
    // the real shapes (server/src/core/app.js info(); util.js errorResponse)
    const char* info = "{\"proto\":1,\"minProto\":1,\"game\":\"smg\",\"serverMs\":1,\"kill\":[],\"message\":null,\"features\":{\"rooms\":true},"
                       "\"app\":{\"min\":\"3.0.0\",\"latest\":\"3.0.0\"},\"link\":{\"pingAfterSec\":2},\"servers\":[]}";
    CHECK(!mp_update_scan_body(info, strlen(info)));
    CHECK(!mp_update_available());
    const char* info2 = "{\"proto\":1,\"app\": {\"min\": \"3.0.0\", \"latest\": \"3.4.1\"},\"servers\":[]}";
    CHECK(mp_update_scan_body(info2, strlen(info2)));
    CHECK_EQ_STR(mp_update_latest(), "3.4.1");
    CHECK(mp_update_take_notice());
    mp_update_reset();
    mp_update_set_current("3.0.0");
    const char* err = "{\"error\":{\"code\":\"version_mismatch\",\"message\":\"that room plays a different version of the game\",\"latestVersion\":\"3.0.1\"}}";
    CHECK(mp_update_scan_body(err, strlen(err)));
    CHECK_EQ_STR(mp_update_latest(), "3.0.1");
    mp_update_reset();
    mp_update_set_current("3.0.0");
    const char* errSame = "{\"error\":{\"code\":\"room_full\",\"latestVersion\":\"3.0.0\"}}";
    CHECK(!mp_update_scan_body(errSame, strlen(errSame)));
    // "latest": null, no app object, a latest that is not a version, an escaped one, an unterminated one, one that is too long
    const char* bad[] = {"{\"app\":{\"min\":null,\"latest\":null}}", "{\"x\":1}", "", "{\"app\":{\"latest\":\"banana\"}}", "{\"app\":{\"latest\":\"3.\\u0031.0\"}}",
                         "{\"app\":{\"latest\":\"3.1.0", "{\"error\":{\"latestVersion\":\"3.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0\"}}",
                         "{\"latest\":\"9.9.9\"}",    // "latest" outside "app" is not the info answer's
                         "\"app\"", "\"latestVersion\"", "\"latestVersion\":", "\"latestVersion\": \"", "{\"app\":"};
    for (const char* b : bad) CHECK(!mp_update_scan_body(b, strlen(b)));
    CHECK(!mp_update_available());
    CHECK(!mp_update_scan_body(nullptr, 5));
    CHECK(!mp_update_scan_body("{\"error\":{\"latestVersion\":\"9.9.9\"}}", 0));   // length 0: nothing is read
    // the scan stops at len: a version after the end is not seen
    const char* cut = "{\"error\":{\"latestVersion\":\"9.9.9\"}}";
    CHECK(!mp_update_scan_body(cut, 25));
    CHECK(!mp_update_available());
    // an oversized body is ignored
    std::string big(70000, ' ');
    big += "{\"error\":{\"latestVersion\":\"9.9.9\"}}";
    CHECK(!mp_update_scan_body(big.data(), big.size()));
    // a not-terminated buffer: only [0, len) is read (ASan would catch a read beyond)
    char* exact = static_cast<char*>(malloc(strlen(err)));
    memcpy(exact, err, strlen(err));
    CHECK(mp_update_scan_body(exact, strlen(err)));
    free(exact);
    mp_update_reset();
}

static void testScrub() {
    // addresses
    CHECK_EQ_STR(Scrub("direct: joining 203.0.113.7:41100"), "direct: joining <address>");
    CHECK_EQ_STR(Scrub("join start conn=direct addr=192.168.1.20:41100"), "join start conn=direct addr=<address>");
    CHECK_EQ_STR(Scrub("watch: the network changed (IP 10.0.0.5 -> 192.168.0.9)"), "watch: the network changed (IP <address> -> <address>)");
    CHECK_EQ_STR(Scrub("peer 1: learned path [2001:db8::1]:5000 (relay)"), "peer 1: learned path <address> (relay)");
    CHECK_EQ_STR(Scrub("fe80::1 and ::1"), "<address> and <address>");
    CHECK_EQ_STR(Scrub("ad hoc peer 00:11:22:aa:bb:cc ok"), "ad hoc peer <address> ok");
    CHECK_EQ_STR(Scrub("the address is 203.0.113.7."), "the address is <address>.");
    CHECK_EQ_STR(Scrub("rejoin: a beacon of the session from an unknown address 0a000105 is ignored"), "rejoin: a beacon of the session from an unknown address <address> is ignored");
    CHECK_EQ_STR(Scrub("hello sent to 0a000105:41100"), "hello sent to <address>");
    // not addresses: times, versions, counts, ordinary hex
    CHECK_EQ_STR(Scrub("12:34:56 version 3.0.0 build 3a4f9c10 fps 29.97 mask 0x80000"), "12:34:56 version 3.0.0 build 3a4f9c10 fps 29.97 mask 0x80000");
    CHECK_EQ_STR(Scrub("session=1a2b3c4d host=1 epoch=0 players=2"), "session=1a2b3c4d host=1 epoch=0 players=2");
    // secrets
    CHECK_EQ_STR(Scrub("console_key = 00112233445566778899aabbccddeeff"), "console_key = <hidden>");
    CHECK_EQ_STR(Scrub("name = Alice B\nserver = https://x.example.org:8443/a\nlook = 1"), "name = <hidden>\nserver = https://<host>\nlook = 1");
    CHECK_EQ_STR(Scrub("x 00112233445566778899aabbccddeeff y"), "x <secret> y");
    CHECK_EQ_STR(Scrub("Authorization: Bearer abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO12"), "Authorization: Bearer <secret>");
    CHECK_EQ_STR(Scrub("token=abc123"), "token=<hidden>");
    CHECK_EQ_STR(Scrub("console=1f2e3d4c"), "console=<hidden>");
    // codes and names
    CHECK_EQ_STR(Scrub("service register ok code=KQ7M2XZA"), "service register ok code=<hidden>");
    // the real friend code format is XXXX-XXXX (mp_service.cpp FriendCodeValid): hyphenated, plain, lower case, with and without a key
    CHECK_EQ_STR(Scrub("service register ok code=KQ7M-2XZA"), "service register ok code=<hidden>");
    CHECK_EQ_STR(Scrub("service signin ok code=KQ7M-2XZA build=1a2b3c4d"), "service signin ok code=<hidden> build=1a2b3c4d");
    CHECK_EQ_STR(Scrub("service friend add code=KQ7M-2XZA"), "service friend add code=<hidden>");
    CHECK_EQ_STR(Scrub("service friend lookup friend=KQ7M-2XZA room=ABC234"), "service friend lookup friend=<hidden> room=<hidden>");
    CHECK_EQ_STR(Scrub("my friend code is KQ7M-2XZA ok"), "my friend code is <code> ok");
    CHECK_EQ_STR(Scrub("KQ7M-2XZA"), "<code>");
    // a room code after the words that introduce one, without a key (np_online.cpp used to log "joining ABC234")
    CHECK_EQ_STR(Scrub("online session started (joining ABC234, local port 41100, star topology)"), "online session started (joining <code>, local port 41100, star topology)");
    CHECK_EQ_STR(Scrub("online session started (joining a room, local port 41100, star topology)"), "online session started (joining a room, local port 41100, star topology)");
    CHECK_EQ_STR(Scrub("service time submit: not in room ABC234 any more"), "service time submit: not in room <code> any more");
    CHECK_EQ_STR(Scrub("the room started fine"), "the room started fine");
    CHECK_EQ_STR(Scrub("code=kq7m-2xza"), "code=<hidden>");
    CHECK_EQ_STR(Scrub("code=12345678"), "code=<hidden>");
    CHECK_EQ_STR(Scrub("service error op=me status=409 code=room_full"), "service error op=me status=409 code=room_full");
    CHECK_EQ_STR(Scrub("online: room code=ABC234"), "online: room code=<hidden>");
    CHECK_EQ_STR(Scrub("service error op=me status=409 code=version_mismatch"), "service error op=me status=409 code=version_mismatch");
    CHECK_EQ_STR(Scrub("service error op=me status=0 code=-"), "service error op=me status=0 code=-");
    CHECK_EQ_STR(Scrub("service friend lookup friend=KQ7M2XZA room=ABC234"), "service friend lookup friend=<hidden> room=<hidden>");
    CHECK_EQ_STR(Scrub("service quick match lookup room=-"), "service quick match lookup room=-");
    CHECK_EQ_STR(Scrub("config: name=Mario Fan 99 look=2 conn=1"), "config: name=<hidden> look=2 conn=1");
    CHECK_EQ_STR(Scrub("slot 2 joined name=Anna B. rejoin=0 look=1 players=2"), "slot 2 joined name=<hidden> rejoin=0 look=1 players=2");
    CHECK_EQ_STR(Scrub("roster: slot 1 joined name=Pat Smith"), "roster: slot 1 joined name=<hidden>");
    CHECK_EQ_STR(Scrub("hello rejected reason=kicked name=Zed console=0a1b2c3d (by address)"), "hello rejected reason=kicked name=<hidden> console=<hidden> (by address)");
    CHECK_EQ_STR(Scrub("service register start name=Kim"), "service register start name=<hidden>");
    CHECK_EQ_STR(Scrub("hello rejected reason=full name="), "hello rejected reason=full name=");
    // the word "name" elsewhere is untouched
    CHECK_EQ_STR(Scrub("filename=a.txt model=Mario hostname"), "filename=a.txt model=Mario hostname");
    // URLs, user folders
    CHECK_EQ_STR(Scrub("service https://my-galaxy.example.org/v1/rooms/ABC234 ok"), "service https://<host> ok");
    CHECK_EQ_STR(Scrub("see http://127.0.0.1:8790/v1/info now"), "see http://<host> now");
    CHECK_EQ_STR(Scrub("save /home/alice/saves/x.bin and /Users/Bob/Library"), "save /home/<user>/saves/x.bin and /Users/<user>/Library");
    CHECK_EQ_STR(Scrub("C:\\Users\\Carol\\AppData\\x"), "C:\\Users\\<user>\\AppData\\x");
    CHECK_EQ_STR(Scrub("ux0:data/smg/mp/mp.cfg"), "ux0:data/smg/mp/mp.cfg");
    // lines and edges
    CHECK_EQ_STR(Scrub("a 1.2.3.4\nb 5.6.7.8\n"), "a <address>\nb <address>\n");
    CHECK_EQ_STR(Scrub(""), "");
    {
        char small[8];
        const size_t n = mp_scrub("0123456789abcdef", 16, small, sizeof small);
        CHECK(n == 7 && small[7] == 0);                       // truncated, terminated
        CHECK(mp_scrub("x", 1, small, 0) == 0);               // no room: nothing written
        CHECK(mp_scrub(nullptr, 0, small, sizeof small) == 0 && small[0] == 0);
    }
    // not NUL terminated input: only n bytes are read
    char* exact = static_cast<char*>(malloc(14));
    memcpy(exact, "joined 1.2.3.4", 14);
    char out[64];
    mp_scrub(exact, 14, out, sizeof out);
    CHECK_EQ_STR(out, "joined <address>");
    free(exact);
    // an address cut by the end of the input
    CHECK_EQ_STR(Scrub("a 203.0.113."), "a 203.0.113.");   // (not a whole address: nothing to hide)
    // binary / odd bytes never crash
    {
        std::string junk;
        for (int i = 0; i < 4000; i++) junk += static_cast<char>((i * 131 + 7) & 255);
        char o2[16384];
        mp_scrub(junk.data(), junk.size(), o2, sizeof o2);
        CHECK(strlen(o2) < sizeof o2);
    }
}

// Table-driven: every form of address, name, code and key a log line can hold (review2 #8), with the exact expected output.
struct ScrubCase { const char* in; const char* want; };
static const ScrubCase kScrubTable[] = {
    // addresses glued to a word or key
    {"peer:1.2.3.4:41100", "peer:<address>"},
    {"ip-10.0.0.5 up", "ip-<address> up"},
    {"from=198.51.100.9:3478 ok", "from=<address> ok"},
    {"addr_203.0.113.7", "addr_<address>"},
    {"(203.0.113.7)", "(<address>)"},
    {"relay 203.0.113.7:3478, next", "relay <address>, next"},
    // partial addresses (np_net.cpp AddrLog prints "a.b.x.x:port" for public addresses)
    {"candidates: public 81.12.x.x:41100 (second server: 81.12.x.x:41101)", "candidates: public <address> (second server: <address>)"},
    {"learned 81.12.x.x", "learned <address>"},
    {"peer 81.x.x.x:5000 relay", "peer <address> relay"},
    {"net 10.0.x.x", "net <address>"},
    {"peer:81.12.x.x:41100", "peer:<address>"},
    // IPv6: full, compressed, bracketed with a port, v4-mapped, loopback, link local
    {"v6 2001:0db8:0000:0000:0000:ff00:0042:8329 ok", "v6 <address> ok"},
    {"v6 2001:db8::ff00:42:8329 ok", "v6 <address> ok"},
    {"v6 [2001:db8::1]:41100 ok", "v6 <address> ok"},
    {"v6 [2001:db8:85a3::8a2e:370:7334] ok", "v6 <address> ok"},
    {"v6 ::ffff:192.0.2.1 ok", "v6 <address> ok"},
    {"v6 [::ffff:192.0.2.1]:5000 ok", "v6 <address> ok"},
    {"v6 ::1", "v6 <address>"},
    {"peer:fe80::1ff:fe23:4567:890a", "peer:<address>"},
    // host names without a scheme
    {"server my-galaxy.example.org ok", "server <host> ok"},
    {"connect custom.example.org:8443 failed", "connect <host> failed"},
    {"lookup smg-online.example.workers.dev", "lookup <host>"},
    {"host:nas.lan:41100 up", "host:<host> up"},
    {"cannot resolve ip-10-0-0-5.ec2.internal", "cannot resolve <host>"},
    {"host=custom.example.org", "host=<hidden>"},
    // not hosts: files, versions, dotted words, numbers
    {"file mp.cfg and GameData.bin and log.txt", "file mp.cfg and GameData.bin and log.txt"},
    {"version 3.0.0 proto 3 fps 29.97", "version 3.0.0 proto 3 fps 29.97"},
    {"e.g. this and i.e. that", "e.g. this and i.e. that"},
    // key: value and JSON
    {"token: abc123", "token: <hidden>"},
    {"Token = abc123", "Token = <hidden>"},
    {"NAME=Alice B look=1", "NAME=<hidden> look=1"},
    {"name: Bob Smith", "name: <hidden>"},
    {"{\"token\":\"abc123\",\"ok\":true}", "{\"token\":\"<hidden>\",\"ok\":true}"},
    {"{\"name\": \"Bob Smith\", \"slot\": 2}", "{\"name\": \"<hidden>\", \"slot\": 2}"},
    {"{\"code\":\"KQ7M-2XZA\"}", "{\"code\":\"<hidden>\"}"},
    {"{\"code\":\"room_full\"}", "{\"code\":\"room_full\"}"},
    {"{\"friend\":\"KQ7M-2XZA\",\"room\":\"ABC234\"}", "{\"friend\":\"<hidden>\",\"room\":\"<hidden>\"}"},
    {"{\"console_key\":\"00112233445566778899aabbccddeeff\"}", "{\"console_key\":\"<hidden>\"}"},
    {"code: KQ7M-2XZA", "code: <hidden>"},
    {"host=Bob's Wii mode=1", "host=<hidden> mode=1"},
    {"scan: room name=Bob Smith mode=1 players=2 compatible=1", "scan: room name=<hidden> mode=1 players=2 compatible=1"},
    {"scan: room incompatible name=Bob Smith proto=2 fingerprint=0a0b0c0d", "scan: room incompatible name=<hidden> proto=2 fingerprint=0a0b0c0d"},
    {"slot=1 session=1a2b3c4d host=0 epoch=0 players=2", "slot=1 session=1a2b3c4d host=0 epoch=0 players=2"},
    {"peer=Zed", "peer=<hidden>"},
    // blobs: hex, base64, base64url, bearer, the console key
    {"key blob 00112233445566778899aabbccddeeff end", "key blob <secret> end"},
    {"blob 0011223344556677889900aabbccddeeff0011 end", "blob <secret> end"},
    {"blob dGhpcyBpcyBhIHNlY3JldCB0b2tlbiB2YWx1ZQ== end", "blob <secret> end"},
    {"blob dGhpcy+pcyBhIHNlY3JldC/0b2tlbiB2YWx1ZXMx end", "blob <secret> end"},
    {"blob Zm9vYmFyMDEyMzQ1Njc4OWFiY2RlZmdoaWprbG1ub3A end", "blob <secret> end"},
    {"Authorization: Bearer abcDEF123", "Authorization: Bearer <secret>"},
    {"bearer abcDEF123xyz", "bearer <secret>"},
    {"console_key=00112233445566778899aabbccddeeff", "console_key=<hidden>"},
    {"id 00112233445566778899aabbccddeeff.", "id <secret>."},
    // paths are not blobs
    {"ux0:data/smg/saves/title/00010000/524d4750/data/GameData.bin", "ux0:data/smg/saves/title/00010000/524d4750/data/GameData.bin"},
    // friend and room codes in URLs and paths
    {"GET https://smg.example.org/v1/friends/KQ7M-2XZA/add ok", "GET https://<host> ok"},
    {"GET /v1/friends/KQ7M-2XZA/add ok", "GET /v1/friends/<code>/add ok"},
    {"GET /v1/rooms/ABC234 ok", "GET /v1/rooms/<code> ok"},
    {"POST smg.example.org/v1/friends/KQ7M2XZA", "POST <host>/v1/friends/<code>"},
    {"?friend=KQ7M-2XZA&room=ABC234", "?friend=<hidden>"},
    // the words "name" and "key" elsewhere stay
    {"filename=a.txt model=Mario hostname keyboard open kind=name", "filename=a.txt model=Mario hostname keyboard open kind=name"},
    {"hello rejected reason=full name=", "hello rejected reason=full name="},
    // an account number after profile / pid / account / uid / player, with or without '=' or ':' (the 18 Nov leak: "login ok: profile 100003")
    {"login ok: profile 100003", "login ok: profile <hidden>"},
    {"pid 4711", "pid <hidden>"},
    {"account 123456 ok", "account <hidden> ok"},
    {"uid 99999", "uid <hidden>"},
    {"player 1234", "player <hidden>"},
    {"PROFILE:100003", "PROFILE:<hidden>"},
    {"Pid=4711 slot=1", "Pid=<hidden> slot=1"},
    {"account : 123456789012", "account : <hidden>"},
    {"uid=1234567890123 x", "uid=<hidden> x"},
    {"login ok: profile 100003, player 4242.", "login ok: profile <hidden>, player <hidden>."},
    // not account numbers: short counts, words, other keys
    {"player 12 left, profile 123 ok", "player 12 left, profile 123 ok"},
    {"pid4711 uid x account of 55555", "pid4711 uid x account of 55555"},
    {"players 12345 slot 12345", "players 12345 slot 12345"},
};

static void testScrubTable() {
    CHECK(sizeof kScrubTable / sizeof kScrubTable[0] >= 50);
    for (const ScrubCase& c : kScrubTable) {
        const std::string got = Scrub(c.in);
        if (got != c.want) { ++sFail; fprintf(stderr, "FAIL scrub table\n  in:   %s\n  got:  %s\n  want: %s\n", c.in, got.c_str(), c.want); }
    }
}

// Fuzz: secrets planted in random places of random lines (in every written form that can carry them) never survive. Deterministic seed.
// A secret class decides which forms can find it: a plain code or a name has no marker of its own, only a key or a word in front of it.
//   a address / host name / blob: found anywhere   f hyphenated friend code: anywhere   c plain room or friend code: after a code-like key or word
//   n name: after a name-like key   t short secret: after a token-like key
static void testScrubFuzz() {
    struct Sec { const char* text; char cls; };
    const Sec secrets[] = {{"KQ7M-2XZA", 'f'}, {"ABCD-EFGH", 'f'}, {"KQ7M2XZA", 'c'}, {"ABC234", 'c'}, {"Zelda Fan 77", 'n'}, {"Bob", 'n'}, {"203.0.113.77", 'a'},
                           {"2001:db8:85a3::8a2e:370:7334", 'a'}, {"81.12.x.x", 'a'}, {"81.12.x.x:41100", 'a'}, {"::ffff:192.0.2.1", 'a'}, {"hunter2hunter2", 't'},
                           {"Zm9vYmFyMDEyMzQ1Njc4OWFiY2RlZmdoaWprbG1ub3A", 'a'}, {"00112233445566778899aabbccddeeff", 'a'}, {"dGhpcy+pcyBhIHNlY3JldC/0b2tlbiB2YWx1ZXMx", 'a'},
                           {"my-galaxy.example.org", 'a'}, {"nas-77.example.org:8443", 'a'}, {"ip-10-0-0-5.ec2.internal", 'a'}};
    struct Form { const char* fmt; char tag; };   // tag: c code-like key, n name-like key, t token-like key, g generic text
    const Form forms[] = {{"code=%s", 'c'}, {"friend=%s", 'c'}, {"room=%s", 'c'}, {"joining %s,", 'c'}, {"GET /v1/x/%s/y", 'c'}, {"code: %s", 'c'},
                          {"{\"code\":\"%s\"}", 'c'}, {"{\"friend\":\"%s\"}", 'c'}, {"name=%s", 'n'}, {"name: %s", 'n'}, {"{\"name\":\"%s\"}", 'n'}, {"host=%s", 'n'},
                          {"peer=%s", 'n'}, {"Name=%s", 'n'}, {"token=%s", 't'}, {"console_key=%s", 't'}, {"token: %s", 't'}, {"{\"token\":\"%s\"}", 't'},
                          {"key=%s", 't'}, {"Authorization: Bearer %s", 't'}, {"peer:%s", 'g'}, {"ip-%s", 'g'}, {"addr=%s", 'g'}, {"(%s)", 'g'}, {"at %s.", 'g'},
                          {"from %s", 'g'}, {"[%s]:5000", 'g'}, {"<%s>", 'g'}, {"x%sy", 'g'}};
    const char* const filler[] = {"slot 2 joined", "status guest slot=1 host=0 epoch=0 players=2 ping=31", "ui: screen S_LOBBY", "save writeback mp=1 sp=2 failed: io",
                                  "online: join started", "version 3.0.0 fps 29.97", "12:34:56 ok"};
    unsigned rng = 12345;
    auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    int cases = 0;
    for (int n = 0; n < 8000; n++) {
        const Sec& sec = secrets[next() % (sizeof secrets / sizeof secrets[0])];
        const Form& form = forms[next() % (sizeof forms / sizeof forms[0])];
        bool ok;
        switch (sec.cls) {
            case 'a': ok = true; break;
            case 'f': ok = true; break;
            case 'c': ok = form.tag != 'g'; break;
            case 'n': ok = form.tag == 'n'; break;
            default: ok = form.tag == 't' || form.tag == 'n'; break;
        }
        const bool blob = strlen(sec.text) >= 32 && !strchr(sec.text, '.');
        if (strcmp(form.fmt, "x%sy") == 0 && !blob) ok = false;                                  // glued to letters: only blobs are found
        if ((strcmp(form.fmt, "[%s]:5000") == 0 || strcmp(form.fmt, "<%s>") == 0) && strchr(sec.text, ':') != nullptr) ok = false;
        if (strcmp(form.fmt, "from %s") == 0 && strchr(sec.text, ':') != nullptr && sec.text[0] != '2') ok = false;
        if (strstr(form.fmt, "Bearer") && sec.cls == 'n') ok = false;
        if (strstr(form.fmt, "/v1/x/") && strchr(sec.text, ':') != nullptr) ok = false;
        if (!ok) continue;
        char piece[200];
        snprintf(piece, sizeof piece, form.fmt, sec.text);
        std::string line;
        const int pre = next() % 3, post = next() % 3;
        for (int i = 0; i < pre; i++) line += std::string(filler[next() % 7]) + " ";
        line += piece;
        for (int i = 0; i < post; i++) line += std::string(" ") + filler[next() % 7];
        const std::string out = Scrub(line);
        ++cases;
        if (out.find(sec.text) != std::string::npos) { ++sFail; fprintf(stderr, "FAIL fuzz leak of \"%s\"\n  in:  %s\n  out: %s\n", sec.text, line.c_str(), out.c_str()); if (sFail > 20) return; }
    }
    CHECK(cases > 2000);
}

// A long sample log: nothing identifying survives, the useful words do.
static void testReport() {
    std::string log =
        "[vgx] frame 1234 ok 60 fps\n"
        "[mp] config: name=Alice Wonder look=1 conn=0\n"
        "[mp] service signin ok code=KQ7M-2XZA build=1a2b3c4d\n"
        "[mp] direct: stun public 203.0.113.7:50000\n"
        "[np-net] candidates: public 203.0.113.7:50000 (second server: 203.0.113.7:50001), local 192.168.1.20:41100, NAT cone\n"
        "[mp] slot 1 joined name=Bob rejoin=0 look=2 players=2\n"
        "[mp] joined slot=1 session=0a0b0c0d host=0 epoch=0 players=2\n"
        "[net-https] connect https://smg-online.example.workers.dev ok\n"
        "[mp] save writeback mp=1 sp=2 failed: io backup=/home/alex/saves/b.bin\n"
        "[mp] console_key = 00112233445566778899aabbccddeeff\n"
        "[mp] status guest slot=1 host=0 epoch=0 players=2 ping=31 bad=0 gaps=0 stray=0\n";
    const char* conntest = "DNS ok\nSTUN ok public 203.0.113.7:50000\nrelay ok 198.51.100.4:3478\nNAT type: cone\n";
    MpReportParts p;
    memset(&p, 0, sizeof p);
    p.appVersion = "3.0.0";
    p.buildTag = "Nov 10 2026 12:00:00";
    p.fingerprint = 0xCAFEF00Du;
    p.protocol = 3;
    p.settings = "look = 1\nconnection = 0\nname = Alice\nserver = https://my.example.org\ncustom_server = yes\n";   // (the caller leaves name and server out; the scrubber is a second guard)
    p.log = log.data();
    p.logLen = log.size();
    p.conntest = conntest;
    p.conntestLen = strlen(conntest);
    char out[16384];
    const size_t n = mp_report_build(out, sizeof out, p);
    const std::string r(out, n);
    CHECK(n == strlen(out));
    for (const char* secret : {"Alice", "Wonder", "KQ7M2XZA", "KQ7M", "2XZA", "203.0.113", "192.168.1", "198.51.100", "Bob", "smg-online", "workers.dev", "alex", "001122334455",
                                "my.example.org"}) {
        if (r.find(secret) != std::string::npos) { ++sFail; fprintf(stderr, "FAIL report contains \"%s\"\n%s\n", secret, r.c_str()); }
    }
    CHECK(r.find("version = 3.0.0") != std::string::npos);
    CHECK(r.find("fingerprint = cafef00d") != std::string::npos);
    CHECK(r.find("protocol = 3") != std::string::npos);
    CHECK(r.find("slot=1 session=0a0b0c0d host=0") != std::string::npos);   // the useful words stay
    CHECK(r.find("status guest slot=1") != std::string::npos);
    CHECK(r.find("NAT type: cone") != std::string::npos);
    CHECK(r.find("frame 1234") == std::string::npos);                       // only multiplayer / network lines
    CHECK(r.find("custom_server = yes") != std::string::npos);
    // no log, no conntest
    MpReportParts q;
    memset(&q, 0, sizeof q);
    q.appVersion = "3.0.0";
    char out2[2048];
    mp_report_build(out2, sizeof out2, q);
    CHECK(strstr(out2, "none yet") != nullptr);
    // a tail that starts mid-line: the first partial line is dropped (it could be cut inside an address)
    const std::string tail = "3.7:50000 half a line\n[mp] status ping=5\n[mp] status second 1.2.3.4\n";
    MpReportParts t;
    memset(&t, 0, sizeof t);
    t.log = tail.data();
    t.logLen = tail.size();
    t.logIsTail = true;
    mp_report_build(out2, sizeof out2, t);
    CHECK(strstr(out2, "half a line") == nullptr && strstr(out2, "ping=5") != nullptr && strstr(out2, "1.2.3.4") == nullptr);
    // too small a buffer: the newest lines are kept, nothing overflows (ASan), result terminated
    std::string many;
    for (int i = 0; i < 400; i++) many += "[mp] status line number " + std::to_string(i) + " from 10.0.0.1\n";
    MpReportParts m;
    memset(&m, 0, sizeof m);
    m.log = many.data();
    m.logLen = many.size();
    char out3[1500];
    const size_t k = mp_report_build(out3, sizeof out3, m);
    CHECK(k < sizeof out3 && out3[k] == 0);
    CHECK(strstr(out3, "line number 399") != nullptr && strstr(out3, "line number 0 ") == nullptr && strstr(out3, "10.0.0.1") == nullptr);
    for (char c : {0}) (void)c;
}

// A realistic log: one line from each family of mp_log / NPN_LOG call sites (grep mp_log), as a Vita would write them, with other people's
// names, room and friend codes and addresses in them. Names of other players must never reach the report, whichever line carries them.
static void testRealisticLog() {
    const char* const names[] = {"Bob Smith", "Zelda Fan 77", "Mia", "Alex's Vita"};
    std::string log =
        "[mp] core ready build=3a4f9c10 proto=3\n"
        "[mp] config: name=Mia look=1 conn=0\n"
        "[mp] service signin ok code=KQ7M-2XZA build=1a2b3c4d\n"
        "[mp] service register start name=Mia\n"
        "[mp] service friend add code=ZZ7M-2XZA\n"
        "[mp] service friend lookup friend=QQ7M-2XZA room=ABC234\n"
        "[mp] service room hosted code=ABC234 mode=1 visibility=0 players=1\n"
        "[mp] service error op=friend status=404 code=not_found\n"
        "[mp] service time queued galaxy=3 star=1 frames=3000 players=2 room=ABC234\n"
        "[mp] service time submit: not in room ABC234 any more: sent again without it\n"
        "[mp] online: join started code=ABC234\n"
        "[mp] online: room code=ABC234\n"
        "[mp] join start conn=online code=ABC234\n"
        "[mp] join start conn=wifi room=Bob Smith\n"
        "[mp] hosting session=0a0b0c0d conn=online code=ABC234\n"
        "[mp] scan: room host=Bob Smith mode=1 players=2 compatible=1\n"
        "[mp] scan: room incompatible host=Zelda Fan 77 proto=2 fingerprint=0a0b0c0d\n"
        "[mp] direct: address 81.12.x.x:41100 source=stun\n"
        "[mp] direct: host on UDP port 41100 (local 192.168.1.20)\n"
        "[mp] upnp: asking the router to forward UDP 41100 (session)\n"
        "[mp] hello sent to 0a000105:41100\n"
        "[mp] hello rejected reason=kicked name=Bob Smith console=0a1b2c3d (by address)\n"
        "[mp] hello rejected reason=full name=Zelda Fan 77\n"
        "[mp] slot 2 joined name=Bob Smith rejoin=0 look=1 players=2\n"
        "[mp] slot 2: Bob Smith is back, the old session goes\n"
        "[mp] roster: slot 3 joined name=Zelda Fan 77\n"
        "[mp] kick list full: Bob Smith (the oldest entry) can join again\n"
        "[mp] auto: joining Zelda Fan 77\n"
        "[mp] auto: tried incompatible room Bob Smith: refused error=3\n"
        "[mp] ui: join room 0a0b0c0d (Bob Smith)\n"
        "[mp] ui: toast \"Bob Smith joined\"\n"
        "[mp] ui: screen S_LOBBY\n"
        "[mp] ui: error shown 4 (room_full) code=room_full\n"
        "[mp] newthing: Mia and Bob Smith say hi\n"
        "[mp] joined slot=1 session=0a0b0c0d host=0 epoch=0 players=2\n"
        "[mp] slot 2 left reason=bye () players=1\n"
        "[mp] status guest slot=1 host=0 epoch=0 players=2 ping=31 bad=0 gaps=0 stray=0\n"
        "[mp] save writeback mp=1 sp=2 backup=/home/alex/saves/b.bin\n"
        "[mp] puppet slot=1 character=mario\n"
        "[mp] bubble on slot=1 lives=3->2 out=0 far=0 target=2 serial=4\n"
        "[mp] flow move type=2 galaxy=HeavenlyPlanet scen=1 serial=3 role=host\n"
        "[mp] watch: the network changed (IP 192.168.1.20 -> 10.0.0.7)\n"
        "[np-net] candidates: public 203.0.113.7:50000 (second server: 203.0.113.7:50001), local 192.168.1.20:41100, NAT cone\n"
        "[np-net] online session started (joining a room), local port 41100, star topology\n"
        "[np-net] cannot resolve smg-online.example.workers.dev\n"
        "[np-net] turn: allocated 198.51.100.4:50000 (our public address as the relay sees it: 203.0.113.7:50000), lifetime 600 s\n"
        "[net-https] connect https://smg-online.example.workers.dev/v1/info ok\n"
        "[vlink] peer 1: learned path [2001:db8::1]:5000 (relay)\n"
        "[vgx] frame 1234 ok 60 fps\n";
    MpReportParts p;
    memset(&p, 0, sizeof p);
    p.appVersion = "3.0.0";
    p.log = log.data();
    p.logLen = log.size();
    char out[16384];
    const size_t n = mp_report_build(out, sizeof out, p);
    const std::string r(out, n);
    for (const char* name : names)
        if (r.find(name) != std::string::npos) { ++sFail; fprintf(stderr, "FAIL report carries the name \"%s\"\n%s\n", name, r.c_str()); }
    for (const char* secret : {"KQ7M", "2XZA", "ABC234", "ZZ7M", "QQ7M", "203.0.113", "192.168.1", "198.51.100", "81.12", "2001:db8", "0a000105", "10.0.0.7", "workers.dev",
                               "/home/alex", "alex"})
        if (r.find(secret) != std::string::npos) { ++sFail; fprintf(stderr, "FAIL report carries \"%s\"\n%s\n", secret, r.c_str()); }
    // what must stay: the useful words
    for (const char* keep : {"core ready build=3a4f9c10", "service error op=friend status=404 code=not_found", "slot 2 joined name=<hidden> rejoin=0 look=1 players=2",
                             "scan: room host=<hidden> mode=1 players=2 compatible=1", "scan: room incompatible host=<hidden> proto=2 fingerprint=0a0b0c0d",
                             "ui: screen S_LOBBY", "ui: error shown 4 (room_full) code=room_full", "joined slot=1 session=0a0b0c0d host=0 epoch=0 players=2",
                             "status guest slot=1 host=0", "flow move type=2 galaxy=HeavenlyPlanet", "bubble on slot=1", "puppet slot=1 character=mario",
                             "direct: address <address> source=stun", "candidates: public <address>", "online session started (joining a room)",
                             "cannot resolve <host>", "turn: allocated <address>", "connect https://<host>", "slot 2 left reason=bye"})
        if (r.find(keep) == std::string::npos) { ++sFail; fprintf(stderr, "FAIL report lost \"%s\"\n%s\n", keep, r.c_str()); }
    // the allowlist: lines that print a person's name, and unknown families, are not in the report at all
    CHECK(r.find("is back") == std::string::npos && r.find("kick list full") == std::string::npos && r.find("auto: joining") == std::string::npos &&
          r.find("auto: tried") == std::string::npos && r.find("join room") == std::string::npos && r.find("toast") == std::string::npos &&
          r.find("newthing") == std::string::npos && r.find("frame 1234") == std::string::npos);
    CHECK(r.find("(7 more multiplayer log lines were left out") != std::string::npos);
    // the host= scan lines also survive a name that is only a hostname-looking word, and a lone "host=" slot number
    CHECK(r.find("host=0") != std::string::npos);
}

// review2 #11: the entry screen's remembered choice. Single Player is only remembered in RAM when it is chosen; the file is written at the flush (the
// file select list is up) or with any other save; a write that fails is silent and not retried; Multiplayer is written at once.
static std::vector<std::string> sEntryLog;
static void EntrySink(const char* line) { sEntryLog.push_back(line); }
static int EntryLogCount(const char* what) {
    int n = 0;
    for (auto& l : sEntryLog) if (l.find(what) != std::string::npos) ++n;
    return n;
}
static std::string ReadAll(const std::string& path) {
    std::string body;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return body;
    char buf[2048];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, k);
    fclose(f);
    return body;
}
static void testEntryChoice() {
    char dir[] = "/tmp/mp_entry_test_XXXXXX";
    if (!mkdtemp(dir)) { ++sFail; return; }
    const std::string d = dir;
    mp_log_set_sink(EntrySink, true);
    mp_config_set_dir((d + "/mp").c_str());
    // Multiplayer: written at once
    mp_entry_remember(1, true);
    CHECK(ReadAll(d + "/mp/mp.cfg").find("entry = 1") != std::string::npos);
    CHECK(mp_config()->entry == 1);
    // Single Player after it: RAM only. The file still says 1; nothing was opened for writing (no "config: saved" line)
    sEntryLog.clear();
    mp_entry_remember(0, false);
    CHECK(mp_config()->entry == 0);
    CHECK(ReadAll(d + "/mp/mp.cfg").find("entry = 1") != std::string::npos);
    CHECK(EntryLogCount("config:") == 0);
    // the flush writes it, once
    mp_entry_flush();
    CHECK(ReadAll(d + "/mp/mp.cfg").find("entry = 0") != std::string::npos);
    CHECK(EntryLogCount("config: saved") == 1);
    mp_entry_flush();
    CHECK(EntryLogCount("config: saved") == 1);               // (nothing waiting: nothing written)
    // the same choice again: nothing to remember, nothing written
    mp_entry_remember(0, false);
    mp_entry_remember(0, true);
    mp_entry_flush();
    CHECK(EntryLogCount("config: saved") == 1);
    // a pending choice is carried by any other save of mp.cfg (and then there is nothing left to flush)
    mp_entry_remember(1, false);
    CHECK(ReadAll(d + "/mp/mp.cfg").find("entry = 0") != std::string::npos);
    CHECK(mp_config_save());
    CHECK(ReadAll(d + "/mp/mp.cfg").find("entry = 1") != std::string::npos);
    sEntryLog.clear();
    mp_entry_flush();
    CHECK(EntryLogCount("config:") == 0);
    // out-of-range values are ignored
    mp_entry_remember(7, true);
    CHECK(mp_config()->entry == 1);
    // a folder that cannot be written (its parent is a file): Single Player's flush fails silently, once, and the RAM choice stays
    { FILE* f = fopen((d + "/notadir").c_str(), "wb"); if (f) fclose(f); }
    mp_config_set_dir((d + "/notadir/mp").c_str());
    sEntryLog.clear();
    mp_entry_remember(0, false);                              // (nothing is written here: no I/O at all)
    CHECK(sEntryLog.empty() && mp_config()->entry == 0);
    mp_entry_flush();                                         // (fails: logged, never reported to the caller)
    CHECK(mp_config()->entry == 0);
    const size_t afterFirst = sEntryLog.size();
    CHECK(afterFirst >= 1 && EntryLogCount("config: saved") == 0);
    mp_entry_flush();
    mp_entry_flush();
    CHECK(sEntryLog.size() == afterFirst);                    // (no retry spam)
    // Multiplayer chosen with the same broken folder: also silent, the menu goes on
    mp_entry_remember(1, true);
    CHECK(mp_config()->entry == 1);
    mp_config_set_dir((d + "/mp").c_str());
    mp_log_set_sink(nullptr, true);
    std::string cmd = "rm -rf " + d;
    if (system(cmd.c_str()) != 0) {}
}

static void testSave() {
    char dir[] = "/tmp/mp_report_test_XXXXXX";
    if (!mkdtemp(dir)) { ++sFail; return; }
    std::string d = dir;
    {
        FILE* f = fopen((d + "/log.txt").c_str(), "wb");
        for (int i = 0; i < 2000; i++) fprintf(f, "[mp] status filler line %d to 192.168.1.%d:41100 name=Zoe\n", i, i % 200);
        fprintf(f, "[mp] status last line ok\n");
        fclose(f);
        mkdir((d + "/mp").c_str(), 0755);
        f = fopen((d + "/mp/conntest.txt").c_str(), "wb");
        fputs("STUN ok 203.0.113.7:3478\n", f);
        fclose(f);
    }
    char path[300];
    CHECK(mp_report_save(d.c_str(), path, sizeof path));
    CHECK_EQ_STR(path, d + "/report.txt");
    FILE* f = fopen(path, "rb");
    CHECK(f != nullptr);
    if (f) {
        std::string body;
        char buf[4096];
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, k);
        fclose(f);
        CHECK(body.find("last line ok") != std::string::npos);
        CHECK(body.find("192.168") == std::string::npos && body.find("Zoe") == std::string::npos && body.find("203.0.113") == std::string::npos);
        CHECK(body.find("filler line 0 ") == std::string::npos);    // only the tail was read
        CHECK(body.size() < 40 * 1024);
        CHECK(body.find("STUN ok <address>") != std::string::npos);
    }
    // writing twice replaces the file
    CHECK(mp_report_save(d.c_str(), nullptr, 0));
    // a missing folder: false, nothing crashes
    CHECK(!mp_report_save("/nonexistent-dir-for-test", path, sizeof path));
    std::string cmd = "rm -rf " + d;
    if (system(cmd.c_str()) != 0) {}
}

int main() {
    testCompare();
    testNoteAndOnce();
    testScan();
    testScrub();
    testScrubTable();
    testScrubFuzz();
    testReport();
    testRealisticLog();
    testEntryChoice();
    testSave();
    if (sFail) { fprintf(stderr, "release3_test: %d FAILED\n", sFail); return 1; }
    printf("release3_test: ok\n");
    return 0;
}
