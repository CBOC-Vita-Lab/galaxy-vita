// Galaxy 3.0 multiplayer: toasts, the waiting screen and the race results card (see MpMenuSequence.hpp, namespace MpOverlay).
// Pure code. Toasts: top of the picture, the message window's look, at most 3 at a time (a 4th waits), 3.5 s each with a fade.
// Results: auto-continue after 8 s or Cross.
#include <stdio.h>
#include <string.h>

#include "Game/MP/MpFlow.hpp"            // MpGameInternal::nowMs
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpPresent.hpp"        // MP3 ingame: the toast rule
#include "Game/MP/MpPresentLogic.hpp"
#include "Game/MP/MpUi.hpp"

namespace MpOverlay {
namespace {

using MpMsgText::Args;
namespace txt = MpMsgText;

constexpr int kQueue = 8;
constexpr int kToastFrames = 210, kFadeFrames = 12;
constexpr int kResultsFrames = 8 * 60;
// B6 / uxfix #9: both waits count wall-clock milliseconds (MpGameInternal::nowMs), not frames: a stage that runs at 20 fps or a console that hitches must
// not stretch 90 / 120 s into minutes, and the count stands still while the core is rejoining (a dropped link is not an idle leader).
constexpr int64_t kGuestIdleMs = MpSeriesLogic::kCardIdleGuestMs;   // a guest's card that waited this long for the leader leaves by itself
constexpr int64_t kSeriesIdleMs = MpSeriesLogic::kCardIdleHostMs;   // a leader who walked away: the series card moves on by itself after 90 s (Race Again; anyone can still leave)

struct Pending { char16_t text[96]; uint8_t icon; };
Pending sQueue[kQueue];
int sQueueCount = 0;

struct Live { MpPanel::Toast t; int age; };
Live sLive[MpPanel::kMaxToasts];
int sLiveCount = 0;
MpPanel::Toast sOut[MpPanel::kMaxToasts];

// fix2: one toast that stays up while a state lasts ("Reconnecting..."), first in the stack, beside the passing ones
bool sSticky = false;
bool sBanner = false;                 // uxwait: "Waiting for <name>..." (a second sticky line; the reconnect line keeps its own)
int sBannerAge = 0;
int sBannerSlot = -1;
MpPanel::Toast sBannerToast;
int sStickyAge = 0;
MpPanel::Toast sStickyToast;

bool sWaiting = false;
bool sWaitHidden = false;   // spectate: the finisher's view of a rival is open: the panel waits
int sWaitDone = 0, sWaitTotal = 0, sWaitSeconds = -1;
uint32_t sStanding[MP_MAX_PLAYERS];
uint32_t sFrame = 0;

bool sResults = false;
int sResultsAge = 0;
int64_t sResGuestIdleMs = 0;   // B6: ms a guest's card with choices has been up without the leader's choice (restarts when the host changes)
int sResHostSlot = -2;          // B6: the host the guest's count belongs to
int64_t sLastMs = 0;            // B6: the clock at the last update()
int64_t sResIdleMs = 0;   // ms since the leader last pressed anything on the series card
MpRaceResults sRes;
char sResNames[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];
uint8_t sResLook[MP_MAX_PLAYERS];
// race: the results card with its series column and its next step (a RESULTS with a tail). The host moves the cursor (rows: Race Again,
// [Next Star], Leave Race) and chooses with one press; guests see the host's choice. No timer: it stays until the session moves on.
int sResCursor = 0;                                         // the host's cursor among the visible choices
uint8_t sResChoice = MpSeriesLogic::CH_NONE;                // the choice made (host's own press or the host's RESULTS)
uint8_t sResTaken = MpSeriesLogic::CH_NONE;                 // the host's press, not yet fetched by the race code
// smallfix: a player who is not the leader has one live row, Leave Race (nothing else on the card is theirs to press, and nothing
// happens on a timer, so a leader who walked away cannot hold the others): Cross on it takes them out of the race.
bool sResLeaveTaken = false;                                // that press, not yet fetched by the scene side
bool sResLeft = false;                                      // pressed: the row stays chosen until the session ends or the card closes
bool sResAsked = false;                                     // lobby: this guest asked for a rematch on this card (one ask per card)
bool sResAskTaken = false;                                  // that press, not yet fetched by the chat sender
bool sGuestOnLeave = false;                                 // lobby: a guest's cursor: Race Again? (false) or Leave Race (true)
uint8_t sAskNudged = 0;                                     // lobby: the leader: slots whose ask already showed its nudge on this card
int sAutoRuns = 0;                                          // lobby: consecutive automatic Race Agains (nobody at the leader's console); a press or an ask clears it
int8_t sRowK[MpPanel::kMaxRows];                                     // the last built card: row -> the leader's choice index (-1: not a choice row)
bool sRowLive[MpPanel::kMaxRows];                                    // the last built card: the row can be pressed by this player
// the last known name and head of every slot (a player who left keeps their greyed line in the series)
char sSlotName[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];
uint8_t sSlotLook[MP_MAX_PLAYERS];
// the head start line on the countdown (under the last winner's name)
struct HeadStart { bool on; uint8_t slot; int sec; char name[MP_NAME_BYTES + 1]; uint8_t look; };
HeadStart sHead;

bool sStarCard = false;
int sStarAge = 0;
MpStarCard sCard;
char sCardNames[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];
uint8_t sCardLook[MP_MAX_PLAYERS];

// MP3 ingame: a toast only for what the player cannot see (MpPresentLogic.hpp). The game turns it on (MpUi::createSystem); off, as
// in the ui host tests, every toast shows as before.
bool sPolicy = false;

// a toast formatted by its caller (no id): the menu sequence's "<name> left." and "<name> changed the rules: ..."
bool PolicyDropsText(const char16_t* text) {
    static const MpText kSeen[] = {MP_T_LEFT, MP_T_JOINED, MP_T_RULES_CHANGED, MP_T_GOT_STAR, MP_T_BUBBLED, MP_T_POPPED, MP_T_PLAYER_DOWN};
    for (MpText id : kSeen)
        if (MpPresent::matchesTemplate(text, txt::raw(id))) return true;
    return false;
}

uint8_t IconOf(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !mp_slot_present(slot)) return 0;
    const int look = mp_slot_look(slot);
    return static_cast<uint8_t>(MpPanel::ICON_MARIO + (look >= 0 && look < MP_LOOK_COUNT ? look : 0));
}

}  // namespace

