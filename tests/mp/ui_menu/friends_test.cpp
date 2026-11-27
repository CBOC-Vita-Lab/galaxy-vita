// The menus' host tests: the friends family (flow split of ui_menu_test.cpp; m-friends rework). Shared helpers: common.hpp.
#include <set>

#include "common.hpp"

namespace {
using namespace MpMenu;

MpPanel::Model& M() { static MpPanel::Model m; S().buildModel(&m); return m; }
std::string bigCode() { return U8(M().big); }
std::string titleLine() { return U8(M().title); }
std::string valueOf(int i) { return i < M().rowCount ? U8(M().rows[i].value) : "<none>"; }
int buttonCount() { return M().buttonCount; }
std::string buttonLabel(int i) { return i < M().buttonCount ? U8(M().buttons[i].label) : "<none>"; }
bool hasButton(const char* word) { for (int i = 0; i < M().buttonCount; i++) if (buttonLabel(i) == word) return true; return false; }
bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
MpFriend mk(const char* name, const char* code, bool online = false, bool joinable = false, uint8_t mode = 0) {
    MpFriend f;
    memset(&f, 0, sizeof f);
    snprintf(f.name, sizeof f.name, "%s", name);
    snprintf(f.code, sizeof f.code, "%s", code);
    f.online = online, f.joinable = joinable, f.mode = mode;
    return f;
}
std::string numCode(const char* prefix2, int i) { char b[16]; snprintf(b, sizeof b, "%s%02d-%04d", prefix2, i % 100, i); return b; }
// walk down until the cursor row says `label` (at most `max` presses): the presses it took, -1 if never
int walkTo(const char* label, int max = 400) {
    for (int i = 0; i <= max; i++) {
        if (cursorLabel() == label) return i;
        down();
    }
    return -1;
}
// a session where `others` (name, code or null) are in the room (slots 1..3), played a while, then ended: Played With is noted
struct Other { const char* name; const char* code; };
void playWith(std::initializer_list<Other> others) {
    int k = 1;
    for (int i = 1; i < 4; i++) fake::players[i].present = false, fake::codes[i] = nullptr;
    for (const Other& o : others) {
        fake::players[k].present = true;
        snprintf(fake::players[k].name, sizeof fake::players[k].name, "%s", o.name);
        fake::codes[k] = o.code;
        ++k;
    }
    session(true, 0, MP_UI_PLAYING);
    frames(62);
    fake::active = false, fake::status.phase = MP_UI_IDLE;
    io.answer = DlgState::No;
    frames(10);
    io.answer = DlgState::Yes;
    S().close();
    frames(2);
}
std::string storeName(int i) { return fake::flow.playedWith[i]; }
template <class T, class = void> struct HasCodes : std::false_type {};
template <class T> struct HasCodes<T, std::void_t<decltype(std::declval<T&>().playedCode[0][0])>> : std::true_type {};
}  // namespace

void testRequests() {
    using namespace MpMenu;
    friendsFresh();
    // the first page of Friends: Requests (N) on top when there are any, then the friends (a page holds three rows, the code box above them);
    // Blocked Players is the last row of the list
    toFriends();
    CHECK(S().screen() == S_FRIENDS && S().itemCount() == 3 && S().cursor() == 0);
    CHECK(rowLabel(0) == "Requests (2)" && rowLabel(1) == "Luigi64" && rowLabel(2) == "Star" && M().pages == 2 && M().page == 0);
    friendsFresh(); fake::incomingCount = 0; toFriends();
    CHECK(S().itemCount() == 3 && rowLabel(0) == "Luigi64");
    friendsFresh(); fake::incomingState = -2; toFriends();   // (an older service: no requests, the screen still works)
    CHECK(S().itemCount() == 3 && rowLabel(0) == "Luigi64");
    friendsFresh(); fake::friendsState = 0; fake::incomingCount = 0; toFriends();
    CHECK(S().itemCount() == 2 && S().cursor() == 0 && statusLine().find("No friends") == 0 && rowLabel(0) == "Blocked Players" && rowLabel(1) == "Delete My Online Account");
    friendsFresh(); fake::friendsState = 0; toFriends();   // (requests but no friends yet)
    CHECK(S().itemCount() == 3 && rowLabel(0) == "Requests (2)");   // (Requests, Blocked Players, Delete My Online Account)
    friendsFresh(); fake::incomingCount = 4; toFriends();
    CHECK(rowLabel(0) == "Requests (4)");
    // loading and error: no extra rows; Triangle retries
    friendsFresh(); fake::friendsState = -1; toFriends();
    CHECK(S().screen() == S_FRIENDS && S().itemCount() == 0 && statusLine() == "Loading...");
    friendsFresh(); fake::friendsState = -2; toFriends();
    CHECK(S().itemCount() == 0 && statusLine().find("Couldn't load") == 0);
    fake::friendsState = 3;
    press(3); frames(3);
    CHECK(S().itemCount() == 3);

    // the requests list: names, loading, error, Back
    friendsFresh(); toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_REQUESTS && S().itemCount() == 2 && rowLabel(0) == "Daisy" && rowLabel(1) == "Wario");
    CHECK(valueOf(0) == "5555-6666");
    back(); CHECK(S().screen() == S_FRIENDS);
    ok(); fake::friendsState = -2; frames(3);   // (the list fails to reload)
    CHECK(S().screen() == S_REQUESTS && S().itemCount() == 0 && statusLine().find("Couldn't load") == 0);
    fake::friendsState = 3; press(3); frames(3);
    CHECK(S().screen() == S_REQUESTS && S().itemCount() == 2);

    // Add: the existing friend add with their code, a window, back to the list without them
    friendsFresh(); toFriends(); ok(); ok(); frames(2);
    CHECK(S().screen() == S_REQUEST && S().itemCount() == 3 && titleLine() == "Friend Request" && contains(subtitleLine(), "5555-6666"));
    ok(); frames(4);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "add:5555-6666" && std::string(S().lastDialog()) == "MP_REQ_ACCEPTED");
    CHECK(S().screen() == S_REQUESTS && S().itemCount() == 1 && rowLabel(0) == "Wario");
    CHECK(logHas("ui: request accepted"));
    // Not Now on the last one: no window, back on the Friends screen (no Requests row any more)
    const int opened = io.opened;
    ok();   // (Wario's choices; Not Now is the second button)
    CHECK(S().screen() == S_REQUEST);
    down(1); ok(); frames(4);
    CHECK(fake::ops.size() == 2 && fake::ops[1] == "dismiss:7777-8888" && io.opened == opened && logHas("ui: request dismissed"));
    CHECK(S().screen() == S_FRIENDS && S().itemCount() == 3 && rowLabel(0) == "Luigi64");

    // Block: asks first (the native Yes / No window); No changes nothing
    friendsFresh(); toFriends(); ok(); ok(); down(2); frames(2);
    io.answer = DlgState::No;
    ok(); frames(2);
    CHECK(std::string(S().lastDialog()) == "MP_REQ_BLOCK" && fake::ops.empty() && S().screen() == S_REQUEST);
    io.answer = DlgState::Yes;
    ok(); frames(4);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "block:5555-6666" && std::string(S().lastDialog()) == "MP_REQ_BLOCKED" && logHas("ui: request blocked"));
    CHECK(S().screen() == S_REQUESTS && S().itemCount() == 1 && rowLabel(0) == "Wario");
    // ... and the mistake can be undone: Friends > Blocked Players > Unblock
    back(); CHECK(S().screen() == S_FRIENDS && S().itemCount() == 3);
    down(4); ok(); frames(2);
    CHECK(S().screen() == S_BLOCKED && S().itemCount() == 1 && rowLabel(0) == "Daisy");
    ok(); frames(2);
    CHECK(fake::ops.size() == 2 && fake::ops[1] == "unblock:5555-6666" && std::string(S().lastDialog()) == "MP_UNBLOCKED" && logHas("ui: player unblocked"));
    CHECK(S().itemCount() == 0 && statusLine().find("No one blocked") == 0 && S().screen() == S_BLOCKED);
    // By Code (Triangle): any code, with or without the dash, spaces, in any case; a refused one says so
    fake::kbResult = 1;
    snprintf(fake::kbText, sizeof fake::kbText, "99990000");
    press(3); frames(2);
    CHECK(fake::ops.size() == 3 && fake::ops[2] == "unblock:9999-0000" && std::string(S().lastDialog()) == "MP_UNBLOCKED" && S().screen() == S_BLOCKED);
    fake::opFails = true;
    press(3); frames(2);
    CHECK(std::string(S().lastDialog()) == "MP_REQ_FAILED" && S().screen() == S_BLOCKED);
    fake::kbResult = -1;   // cancelled: nothing happens
    const size_t n = fake::ops.size();
    press(3); frames(2);
    CHECK(fake::ops.size() == n);
    back(); CHECK(S().screen() == S_FRIENDS);

    // a typed code unblocks the listed player too (dash or not), and the list forgets them
    friendsFresh(); toFriends(); ok(); ok(); down(2); ok(); frames(4);
    CHECK(std::string(S().lastDialog()) == "MP_REQ_BLOCKED");
    back(); down(4); ok(); frames(2);
    CHECK(S().screen() == S_BLOCKED && S().itemCount() == 1);
    fake::kbResult = 1;
    snprintf(fake::kbText, sizeof fake::kbText, "55556666");
    press(3); frames(2);
    CHECK(S().itemCount() == 0);

    // refused by the service: a window, the choice stays, nothing is listed as blocked
    friendsFresh(); toFriends(); ok(); ok(); frames(2);
    fake::opFails = true;
    ok(); frames(3);
    CHECK(std::string(S().lastDialog()) == "MP_REQ_FAILED" && S().screen() == S_REQUEST && fake::ops.size() == 1);
    down(1); ok(); frames(3);
    CHECK(std::string(S().lastDialog()) == "MP_REQ_FAILED" && S().screen() == S_REQUEST && fake::ops.size() == 2);
    down(1); ok(); frames(3);   // Block: asked, answered Yes, refused
    CHECK(std::string(S().lastDialog()) == "MP_REQ_FAILED" && S().screen() == S_REQUEST && fake::ops.size() == 3);
    back(); back(); down(4); ok(); frames(2);
    CHECK(S().screen() == S_BLOCKED && S().itemCount() == 0);

    // a lot of blocking: the list keeps every one of them (pages of five), newest first
    friendsFresh();
    for (int i = 0; i < 20; i++) {
        fake::incomingCount = 1;
        snprintf(fake::incoming[0].code, sizeof fake::incoming[0].code, "0000-%04d", i);
        snprintf(fake::incoming[0].name, sizeof fake::incoming[0].name, "P%d", i);
        S().close();
        toFriends(); ok(); ok(); down(2); ok(); frames(4);
    }
    S().close();
    fake::incomingCount = 0;
    toFriends(); down(3); ok(); frames(2);
    CHECK(S().screen() == S_BLOCKED && S().itemCount() == 3 && rowLabel(0) == "P19" && M().pages == 7);
    friendsFresh();
}


