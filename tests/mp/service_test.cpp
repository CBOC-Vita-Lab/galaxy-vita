// Host test of platform/src/mp/mp_service.cpp against the local fake service (server/fake/server.mjs, GAME=smg).
// Run through tests/mp/service_test.sh (builds with ASan/UBSan, starts the fake, runs this, stops the fake).
// Two accounts on one machine: each lives in its own SMG_ONLINE_DIR; mp_service_shutdown() forgets the cached one between switches.
#include "mp_service.h"
#include "port/mp_invite.h"
#include "port/mp_direct_code.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

static int g_fail = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } \
    } while (0)

static std::string g_root;

#ifdef SMG_E2E
// core2 e2e (tests/mp/service_e2e_test.sh): built with the build wiring's own flags (cmake/MpBuildDefs.cmake: NP_ONLINE_URL = the
// local fake, MP_BUILD_ID = e2e); SMG_ONLINE_URL is never set, so every request goes to the address compiled in. The service layer
// hands this console's code to the session (the core is not linked here: this stands in for it).
#include "mp_config.h"
static std::string g_ownCode;
static int g_ownCodeCalls = 0;
// (C linkage like the real definition in mp_core.cpp, port/mp_core_game.h: a C++ stub here hid a C++-weak reference to the mangled name)
extern "C" void mp_core_set_own_friend_code(const char* code) { g_ownCode = code ? code : ""; g_ownCodeCalls++; }
#define RESTORE_URL() unsetenv("SMG_ONLINE_URL")
#else
#define RESTORE_URL() setenv("SMG_ONLINE_URL", savedUrl.c_str(), 1)
#endif

// integ: the online transport's configuration as mp_svc_signin_poll hands it over (net/ is not linked here)
static std::string g_cfgUrl, g_cfgToken, g_cfgApp, g_cfgBuild;
extern "C" void NpNetConfigureService(const char* baseUrl, const char* token, const char* appVersion, const char* buildHash) {
    g_cfgUrl = baseUrl; g_cfgToken = token; g_cfgApp = appVersion; g_cfgBuild = buildHash;
}

static void Use(const char* who) {
    mp_service_shutdown();
    const std::string d = g_root + "/" + who;
    setenv("SMG_ONLINE_DIR", d.c_str(), 1);
}

// Pump until fn() says done (!= -1), at most 10 s.
template <class F> static int WaitFor(F fn) {
    for (int i = 0; i < 1000; ++i) {
        mp_service_pump();
        const int r = fn();
        if (r != -1) return r;
        usleep(10000);
    }
    return -1;
}

static int Board(uint8_t g, uint8_t s, uint8_t which, MpBoardRow* rows, int max) {
    if (!mp_ui_board_request(g, s, which)) return -3;
    return WaitFor([&] { return mp_ui_board_get(rows, max); });
}

static int Friends(MpFriend* rows, int max) {
    if (!mp_ui_friends_request()) return -3;
    return WaitFor([&] { return mp_ui_friends_get(rows, max); });
}

// code5: the invite calls: poll until done (1) or failed (-MpInviteError, returned as the positive error number); 0 = never finished
static int InvWait(bool resolve, char* blob = nullptr, int* keepSec = nullptr) {
    for (int i = 0; i < 1000; ++i) {
        mp_service_pump();
        int k = 0;
        const int r = resolve ? mp_svc_invite_resolve_poll(blob, 64) : mp_svc_invite_register_poll(&k);
        if (keepSec && k) *keepSec = k;
        if (r == 1) return 1;
        if (r < 0) return -r + 100;   // (an error: 100 + its number)
        usleep(10000);
    }
    return 0;
}