void reset() {
    sQueueCount = sLiveCount = 0;
    sWaiting = sResults = sStarCard = sSticky = sWaitHidden = false;
    sBanner = false;
    sBannerSlot = -1;
    memset(sStanding, 0, sizeof sStanding);
    sResChoice = sResTaken = MpSeriesLogic::CH_NONE;
    sResLeaveTaken = sResLeft = false;
    sResAsked = sResAskTaken = sGuestOnLeave = false;
    sAskNudged = 0;
    sAutoRuns = 0;
    sHead.on = false;
    memset(sSlotName, 0, sizeof sSlotName);
    memset(sSlotLook, 0, sizeof sSlotLook);
}

void setPolicy(bool on) { sPolicy = on; }
bool policy() { return sPolicy; }

void toastText(const char16_t* text, uint8_t icon) {
    if (!text || !text[0]) return;
    if (sPolicy && PolicyDropsText(text)) {   // MP3 ingame: the picture shows it (the strip's head, the rules card)
        mp_log("ui: toast not shown (seen on screen)");
        return;
    }
    if (sQueueCount >= kQueue) {   // (the oldest waiting one goes)
        memmove(&sQueue[0], &sQueue[1], sizeof(Pending) * (kQueue - 1));
        sQueueCount = kQueue - 1;
    }
    MpPanel::copy(sQueue[sQueueCount].text, 96, text);
    sQueue[sQueueCount].icon = icon;
    ++sQueueCount;
    char line[100];
    int n = 0;
    for (const char16_t* p = text; *p && n < 98; ++p) line[n++] = *p < 0x80 && *p >= 0x20 ? static_cast<char>(*p) : '?';
    line[n] = 0;
    mp_log("ui: toast \"%s\"", line);
}

// flow's toast ids (Game/MP/MpFlow.hpp MP_TXT_*: 0x0301..) -> ours; returns MP_T_NONE for ones shown otherwise
static uint16_t FromFlow(uint16_t id, uint32_t* arg) {
    switch (id) {
    case 0x0301: return MP_T_HOST_CHOOSES;
    case 0x0302: return MP_T_GOT_STAR;
    case 0x0303: return MP_T_PLAYER_DOWN;
    case 0x0304: return MP_T_WARPED_TO_YOU;
    case 0x0305: return MP_T_WARP_FAILED;
    case 0x0306: return MP_T_NO_SETTINGS;
    case 0x0307: return MP_T_SKIP_VOTE;
    case 0x0308: return MP_T_FINISHED;
    case 0x0309: return *arg < 4 ? static_cast<uint16_t>(MP_T_STAMP_0 + *arg) : static_cast<uint16_t>(MP_T_NONE);
    case 0x030A: return MP_T_NONE;   // (new host: the ui's own toast says it, with the old host's name)
    case 0x030B: return MP_T_HOST_RACES_ONLY;
    default: return id;
    }
}

void toast(uint16_t textId, int slot, uint32_t arg) {
    if (sPolicy && !MpPresent::toastShown(textId)) {   // MP3 ingame: the picture shows it
        mp_log("ui: toast 0x%04x not shown (seen on screen)", static_cast<unsigned>(textId));
        return;
    }
    if (textId >= 0x0300) {
        const uint16_t mapped = FromFlow(textId, &arg);
        if (mapped == MP_T_NONE) {
            mp_log("ui: toast 0x%04x shown otherwise", static_cast<unsigned>(textId));
            return;
        }
        textId = mapped;
    }
    if (textId == MP_T_NONE || textId > MP_T_TOAST_LAST || textId == MP_T_PICKED) {   // (PICKED needs a galaxy: toastPick only)
        mp_log("ui: toast id %u ignored", static_cast<unsigned>(textId));
        return;
    }
    Args a;
    const bool hasSlot = slot >= 0 && slot < MP_MAX_PLAYERS;
    a.str = hasSlot ? mp_slot_name(slot) : "";
    a.num = static_cast<int>(arg > 99 ? 99 : arg);
    a.turns = arg;
    // words: no role words: "<name> chooses the galaxy." names the player who chooses (the one leading the game now)
    if ((textId == MP_T_HOST_CHOOSES || textId == MP_T_HOST_RACES_ONLY) && mp_host_slot() >= 0 && mp_host_slot() < MP_MAX_PLAYERS)
        a.str = mp_slot_name(mp_host_slot());
    if (textId == MP_T_HOST_MIGRATED || textId == MP_T_POPPED) a.str2 = arg < MP_MAX_PLAYERS ? mp_slot_name(static_cast<int>(arg)) : "";
    char votes[12];
    if (textId == MP_T_SKIP_VOTE) {
        snprintf(votes, sizeof votes, "%u/%u", static_cast<unsigned>(arg & 0xFF), static_cast<unsigned>((arg >> 8) & 0xFF));
        a.str2 = votes;
    }
    char16_t t[96];
    txt::format(t, 96, static_cast<MpText>(textId), a);
    toastText(t, hasSlot ? IconOf(slot) : 0);
}