// m-friends: the account is made without a screen: every screen here asks mp_service_ensure_account() each frame and shows only
// "Connecting..." while it answers 0, its list when it answers 1, one plain line and Back when it fails
// uxwait: the Friends list asks the service again by itself (15 s, quietly), Start refreshes by hand, an error keeps the list, it stops when idle
void testFriendsSelfRefresh() {
    using namespace MpMenu;
    friendsFresh();
    toFriends();
    frames(4);
    CHECK(S().screen() == S_FRIENDS && rowLabel(1) == "Luigi64" && M().rowCount == 3);
    CHECK(hasButton("Refresh"));
    const int r0 = fake::friendRequests;
    frames(29 * 60);
    CHECK(fake::friendRequests == r0);                       // nothing before the interval
    fake::friends[0] = mk("Luigi64", "1111-2222", true, true, 1);   // (the friend came online meanwhile)
    frames(2 * 60);
    CHECK(fake::friendRequests == r0 + 1);                   // one request, by itself
    CHECK(statusLine().find("Loading") == std::string::npos && M().rowCount == 3);   // (quiet: the rows stay)
    frames(60 * 60);
    CHECK(fake::friendRequests >= r0 + 2 && fake::friendRequests <= r0 + 3);         // about one per 30 s, never faster
    // an error keeps the list and backs off (60 s, not 30)
    press(2);
    { const int rq = fake::friendRequests; for (int i = 0; i < 40 * 60 && fake::friendRequests == rq; i++) step(); CHECK(fake::friendRequests == rq + 1); }   // the next request goes out...
    fake::friendsState = -2;                                                                // ...and the service answers with an error
    press(2); frames(31 * 60);
    CHECK(M().rowCount == 3 && statusLine().find("Couldn't load") == std::string::npos);
    const int r1 = fake::friendRequests;
    frames(25 * 60);                                         // (56 s after the error: still inside the 60 s back-off)
    CHECK(fake::friendRequests == r1);
    fake::friendsState = 3;
    // by hand: Start asks at once (loud: "Loading..."), and no window blocks it
    const int r2 = fake::friendRequests;
    press(7);
    CHECK(fake::friendRequests == r2 + 1);
    press(7);                                                // (a second START within 10 s: only a sound)
    CHECK(fake::friendRequests == r2 + 1);
    frames(5);
    CHECK(S().screen() == S_FRIENDS && M().rowCount == 3);
    // untouched for 3 minutes: it stops; one press wakes it
    frames(4 * 60 * 60);
    const int r3 = fake::friendRequests;
    frames(60 * 60);
    CHECK(fake::friendRequests == r3);
    press(2); frames(2);
    CHECK(fake::friendRequests == r3 + 1);
    // the cursor stays on the same friend when a refresh changes the order
    friendsFresh();
    toFriends();
    frames(4);
    down(2);
    CHECK(cursorLabel() == "Star");
    { MpFriend t = fake::friends[0]; fake::friends[0] = fake::friends[1]; fake::friends[1] = t; }   // Star, Luigi64, Ros
    frames(33 * 60);
    CHECK(rowLabel(1) == "Star" && cursorLabel() == "Star");
    // the hourly ceiling is hard for the screen's own request on entering too: after 30 requests in the hour, entering (and signing in again)
    // asks nothing and shows the rows already there
    friendsFresh();
    toFriends();
    frames(4);
    int made = 0;
    for (int i = 0; i < 40; i++) {
        const int before = fake::friendRequests;
        press(7);
        frames(11 * 60);
        if (fake::friendRequests > before) ++made;
    }
    std::printf("  friends ceiling: %d manual refreshes in 40 presses 11 s apart\n", made);
    CHECK(made >= 20 && made <= 30);
    S().close();
    frames(2);
    const int rq = fake::friendRequests;
    fake::ensureWork = 3;   // (signing in takes a moment again: the entry would ask once it is ready)
    toFriends();
    frames(10);
    CHECK(S().screen() == S_FRIENDS && M().rowCount == 3);
    CHECK(fake::friendRequests == rq);
    // Retry in the error state goes through the pacer too: mashed 20 times in a few seconds it sends one request, and the error line stays
    friendsFresh();
    fake::friendsState = -2;
    toFriends();
    frames(5);
    CHECK(S().screen() == S_FRIENDS && statusLine().find("Couldn't load") == 0);
    const int rr = fake::friendRequests;
    for (int i = 0; i < 20; i++) { press(3); frames(20); }   // (about 7 s in all: under the 10 s gap)
    CHECK(fake::friendRequests <= rr + 1);
    CHECK(statusLine().find("Couldn't load") == 0);
    // a window up: nothing is asked and nothing moves under it
    back(); frames(2);
}