int main() {
#ifdef SMG_E2E
    unsetenv("SMG_ONLINE_URL");
    const std::string savedUrl = NP_ONLINE_URL;
    printf("service e2e: build id %s, the build's own service %s\n", MP_BUILD_ID, NP_ONLINE_URL);
#else
    const char* url = getenv("SMG_ONLINE_URL");
    if (!url || !*url) { fprintf(stderr, "set SMG_ONLINE_URL (service_test.sh does)\n"); return 2; }
    const std::string savedUrl = url;
#endif
    char tmpl[] = "/tmp/mp_service_test.XXXXXX";
    g_root = mkdtemp(tmpl);
#ifdef SMG_E2E
    setenv("PETARI_DATA_DIR", g_root.c_str(), 1);   // (mp.cfg of this run: the `server` key part at the end)
    Use("a");
    CHECK(mp_service_enabled());   // the build's own address, nothing else named
#else

    // 1. online off: no URL
    unsetenv("SMG_ONLINE_URL");
    Use("a");
    CHECK(!mp_service_enabled());
    CHECK(!mp_ui_register("Player"));
    CHECK(mp_service_last_error() == MP_ERR_SERVICE_DOWN);
    CHECK(mp_ui_friends_get(nullptr, 0) == -2);
    setenv("SMG_ONLINE_URL", savedUrl.c_str(), 1);
    CHECK(mp_service_enabled());
#endif

    // 2. names are checked locally, before any request
    const char* bad[] = {"K", "Player Playe", "-Player", "Pla  yer", "Play/er", "..", nullptr, "Player\xc3\xa9"};
    for (const char* b : bad) {
        CHECK(!mp_ui_register(b));
        CHECK(mp_service_last_error() == MP_ERR_BAD_NAME);
    }
    CHECK(mp_service_register_state() == 0);

    // 3. register A (Player)
    MpProfile pa;
    mp_ui_profile(&pa);
    CHECK(!pa.registered);
    CHECK(mp_ui_register("  Player "));
    CHECK(!mp_ui_register("Player"));   // one at a time
    CHECK(WaitFor([] { return mp_service_register_state(); }) == 1);
    mp_ui_profile(&pa);
    CHECK(pa.registered && !strcmp(pa.name, "Player") && strlen(pa.friendCode) == 9 && pa.friendCode[4] == '-');
    struct stat st;
    CHECK(stat((g_root + "/a/online.dat").c_str(), &st) == 0 && st.st_size == 112);
    char cu[256], ct[64];
    CHECK(mp_service_credentials(cu, sizeof cu, ct, sizeof ct) && !strcmp(cu, savedUrl.c_str()) && strlen(ct) == 43);

    // 4. register B in its own folder. Names are display names now (service review): "PLAYER" is fine next to "Player"; then B's
    // profile is renamed to Luigi and the service follows quietly at the next mp_service_ensure_account
    Use("b");
    MpProfile pb;
    mp_ui_profile(&pb);
    CHECK(!pb.registered);
    CHECK(mp_ui_register("PLAYER"));
    CHECK(WaitFor([] { return mp_service_register_state(); }) == 1);
    mp_ui_profile(&pb);
    CHECK(pb.registered && !strcmp(pb.name, "PLAYER") && strcmp(pb.friendCode, pa.friendCode) != 0);
    mp_service_set_name("Luigi");
    CHECK(mp_service_ensure_account() == 1);
    CHECK(WaitFor([&] { mp_ui_profile(&pb); return strcmp(pb.name, "Luigi") ? -1 : 1; }) == 1);
    mp_service_set_name("Admin");   // refused (reserved; svcfix2 item 7: the game's copy of the filter says so first): the character's name, no error
    CHECK(mp_service_ensure_account() == 1);
    CHECK(WaitFor([&] { mp_ui_profile(&pb); return strcmp(pb.name, "Mario") ? -1 : 1; }) == 1);
    mp_service_set_name("Luigi");
    CHECK(mp_service_ensure_account() == 1);
    CHECK(WaitFor([&] { mp_ui_profile(&pb); return strcmp(pb.name, "Luigi") ? -1 : 1; }) == 1);
    mp_service_set_name("Luigi");

    // 5. friends both ways: B adds A, A adds B (codes typed loosely)
    CHECK(!mp_ui_friend_add("NOPE"));
    std::string loose = std::string(pa.friendCode).substr(0, 4) + std::string(pa.friendCode).substr(5);
    for (char& c : loose) c = char(tolower(c));
    CHECK(mp_ui_friend_add(loose.c_str()));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);
    MpFriend fr[8];
    CHECK(Friends(fr, 8) == 1);
    CHECK(!strcmp(fr[0].name, "Player") && !fr[0].online);   // not mutual yet: no presence

    Use("a");
    mp_ui_profile(&pa);
    CHECK(pa.registered && !strcmp(pa.name, "Player"));        // from the file
    CHECK(mp_ui_friend_add(pb.friendCode));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);
    CHECK(WaitFor([] { return mp_ui_friends_get(nullptr, 0); }) >= 0);   // the automatic refresh after the add
    int n = Friends(fr, 8);
    CHECK(n == 1);
    CHECK(n == 1 && !strcmp(fr[0].name, "Luigi") && !strcmp(fr[0].code, pb.friendCode) && fr[0].online && !fr[0].joinable);   // presence: B called < 60 s ago

    // 5b. integ: MpCore's hooks. Sign-in configures the online transport with the account and the build tag; quick match finds
    // no public room ("" = host one); a friend who is in no room is ROOM_NOT_FOUND; a bad friend code is refused at once.
    mp_service_set_build(0x1234abcdu);
    CHECK(mp_svc_signin_poll() == 1);
    CHECK(g_cfgApp == "3.0.0" && g_cfgBuild == "1234abcd" && g_cfgToken.size() == 43 && !g_cfgUrl.empty());
    // netport31 (Strikers onmatch23: their match search asked a different address than their sign-in, "the online service is not set up"):
    // the rooms / signaling get the very address the sign-in and every account request use, never one of their own
    {
        char u[256], t[64];
        CHECK(mp_service_credentials(u, sizeof u, t, sizeof t) && g_cfgUrl == u && g_cfgToken == t);
        CHECK(g_cfgUrl == savedUrl);
    }
    char code[16] = "x";
    CHECK(mp_svc_room_lookup_begin(0, MP_MODE_TOGETHER, ""));
    CHECK(WaitFor([&] { int r = mp_svc_room_lookup_poll(code, sizeof code); return r == 0 ? -1 : r; }) == 1 && code[0] == 0);
    CHECK(mp_svc_room_lookup_begin(1, 0, pb.friendCode));
    CHECK(WaitFor([&] { int r = mp_svc_room_lookup_poll(code, sizeof code); return r == 0 ? -1 : r; }) == -int(MP_ERR_ROOM_NOT_FOUND));
    CHECK(!mp_svc_room_lookup_begin(1, 0, "NOPE"));
    mp_service_set_build(0);   // (the rest of the test as before: no build tag)
    g_cfgBuild.clear();

    // 6. star times: A 4000 frames, B 3900 frames (galaxy 5, star 2); the boards
    uint8_t digest[16];
    for (int i = 0; i < 16; ++i) digest[i] = uint8_t(i * 17);
    mp_race_submit(5, 2, 4000, digest);
    MpBoardRow rows[20];
    int got = -1;
    for (int tries = 0; tries < 50 && got < 1; ++tries) got = Board(5, 2, 0, rows, 20);
    CHECK(got == 1 && rows[0].frames == 4000 && rows[0].you && rows[0].rank == 1 && rows[0].players == 1 && !rows[0].verified);
    CHECK(!mp_ui_board_request(5, 0, 0));    // star 0 does not exist
    Use("b");
    mp_race_submit(5, 2, 3900, nullptr);
    got = -1;
    for (int tries = 0; tries < 50 && got < 2; ++tries) got = Board(5, 2, 0, rows, 20);
    CHECK(got == 2);
    CHECK(got == 2 && rows[0].rank == 1 && !strcmp(rows[0].name, "Luigi") && rows[0].you && rows[1].rank == 2 && !strcmp(rows[1].name, "Player") && !rows[1].you);
    CHECK(Board(5, 2, 2, rows, 20) == 2);    // friends board: B and A are mutual
    CHECK(Board(5, 2, 1, rows, 1) == 1);     // near me, capped by max
    mp_race_submit(5, 2, 100, nullptr);      // faster than the 10 s floor: refused by the service, dropped (no retry)
    for (int i = 0; i < 100; ++i) { mp_service_pump(); usleep(10000); }
    CHECK(mp_service_last_error() == MP_ERR_INTERNAL && !strcmp(mp_service_last_code(), "bad_time"));

    // 7. remove: B removes A; B's list is empty after the refresh
    CHECK(mp_ui_friend_remove(pa.friendCode));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);
    CHECK(Friends(fr, 8) == 0);

    // 7b. incoming requests (MP3): A's request to B is listed; dismiss drops it; block stops further requests and presence
    MpFriend inc[8];
    CHECK(mp_ui_incoming_get(inc, 8) == 1);
    CHECK(!strcmp(inc[0].code, pa.friendCode) && !strcmp(inc[0].name, "Player") && !inc[0].online);
    CHECK(!mp_ui_friend_dismiss("NOPE"));                    // a bad code is refused at once
    CHECK(!mp_ui_friend_block(pb.friendCode));               // own code
    CHECK(mp_ui_friend_dismiss(pa.friendCode));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);
    mp_service_pump();
    CHECK(WaitFor([&] { return mp_ui_incoming_get(inc, 8); }) == 0);
    Use("a");                                                // A asks again, B blocks A
    CHECK(mp_ui_friend_add(pb.friendCode));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);
    Use("b");
    CHECK(Friends(fr, 8) == 0);
    CHECK(mp_ui_incoming_get(inc, 8) == 1);
    CHECK(mp_ui_friend_block(pa.friendCode));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);
    mp_service_pump();
    CHECK(WaitFor([&] { return mp_ui_incoming_get(inc, 8); }) == 0);
    Use("a");                                                // A tries once more: accepted silently, nothing arrives
    CHECK(mp_ui_friend_add(pb.friendCode));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);
    Use("b");
    CHECK(Friends(fr, 8) == 0);
    CHECK(mp_ui_incoming_get(inc, 8) == 0);
    CHECK(mp_ui_friend_unblock(pa.friendCode));
    CHECK(WaitFor([] { return mp_service_friend_op_state(); }) == 1);

    // 7b. code5: a 5-character invite. A (the host) seals its address and registers; B resolves it and opens the answer; a wrong code, a closed
    // code, the host's own code and a second registration are told apart; nothing about the address crosses the service in clear
    {
        Use("a");
        const uint8_t secret[MP_DIRECT_SECRET_BYTES] = {9, 8, 7, 6, 5};
        const uint8_t nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        char blob[MP_INVITE_BLOB_CHARS + 1];
        CHECK(mp_invite_seal("KCBC4", 0x0A000203u, 9001, secret, nonce, blob) == MP_INVITE_BLOB_CHARS);
        CHECK(mp_svc_invite_register_begin("kc-bc4", blob));   // (typed loosely)
        int keep = 0;
        CHECK(InvWait(false, nullptr, &keep) == 1 && keep > 0);
        Use("b");
        char got[64] = {0};
        CHECK(mp_svc_invite_resolve_begin("kcbc4") && InvWait(true, got) == 1 && !strcmp(got, blob));
        uint32_t ip = 0;
        uint16_t port = 0;
        uint8_t s2[MP_DIRECT_SECRET_BYTES] = {0};
        CHECK(mp_invite_open("KCBC4", got, &ip, &port, s2) == MP_CODE_OK && ip == 0x0A000203u && port == 9001 && !memcmp(s2, secret, 5));
        CHECK(mp_invite_open("KCBC5", got, &ip, &port, s2) != MP_CODE_OK);   // (another code does not open it)
        CHECK(mp_svc_invite_resolve_begin("QQQQ2") && InvWait(true, got) == 100 + MP_INV_WRONG);   // (no such code)
        CHECK(!mp_svc_invite_resolve_begin("KC0C4"));   // (a look-alike: no request at all)
        Use("a");
        CHECK(mp_svc_invite_resolve_begin("KCBC4") && InvWait(true, got) == 100 + MP_INV_WRONG);   // (its own code: not a friend's)
        mp_svc_invite_keep("KCBC4");
        mp_svc_invite_close("KCBC4");
        for (int i = 0; i < 50; i++) { mp_service_pump(); usleep(10000); }
        Use("b");
        CHECK(mp_svc_invite_resolve_begin("KCBC4") && InvWait(true, got) == 100 + MP_INV_EXPIRED);   // (the host closed it: "expired", not "wrong")
        Use("a");
        CHECK(mp_svc_invite_register_begin("KCBC4", blob) && InvWait(false) == 1);   // (a dead code is free again)
        Use("b");
        CHECK(mp_svc_invite_register_begin("KCBC4", blob) && InvWait(false) == 100 + MP_INV_TAKEN);   // (a live one is not: pick another)
        Use("a");
        CHECK(mp_svc_invite_register_begin("M7R2S", blob) && InvWait(false) == 1);   // (one live code to an account: this replaces the other)
        mp_svc_invite_close("M7R2S");
        for (int i = 0; i < 50; i++) { mp_service_pump(); usleep(10000); }
    }

    // 8. a damaged account file is refused (CRC), never half-used
    Use("a");
    {
        const std::string p = g_root + "/a/online.dat";
        FILE* f = fopen(p.c_str(), "r+b");
        CHECK(f != nullptr);
        // one token character swapped for another valid one: only the CRC can tell
        if (f) { fseek(f, 20, SEEK_SET); const int c = fgetc(f); fseek(f, 20, SEEK_SET); fputc(c == 'A' ? 'B' : 'A', f); fclose(f); }
    }
    mp_ui_profile(&pa);
    CHECK(!pa.registered);
    CHECK(!mp_ui_friends_request());

    // 9. service review: sign-up without a screen. A brand-new console (no account file) goes from "Play with Anyone" to the quick-match
    // lookup with no input: ensure (challenge, proof of work, register) -> sign-in -> lookup. (MpCore itself is not linked here.)
    Use("c");
    mp_service_set_name("Peach");
    int calls = 0, r9 = 0;
    for (; calls < 2000 && (r9 = mp_service_ensure_account()) == 0; ++calls) usleep(5000);
    CHECK(r9 == 1);
    fprintf(stderr, "service_test: silent sign-up took %d ensure calls\n", calls);
    MpProfile pc;
    mp_ui_profile(&pc);
    CHECK(pc.registered && !strcmp(pc.name, "Peach"));
    CHECK(mp_svc_signin_poll() == 1);
    CHECK(mp_svc_room_lookup_begin(0, MP_MODE_RACE, ""));
    CHECK(WaitFor([&] { int r = mp_svc_room_lookup_poll(code, sizeof code); return r == 0 ? -1 : r; }) == 1);
    // a second new console with the SAME name, straight into Friends
    Use("d");
    mp_service_set_name("Peach");
    CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == 1);
    MpProfile pd;
    mp_ui_profile(&pd);
    CHECK(pd.registered && !strcmp(pd.name, "Peach") && strcmp(pd.friendCode, pc.friendCode) != 0);
    CHECK(Friends(fr, 8) == 0);
    // a name the service cannot take at all (non-ASCII console name) becomes the character's name ("Mario" here)
    Use("e");
    mp_service_set_name("\xe3\x83\x94\xe3\x83\xbc\xe3\x83\x81");
    CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == 1);
    MpProfile pe;
    mp_ui_profile(&pe);
    CHECK(!strcmp(pe.name, "Mario"));
    // a wiped account file: a new account, quietly (a new friend code)
    Use("c");
    unlink((g_root + "/c/online.dat").c_str());
    mp_service_shutdown();
    mp_service_set_name("Peach");
    CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == 1);
    MpProfile pc2;
    mp_ui_profile(&pc2);
    CHECK(pc2.registered && strcmp(pc2.friendCode, pc.friendCode) != 0);
    // the service forgot the account (expired / removed): its token is refused (401) and the next ensure registers again, at most twice
    {
        const std::string p = g_root + "/c/online.dat";
        FILE* f = fopen(p.c_str(), "r+b");
        CHECK(f != nullptr);
        if (f) {
            unsigned char buf[112];
            CHECK(fread(buf, 1, 112, f) == 112);
            buf[12] = buf[12] == 'A' ? 'B' : 'A';   // another (well-formed) token: unknown to the service
            uint32_t crc = ~0u;
            for (int i = 0; i < 108; ++i) { crc ^= buf[i]; for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1; }
            crc = ~crc;
            for (int i = 0; i < 4; ++i) buf[108 + i] = uint8_t(crc >> (8 * i));
            fseek(f, 0, SEEK_SET);
            fwrite(buf, 1, 112, f);
            fclose(f);
        }
    }
    mp_service_shutdown();
    mp_service_set_name("Peach");
    CHECK(mp_service_ensure_account() == 1);                   // the file looks fine: no request yet
    CHECK(Friends(fr, 8) == -2);                               // 401: the service does not know it
    CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == 1);
    MpProfile pc3;
    mp_ui_profile(&pc3);
    CHECK(pc3.registered && strcmp(pc3.friendCode, pc2.friendCode) != 0);
    CHECK(Friends(fr, 8) == 0);