// touches: "<host> picked <galaxy>, Star <n>." (the galaxy's name comes from the game's own message data, already in the console's language)
void toastPick(int slot, const char16_t* galaxy, int star) {
    char name[3 * 40 + 1];   // (UTF-8 of a BMP name; the Args take UTF-8)
    int n = 0;
    for (int i = 0; galaxy && galaxy[i] && n < static_cast<int>(sizeof name) - 4; ++i) {
        const char16_t c = galaxy[i];
        if (txt::isPicChar(c) || c < 0x20) continue;
        if (c < 0x80) name[n++] = static_cast<char>(c);
        else if (c < 0x800) { name[n++] = static_cast<char>(0xC0 | (c >> 6)); name[n++] = static_cast<char>(0x80 | (c & 0x3F)); }
        else { name[n++] = static_cast<char>(0xE0 | (c >> 12)); name[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F)); name[n++] = static_cast<char>(0x80 | (c & 0x3F)); }
    }
    name[n] = 0;
    if (n == 0) return;
    Args a;
    const bool hasSlot = slot >= 0 && slot < MP_MAX_PLAYERS;
    a.str = hasSlot ? mp_slot_name(slot) : "";
    a.str2 = name;
    a.num = star < 1 ? 1 : (star > 99 ? 99 : star);
    char16_t t[96];
    txt::format(t, 96, MP_T_PICKED, a);
    toastText(t, hasSlot ? IconOf(slot) : 0);
}

void sticky(const char16_t* text, uint8_t icon) {
    const bool on = text && text[0];
    if (on) {
        if (!sSticky) sStickyAge = 0;
        MpPanel::copy(sStickyToast.text, 96, text);
        sStickyToast.icon = icon;
    }
    if (on != sSticky) mp_log("ui: sticky toast %s", on ? "on" : "off");
    sSticky = on;
}

// uxwait: slot >= 0 = a small banner "Waiting for <name>..." until waitBanner(-1); it never takes a button or holds anything
void waitBanner(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !mp_slot_present(slot)) {
        if (sBanner) mp_log("ui: wait banner off");
        sBanner = false;
        sBannerSlot = -1;
        return;
    }
    if (sBanner && slot == sBannerSlot) return;
    Args a;
    a.str = mp_slot_name(slot);
    char16_t t[96];
    txt::format(t, 96, MP_T_WAIT_NAME, a);
    if (!sBanner) sBannerAge = 0;
    MpPanel::copy(sBannerToast.text, 96, t);
    sBannerToast.icon = IconOf(slot);
    if (!sBanner || slot != sBannerSlot) mp_log("ui: wait banner on slot %d", slot);
    sBanner = true;
    sBannerSlot = slot;
}
bool waitBannerShowing() { return sBanner; }

void showWaiting(bool on, int done, int total, int secondsLeft) {
    if (on && !sWaiting) mp_log("ui: waiting screen on %d/%d", done, total);
    if (!on && sWaiting) {
        mp_log("ui: waiting screen off");
        memset(sStanding, 0, sizeof sStanding);
    }
    sWaiting = on;
    sWaitDone = done < 0 ? 0 : (done > MP_MAX_PLAYERS ? MP_MAX_PLAYERS : done);
    sWaitTotal = total < 0 ? 0 : (total > MP_MAX_PLAYERS ? MP_MAX_PLAYERS : total);
    sWaitSeconds = secondsLeft;
}

void hideWaiting(bool hidden) { sWaitHidden = hidden; }

void setStanding(int slot, uint32_t turns) {
    if (slot >= 0 && slot < MP_MAX_PLAYERS) sStanding[slot] = turns;
}

bool showResults(const MpMsg& m) {
    MpRaceResults r;
    MpReader rd(m.data, m.len);
    if (m.len > sizeof m.data || !mp_read_results(rd, &r)) {
        mp_log("ui: results dropped (bad payload, %u bytes)", static_cast<unsigned>(m.len));
        return false;
    }
    sRes = r;
    for (int i = 0; i < r.count; i++) {
        const int s = r.e[i].slot;
        strncpy(sResNames[i], mp_slot_name(s), MP_NAME_BYTES);
        sResNames[i][MP_NAME_BYTES] = 0;
        sResLook[i] = IconOf(s);
        strncpy(sSlotName[s], sResNames[i], MP_NAME_BYTES);   // (the series keeps a leaver's line: its name stays known)
        sSlotLook[s] = sResLook[i];
    }
    sResults = true;
    sResultsAge = 0;
    sResGuestIdleMs = 0;
    sResIdleMs = 0;
    sResHostSlot = mp_host_slot();
    sLastMs = MpGameInternal::nowMs();   // (the card's wait starts now, not at the last update of some earlier scene)
    sWaiting = false;
    sResCursor = 0;
    sResTaken = MpSeriesLogic::CH_NONE;
    sResLeaveTaken = sResLeft = false;
    sResAsked = sResAskTaken = sGuestOnLeave = false;   // lobby: a new card: a new ask
    sAskNudged = 0;
    sResChoice = r.hasTail ? r.tail.choice : static_cast<uint8_t>(MpSeriesLogic::CH_NONE);
    mp_log("ui: results shown count=%d series=%d choice=%d", r.count, r.hasTail ? 1 : 0, static_cast<int>(sResChoice));
    return true;
}