void testFriendsSignIn() {
    // working: only "Connecting...", no rows, no code box, Back is the only button; then the list
    friendsFresh();
    fake::ensureWork = 40;
    toFriends();
    CHECK(S().screen() == S_FRIENDS && S().itemCount() == 0 && statusLine() == "Connecting..." && M().rowCount == 0 && bigCode().empty());
    CHECK(buttonCount() == 1 && buttonLabel(0) == "Back" && M().spinner >= 0);
    const int calls = fake::ensureCalls;
    frames(10);
    CHECK(fake::ensureCalls >= calls + 10);   // (every frame while it answers 0)
    frames(40);
    CHECK(statusLine() != "Connecting..." && S().itemCount() == 3 && bigCode() == "4827 1193" && rowLabel(0) == "Requests (2)");
    // ready at once: nothing but the list, ever
    friendsFresh();
    toFriends();
    CHECK(statusLine() != "Connecting..." && S().itemCount() == 3);
    // Back while connecting is never stuck
    friendsFresh();
    fake::ensureWork = 100000;
    toFriends(); frames(5);
    back();
    CHECK(S().screen() == S_MAIN);

    // each failure: one plain line, no rows, only Back; the service's reason picks the line
    struct { int result; const char* code; const char* word; } why[] = {
        {-static_cast<int>(MP_ERR_SERVICE_DOWN), "busy", "busy right now"},
        {-static_cast<int>(MP_ERR_NO_WIFI), "no_wifi", "Wi-Fi"},
        {-(static_cast<int>(MP_ERR_INTERNAL) + 1), "clock", "date and time"},   // MP_ERR_CLOCK (service2)
        {-static_cast<int>(MP_ERR_UPDATE_REQUIRED), "update", "update the game"},
        {-static_cast<int>(MP_ERR_SERVICE_DOWN), "offline_build", "isn't available yet"},
        {-static_cast<int>(MP_ERR_SERVICE_DOWN), "", "busy right now"},   // (no reason code: busy)
    };
    for (auto& w : why) {
        friendsFresh();
        fake::ensureResult = w.result;
        fake::svcCode = w.code;
        toFriends();
        const std::string line = statusLine();
        if (!contains(line, w.word)) fprintf(stderr, "  sign-in line [%s] for %s\n", line.c_str(), w.code);
        CHECK(S().screen() == S_FRIENDS && contains(line, w.word) && S().itemCount() == 0 && M().rowCount == 0 && bigCode().empty());
        CHECK(buttonCount() == 1 && buttonLabel(0) == "Back");
        CHECK(!contains(line, "ccount") && !contains(line, "egister") && !contains(line, "erver") && !contains(line, "oken") && !contains(line, "sync"));
        // the other keys do nothing here (no dead buttons: Cross, Triangle, Square are not shown, and pressing them opens nothing)
        const int opened = io.opened;
        ok(); press(3); press(4);
        CHECK(S().screen() == S_FRIENDS && !S().keyboardActive() && io.opened == opened);
        // the service answers again (the back-off ended): the list, with no new press
        fake::ensureResult = 1;
        frames(3);
        CHECK(S().itemCount() == 3 && statusLine() != line && !bigCode().empty());
        back(); CHECK(S().screen() == S_MAIN);
    }
    // a failure shows on the other screens too (the service can fail while a list is open): and Back goes up one screen
    struct { const char* name; void (*go)(); Scr scr; Scr up; int rows; } screens[] = {
        {"requests", [] { toFriends(); ok(); }, S_REQUESTS, S_FRIENDS, 2},
        {"blocked", [] { toFriends(); down(4); ok(); }, S_BLOCKED, S_FRIENDS, 0},
        {"played", [] { fake::flow.playedCount = 1; snprintf(fake::flow.playedWith[0], 17, "Ros"); fake::codes[1] = nullptr; toFriends(); ok(); }, S_PLAYED, S_FRIENDS, 0},
        {"board", [] { toBoard(); }, S_BOARD, S_GATHER, 2},
    };
    for (auto& s : screens) {
        friendsFresh();
        s.go();
        if (S().screen() != s.scr) { fprintf(stderr, "  sign-in screen %s ended on %s\n", s.name, screenName(S().screen())); }
        CHECK(S().screen() == s.scr);
        if (s.scr == S_PLAYED) CHECK(S().itemCount() == 0);   // (a name without a code is shown, not choosable)
        else CHECK(S().itemCount() == s.rows);
        fake::ensureResult = -static_cast<int>(MP_ERR_SERVICE_DOWN);
        fake::svcCode = "busy";
        frames(3);
        CHECK(S().screen() == s.scr && S().itemCount() == 0 && contains(statusLine(), "busy right now") && buttonCount() == 1 && buttonLabel(0) == "Back");
        fake::ensureResult = 1;
        frames(3);
        CHECK(!contains(statusLine(), "busy") && (s.scr == S_PLAYED || S().itemCount() == s.rows));
        back();
        CHECK(S().screen() == s.up);
    }
    // the same on entering: working first
    friendsFresh();
    fake::ensureWork = 25;
    toBoard();
    CHECK(S().screen() == S_BOARD && S().itemCount() == 0 && statusLine() == "Connecting...");
    frames(30);
    CHECK(S().itemCount() == 2 && statusLine() != "Connecting...");
    // wire: the times the service answered are on the screen (the poll of mp_ui_board_get was lost once: "Loading..." for ever)
    auto timesShown = [] { static MpPanel::Model m; S().buildModel(&m); return m.rowCount; };
    frames(3);
    const int withTimes = timesShown();
    CHECK(statusLine() != "Loading..." && withTimes > 2);   // (Galaxy and Star rows, then the times)
    fake::boardState = 0;   // none yet: its own line
    frames(3);
    CHECK(statusLine() != "Loading..." && timesShown() < withTimes && !statusLine().empty());
    fake::boardState = -2;  // the service failed: its own line again
    frames(3);
    CHECK(statusLine() != "Loading..." && !statusLine().empty());
    fake::boardState = -1;  // still answering
    frames(3);
    CHECK(statusLine() == "Loading...");
    friendsFresh();
}