#ifdef SMG_E2E
    // the session was told this console's code (the last account used: "a" registered, then others in their own folders)
    CHECK(g_ownCodeCalls > 0);
    {
        Use("e");   // (a fresh account: "a"'s file was spoiled on purpose above)
        CHECK(mp_ui_register("Eve"));
        CHECK(WaitFor([] { return mp_service_register_state(); }) == 1);
        MpProfile p1;
        mp_ui_profile(&p1);
        CHECK(p1.registered && g_ownCode == p1.friendCode);
        // `server = ...` in mp.cfg still wins over the address compiled in: another account file, nothing registered there; the https
        // request is refused by the host test's TLS stub (nothing is contacted)
        MpConfig c = *mp_config();
        snprintf(c.server, sizeof c.server, "%s", "https://override.invalid");
        mp_config_set(&c);
        Use("e");
        MpProfile p2;
        mp_ui_profile(&p2);
        CHECK(!p2.registered && g_ownCode.empty());   // (another address: another account file, none there)
        if (mp_ui_register("Over")) CHECK(WaitFor([] { return mp_service_register_state(); }) != 1);
        CHECK(mp_service_last_error() == MP_ERR_SERVICE_DOWN);
        c.server[0] = 0;
        mp_config_set(&c);
        Use("e");
        mp_ui_profile(&p2);
        CHECK(p2.registered && !strcmp(p2.friendCode, p1.friendCode) && g_ownCode == p1.friendCode);   // back on the build's own service: the same account
    }