void setResultsChoice(int choice) {
    if (!sResults || !sRes.hasTail || choice < 0 || choice >= MpSeriesLogic::CH_COUNT) return;
    if (sResChoice != choice) mp_log("ui: results choice %d", choice);
    sResChoice = static_cast<uint8_t>(choice);
}

void dismissResults() {
    if (sResults) mp_log("ui: results closed (scene)");
    sResults = false;
    sResChoice = sResTaken = MpSeriesLogic::CH_NONE;
    sResLeaveTaken = sResLeft = false;
    sResAskTaken = false;
}

// Who can press what on the card with choices (race). The leader: Race Again / [Next Star] / Leave Race, once. Everyone else: Leave Race only,
// until the leader has chosen (then the card is about to move on) or they pressed it.
bool guestLeaveLive() { return sResults && sRes.hasTail && !mp_is_host() && sResChoice == MpSeriesLogic::CH_NONE && !sResLeft; }

// The keys on the card with choices (race). The first half second swallows the press that ended the star wipe.
void resultsInput(bool up, bool down, bool ok, bool isHost) {
    if (!sResults || !sRes.hasTail || sResultsAge < 30) return;
    if (!isHost) {   // smallfix: Leave Race is live; lobby: and Race Again? (one ask per card), the cursor moves between the two
        if (!guestLeaveLive()) return;
        if (sResAsked) sGuestOnLeave = true;
        else if (up) sGuestOnLeave = false;
        else if (down) sGuestOnLeave = true;
        if (ok && sGuestOnLeave) {
            sResLeft = sResLeaveTaken = true;
            mp_log("ui: results leave pressed");
            mp_log("race leave slot=%d", mp_local_slot());   // hooks: the scenarios' line
        } else if (ok && !sResAsked) {
            sResAsked = sResAskTaken = true;
            sGuestOnLeave = true;
            mp_log("ui: results race again asked");
        }
        return;
    }
    if (sResChoice != MpSeriesLogic::CH_NONE) return;
    if (up || down || ok) sResIdleMs = 0, sAutoRuns = 0;   // (someone is at the leader's console)
    const int n = sRes.tail.next > 0 ? 3 : 2;
    if (up && sResCursor > 0) --sResCursor;
    if (down && sResCursor < n - 1) ++sResCursor;
    if (ok) {
        const int k = sResCursor;
        sResTaken = static_cast<uint8_t>(k == 0 ? MpSeriesLogic::CH_AGAIN : (n == 3 && k == 1 ? MpSeriesLogic::CH_NEXT : MpSeriesLogic::CH_LEAVE));
        sResChoice = sResTaken;
        mp_log("ui: results chosen %d", static_cast<int>(sResChoice));
    }
}

// A finger (or the pointer) is on this row of the card built last: the leader's cursor follows it when it is a live choice. True when the
// row is one this player can press (a tap elsewhere on the card does nothing).
bool resultsPoint(int row) {
    if (!sResults || !sRes.hasTail || row < 0 || row >= MpPanel::kMaxRows || !sRowLive[row]) return false;
    if (mp_is_host() && sRowK[row] >= 0) sResCursor = sRowK[row];
    if (mp_is_host()) sResIdleMs = 0, sAutoRuns = 0;   // (a finger on the leader's card: someone is there)
    if (!mp_is_host() && sRowK[row] >= 0) sGuestOnLeave = sRowK[row] != 0;   // lobby: a guest's finger: Race Again? is the first choice, Leave Race the last
    return true;
}

bool takeResultsAsk() {
    if (!sResAskTaken) return false;
    sResAskTaken = false;
    return true;
}
// menus4: the quick-phrase wheel sent "Let's go!" while this guest's card (with its choices) is up and nothing was asked yet: that phrase is
// the ask the leader reads, so the card says so (one ask per card) and does not send it a second time.
bool noteAskSentByHand() {
    if (!guestLeaveLive() || sResAsked) return false;
    sResAsked = true;
    sGuestOnLeave = true;
    mp_log("ui: results race again asked (by hand)");
    return true;
}
void resultsAskFailed() {
    if (!sResAsked) return;
    sResAsked = false;
    sGuestOnLeave = false;
    mp_log("ui: results ask not sent");
}

// lobby: a guest asked for a rematch (their quick phrase 3 arrived). Only the leader's open card shows it, once per player per card; it also
// tells the card's idle timer that someone is there and wants to go on.
void noteRaceAgainAsk(int slot) {
    if (!sResults || !sRes.hasTail || !mp_is_host() || sResChoice != MpSeriesLogic::CH_NONE) return;
    if (slot < 0 || slot >= MP_MAX_PLAYERS || slot == mp_local_slot() || !mp_slot_present(slot)) return;
    sAutoRuns = 0;
    if (sAskNudged & (1u << slot)) return;
    sAskNudged |= static_cast<uint8_t>(1u << slot);
    Args a;
    a.str = mp_slot_name(slot);
    char16_t t[96];
    txt::format(t, 96, MP_T_RACE_WANTS, a);
    toastText(t, IconOf(slot));
    mp_log("ui: results race again asked by slot %d", slot);
}

// Leave Race was pressed by a player who is not the leader (once).
bool takeResultsLeave() {
    if (!sResLeaveTaken) return false;
    sResLeaveTaken = false;
    return true;
}