// m-friends: names are not unique, the friend code is: two rows with one name both show the tail of their code; nothing says "taken"
void testFriendsDuplicateNames() {
    friendsFresh();
    fake::friends[0] = mk("Mario", "AAAA-1111", true);
    fake::friends[1] = mk("mario", "BBBB-2222");
    fake::friends[2] = mk("Luigi64", "CCCC-3333");
    fake::incomingCount = 0;
    toFriends();
    CHECK(rowLabel(0) == "Mario (1111)" && rowLabel(1) == "mario (2222)" && rowLabel(2) == "Luigi64");   // (not a letter of the code on the one with a name of its own)
    CHECK(valueOf(0) == "Online" && valueOf(1) == "Offline");
    // the friend's page says which one it is
    ok();
    CHECK(S().screen() == S_REQUEST && titleLine() == "Mario" && contains(subtitleLine(), "AAAA 1111"));
    back();
    // requests: the code is the value of each row, the tail is there too
    friendsFresh();
    fake::incoming[0] = mk("Mario", "AAAA-1111"), fake::incoming[1] = mk("Mario", "BBBB-2222"), fake::incomingCount = 2;
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_REQUESTS && rowLabel(0) == "Mario (1111)" && rowLabel(1) == "Mario (2222)" && valueOf(1) == "BBBB-2222");
    // blocked: both rows, told apart
    friendsFresh();
    fake::incoming[0] = mk("Mario", "AAAA-1111"), fake::incoming[1] = mk("Mario", "BBBB-2222"), fake::incomingCount = 2;
    toFriends(); ok(); ok(); down(2); ok(); frames(4);    // blocks AAAA-1111
    CHECK(S().screen() == S_REQUESTS && S().itemCount() == 1 && rowLabel(0) == "Mario");   // (one left: no tail needed)
    ok(); down(2); ok(); frames(4);                        // blocks BBBB-2222
    S().close();
    toFriends(); walkTo("Blocked Players"); ok(); frames(2);
    CHECK(S().screen() == S_BLOCKED && S().itemCount() == 2 && rowLabel(0) == "Mario (2222)" && rowLabel(1) == "Mario (1111)");
    // played with: two players of one name
    friendsFresh();
    playWith({{"Mario", "AAAA-1111"}, {"Mario", "BBBB-2222"}});
    CHECK(fake::flow.playedCount == 2 && storeName(0) == "Mario" && storeName(1) == "Mario");
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && S().itemCount() == 2 && rowLabel(0) == "Mario (2222)" && rowLabel(1) == "Mario (1111)");
    // nothing on these screens says a name is "taken" / "in use" / unique
    friendsFresh();
    fake::friends[0] = mk("Mario", "AAAA-1111"), fake::friends[1] = mk("Mario", "BBBB-2222");
    toFriends();
    for (int lang = 0; lang < MpMsgText::LANG_COUNT; lang++) {
        MpMsgText::setLanguage(lang);
        const MpPanel::Model& m = M();
        std::string all = U8(m.title) + U8(m.hint) + U8(m.status) + U8(m.bigLabel);
        for (int i = 0; i < m.rowCount; i++) all += U8(m.rows[i].label) + U8(m.rows[i].value);
        CHECK(!contains(all, "taken") && !contains(all, "in use") && !contains(all, "unique"));
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    friendsFresh();
}

// Delete My Online Account: a row at the end of the Friends list, only for a console with an online account; Cross asks (Yes / No); Yes asks the
// service; when it says yes the console forgets the account (the port removes the file) and says so; when it cannot, nothing changes and the plain error shows.
static int deleteRow() {
    for (int i = 0; i < S().itemCount(); i++) {
        if (rowLabel(i) == "Delete My Online Account") return i;
    }
    return -1;
}
static void toDeleteRow() {
    for (int tries = 0; tries < 12 && cursorLabel() != "Delete My Online Account"; tries++) {   // (the list pages: Down goes on to the next page)
        down();
        frames(1);
    }
}
void testDeleteAccount() {
    // no account yet (the silent sign-in is still answering): no such row, on any page
    friendsFresh();
    fake::registerState = -1;
    fake::profile.registered = false;
    toFriends();
    CHECK(deleteRow() < 0 && !contains(U8(M().hint), "online account"));
    // an account: the row is the last of the list; its line is one plain sentence
    friendsFresh();
    fake::friendsState = 0;
    toFriends();
    CHECK(S().screen() == S_FRIENDS);
    CHECK(deleteRow() == S().itemCount() - 1);
    toDeleteRow();
    CHECK(cursorLabel() == "Delete My Online Account");
    CHECK(U8(M().hint) == "Removes your online account and friends list.");
    // No (the answer when the window is closed any other way): nothing is sent, nothing changes
    io.answer = DlgState::No;
    ok(); frames(3);
    CHECK(std::string(S().lastDialog()) == "MP_DELETE_ACCOUNT" && fake::acctDeletes == 0 && S().screen() == S_FRIENDS && fake::profile.registered);
    // Yes: the service is asked once; the waiting screen shows until it answers
    io.answer = DlgState::Yes;
    fake::acctDeleteState = -1;
    ok(); frames(3);
    CHECK(fake::acctDeletes == 1 && S().screen() == S_BUSY && fake::profile.registered && MpOverlay::toastCount() == 0);
    frames(30);
    CHECK(S().screen() == S_BUSY && fake::acctDeletes == 1);
    // answered yes: the account is gone (the fake port drops it as the real one removes the file), the toast says so, the menus start over
    fake::profile.registered = false;
    fake::acctDeleteState = 1;
    frames(3);
    CHECK(MpOverlay::toastCount() == 1 && U8(MpOverlay::toasts()[0].text) == "Your online account was deleted.");
    CHECK(S().screen() == S_MAIN && fake::acctDeletes == 1);
    // like a console that never signed up: opening Friends asks for an account again by itself, with no Delete row
    const int ensures = fake::ensureCalls;
    fake::friendsState = 0;
    press(4); frames(3);
    CHECK(S().screen() == S_FRIENDS && fake::ensureCalls > ensures && deleteRow() < 0);   // (the Friends screen asks for an account again; no row for a console without one)
    // the service cannot be reached: nothing changes here, the plain error shows, no toast
    friendsFresh();
    fake::friendsState = 0;
    toFriends();
    toDeleteRow();
    io.answer = DlgState::Yes;
    fake::acctDeleteState = -1;
    ok(); frames(3);
    fake::acctDeleteState = -2;
    frames(3);
    CHECK(fake::profile.registered && S().screen() == S_FRIENDS && MpOverlay::toastCount() == 0 && std::string(S().lastDialog()) == "MP_ERR_SERVICE_DOWN");
    CHECK(fake::acctDeletes == 1);
    // refused at once (no network, busy): the same
    friendsFresh();
    fake::friendsState = 0;
    toFriends();
    toDeleteRow();
    fake::acctDeleteRefused = true;
    ok(); frames(3);
    CHECK(fake::profile.registered && fake::acctDeletes == 1 && S().screen() == S_FRIENDS && MpOverlay::toastCount() == 0 && std::string(S().lastDialog()) == "MP_ERR_SERVICE_DOWN");
    // every language says it, plainly (no technical word)
    for (int lang = 0; lang < MpMsgText::LANG_COUNT; lang++) {
        MpMsgText::setLanguage(lang);
        for (MpText id : {MP_T_DELETE_ACCOUNT, MP_T_DESC_DELETE_ACCOUNT, MP_T_DLG_DELETE_ACCOUNT, MP_T_ACCOUNT_DELETED}) {
            const char16_t* t = MpMsgText::raw(id);
            CHECK(t && t[0]);
            if (lang != MpMsgText::LANG_EN) CHECK(std::u16string(t) != std::u16string(MpMsgText::raw(MP_T_DELETE_ACCOUNT)) || id == MP_T_DELETE_ACCOUNT);
        }
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    friendsFresh();
}

// m-friends: adding a friend by code: spaces, dashes and letter case are forgiven; a code that cannot be one is refused plainly
void testFriendsCodeEntry() {
    friendsFresh();
    toFriends();
    fake::kbResult = 0;
    press(3);   // Triangle: the system keyboard, 12 characters (room for "ABCD - EFGH")
    CHECK(fake::kbMax == 12);
    const char* good[] = {"abcd-efgh", "ABCD EFGH", "abcdefgh", "AbCd - eFgH", " ab cd ef gh", "ABCD-EFGH"};
    for (const char* g : good) {
        friendsFresh();
        toFriends();
        fake::friendOpState = 1;
        fake::kbResult = 1;
        snprintf(fake::kbText, sizeof fake::kbText, "%s", g);
        press(3); frames(2);
        fake::kbResult = 0;
        if (fake::ops.size() != 1 || fake::ops[0] != "add:ABCD-EFGH") fprintf(stderr, "  code [%s] gave %zu ops\n", g, fake::ops.size());
        CHECK(fake::ops.size() == 1 && fake::ops[0] == "add:ABCD-EFGH");
        frames(5);
        CHECK(S().screen() == S_FRIENDS);
    }
    const char* bad[] = {"ABCD", "ABCD-EFGH1", "ABC$-EFGH", "ABCD-EFG", "1234 567", "ab cd ef gh ij", "ABCD_EFGH"};
    for (const char* b : bad) {
        friendsFresh();
        toFriends();
        fake::kbResult = 1;
        snprintf(fake::kbText, sizeof fake::kbText, "%s", b);
        press(3); frames(2);
        fake::kbResult = 0;
        // refused at once and plainly: one window, no request sent, no waiting screen
        if (!fake::ops.empty() || std::string(S().lastDialog()) != "MP_ERR_NO_PLAYER") fprintf(stderr, "  bad code [%s]: %zu ops, window %s\n", b, fake::ops.size(), S().lastDialog());
        CHECK(fake::ops.empty() && std::string(S().lastDialog()) == "MP_ERR_NO_PLAYER" && S().screen() == S_FRIENDS);
    }
    // an empty box is a cancel: nothing at all
    friendsFresh();
    toFriends();
    fake::kbResult = 1;
    fake::kbText[0] = 0;
    press(3); frames(2);
    fake::kbResult = 0;
    CHECK(fake::ops.empty() && !io.open && S().screen() == S_FRIENDS);
    // the service refuses (your own code, no such player, a full list): its own words; a refusal with no reason: the plain sentence
    friendsFresh();
    toFriends();
    fake::opFails = true;
    fake::kbResult = 1;
    snprintf(fake::kbText, sizeof fake::kbText, "abcd efgh");
    press(3); frames(2);
    fake::kbResult = 0;
    CHECK(std::string(S().lastDialog()) == "MP_FRIEND_FAILED" && S().screen() == S_FRIENDS);
    // M1: a code that failed comes back in the keyboard (a typo is fixed, not typed again); a first keyboard is empty; a success clears it
    friendsFresh();
    toFriends();
    fake::kbResult = 0;
    press(3);
    CHECK(fake::kbInitial[0] == 0);                                        // (nothing typed yet)
    fake::kbResult = 1;
    snprintf(fake::kbText, sizeof fake::kbText, "abcd efg");              // a typo: 7 letters, refused at once
    press(3); frames(2);
    fake::kbResult = 0;
    CHECK(fake::ops.empty() && std::string(S().lastDialog()) == "MP_ERR_NO_PLAYER");
    frames(5);                                                             // (the window answers itself in the fake)
    fake::kbResult = 0;
    press(3);
    CHECK(std::string(fake::kbInitial) == "abcd efg");                     // the same text, to be fixed
    fake::opFails = true;                                                  // the service refuses the fixed code
    fake::kbResult = 1;
    snprintf(fake::kbText, sizeof fake::kbText, "abcd efgh");
    press(3); frames(2);
    fake::kbResult = 0;
    CHECK(std::string(S().lastDialog()) == "MP_FRIEND_FAILED");
    frames(5);
    press(3);
    CHECK(std::string(fake::kbInitial) == "abcd efgh");                    // a failed add keeps what was typed
    fake::opFails = false;
    fake::friendOpState = 1;
    fake::kbResult = 1;
    press(3); frames(2);                                                   // (the keyboard answers with the same text: it goes through)
    fake::kbResult = 0;
    frames(5);
    CHECK(fake::ops.size() >= 1 && fake::ops.back() == "add:ABCD-EFGH" && LoggedPart("MP_FRIEND_ADDED"));
    frames(5);
    press(3);
    CHECK(fake::kbInitial[0] == 0);                                        // success: cleared
    // Blocked Players > By Code: the same forgiveness and refusal
    friendsFresh();
    toFriends(); down(4); ok(); frames(2);
    CHECK(S().screen() == S_BLOCKED);
    fake::kbResult = 1;
    snprintf(fake::kbText, sizeof fake::kbText, "wxyz - 2345");
    press(3); frames(2);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "unblock:WXYZ-2345" && fake::kbMax == 12);
    snprintf(fake::kbText, sizeof fake::kbText, "wxyz");
    press(3); frames(2);
    CHECK(fake::ops.size() == 1 && std::string(S().lastDialog()) == "MP_ERR_NO_PLAYER" && S().screen() == S_BLOCKED);
    fake::kbResult = 0;
    friendsFresh();
}