#endif

    // Delete My Online Account: the service removes the account, then this console forgets it (the account file and its write leftovers, the cached
    // friend list); nothing else is removed; a console with no account is refused at once; after it the console makes a new account, as one that never signed up
    {
        Use("del");
        CHECK(!mp_service_account_delete());                                   // (no account yet: nothing to delete)
        CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == 1);
        MpProfile pd;
        mp_ui_profile(&pd);
        CHECK(pd.registered);
        const std::string dir = g_root + "/del";
        const std::string file = dir + "/online.dat";
        struct stat st;
        CHECK(stat(file.c_str(), &st) == 0);
        FILE* keep = fopen((dir + "/online.dat.prev").c_str(), "wb");   // (an older credential: not part of this)
        if (keep) { fputs("old", keep); fclose(keep); }
        FILE* bak = fopen((file + ".mpbak").c_str(), "wb");              // (the leftover of the last write: a boot must not bring the account back from it)
        if (bak) { fputs("old", bak); fclose(bak); }
        CHECK(mp_service_account_delete_state() == 0);
        CHECK(mp_service_account_delete());
        CHECK(!mp_service_account_delete());                                // (one at a time)
        CHECK(WaitFor([] { return mp_service_account_delete_state(); }) == 1);
        CHECK(stat(file.c_str(), &st) != 0 && stat((file + ".mpbak").c_str(), &st) != 0);
        CHECK(stat((dir + "/online.dat.prev").c_str(), &st) == 0);          // (nothing else was removed)
        mp_ui_profile(&pd);
        CHECK(!pd.registered && pd.friendCode[0] == 0);
        CHECK(mp_ui_friends_get(fr, 8) == -2);                               // (no cached list)
        CHECK(!mp_service_account_delete());                                // (no account any more)
        mp_service_shutdown();
        CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == 1);   // (a new account, as for a console that never signed up)
        mp_ui_profile(&pd);
        CHECK(pd.registered);
        unlink((dir + "/online.dat.prev").c_str());
    }

    // 10. the daily cap reached (a second fake with REGISTER_MAX_PER_DAY=1): "busy", the same answer without new requests while it
    // backs off, and everything else keeps working offline (no crash, no loop)
    if (const char* capped = getenv("SMG_ONLINE_URL_CAPPED")) {
        setenv("SMG_ONLINE_URL", capped, 1);
        Use("x");
        CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == 1);   // the day's only one
        Use("y");
        const int rc = WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; });
        CHECK(rc == -int(MP_ERR_SERVICE_DOWN) && !strcmp(mp_service_last_code(), "busy"));
        for (int i = 0; i < 50; ++i) CHECK(mp_service_ensure_account() == -int(MP_ERR_SERVICE_DOWN));   // backing off: no request
        CHECK(!strcmp(mp_service_last_code(), "busy"));
        CHECK(!mp_ui_friends_request() && mp_ui_friends_get(fr, 8) == -2);
        MpProfile py;
        mp_ui_profile(&py);
        CHECK(!py.registered);
        // nothing listens here: unreachable is "busy" too
        setenv("SMG_ONLINE_URL", "http://127.0.0.1:9", 1);
        Use("z");
        CHECK(WaitFor([] { int r = mp_service_ensure_account(); return r == 0 ? -1 : r; }) == -int(MP_ERR_SERVICE_DOWN));
        CHECK(!strcmp(mp_service_last_code(), "busy"));
        RESTORE_URL();
    } else {
        fprintf(stderr, "service_test: SMG_ONLINE_URL_CAPPED not set: the daily-cap part was skipped\n");
        CHECK(false);
    }

    mp_service_shutdown();
    if (g_fail) { fprintf(stderr, "service_test: %d check(s) failed\n", g_fail); return 1; }
    fprintf(stderr, "service_test: all checks passed\n");
    return 0;
}