bool takeResultsChoice(int* choice) {
    if (sResTaken == MpSeriesLogic::CH_NONE) return false;
    *choice = sResTaken;
    sResTaken = MpSeriesLogic::CH_NONE;
    return true;
}

void forgetSeriesNames() {
    sAutoRuns = 0;   // (a new series)
    memset(sSlotName, 0, sizeof sSlotName);
    memset(sSlotLook, 0, sizeof sSlotLook);
}

bool resultsHaveChoices() { return sResults && sRes.hasTail; }

void showHeadStart(int slot, int seconds) {
    if (seconds <= 0 || slot < 0 || slot >= MP_MAX_PLAYERS) {
        sHead.on = false;
        return;
    }
    sHead.on = true;
    sHead.slot = static_cast<uint8_t>(slot);
    sHead.sec = seconds;
    strncpy(sHead.name, mp_slot_name(slot), MP_NAME_BYTES);
    sHead.name[MP_NAME_BYTES] = 0;
    sHead.look = IconOf(slot);
}

void showStarCard(const MpStarCard& card) {
    if (card.count < 1 || card.count > MP_MAX_PLAYERS) return;
    sCard = card;
    for (int i = 0; i < card.count; i++) {
        const int s = card.e[i].slot < MP_MAX_PLAYERS ? card.e[i].slot : 0;
        strncpy(sCardNames[i], mp_slot_name(s), MP_NAME_BYTES);
        sCardNames[i][MP_NAME_BYTES] = 0;
        sCardLook[i] = IconOf(s);
    }
    if (!sStarCard) sStarAge = 0;
    sStarCard = true;
    mp_log("ui: star card shown count=%d team=%d", card.count, card.team ? 1 : 0);
}

void dismissStarCard() {
    if (sStarCard) mp_log("ui: star card closed (scene)");
    sStarCard = false;
}

bool starCardShowing() { return sStarCard; }

void update(bool okPressed, bool hold) {
    ++sFrame;
    // B6: wall-clock time since the last call; nothing counts while the session is being rejoined (hold)
    const int64_t nowMs = MpGameInternal::nowMs();
    int64_t dtMs = sLastMs ? nowMs - sLastMs : 0;
    if (dtMs < 0) dtMs = 0;
    if (dtMs > 5000) dtMs = 5000;   // (a long gap between two updates is a pause of the game, not an idle leader)
    sLastMs = nowMs;
    if (hold) dtMs = 0;
    if (sStarCard && ++sStarAge >= kStarCardFrames) {
        sStarCard = false;
        mp_log("ui: star card closed (timer)");
    }
    // toasts: age, fade, retire, admit
    int w = 0;
    for (int i = 0; i < sLiveCount; i++) {
        Live& l = sLive[i];
        ++l.age;
        if (l.age >= kToastFrames) continue;
        const int left = kToastFrames - l.age;
        float a = l.age < kFadeFrames ? l.age / static_cast<float>(kFadeFrames) : 1.0f;
        if (left < kFadeFrames) a = left / static_cast<float>(kFadeFrames);
        l.t.alpha = a;
        sLive[w++] = l;
    }
    sLiveCount = w;
    if (sSticky) ++sStickyAge;
    if (sBanner && (!mp_active() || !mp_slot_present(sBannerSlot))) {   // uxwait backstop: never names a player who left, or outlives the session / a reconnect
        sBanner = false;
        sBannerSlot = -1;
        mp_log("ui: wait banner off (session or player gone)");
    }
    if (sBanner) ++sBannerAge;
    while (sLiveCount + (sSticky ? 1 : 0) + (sBanner ? 1 : 0) < MpPanel::kMaxToasts && sQueueCount > 0) {
        Live& l = sLive[sLiveCount++];
        MpPanel::copy(l.t.text, 96, sQueue[0].text);
        l.t.icon = sQueue[0].icon;
        l.t.alpha = 0;
        l.age = 0;
        memmove(&sQueue[0], &sQueue[1], sizeof(Pending) * (kQueue - 1));
        --sQueueCount;
    }
    // results
    if (sResults) {
        ++sResultsAge;
        if (sRes.hasTail) {
            if (sResHostSlot != mp_host_slot()) {   // B6: the host changed (migration): the new leader's card starts its own wait
                sResHostSlot = mp_host_slot();
                sResGuestIdleMs = 0;
            }
            if (guestLeaveLive() && (sResGuestIdleMs += dtMs) >= kGuestIdleMs) {   // B6: the leader never chose: this guest leaves (as the Leave Race row)
                sResLeft = sResLeaveTaken = true;
                mp_log("ui: results leave (the leader chose nothing)");
                mp_log("race leave slot=%d", mp_local_slot());
                char16_t t[96];
                txt::format(t, 96, MP_T_RACE_IDLE_LEFT, Args());
                toastText(t, 0);
            }
            // the card with choices stays until the session moves on (dismissResults)
            if (mp_is_host() && sResChoice == MpSeriesLogic::CH_NONE && (sResIdleMs += dtMs) >= kSeriesIdleMs) {   // uxfix #9: nobody at the leader's console
                // lobby: Race Again twice for the others; the third time with nobody pressing or asking in between, the series ends (Leave Race)
                sResTaken = sResChoice = MpSeriesLogic::autoChoice(sAutoRuns);
                ++sAutoRuns;
                mp_log("ui: results chosen %d (the leader was away, %d in a row)", static_cast<int>(sResChoice), sAutoRuns);
            }
        } else if ((okPressed && sResultsAge > 30) || sResultsAge >= kResultsFrames) {
            sResults = false;
            mp_log("ui: results closed (%s)", okPressed ? "button" : "timer");
        }
    }
}