// m-friends: a friend's page: their name, their code, Join (only when they are in a game), Remove, Block
void testFriendsPage() {
    friendsFresh();
    toFriends(); down();   // Luigi64: in a Star Race room
    CHECK(cursorLabel() == "Luigi64" && S().screen() == S_FRIENDS);
    ok(); frames(2);
    CHECK(S().screen() == S_REQUEST && S().itemCount() == 3 && titleLine() == "Luigi64" && contains(subtitleLine(), "1234 5678"));
    CHECK(rowLabel(0) == "Play with This Friend" && rowLabel(1) == "Remove Friend" && rowLabel(2) == "Block");
    CHECK(hasButton("OK") && hasButton("Back") && !hintLine().empty());
    // wire: the page's words are table words in every language (they were English placeholders in all of them)
    {
        const std::string en = rowLabel(1), enHint = hintLine();
        for (int l = 0; l < 5; l++) {
            MpMsgText::setLanguage(l);
            frames(1);
            if (l != 0) CHECK(rowLabel(1) != en && rowLabel(0) != "Play with This Friend" && !hintLine().empty() && hintLine() != enHint);
        }
        MpMsgText::setLanguage(MpMsgText::LANG_EN);
        frames(1);
    }
    // Play goes to the friend's room
    ok(); frames(2);
    CHECK(S().screen() == S_BUSY && fake::status.phase == MP_UI_CONNECTING);
    // a friend who is not in a game: no Join (no dead row)
    friendsFresh();
    toFriends(); down(2);
    CHECK(cursorLabel() == "Star");
    ok(); frames(2);
    CHECK(S().screen() == S_REQUEST && S().itemCount() == 2 && rowLabel(0) == "Remove Friend" && rowLabel(1) == "Block" && contains(subtitleLine(), "2222 3333"));
    // Remove asks first; No keeps them
    io.answer = DlgState::No;
    ok(); frames(2);
    CHECK(std::string(S().lastDialog()) == "MP_FRIEND_REMOVE" && fake::ops.empty() && S().screen() == S_REQUEST);
    io.answer = DlgState::Yes;
    ok(); frames(3);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "remove:2222-3333" && S().screen() == S_FRIENDS);
    // Block asks first; Yes blocks, ends up on the Blocked list, the page closes
    friendsFresh();
    toFriends(); down(2); ok(); frames(2); down();
    CHECK(cursorLabel() == "Block");
    ok(); frames(4);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "block:2222-3333" && std::string(S().lastDialog()) == "MP_REQ_BLOCKED" && S().screen() == S_FRIENDS);
    // Back from the page: the list, same row
    friendsFresh();
    toFriends(); down(); ok(); back();
    CHECK(S().screen() == S_FRIENDS && cursorLabel() == "Luigi64");
    // Square on a friend's row: Remove, as before
    io.answer = DlgState::Yes;
    press(4); frames(3);
    CHECK(std::string(S().lastDialog()) == "MP_FRIEND_REMOVE" && fake::ops.size() == 1 && fake::ops[0] == "remove:1234-5678");
    friendsFresh();
}

