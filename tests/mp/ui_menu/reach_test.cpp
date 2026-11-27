// menus4: reachability by breadth-first search over the menu state machine (Safe Copies reach; the first-time flow's press counts).
// A "press" is a button the player pushes: a window held open takes Cross (Yes / OK) or Circle (No) as a press of its own.
#include <functional>
#include <set>
#include <deque>
#include "common.hpp"
#include "port/mp_gather.h"

namespace reach {
enum Btn { CROSS, CIRCLE, DOWN, UP, TRI, SQ, START, KB_OK, N_BTN };
inline const char* kName[N_BTN] = {"Cross", "Circle", "Down", "Up", "Triangle", "Square", "Start", "KeyboardOK"};

inline void apply(int b) {
    if (io.open) {   // a window waits for its answer: Cross answers Yes (OK), Circle answers No; nothing else does anything
        if (b != CROSS && b != CIRCLE) return;
        io.answer = b == CROSS ? MpMenu::DlgState::Yes : MpMenu::DlgState::No;
        io.hold = false;
        frames(2);
        io.hold = true;
        io.answer = MpMenu::DlgState::Yes;
        return;
    }
    if (b == KB_OK) {
        if (!S().keyboardActive()) return;
        fake::kbResult = 1;
        fake::kbOnce = true;
        frames(4);
        fake::kbResult = 0;
        return;
    }
    if (S().keyboardActive()) {   // (only OK / cancel leave a keyboard)
        if (b != CIRCLE) return;
        fake::kbResult = -1;
        fake::kbOnce = true;
        frames(4);
        fake::kbResult = 0;
        return;
    }
    MpMenu::Input in;
    switch (b) {
    case CROSS: in.ok = true; break;
    case CIRCLE: in.back = true; break;
    case DOWN: in.down = true; break;
    case UP: in.up = true; break;
    case TRI: in.tri = true; break;
    case SQ: in.sq = true; break;
    case START: in.start = true; break;
    default: break;
    }
    step(in);
    frames(8);   // (a player's next press comes more than 8 frames later: screens have changed, the room has answered)
}

inline std::string key() {
    char b[160];
    snprintf(b, sizeof b, "%s/d%d/c%d/dlg%d/%s/kb%d", MpMenu::screenName(S().screen()), S().depth(), S().cursor(), io.open ? 1 : 0, io.open ? io.last.c_str() : "", S().keyboardActive() ? 1 : 0);
    std::string k = b;
    if (S().screen() == Scr::S_GATHER) {   // (the gathering's buttons change with who is there)
        MpGatherStatus g;
        memset(&g, 0, sizeof g);
        mp_gather_status(&g);
        char c[40];
        snprintf(c, sizeof c, "/h%d", g.headCount);
        k += c;
    }
    return k;
}

// Replay `seq` from a fresh menu: `setup` leaves the menu open (windows it raises stay open: io.hold is on).
inline std::function<void()> gWorld;   // what the rest of the room does in answer (a friend arrives); called after the setup and after every press
inline void replay(const std::function<void()>& setup, const std::vector<int>& seq) {
    fresh();
    io.hold = true;
    setup();
    if (gWorld) gWorld();
    for (int b : seq) {
        apply(b);
        if (gWorld) gWorld();
    }
}

// The fewest presses to a state the goal accepts (-1: not within maxDepth). `path` gets the buttons.
inline int search(const std::function<void()>& setup, const std::function<bool()>& goal, int maxDepth, std::string* path = nullptr,
                  std::function<void(int)> each = nullptr) {
    std::set<std::string> seen;
    std::deque<std::vector<int>> q;
    q.push_back({});
    int best = -1;
    while (!q.empty()) {
        std::vector<int> seq = q.front();
        q.pop_front();
        replay(setup, seq);
        if (goal()) {
            best = static_cast<int>(seq.size());
            if (path) {
                path->clear();
                for (int b : seq) *path += std::string(kName[b]) + " ";
            }
            break;
        }
        if (static_cast<int>(seq.size()) >= maxDepth || !S().isOpen()) continue;
        if (!seen.insert(key()).second) continue;
        for (int b = 0; b < N_BTN; b++) {
            seq.push_back(b);
            q.push_back(seq);
            seq.pop_back();
        }
    }
    io.hold = false;
    gWorld = nullptr;
    (void)each;
    return best;
}
inline bool onScreen(Scr s) { return S().isOpen() && !io.open && S().screen() == s; }
}  // namespace reach

static void withCopies() {
    fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000));
    fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_BEFORE, "b.smgmp", 1794300000));
}
static void toKeep() { toSave(); press(3); }
static void clearTexts() { io.texts.clear(); }
// the cursor goes down the rows (a page holds three) until it stands on Bring Back a Copy; false when no row is it
static bool cursorToBringBack() {
    for (int i = 0; i <= S().itemCount(); i++) {
        if (cursorLabel() == "Bring Back a Copy") return true;
        down();
    }
    return false;
}
static void goodFileHere() { fake::flow.lastCoop = 2; fake::coop[1].galaxy = 0; }