bool resultsShowing() { return sResults; }
bool waitingShowing() { return sWaiting; }
bool headStartShowing() { return sHead.on; }
int toastCount() {
    int n = 0;
    if (sSticky) {
        sOut[n] = sStickyToast;
        sOut[n++].alpha = sStickyAge < kFadeFrames ? sStickyAge / static_cast<float>(kFadeFrames) : 1.0f;
    }
    if (sBanner && n < MpPanel::kMaxToasts) {
        sOut[n] = sBannerToast;
        sOut[n++].alpha = sBannerAge < kFadeFrames ? sBannerAge / static_cast<float>(kFadeFrames) : 1.0f;
    }
    for (int i = 0; i < sLiveCount && n < MpPanel::kMaxToasts; i++) sOut[n++] = sLive[i].t;
    return n;
}
const MpPanel::Toast* toasts() { return sOut; }

bool buildWaiting(MpPanel::Model* m) {
    using namespace MpPanel;
    if (!sWaiting || sWaitHidden || sResults || sStarCard) return false;
    clear(m);
    m->kind = KIND_WAITING;
    char16_t frac[8] = {static_cast<char16_t>(u'0' + sWaitDone), u'/', static_cast<char16_t>(u'0' + sWaitTotal), 0};
    char frac8[8] = {static_cast<char>('0' + sWaitDone), '/', static_cast<char>('0' + sWaitTotal), 0};
    (void)frac;
    Args a;
    a.str = frac8;
    txt::format(m->title, 48, MP_T_WAITING_TITLE, a);
    if (sWaitSeconds >= 0) {
        txt::format(m->bigLabel, 24, MP_T_TIME_LEFT, Args());
        const int s = sWaitSeconds > 5999 ? 5999 : sWaitSeconds;
        char16_t* b = m->big;
        int n = 0;
        if (s / 60 >= 10) b[n++] = static_cast<char16_t>(u'0' + s / 600);
        b[n++] = static_cast<char16_t>(u'0' + (s / 60) % 10);
        b[n++] = u':';
        b[n++] = static_cast<char16_t>(u'0' + (s % 60) / 10);
        b[n++] = static_cast<char16_t>(u'0' + s % 10);
        b[n] = 0;
    }
    m->spinner = (sFrame % 120) / 120.0f;
    // standings: finished players by time, then the others
    int order[MP_MAX_PLAYERS], n = 0;
    for (int k = 0; k < MP_MAX_PLAYERS; k++)
        if (mp_slot_present(k)) order[n++] = k;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            const uint32_t a0 = sStanding[order[j - 1]], b0 = sStanding[order[j]];
            const bool swap = (a0 == 0 && b0 != 0) || (a0 != 0 && b0 != 0 && b0 < a0);
            if (!swap) break;
            const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    int place = 0;
    for (int i = 0; i < n && m->rowCount < kMaxRows; i++) {
        const int k = order[i];
        Row& r = m->rows[m->rowCount++];
        r.ping = -1;
        r.marks = MARK_STATIC;
        txt::utf8To16(r.label, kLabel, mp_slot_name(k));
        r.icon = IconOf(k);
        if (k == mp_local_slot()) r.marks |= MARK_YOU;
        if (sStanding[k]) {
            ++place;
            txt::format(r.lead, kCol, static_cast<MpText>(MP_T_PLACE_1 + (place - 1)), Args());
            if (place == 1) r.marks |= MARK_GOLD;
            txt::formatTime(r.value, kValue, sStanding[k]);
        } else {
            txt::format(r.value, kValue, MP_T_PLAYING, Args());
        }
    }
    return true;
}

namespace {
// "<time>\t<Power Star><points>": the results card's two columns (the time, then the series: a Power Star and the points).
void seriesValue(char16_t* v, const char16_t* time, int points) {
    int k = 0;
    for (const char16_t* p = time; *p && k < MpPanel::kValue - 12; ++p) v[k++] = *p;
    v[k++] = u'\t';
    v[k++] = txt::picChar(txt::PIC_STAR);
    char digits[8];
    const int dn = snprintf(digits, sizeof digits, "%d", points > 99 ? 99 : (points < 0 ? 0 : points));
    for (int d = 0; d < dn && k < MpPanel::kValue - 1; d++) v[k++] = static_cast<char16_t>(digits[d]);
    v[k] = 0;
}
}  // namespace