// m-friends: Played With: the last players met in any kind of game, newest first; Add Friend when their code is known, Block;
// a friend is marked and has no Add row; a player whose code is not known is shown and not choosable
void testPlayedWith() {
    friendsFresh();
    playWith({{"Ana", "ABCD-1111"}, {"Ben", nullptr}});
    CHECK(fake::flow.playedCount == 2 && storeName(0) == "Ben" && storeName(1) == "Ana");   // (newest first: the later slot is noted last)
    toFriends();
    CHECK(rowLabel(0) == "Played With (2)");
    ok(); frames(2);
    CHECK(S().screen() == S_PLAYED);
    // the player with a code first (a row you can use), the one without after, dim
    CHECK(S().itemCount() == 1 && M().rowCount == 2 && rowLabel(0) == "Ana" && rowLabel(1) == "Ben");
    CHECK((M().rows[1].marks & MpPanel::MARK_STATIC) && (M().rows[1].marks & MpPanel::MARK_DISABLED) && !(M().rows[0].marks & MpPanel::MARK_STATIC));
    CHECK(hasButton("Add Friend") && hasButton("Block") && hasButton("Back"));
    // Add: the code the other console announced
    ok(); frames(4);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "add:ABCD-1111" && LoggedPart("MP_FRIEND_ADDED"));
    CHECK(S().screen() == S_PLAYED);
    // Block: asks, then the service, the row goes, the Blocked list has them
    io.answer = DlgState::No;
    press(4); frames(2);
    CHECK(std::string(S().lastDialog()) == "MP_REQ_BLOCK" && fake::ops.size() == 1 && S().itemCount() == 1);
    io.answer = DlgState::Yes;
    press(4); frames(4);
    CHECK(fake::ops.size() == 2 && fake::ops[1] == "block:ABCD-1111" && std::string(S().lastDialog()) == "MP_REQ_BLOCKED");
    CHECK(S().screen() == S_PLAYED && S().itemCount() == 0 && M().rowCount == 1 && rowLabel(0) == "Ben");   // (Ana is gone from the list)
    // a name with no code: nothing to choose, no keyboard, no request
    const size_t ops = fake::ops.size();
    ok(); frames(2);
    CHECK(fake::ops.size() == ops && !S().keyboardActive());
    back();
    CHECK(S().screen() == S_FRIENDS);

    // already a friend: marked, no Add; their page opens instead
    friendsFresh();
    fake::friends[0] = mk("Luigi64", "1234-5678", true);
    playWith({{"Luigi64", "1234-5678"}, {"Cy", "CYCY-0001"}});
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && S().itemCount() == 2 && rowLabel(0) == "Cy" && rowLabel(1) == "Luigi64");
    CHECK(valueOf(1) == "Friend" && valueOf(0).empty());
    down();
    CHECK(cursorLabel() == "Luigi64" && !hasButton("Add Friend") && !hasButton("Block"));
    ok(); frames(2);
    CHECK(S().screen() == S_REQUEST && titleLine() == "Luigi64" && fake::ops.empty());
    back();
    press(4); frames(2);   // (Square on a friend: nothing to block from here)
    CHECK(fake::ops.empty() && S().screen() == S_PLAYED);

    // the same player is one row (their code), however the name changed; a later session puts them first again
    friendsFresh();
    playWith({{"Ana", "ABCD-1111"}});
    playWith({{"Ben", "BBBB-2222"}});
    playWith({{"Ana2", "ABCD-1111"}});
    CHECK(fake::flow.playedCount == 2 && storeName(0) == "Ana2" && storeName(1) == "Ben");
    // two players of one name are two rows (the code tells them apart)
    friendsFresh();
    playWith({{"Mario", "MARI-0001"}});
    playWith({{"Mario", "MARI-0002"}});
    CHECK(fake::flow.playedCount == 2);
    // a code that arrives later fills in a name noted without one
    friendsFresh();
    playWith({{"Cy", nullptr}});
    CHECK(fake::flow.playedCount == 1);
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && S().itemCount() == 0 && M().rowCount == 1);
    S().close();
    playWith({{"Cy", "CYCY-0001"}});
    CHECK(fake::flow.playedCount == 1);
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && S().itemCount() == 1 && rowLabel(0) == "Cy");
    // the code arrives in the SAME session, after the player was first noted: the entry takes it at once (Add appears)
    friendsFresh();
    for (int i = 1; i < 4; i++) fake::players[i].present = false, fake::codes[i] = nullptr;
    fake::players[1].present = true;
    snprintf(fake::players[1].name, sizeof fake::players[1].name, "%s", "Late");
    session(true, 0, MP_UI_PLAYING);
    frames(62);
    CHECK(fake::flow.playedCount == 1);
    fake::codes[1] = "LATE-0001";
    frames(70);
    fake::active = false, fake::status.phase = MP_UI_IDLE;
    io.answer = DlgState::No; frames(10); io.answer = DlgState::Yes; S().close(); frames(2);
    CHECK(fake::flow.playedCount == 1);
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && S().itemCount() == 1 && rowLabel(0) == "Late" && hasButton("Add Friend"));
    S().close();
    // a code that cannot be one (a stranger's console) is not a code
    friendsFresh();
    playWith({{"Eve", "bad code!!"}});
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && S().itemCount() == 0);

    // played in a game of every kind: the room's kind does not matter (the list comes from who was in the session)
    // more than the store holds: the newest MP_PLAYED_WITH (20 once the store has room: core-wanted)
    friendsFresh();
    for (int i = 0; i < 9; i++) {
        char n0[8], n1[8], n2[8], c0[12], c1[12], c2[12];
        snprintf(n0, sizeof n0, "P%d", i * 3), snprintf(n1, sizeof n1, "P%d", i * 3 + 1), snprintf(n2, sizeof n2, "P%d", i * 3 + 2);
        snprintf(c0, sizeof c0, "PL%02d-0000", i * 3), snprintf(c1, sizeof c1, "PL%02d-0000", i * 3 + 1), snprintf(c2, sizeof c2, "PL%02d-0000", i * 3 + 2);
        static char keep[3][12];
        snprintf(keep[0], 12, "%s", c0), snprintf(keep[1], 12, "%s", c1), snprintf(keep[2], 12, "%s", c2);
        playWith({{n0, keep[0]}, {n1, keep[1]}, {n2, keep[2]}});
    }
    const int cap = MP_PLAYED_WITH < 20 ? MP_PLAYED_WITH : 20;
    CHECK(fake::flow.playedCount == cap);
    CHECK(storeName(0) == "P26" || storeName(0) == "P25" || storeName(0) == "P24");
    if (MP_PLAYED_WITH < 20) fprintf(stderr, "  NOTE: MP_PLAYED_WITH is %d: the cap of 20 and codes that survive a restart wait for core-wanted (MpFlowStore)\n", MP_PLAYED_WITH);
    toFriends(); ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && M().pages == (cap + 2) / 3);
    // 20 distinct entries reachable
    std::set<std::string> seen;
    for (int i = 0; i < cap; i++) {
        seen.insert(cursorLabel());
        down();
    }
    CHECK(static_cast<int>(seen.size()) == cap);

    // kept across restarts: the names are in the store; a code only if the store can hold it
    friendsFresh();
    playWith({{"Ana", "ABCD-1111"}, {"Ben", "BBBB-2222"}});
    S().close();
    S().init(&io);   // (a restart: the menus start again, the store stays)
    io.answer = DlgState::Yes;
    toFriends();
    CHECK(S().screen() == S_FRIENDS && rowLabel(0) == "Played With (2)");
    ok(); frames(2);
    CHECK(S().screen() == S_PLAYED && M().rowCount == 2 && rowLabel(0).size() > 0);
    if (HasCodes<MpFlowStore>::value) CHECK(S().itemCount() == 2);
    else CHECK(S().itemCount() == 0);   // (codes are only in RAM here: after a restart the rows are names only)
    friendsFresh();
}