// menus4 item 1: Safe Copies is reachable from the Multiplayer menu in at most 3 presses in every situation, without being a front-door
// button, and "Bring Back a Copy" is offered where the player needs it
void testSafeCopiesReach() {
    struct Env { const char* name; std::function<void()> setup; };
    const Env envs[] = {
        {"first run, alone", [] { fake::flow.charPicked = 0; fake::flow.fallsAsked = 0; S().openMain(1); frames(3); }},
        {"first run, alone, a copy exists", [] { fake::flow.charPicked = 0; fake::flow.fallsAsked = 0; withCopies(); S().openMain(1); frames(3); }},
        {"a Team File cannot be read", [] { goodFileHere(); fake::coopBad = 1u << 1; withCopies(); S().openMain(1); frames(3); }},
        {"Star Race", [] { goodFileHere(); S().openMain(1); frames(3); reach::apply(reach::DOWN); }},
        {"returning leader", [] { goodFileHere(); withCopies(); S().openMain(1); frames(3); }},
        {"returning leader, no Wi-Fi", [] { goodFileHere(); fake::wifi = false; fake::service = false; S().openMain(1); frames(3); }},
    };
    for (const Env& e : envs) {
        std::string path;
        const int n = reach::search(e.setup, [] { return reach::onScreen(Scr::S_KEEP); }, 3, &path);
        if (n < 0 || n > 3) fprintf(stderr, "  Safe Copies not within 3 presses: %s\n", e.name);
        CHECK(n >= 0 && n <= 3);
        // not a front-door button: the Multiplayer menu's own big buttons never include it
        fresh();
        e.setup();
        for (int i = 0; i < S().itemCount(); i++) CHECK(rowLabel(i) != "Safe Copies" && rowLabel(i) != "Keep My Multiplayer Data Safe");
        io.hold = false;
    }
    // the second page of How to Play is the way: Triangle, OK, Yes
    fresh();
    openMain();
    press(3);
    CHECK(Logged("ui: how to play") && anyText("Hold") && anyText("Keep My Multiplayer Data Safe") && windowFits(io.texts.back()));
    CHECK(S().screen() == Scr::S_KEEP);
    // the old route stays: Which File?'s Triangle
    fresh();
    toSave();
    press(3);
    CHECK(S().screen() == Scr::S_KEEP);
    // the page is a question: No leaves the player on the Multiplayer menu
    fresh();
    io.answer = MpMenu::DlgState::No;
    openMain();
    press(3);
    CHECK(S().screen() == Scr::S_MAIN);
    io.answer = MpMenu::DlgState::Yes;

    // (a) "Bring Back a Copy" right where it is needed, and only when a copy exists
    for (int withCopy = 0; withCopy < 2; withCopy++) {
        // Which File?: a Team File cannot be read
        fresh();
        fake::coopBad = 1u << 1;
        fake::flow.lastCoop = 2;   // (the one played last: still not chosen by default)
        if (withCopy) withCopies();
        toSave();
        bool row = cursorToBringBack();
        CHECK(row == (withCopy != 0));
        if (withCopy) {
            ok();
            CHECK(S().screen() == Scr::S_KEEPLIST);
            back();
            CHECK(S().screen() == Scr::S_SAVE && cursorLabel() == "Bring Back a Copy");
        }
        // nothing unreadable: no extra button (no dead or needless rows)
        fresh();
        if (withCopy) withCopies();
        toSave();
        CHECK(!cursorToBringBack());
        // the after-a-crash recovery question's list of Team Files (where to keep the progress): the button is there with a copy
        fresh();
        fake::recovery = true;
        if (withCopy) withCopies();
        S().openMain(1);
        frames(3);
        CHECK(S().screen() == Scr::S_SLOT);
        row = cursorToBringBack();
        CHECK(row == (withCopy != 0));
        if (withCopy) {
            ok();
            CHECK(S().screen() == Scr::S_KEEPLIST);
            back();
            CHECK(S().screen() == Scr::S_SLOT);
        }
        // the guest's / leaver's "keep this progress" list with a Team File that cannot be read
        fresh();
        session(false, 1, MP_UI_PLAYING);
        frames(3);
        fake::coopBad = 1u << 2;
        if (withCopy) withCopies();
        fake::active = false; fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_HOST_LEFT;
        frames(6);
        CHECK(S().screen() == Scr::S_SLOT);
        row = cursorToBringBack();
        CHECK(row == (withCopy != 0));
    }
    // one press chain from the unreadable Which File?: Down (to the last row), Cross: the copies
    {
        std::string path;
        const int n = reach::search([] { fake::coopBad = 1u << 1; fake::flow.lastCoop = 2; withCopies(); toSave(); },
                                    [] { return reach::onScreen(Scr::S_KEEPLIST); }, 4, &path);
        CHECK(n >= 0 && n <= 3);
    }
}