bool buildResults(MpPanel::Model* m) {
    using namespace MpPanel;
    if (!sResults) return false;
    clear(m);
    for (int i = 0; i < kMaxRows; i++) sRowK[i] = -1, sRowLive[i] = false;
    m->kind = KIND_RESULTS;
    txt::format(m->title, 48, MP_T_RESULTS, Args());
    m->alpha = sResultsAge < 10 ? sResultsAge / 10.0f : 1.0f;
    const bool series = sRes.hasTail;
    if (series) m->look = LOOK_GAME;   // lobby: the series card in the file select's look (no glove: the highlighted choice is the cursor)
    const MpSeriesLogic::Block& sb = sRes.tail.series;
    // by place
    int idx[MP_MAX_PLAYERS];
    for (int i = 0; i < sRes.count; i++) idx[i] = i;
    for (int i = 1; i < sRes.count; i++)
        for (int j = i; j > 0 && (sRes.e[idx[j]].place ? sRes.e[idx[j]].place : 99) < (sRes.e[idx[j - 1]].place ? sRes.e[idx[j - 1]].place : 99); j--) {
            const int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    unsigned shown = 0;
    if (series && m->rowCount < kMaxRows) {
        // a small heading over the points column: a greyed row with only the "Series" cell (the time column empty)
        Row& h = m->rows[m->rowCount++];
        h.ping = -1;
        h.marks = MARK_STATIC | MARK_DISABLED;
        h.lead[0] = 0;
        h.label[0] = 0;
        char16_t head[kValue];
        txt::format(head, kValue, MP_T_SERIES, Args());
        int k = 0;
        h.value[k++] = u'\t';
        for (const char16_t* p = head; *p && k < kValue - 1; ++p) h.value[k++] = *p;
        h.value[k] = 0;
    }
    for (int i = 0; i < sRes.count && m->rowCount < kMaxRows; i++) {
        const MpRaceResults::Entry& e = sRes.e[idx[i]];
        Row& r = m->rows[m->rowCount++];
        r.ping = -1;
        r.marks = MARK_STATIC;
        txt::utf8To16(r.label, kLabel, sResNames[idx[i]]);
        r.icon = sResLook[idx[i]];
        shown |= 1u << e.slot;
        if (e.slot == mp_local_slot()) r.marks |= MARK_YOU;
        char16_t time[kValue];
        if (series && e.slot != mp_local_slot() && !mp_slot_present(e.slot)) {
            // smallfix: a player who left while the card is up: their line is greyed at once (no place, no time), as a leaver's line is
            r.marks |= MARK_DISABLED;
            r.lead[0] = u'-', r.lead[1] = 0;
            time[0] = 0;
        } else if (e.turns == MP_RACE_DNF) {
            r.lead[0] = u'-', r.lead[1] = 0;
            txt::format(time, kValue, MP_T_DNF, Args());
        } else {
            txt::format(r.lead, kCol, static_cast<MpText>(MP_T_PLACE_1 + (e.place - 1)), Args());
            txt::formatTime(time, kValue, e.turns);
            if (e.place == 1) r.marks |= MARK_GOLD;
        }
        if (series) seriesValue(r.value, time, sb.pts[e.slot]);
        else MpPanel::copy(r.value, kValue, time);
    }
    if (series) {
        // a player who left keeps their line until the series ends: greyed, no place, no time, the points they have
        for (int s = 0; s < MP_MAX_PLAYERS && m->rowCount < kMaxRows - 3; s++) {
            if (!(sb.left & (1u << s)) || (shown & (1u << s)) || !sSlotName[s][0]) continue;
            Row& r = m->rows[m->rowCount++];
            r.ping = -1;
            r.marks = MARK_STATIC | MARK_DISABLED;
            r.lead[0] = u'-', r.lead[1] = 0;
            txt::utf8To16(r.label, kLabel, sSlotName[s]);
            r.icon = sSlotLook[s];
            const char16_t none[2] = {0, 0};
            seriesValue(r.value, none, sb.pts[s]);
        }
        // the next step: Race Again, [Next Star], Leave Race. The host's cursor (one press chooses); a guest sees the host's choice
        const bool host = mp_is_host();
        const bool chosen = sResChoice != MpSeriesLogic::CH_NONE;
        const struct { uint8_t ch; MpText id; } opts[3] = {{MpSeriesLogic::CH_AGAIN, MP_T_RACE_AGAIN}, {MpSeriesLogic::CH_NEXT, MP_T_NEXT_STAR}, {MpSeriesLogic::CH_LEAVE, MP_T_LEAVE_RACE}};
        int k = 0;
        for (int o = 0; o < 3 && m->rowCount < kMaxRows; o++) {
            if (opts[o].ch == MpSeriesLogic::CH_NEXT && sRes.tail.next == 0) continue;
            Row& r = m->rows[m->rowCount];
            r.ping = -1;
            r.marks = MARK_STATIC;
            txt::format(r.label, kLabel, opts[o].id, Args());
            const bool mine = chosen && opts[o].ch == sResChoice;
            if (chosen && !mine) r.marks |= MARK_DISABLED;
            if (!host && !chosen) {
                // smallfix: not the leader: every row is greyed but Leave Race; lobby: and Race Again? (one ask per card)
                if (opts[o].ch == MpSeriesLogic::CH_AGAIN) {
                    txt::format(r.label, kLabel, MP_T_RACE_ASK, Args());
                    if (sResAsked) r.marks |= MARK_DISABLED;
                    else if (!sGuestOnLeave) m->cursor = m->rowCount;
                } else if (opts[o].ch != MpSeriesLogic::CH_LEAVE) {
                    r.marks |= MARK_DISABLED;
                } else if (sGuestOnLeave || sResAsked) {
                    m->cursor = m->rowCount;
                }
            }
            if (mine || (host && !chosen && k == sResCursor)) m->cursor = m->rowCount;
            sRowK[m->rowCount] = static_cast<int8_t>(k);
            sRowLive[m->rowCount] = host ? !chosen : (guestLeaveLive() && (opts[o].ch == MpSeriesLogic::CH_LEAVE || (opts[o].ch == MpSeriesLogic::CH_AGAIN && !sResAsked)));
            if (sRowLive[m->rowCount] || host) r.marks |= MARK_CHOICE;   // lobby: a choice this player can press is a big row (at least kListRowH high)
            ++m->rowCount;
            ++k;
        }
        if (!host && !chosen) {
            Args a;
            a.str = mp_slot_name(mp_host_slot());
            txt::format(m->status, 80, sResAsked ? MP_T_RACE_ASKED : MP_T_RACE_HOST_PICKS, a);
            if (!sResLeft) {
                m->buttonCount = 1;
                m->buttons[0].pic = txt::PIC_CROSS;
                txt::format(m->buttons[0].label, 20, MP_T_SELECT, Args());
            }
        }
        if (host && !chosen) {
            m->buttonCount = 1;
            m->buttons[0].pic = txt::PIC_CROSS;
            txt::format(m->buttons[0].label, 20, MP_T_SELECT, Args());
        }
        return true;
    }
    const int left = (kResultsFrames - sResultsAge + 59) / 60;
    m->buttonCount = 1;
    m->buttons[0].pic = txt::PIC_CROSS;
    Args a;
    a.num = left < 0 ? 0 : left;
    txt::format(m->buttons[0].label, 20, MP_T_CONTINUE_IN, a);
    return true;
}

// race: the head start line on the countdown: the last winner's head and name, and under it "Starts 3 s late". A small window low on the
// picture (the countdown's numbers stay clear in the middle). No button, nothing to choose.
bool buildHeadStart(MpPanel::Model* m) {
    using namespace MpPanel;
    if (!sHead.on) return false;
    clear(m);
    m->kind = KIND_WAITING;
    m->lift = -128.0f;
    m->width = 380.0f;
    Row& r = m->rows[m->rowCount++];
    r.ping = -1;
    r.marks = MARK_STATIC;
    txt::utf8To16(r.label, kLabel, sHead.name);
    r.icon = sHead.look;
    if (sHead.slot == mp_local_slot()) r.marks |= MARK_YOU;
    Args a;
    a.num = sHead.sec;
    txt::format(m->status, 80, MP_T_HEAD_START, a);
    return true;
}

}  // namespace MpOverlay

