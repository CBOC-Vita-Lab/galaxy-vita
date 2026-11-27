// Galaxy 3.0 multiplayer: the ui module's public extras (owner: ui), beside MpUi in MpGame.hpp. Pure declarations (no game headers).
//
//  - MpUi::handleMessage: TOAST (0x50) and STAMP (0x51) for MpGame.cpp's router.
//  - MpUi::setStanding: the race waiting screen's live standings (flow calls it on FINISH).
//  - MpUi::takeLaunchRequest / takeManualRequest / onFileSelect: the file select scene's side (FileSelector hooks).
//  - The RESULTS payload codec (flow writes it, ui reads it).
#pragma once

#include <stdint.h>

#include "Game/MP/MpGame.hpp"
#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpSeriesLogic.hpp"

namespace MpUi {
    bool handleMessage(const MpMsg& m);              // true = it was a TOAST or STAMP (consumed; bad payloads are dropped)
    void setStanding(int slot, uint32_t turns);      // race: this slot finished in `turns` (0 = still playing); cleared by showWaiting(false,..)
    bool takeLaunchRequest(int* fileId);             // file select: the session started; load this user file (1..6) now
    bool takeManualRequest();                        // file select: "How to Play" was chosen; open the game's 2P manual
    void onFileSelect();                             // FileSelector::init: back at the file select (session over: tidy the save root)
    // fix2: FileSelector::control: retries a failed save root switch, then warns. fix3: only when canWarn (the file select can
    // hold its input); true on the turn our window opened: the file select holds its input until isMenuOpen() is false
    bool fileSelectTick(bool canWarn);
    // flow (review 2 #4): the entry screen's Single Player: true = not now (the save root is still the multiplayer one and could not be
    // switched back: the game's window says so); false = go on (switched now, or nothing pending)
    bool singlePlayerGate();
    bool featureEnabled();                           // SMG_MP_MENU (env, default 1): the 2P button opens the Multiplayer menu
    const wchar_t* balloonText();                    // the 2P button's pointing balloon: "Multiplayer" (current language)
    const wchar_t* fileSelectLabel();                // the 2P button's own small label (two short lines)
    void sendStamp(int stamp);                       // 0..3: send to everyone and show locally
    bool takeWarpRequest(int* slot);                 // flow polls: the session menu's "Warp to <player>" was chosen
    bool takeDomeRequest();                          // flow polls: the host chose "Return to the Observatory" (and confirmed)
    bool settledQuiet();                             // optfix: nothing of ours on screen or waiting: MpGame may retire mp_entered()
    bool reconnecting();                             // fix3: the core rejoins the session on its own (mp_active() is false meanwhile)
    void openMultiplayerMenuFromEntry();             // entry: the entry screen's Multiplayer (MpEntry; no file chosen beforehand)
    bool openSettingsFromPause();                    // settingsnative: the pause menu's Settings (false: refused, e.g. no room for the menu's layouts in this scene)
    bool requestSettings(bool cold);                 // settingsnative: port_settings_native_request: cold latches the open for the entry screen; running asks the pause menu
    bool takeSettingsLatch();                        // settingsnative: the stale LiveArea tile started the game with -settings: the entry screen takes this once
    void openSettingsFromEntry();                    // settingsnative: the entry screen's Settings: the menu is drawn and read, nothing of multiplayer starts
    // race: the results card's next step and the head start line (MpRace.cpp). The card with choices stays until closeResults.
    void closeResults();                             // the session moved on (new scene / left): the card goes
    void setResultsChoice(int choice);               // MpSeriesLogic::Choice the host made: guests see it
    bool takeResultsChoice(int* choice);             // the host pressed a choice on the card (once)
    void showHeadStart(int slot, int seconds);       // the countdown's line under the last winner's name; seconds <= 0 hides
    void waitBanner(int slot);                       // uxwait: a small "Waiting for <name>..." line while another console holds this one (slot < 0 = off)
    void forgetSeriesNames();                        // a new session: the names kept for greyed lines go
    bool demoStripShown();                           // MP3 demo: the party strip was composed this frame
}