// m-friends: the service's limits are reachable: 50 friends, 50 requests, 200 blocked players, by scrolling (pages of five, wrapping)
void testFriendsLimits() {
    // 50 friends
    friendsFresh();
    fake::useMany = true;
    for (int i = 0; i < 50; i++) fake::manyFriends.push_back(mk(("F" + std::string(i < 10 ? "0" : "") + std::to_string(i)).c_str(), numCode("FR", i).c_str(), i % 3 == 0));
    fake::incomingCount = 0;
    toFriends();
    CHECK(S().screen() == S_FRIENDS && rowLabel(0) == "F00" && M().page == 0 && M().pages == 18 && S().itemCount() == 3);
    std::set<std::string> seen;
    int pagesSeen = 0, lastPage = -1;
    for (int i = 0; i < 50; i++) {
        seen.insert(cursorLabel());
        if (M().page != lastPage) ++pagesSeen, lastPage = M().page;
        CHECK(S().screen() == S_FRIENDS);
        down();
    }
    CHECK(static_cast<int>(seen.size()) == 50 && pagesSeen == 17 && cursorLabel() == "Blocked Players");   // (the 51st row; every friend was on the cursor once)
    down();
    CHECK(cursorLabel() == "Delete My Online Account" && M().page == 17);   // (the 52nd and last: alone on the 18th page)
    down();
    CHECK(cursorLabel() == "F00" && M().page == 0);   // (wraps round)
    press(8);
    CHECK(cursorLabel() == "Delete My Online Account" && M().page == 17);
    press(8);
    CHECK(cursorLabel() == "Blocked Players" && M().page == 16);
    press(8); press(8);
    CHECK(cursorLabel() == "F48");
    // Left / Right turn a page
    friendsFresh();
    fake::useMany = true;
    for (int i = 0; i < 50; i++) fake::manyFriends.push_back(mk(("F" + std::string(i < 10 ? "0" : "") + std::to_string(i)).c_str(), numCode("FR", i).c_str()));
    fake::incomingCount = 0;
    toFriends();
    right(); CHECK(M().page == 1 && cursorLabel() == "F03");
    press(5); CHECK(M().page == 0 && cursorLabel() == "F00");
    press(5); CHECK(M().page == 17);
    // the fiftieth friend can be opened, and removed
    friendsFresh();
    fake::useMany = true;
    for (int i = 0; i < 50; i++) fake::manyFriends.push_back(mk(("F" + std::string(i < 10 ? "0" : "") + std::to_string(i)).c_str(), numCode("FR", i).c_str()));
    fake::incomingCount = 0;
    toFriends();
    CHECK(walkTo("F49") == 49);
    ok(); frames(2);
    CHECK(S().screen() == S_REQUEST && titleLine() == "F49" && contains(subtitleLine(), "FR49 0049"));
    ok(); frames(3);   // Remove Friend (Yes): they are not in a game, so it is the first button
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "remove:FR49-0049");

    // 50 requests
    friendsFresh();
    fake::useMany = true;
    for (int i = 0; i < 50; i++) fake::manyIncoming.push_back(mk(("R" + std::string(i < 10 ? "0" : "") + std::to_string(i)).c_str(), numCode("RQ", i).c_str()));
    fake::friendsState = 0;
    toFriends();
    CHECK(rowLabel(0) == "Requests (50)");
    ok(); frames(2);
    CHECK(S().screen() == S_REQUESTS && M().pages == 17 && rowLabel(0) == "R00");
    seen.clear();
    for (int i = 0; i < 50; i++) { seen.insert(cursorLabel()); down(); }
    CHECK(static_cast<int>(seen.size()) == 50);
    press(8);
    CHECK(cursorLabel() == "R49");
    ok(); frames(2);
    CHECK(S().screen() == S_REQUEST && titleLine() == "Friend Request" && contains(subtitleLine(), "RQ49"));

    // 200 blocked players (the service's limit), newest first, each reachable and each unblockable
    friendsFresh();
    for (int i = 0; i < 200; i++) {
        fake::incomingCount = 1;
        fake::incoming[0] = mk(("B" + std::string(i < 100 ? (i < 10 ? "00" : "0") : "") + std::to_string(i)).c_str(), numCode("BL", i).c_str());
        S().close();
        toFriends(); ok(); ok(); down(2); ok(); frames(4);
    }
    S().close();
    fake::incomingCount = 0;
    toFriends(); CHECK(walkTo("Blocked Players") >= 0); ok(); frames(2);
    CHECK(S().screen() == S_BLOCKED && M().pages == 67 && rowLabel(0) == "B199");
    seen.clear();
    for (int i = 0; i < 200; i++) { seen.insert(cursorLabel()); down(); }
    CHECK(static_cast<int>(seen.size()) == 200 && cursorLabel() == "B199");
    press(8);
    CHECK(cursorLabel() == "B000" && M().page == 66);
    fake::ops.clear();
    ok(); frames(2);
    CHECK(fake::ops.size() == 1 && fake::ops[0] == "unblock:BL00-0000" && S().screen() == S_BLOCKED && M().pages == 67 && S().itemCount() == 1);
    friendsFresh();
}

// m-friends: Back goes up one screen from every screen of the family, whatever state it is in
void testFriendsBack() {
    struct { const char* name; void (*go)(); Scr scr, up; } p[] = {
        {"friends", [] { toFriends(); }, S_FRIENDS, S_MAIN},
        {"requests", [] { toFriends(); ok(); }, S_REQUESTS, S_FRIENDS},
        {"request", [] { toFriends(); ok(); ok(); }, S_REQUEST, S_REQUESTS},
        {"friend page", [] { toFriends(); down(); ok(); }, S_REQUEST, S_FRIENDS},
        {"blocked", [] { toFriends(); down(4); ok(); }, S_BLOCKED, S_FRIENDS},
        {"played", [] { playWith({{"Ana", "ABCD-1111"}}); toFriends(); ok(); }, S_PLAYED, S_FRIENDS},
        {"board", [] { toBoard(); }, S_BOARD, S_GATHER},
        {"friends(loading)", [] { fake::friendsState = -1; toFriends(); }, S_FRIENDS, S_MAIN},
        {"friends(error)", [] { fake::friendsState = -2; toFriends(); }, S_FRIENDS, S_MAIN},
        {"friends(connecting)", [] { fake::ensureWork = 1000000; toFriends(); }, S_FRIENDS, S_MAIN},
        {"friends(failed)", [] { fake::ensureResult = -2; fake::svcCode = "busy"; toFriends(); }, S_FRIENDS, S_MAIN},
        {"blocked(connecting)", [] { toFriends(); down(4); ok(); fake::ensureWork = 1000000; frames(2); }, S_BLOCKED, S_FRIENDS},
    };
    for (auto& s : p) {
        friendsFresh();
        s.go();
        if (S().screen() != s.scr) fprintf(stderr, "  back path %s ended on %s\n", s.name, screenName(S().screen()));
        CHECK(S().screen() == s.scr);
        back();
        if (S().screen() != s.up) fprintf(stderr, "  back from %s went to %s\n", s.name, screenName(S().screen()));
        CHECK(S().screen() == s.up);
        if (s.up == S_MAIN) { back(); CHECK(!S().isOpen()); }
    }
    friendsFresh();
}