namespace MpOverlay {

// "TEAM STAR!" / "<name> got the Power Star!" between two Power Star pictures, then one row per player in slot order: head, name and
// what that player did for this star (star bits, coins, helped), each count after its picture. No place, no time, no button.
bool buildStarCard(MpPanel::Model* m) {
    using namespace MpPanel;
    if (!sStarCard) return false;
    clear(m);
    m->kind = KIND_RESULTS;
    char16_t head[80];
    if (sCard.team) {
        txt::format(head, 80, MP_T_TEAM_STAR, Args());
    } else {
        Args a;
        a.str = sCard.by >= 0 && sCard.by < MP_MAX_PLAYERS ? mp_slot_name(sCard.by) : "";
        txt::format(head, 80, MP_T_GOT_STAR, a);
    }
    char16_t* t = m->title;
    int n = 0;
    t[n++] = txt::picChar(txt::PIC_STAR);
    t[n++] = u' ';
    for (const char16_t* p = head; *p && n < 44; ++p) t[n++] = *p;
    t[n++] = u' ';
    t[n++] = txt::picChar(txt::PIC_STAR);
    t[n] = 0;
    const int age = sStarAge, left = kStarCardFrames - sStarAge;
    m->alpha = age < 12 ? age / 12.0f : (left < 12 ? (left < 0 ? 0.0f : left / 12.0f) : 1.0f);
    for (int i = 0; i < sCard.count && m->rowCount < kMaxRows; i++) {
        const MpStarCard::Entry& e = sCard.e[i];
        Row& r = m->rows[m->rowCount++];
        r.ping = -1;
        r.marks = MARK_STATIC;
        txt::utf8To16(r.label, kLabel, sCardNames[i]);
        r.icon = sCardLook[i];
        if (e.slot == mp_local_slot()) r.marks |= MARK_YOU;
        char16_t* v = r.value;
        int k = 0;
        if (!e.have) {
            v[k++] = u'-';
        } else {
            const struct { int pic; unsigned n; } cells[3] = {{txt::PIC_STAR_BIT, e.starBits}, {txt::PIC_COIN, e.coins}, {txt::PIC_MARIO, e.helped}};
            for (int c = 0; c < 3; c++) {
                if (c > 0) v[k++] = u'\t';   // (columns: the panel's Rows split at tabs)
                v[k++] = txt::picChar(cells[c].pic);
                char digits[8];
                const int dn = snprintf(digits, sizeof digits, "%u", cells[c].n > 9999 ? 9999u : cells[c].n);
                for (int d = 0; d < dn && k < kValue - 1; d++) v[k++] = static_cast<char16_t>(digits[d]);
            }
        }
        v[k] = 0;
    }
    if (sCard.teamBits >= 0 && m->rowCount < kMaxRows) {   // costar: the team line, the shared count that feeds the Hungry Lumas
        Row& r = m->rows[m->rowCount++];
        r.ping = -1;
        r.marks = MARK_STATIC;
        r.icon = 0;
        txt::format(r.label, kLabel, MP_T_TEAM_TOTAL, Args());
        char16_t* v = r.value;
        int k = 0;
        v[k++] = txt::picChar(txt::PIC_STAR_BIT);
        char digits[8];
        const int dn = snprintf(digits, sizeof digits, "%d", sCard.teamBits > 9999 ? 9999 : sCard.teamBits);
        for (int d = 0; d < dn && k < kValue - 1; d++) v[k++] = static_cast<char16_t>(digits[d]);
        v[k] = 0;
    }
    return true;
}

}  // namespace MpOverlay