// ---- the team star card (Play Together, after a Power Star): who was there and what each player did for this star. Friendly counts, no
// ranking (slot order). Fed by MpTeam.cpp; shown by MpOverlay for kStarCardFrames; never holds the player or takes a button.
struct MpStarCard {
    uint8_t count;    // entries (1..4)
    int8_t by;        // slot that collected the star, -1 unknown
    bool team;        // everyone present was near the star: "TEAM STAR!"
    int32_t teamBits; // costar: the shared Star Bit count (Play Together), -1 = no team line
    struct Entry { uint8_t slot; bool have; uint16_t starBits, coins; uint8_t helped; } e[MP_MAX_PLAYERS];
};
namespace MpUi {
    void showStarCard(const MpStarCard& card);       // (a card still showing is refreshed, its timer keeps running)
    void hideStarCard();                             // a later scene plays: the card goes
    void announcePick(int slot, const char* stage, int star);   // touches: toast "<slot> picked <galaxy>, Star <star>." (game's own names)
}

// ---- RESULTS (0x36) payload, as flow writes it (MpFlowLogic.hpp writeResults): serial u16, count u8 (1..4), then
// count x {slot u8, frames u32 (60/s; 0xFFFFFFFF = DNF), place u8 (1..count; 0 = DNF)}, then optionally the 8-byte tail (race: the series
// after this race, the host's choice for the next step, the next scenario; MpSeriesLogic::Tail). Without a tail the card is the old one
// (Continue, closes by itself).
struct MpRaceResults {
    uint16_t serial;
    uint8_t count;
    struct Entry { uint8_t slot, place; uint32_t turns; } e[MP_MAX_PLAYERS];
    bool hasTail = false;
    MpSeriesLogic::Tail tail = MpSeriesLogic::Tail();
};
#define MP_RACE_DNF 0xFFFFFFFFu
static inline void mp_write_results(MpWriter& w, const MpRaceResults& r) {
    w.u16(r.serial); w.u8(r.count);
    for (int i = 0; i < r.count && i < MP_MAX_PLAYERS; i++) { w.u8(r.e[i].slot); w.u32(r.e[i].turns); w.u8(r.e[i].place); }
    if (r.hasTail) MpSeriesLogic::writeTail(w, r.tail);
}
static inline bool mp_read_results(MpReader& r, MpRaceResults* out) {
    out->serial = r.u16(); out->count = r.u8();
    out->hasTail = false;
    if (!r.ok() || out->count < 1 || out->count > MP_MAX_PLAYERS) return false;
    for (int i = 0; i < out->count; i++) {
        out->e[i].slot = r.u8(); out->e[i].turns = r.u32(); out->e[i].place = r.u8();
        if (!r.ok() || out->e[i].slot >= MP_MAX_PLAYERS || out->e[i].place > out->count) return false;
        if ((out->e[i].place == 0) != (out->e[i].turns == MP_RACE_DNF)) return false;
        for (int k = 0; k < i; k++) if (out->e[k].slot == out->e[i].slot) return false;
    }
    if (r.ok() && r.len > r.n) {
        if (r.len - r.n != (size_t)MpSeriesLogic::kTailBytes || !MpSeriesLogic::readTail(r, &out->tail)) return false;
        out->hasTail = true;
        for (int i = 0; i < out->count; i++) if (!(out->tail.series.in & (1u << out->e[i].slot))) return false;
    }
    return r.done();
}
// TOAST (0x50): text id u16, slot u8 (0xFF none), arg u32.   STAMP (0x51): stamp id u8 (0..3).

// The save module: Game/MP/MpSaveGame.hpp (owner: save), included through MpGame.hpp.

// memfix: the multiplayer UI's layouts belong to a scene (MpUiLayouts.hpp): nothing is made at boot.
namespace MpUi {
    void sceneBegin(bool session);                   // GameScene::init after placement (MpGame.cpp): a session scene makes its layouts now
    void sceneEnd();                                 // GameScene destructor (MpGame.cpp): the scene's layouts are forgotten
    void sceneRunning();                             // integ: first update of the scene: its init thread is done
    void addArchives(void* heap);                    // integ: the archive-request phase of a session scene: the layout archives its UI needs (heap: JKRHeap*, null = apropos)
}