// m-friends: every screen of the family composes in all five languages, in both pictures, with the layout rules; every guide button
// answers a touch; no Wii control anywhere
void testFriendsCompose() {
    MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static MpPanel::Model model;
    static MpPanel::Plan plan;
    static MpPanel::Scene scene;
    const char* langs[] = {"UsEnglish", "UsFrench", "UsSpanish", "EuGerman", "EuItalian"};
    struct St { const char* name; void (*go)(); };
    const St states[] = {
        {"friends", [] { toFriends(); }},
        {"friends(second page)", [] { toFriends(); down(3); }},
        {"friends(none)", [] { fake::friendsState = 0; fake::incomingCount = 0; toFriends(); }},
        {"friends(connecting)", [] { fake::ensureWork = 1000000; toFriends(); }},
        {"friends(busy)", [] { fake::ensureResult = -2; fake::svcCode = "busy"; toFriends(); }},
        {"friends(no wifi)", [] { fake::ensureResult = -1; fake::svcCode = "no_wifi"; toFriends(); }},
        {"friends(clock)", [] { fake::ensureResult = -15; fake::svcCode = "clock"; toFriends(); }},
        {"friends(update)", [] { fake::ensureResult = -3; fake::svcCode = "update"; toFriends(); }},
        {"friends(offline)", [] { fake::ensureResult = -2; fake::svcCode = "offline_build"; toFriends(); }},
        {"friend page", [] { toFriends(); down(); ok(); }},
        {"friend page(no join)", [] { toFriends(); down(2); ok(); }},
        {"requests", [] { toFriends(); ok(); }},
        {"request", [] { toFriends(); ok(); ok(); }},
        {"blocked(empty)", [] { toFriends(); down(4); ok(); }},
        {"blocked", [] { toFriends(); ok(); ok(); down(2); ok(); frames(4); back(); down(4); ok(); }},
        {"played", [] { playWith({{"Ana", "ABCD-1111"}, {"Ben", nullptr}, {"Luigi64", "1234-5678"}}); toFriends(); ok(); }},
        {"played(connecting)", [] { playWith({{"Ana", "ABCD-1111"}}); fake::ensureWork = 1000000; toFriends(); ok(); frames(2); }},
        {"board(connecting)", [] { fake::ensureWork = 1000000; toBoard(); }},
        {"friends(50)", [] {
            fake::useMany = true;
            for (int i = 0; i < 50; i++) fake::manyFriends.push_back(mk(("Friend" + std::to_string(i)).c_str(), numCode("FR", i).c_str(), i % 2 == 0, i % 4 == 0));
            fake::incomingCount = 0;
            toFriends();
            for (int i = 0; i < 30; i++) down();
        }},
    };
    for (float width : {810.6667f, 608.0f})
    for (const char* l : langs)
    for (const St& st : states) {
        MpPanel::Screen scr;
        scr.width = width;
        friendsFresh();
        MpMsgText::setLanguage(MpMsgText::languageIndex(l));
        st.go();
        S().buildModel(&model);
        vitaCheckModel(model, st.name, l);
        CHECK(model.title[0] != 0);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(model, scr, fm, 0.25f, &scene, &plan);
        CHECK(!plan.overflow);
        if (model.kind == MpPanel::KIND_LIST && model.rowCount > 0) CHECK(plan.hitCount == model.rowCount);
        CHECK(plan.guideHitCount == model.buttonCount);
        CHECK(model.buttonCount >= 1 && model.buttonCount <= 4);
        uinativeGeometry(st.name, l, model, scr, scene, plan);
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    friendsFresh();
}

// fix3: the online service's own refusals are worded in the game's window (EN/FR/ES), never "Something went wrong."
void testServiceErrors() {
    // an Internet room refused for the account (403 banned): the core says INTERNAL, the service's code says why
    fresh();
    toQuick();   // Quick Match: busy
    CHECK(S().screen() == MpMenu::S_BUSY);
    fake::svcCode = "banned";
    fake::status.phase = MP_UI_FAILED; fake::status.error = MP_ERR_INTERNAL;
    frames(2);
    CHECK(io.last == "MP_ERR_BANNED" && Logged("ui: error shown 14 (MP_ERR_BANNED) code=banned"));
    CHECK(S().screen() == MpMenu::S_GATHER && fake::leaves >= 1);   // (uxfix #3: back to looking for friends)
    // a code we do not word (or none): the core's error as before
    fresh();
    toQuick();
    fake::svcCode = "bad_code";
    fake::status.phase = MP_UI_FAILED; fake::status.error = MP_ERR_ROOM_NOT_FOUND;
    frames(2);
    CHECK(io.last == "MP_ERR_ROOM_NOT_FOUND" && LoggedPart("ui: error shown 4 (MP_ERR_ROOM_NOT_FOUND)"));
    // friend add: "added" only once the service said so; its refusal in its own words; the code under the cursor stays usable
    struct { const char* code; int state; const char* window; } cases[] = {
        {"", 1, "MP_FRIEND_ADDED"}, {"friends_full", -2, "MP_ERR_FRIENDS_FULL"}, {"player_not_found", -2, "MP_ERR_NO_PLAYER"},
        {"own_code", -2, "MP_ERR_OWN_CODE"}, {"", -2, "MP_FRIEND_FAILED"}, {"banned", -2, "MP_ERR_BANNED"}};
    for (auto& c : cases) {
        fresh();
        toFriends();
        CHECK(S().screen() == MpMenu::S_FRIENDS);
        fake::friendOpState = -1;
        snprintf(fake::kbText, sizeof fake::kbText, "1234-5678");
        fake::kbResult = 1;
        const int opened = io.opened;
        press(3); frames(2);   // Triangle: add a friend (the keyboard answers at once)
        fake::kbResult = 0;
        CHECK(S().screen() == MpMenu::S_BUSY && io.opened == opened);
        frames(30);
        CHECK(S().screen() == MpMenu::S_BUSY);
        fake::svcCode = c.code;
        fake::friendOpState = c.state;
        frames(2);
        CHECK(c.state == 1 ? LoggedPart("MP_FRIEND_ADDED") : io.last == c.window);   // ("added" names the code: a dynamic window)
        CHECK(S().screen() == MpMenu::S_FRIENDS);
    }
    // (one door: no registration screen any more; the Friends screens sign in by themselves: m-friends' tests)
    // every new window text exists in the three languages
    {
        bool found = false;
        for (int lang = 0; lang < MpMsgText::LANG_COUNT; lang++) {
            MpMsgText::setLanguage(lang);
            const char16_t* t = MpMsgText::raw(MP_T_ERR_FRIENDS_FULL);
            found = found || (t && t[0]);
            CHECK(t && t[0] && std::u16string(t) != std::u16string(MpMsgText::raw(MP_T_ERR_INTERNAL)));
        }
        MpMsgText::setLanguage(MpMsgText::LANG_EN);
        CHECK(found);
    }
    CHECK(MpMsgText::serviceText("not_friends") == MP_T_ERR_NOT_FRIENDS && MpMsgText::serviceText("not_member") == MP_T_ERR_NOT_MEMBER);
    CHECK(MpMsgText::serviceText("daily_cap") == MP_T_ERR_DAILY && MpMsgText::serviceText("version_mismatch") == MP_T_ERR_VERSION);
    CHECK(MpMsgText::serviceText("") == MP_T_NONE && MpMsgText::serviceText(nullptr) == MP_T_NONE && MpMsgText::serviceText("bad_code") == MP_T_NONE);
}