// menus4 item 1(c): the words after Make a Copy: no path as the instruction, no alarm, in five languages
void testSafeCopiesWords() {
    static const char* const kLang[5] = {"en", "fr", "es", "de", "it"};
    for (int lang = 0; lang < 5; lang++) {
        MpMsgText::setLanguage(lang);
        for (int share = 0; share < 2; share++) {
            fresh();
            toKeep();
            if (share) down();
            clearTexts();
            ok(); frames(3);
            const std::string& t = io.lastText;
            if (t.find("ux0:") != std::string::npos) fprintf(stderr, "  ux0 in %s share=%d: %s\n", kLang[lang], share, t.c_str());
            CHECK(!t.empty() && windowFits(t));
            CHECK(t.find("ux0:") == std::string::npos && t.find("data/smg") == std::string::npos);
            // the folder's name is shown (small / plain), the one line says where it is
            if (share) CHECK(t.find("mp-backups") != std::string::npos);
            // nothing that reads as an alarm
            for (const char* alarm : {"don't give", "ne la donnez", "no se la des", "gib sie niemandem", "non darla", "anyone", "personne", "nadie", "niemandem", "nessuno"}) {
                if (t.find(alarm) != std::string::npos) fprintf(stderr, "  alarm '%s' in %s share=%d\n", alarm, kLang[lang], share);
                CHECK(t.find(alarm) == std::string::npos);
            }
        }
    }
    MpMsgText::setLanguage(0);
}


// menus4 item 6: the first-time flow, press by press, from the title: a brand-new player on the same Wi-Fi as a friend. The title and the
// entry screen (outside the menus) cost three presses: START, Down to Multiplayer, Cross. From there the menus are searched breadth first
// (every button, every window's answer): the fewest presses to a started Play Together game as the leader, and to waiting in the leader's game
// as a friend. The UX review's counts are the maxima (8 and 5): they must never grow.
namespace {
constexpr int kTitlePresses = 3;
constexpr int kSearchDepth = 6;   // (menu presses searched; a count over the maximum fails the check, and says how many it really takes)
constexpr int kLeaderMax = 8;
constexpr int kFriendMax = 5;
// a returning player (the character was picked and the falls question answered on an earlier visit): the owner's 4 and 3 count the menu's
// presses (the title and entry screen's three are the same for everyone: 6 and 4 in all)
constexpr int kReturningLeaderMax = 4;
constexpr int kReturningFriendMax = 3;
void BrandNewPlayer() {
    fake::flow.charPicked = 0, fake::flow.fallsAsked = 0, fake::flow.lastCoop = 0;
    S().openFromEntry();
    frames(3);
}
void ReturningPlayer() {   // (fresh() already says: character picked, falls answered)
    CHECK(fake::flow.charPicked == 1 && fake::flow.fallsAsked == 1);
    S().openFromEntry();
    frames(3);
}
int Heads() {
    MpGatherStatus g;
    memset(&g, 0, sizeof g);
    mp_gather_status(&g);
    return g.headCount;
}
bool FriendIsLeader() {
    MpGatherStatus g;
    memset(&g, 0, sizeof g);
    mp_gather_status(&g);
    for (int i = 0; i < g.headCount; i++)
        if (g.heads[i].flags & MP_HEAD_LEADER) return !(g.heads[i].flags & MP_HEAD_SELF);
    return false;
}
}  // namespace

namespace {
// the leader: a friend turns up once the gathering screen is looking; the friend: the leader's game is already gathering when it opens
void CountPresses(const char* who, void (*setup)(), int leaderMax, int friendMax, int titleCounted) {
    reach::gWorld = [] {
        if (S().screen() == Scr::S_GATHER && Heads() < 2 && !fake::active) {
            gatherHeads(2, 1);
            session(true, 0, MP_UI_LOBBY);
            frames(2);
        }
    };
    std::string path;
    int n = reach::search(setup, [] { return startsAll() >= 1; }, kSearchDepth, &path);
    fprintf(stderr, "  %s leader: %d presses (%d with the title and entry screen): %s\n", who, n, n + kTitlePresses, path.c_str());
    CHECK(n >= 0 && n + titleCounted * kTitlePresses <= leaderMax);
    reach::gWorld = [] {
        if (S().screen() == Scr::S_GATHER && Heads() < 2 && !fake::active) {
            gatherHeads(2, 2);
            session(false, 1, MP_UI_LOBBY);
            frames(2);
        }
    };
    n = reach::search(setup, [] { return S().screen() == Scr::S_GATHER && !io.open && Heads() >= 2 && FriendIsLeader(); }, kSearchDepth, &path);
    fprintf(stderr, "  %s friend: %d presses (%d with the title and entry screen): %s\n", who, n, n + kTitlePresses, path.c_str());
    CHECK(n >= 0 && n + titleCounted * kTitlePresses <= friendMax);
    reach::gWorld = nullptr;
}
}  // namespace

void testFirstTimeFlowPresses() {
    CountPresses("first-time", BrandNewPlayer, kLeaderMax, kFriendMax, 1);
    CountPresses("returning", ReturningPlayer, kReturningLeaderMax, kReturningFriendMax, 0);
}
