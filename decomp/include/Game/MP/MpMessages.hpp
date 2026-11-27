// Galaxy 3.0 multiplayer: every word the players see (owner: ui). English, French, Spanish, German, Italian; the language follows the game's.
// Pure code (no game headers): the host tests and tools/mp/panel_preview build it as is.
//
// Two ways in:
//  - numeric text ids (MpText) for the panel, the toasts and other modules (MpUi::toast(MP_T_..., slot, arg));
//  - string ids "MP_..." for the game's own message windows: MessageData::getMessageDirect falls back to MpUi::findMessage
//    for unknown ids that start with "MP_" (SysInfoWindow asks for "<id>", "<id>_Yes" and "<id>_No").
//
// Markup inside the texts: U+E000 + n = the picture font's picture n (0 Cross, 1 Circle, 2 Triangle, 5 Square, 7 Power Star, 10 star bit, 16 "x", 17 coin, 18 Mario's head);
// '\x01' = the string argument, '\x02' = the second string argument, '\x03' = the number argument, '\x04' = a time (m:ss.cc).
// For the message windows the pictures become the game's picture tags (0x1A, size 6 | group 3 << 8, picture).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace MpMsgText {

enum Lang : int { LANG_EN = 0, LANG_FR = 1, LANG_ES = 2, LANG_DE = 3, LANG_IT = 4, LANG_COUNT = 5 };   // DE/IT live in MpMessagesExtra.cpp

// Picture font pictures (message tag picture numbers; the picture font's character is 0x30 + n).
enum Pic : uint16_t { PIC_CROSS = 0, PIC_CIRCLE = 1, PIC_TRIANGLE = 2, PIC_SQUARE = 5, PIC_STAR = 7, PIC_STAR_BIT = 10, PIC_TIMES = 16, PIC_COIN = 17,
                      PIC_MARIO = 18, PIC_SELECT = 23, PIC_START = 24, PIC_L = 25, PIC_HAND = 38 };   // 23 / 24 / 25: the font's - + Z: the handhelds draw SELECT, START, L there (MpControls.hpp)
inline constexpr char16_t picChar(int pic) { return static_cast<char16_t>(0xE000 + pic); }
inline constexpr bool isPicChar(char16_t c) { return c >= 0xE000 && c < 0xE100; }


// The galaxies with Power Stars, in the Observatory's order: the "galaxy index" of the leaderboard (mp_ui_board_request,
// mp_race_submit). Names come from the game's messages "GalaxyName_<stage>" / "ScenarioName_<stage><n>".
int galaxyCount();
const char* galaxyStage(int index);   // null when out of range
int galaxyIndexOf(const char* stage); // -1 when not a race galaxy

}  // namespace MpMsgText

// Text ids. The toast ids (MP_T_TOAST_FIRST..) are a contract with the other modules (TOAST message: text id u16, slot u8, arg u32).
enum MpText : uint16_t {
    MP_T_NONE = 0,
    // ---- toasts (contract: keep the numbers)
    MP_T_JOINED = 1,            // "<slot> joined."
    MP_T_LEFT = 2,              // "<slot> left."
    MP_T_GOT_STAR = 3,          // "<slot> got the Power Star!"
    MP_T_FINISHED = 4,          // "<slot> finished in <arg: turns at 60/s>!"
    MP_T_HOST_CHOOSES = 5,      // "The host chooses the galaxy."
    // (6, 11 and 15 are unused numbers of the toast contract: no names, no text; a toast that names one shows nothing)
    MP_T_STAMP_0 = 7,           // "<slot>: Over here!"   (7..10: the four stamps; arg unused)
    MP_T_STAMP_1 = 8,
    MP_T_STAMP_2 = 9,
    MP_T_STAMP_3 = 10,
    MP_T_HOST_MIGRATED = 12,    // "<arg: old slot> left. <slot> is now the host."
    MP_T_YOU_ARE_HOST = 13,     // "You are now the host."
    MP_T_EVERYONE_LEFT = 14,    // "Everyone left."
    MP_T_WARPING = 16,          // "Warping to <slot>..."
    MP_T_RACE_GO_SOON = 17,     // "The race starts soon!"
    MP_T_PLAYER_DOWN = 18,      // "<slot> fell."
    MP_T_WARPED_TO_YOU = 19,    // "<slot> warped to you."
    MP_T_WARP_FAILED = 20,      // "Can't warp to <slot> right now."
    MP_T_NO_SETTINGS = 21,      // "Settings can't be opened during a session."
    MP_T_SKIP_VOTE = 22,        // "<slot> wants to skip (n/m)."   (the second string argument: "n/m")
    MP_T_HOST_RACES_ONLY = 23,  // "Only the host races Cosmic Mario."
    MP_T_BUBBLED = 24,          // "<slot> is in a bubble!"                (bubble agent: Play Together "Bubbles" falls)
    MP_T_POPPED = 25,           // "<slot> popped <arg: slot>'s bubble!"
    MP_T_ALL_CAUGHT = 26,       // "Everyone was caught! Back to the checkpoint."
    MP_T_OUT_OF_LIVES = 27,     // "<slot> is out of lives: pop the bubble to save them!"
    MP_T_PICKED = 28,           // "<slot> picked <galaxy>, Star <n>." (touches: shown before a load; MpOverlay::toastPick, never from the wire)
    MP_T_HOST_BROWSING = 29,    // "<slot> is choosing a galaxy."      (touches: replaces "The host chooses the galaxy.")
    MP_T_HOST_BROWSING_STAR = 31,   // "<slot> is choosing a star."
    MP_T_HOST_DATA_BAD = 30,    // "The host's save data can't be used." (snapshot refused by the validator; 24..27: bubble)
    // (flow's toast ids MP_TXT_* 0x0301..0x030B, Game/MP/MpFlow.hpp, are understood too: MpOverlay::toast maps them)
    MP_T_STAR_NOT_SAVED = 32,   // "<slot>'s Power Star couldn't be saved this time. Sorry!" (fix2: a star claim refused on a single-player file; arg = star id)
    MP_T_TOAST_LAST = 32,   // (28, 29, 31: touches)
    // ---- menus (internal to ui; numbers may change)
    MP_T_TITLE_MULTI = 33, MP_T_PLAY_TOGETHER, MP_T_STAR_RACE, MP_T_LEADERBOARDS, MP_T_FRIENDS, MP_T_HOW_TO_PLAY, MP_T_BACK,
    MP_T_INTERNET, MP_T_SAME_WIFI, MP_T_NEARBY,
    MP_T_QUICK_MATCH,
    MP_T_PLAYERS, 
    MP_T_OFF, MP_T_ON,
    MP_T_CODE_ONLY, MP_T_VIS_FRIENDS, MP_T_ANYONE,
    MP_T_FALLS, MP_T_FALLS_BUBBLES, MP_T_FALLS_LIVES, // (MpSettings.downed: 0 Bubble, 1 Try again)
    MP_T_TITLE_SAVE, MP_T_COOP_FILE, MP_T_NEW,
    MP_T_TITLE_SLOT, MP_T_DESC_SLOT,
    MP_T_DESC_SLOT_KEEP,
    MP_T_OTHER_VERSION,
    MP_T_YOUR_CODE, MP_T_ONLINE, MP_T_IN_ROOM, MP_T_OFFLINE, MP_T_JOIN, MP_T_ADD, MP_T_REMOVE, MP_T_NO_FRIENDS, MP_T_LOADING,
    MP_T_LOAD_FAILED, MP_T_RETRY,
    MP_T_TAB_TOP, MP_T_TAB_NEAR, MP_T_TAB_FRIENDS, MP_T_GALAXY, MP_T_STAR, MP_T_NO_TIMES, MP_T_TAB_HINT, MP_T_STAR_N,
    MP_T_CONNECTING, MP_T_CREATING, MP_T_SEARCHING, MP_T_JOINING, MP_T_RECEIVING, MP_T_CANCEL,
    MP_T_START, MP_T_LEAVE, MP_T_YOU, 
    
    MP_T_RESUME, MP_T_WARP, MP_T_STAMPS, MP_T_PLAYER_LIST, MP_T_BACK_DOME, MP_T_LEAVE_SESSION, MP_T_TITLE_WARP, MP_T_CLOSE,
    MP_T_STAMP_TEXT_0, MP_T_STAMP_TEXT_1, MP_T_STAMP_TEXT_2, MP_T_STAMP_TEXT_3, MP_T_SEND, MP_T_LAG,
    MP_T_WAITING_TITLE, MP_T_TIME_LEFT, MP_T_PLAYING, MP_T_RESULTS, MP_T_PLACE_1, MP_T_PLACE_2, MP_T_PLACE_3, MP_T_PLACE_4, MP_T_DNF,
    MP_T_CONTINUE, MP_T_CONTINUE_IN, MP_T_OK, MP_T_SELECT, MP_T_CHANGE, MP_T_TAB,
    MP_T_KB_NAME, MP_T_KB_FRIEND, MP_T_FILESEL_ICON, MP_T_FILESEL_BALLOON, MP_T_SESSION_TITLE, MP_T_HOST_TAG,
    MP_T_TITLE_CHARS, MP_T_CHAR_MARIO, MP_T_CHAR_LUIGI, MP_T_CHAR_TOAD, MP_T_CHAR_COSMIC, MP_T_CHAR_YELLOW_TOAD,
    MP_T_CHAR_COSMIC_LUIGI, MP_T_DESC_CHARS, MP_T_TAKEN,
    MP_T_CHANGE_CHAR, MP_T_SAVE_TO_MY_FILE, MP_T_DESC_SLOT_LEAVE, MP_T_DESC_SLOT_RECOVER, MP_T_WAIT_CHAR,
    MP_T_TEAM_STAR,             // the star card's title when everyone was near the star (the Power Star picture goes either side)
    MP_T_RECONNECTING,   // fix2
    // netx: internet play by address (Online > Play by Address)
    // friendsui: friend requests and blocked players
    MP_T_REQUESTS, MP_T_TITLE_REQUESTS, MP_T_TITLE_REQUEST, MP_T_NOT_NOW, MP_T_BLOCK, MP_T_UNBLOCK, MP_T_BY_CODE, MP_T_BLOCKED_PLAYERS,
    MP_T_DESC_REQUESTS, MP_T_DESC_BLOCKED, MP_T_DESC_REQ_ADD, MP_T_DESC_NOT_NOW, MP_T_DESC_BLOCK, MP_T_DESC_UNBLOCK,
    MP_T_NO_BLOCKED, MP_T_KB_UNBLOCK,
    // chat: the Messages page (MP_CHAT_PHRASES phrases; 0..3 are the four stamps' words), typed messages, mute
    MP_T_PHRASE_0, MP_T_PHRASE_1, MP_T_PHRASE_2, MP_T_PHRASE_3, MP_T_PHRASE_4, MP_T_PHRASE_5, MP_T_PHRASE_6, MP_T_PHRASE_7,
    MP_T_PHRASE_8, MP_T_PHRASE_9, MP_T_PHRASE_10, MP_T_PHRASE_11,
    MP_T_WRITE, MP_T_DESC_MESSAGES, MP_T_KB_CHAT, MP_T_MUTE, MP_T_UNMUTE, MP_T_MUTED, MP_T_CHAT_FROM,
    MP_T_WATCHING, MP_T_WATCH_HINT,   // spectate: the label of a teammate's view, and its one-time hint
    // entry: the entry screen's two buttons; the Multiplayer menu's single-player file picker (opened from the entry screen)
    MP_T_ENTRY_SINGLE, MP_T_ENTRY_MULTI, MP_T_TITLE_SPFILE, MP_T_SP_FILE,
    // flow (wave 2b): Join / Host, the Join list, Room Settings in plain words, Which file?, a player's choices, Played With, Rejoin
    MP_T_REJOIN, MP_T_SCAN_NEARBY, MP_T_SCAN_WIFI, 
    MP_T_NEW_COOP_FILE, MP_T_OWN_FILE, MP_T_DESC_NEW_COOP, MP_T_DESC_OWN_FILE,
    MP_T_DESC_COOP_N, MP_T_LAST_PLAYED, MP_T_DESC_SPFILE_PICK, MP_T_USE_COPY, MP_T_USE_PLAY, MP_T_PLAYED_WITH, MP_T_TITLE_PLAYED,
    MP_T_DESC_PLAYED, MP_T_CHANGE_CHAR_LONG, MP_T_ADD_FRIEND, MP_T_REMOVE_ROOM, 
    // flow (one door): the gathering screen's lines and buttons, the falls question, Join with a Code
    MP_T_G_WAITING, MP_T_G_GETTING_CODE, MP_T_G_TELL_CODE, MP_T_G_PRESS_START, MP_T_G_WAIT_NAME, MP_T_G_NOBODY, MP_T_G_FULL, MP_T_G_OTHER_VER, MP_T_REJOIN_CLOSED, MP_T_G_JOIN_PLAYING, MP_T_INVITE_FAR, MP_T_JOIN_CODE,
    MP_T_SESSION_RUNS, MP_T_GO_TO_Q, MP_T_PLAYERS_Q,   // m-session: the pause screen's, Go to a Friend's and Players' one line
    // m-conntest: "Who Can I Play With?" (its stopgap table, now here)
    MP_T_CT_TITLE, MP_T_CT_Q_ROOM, MP_T_CT_Q_FAR, MP_T_CT_Q_ONLINE, MP_T_CT_V_GOOD, MP_T_CT_V_CHOPPY, MP_T_CT_V_NOT_NOW, MP_T_CT_CHECKING, MP_T_CT_AGAIN, MP_T_CT_WAIT_ROOM, MP_T_CT_WAIT_FAR, MP_T_CT_WAIT_ONLINE, MP_T_CT_TOOK_LONG, MP_T_CT_L_CLOSER, MP_T_CT_L_WIFI, MP_T_CT_L_SEE, MP_T_CT_L_FAR_MAYBE, MP_T_CT_L_FAR_NO, MP_T_CT_L_SLOW, MP_T_CT_L_DOWN, MP_T_CT_L_UPDATE, MP_T_CT_L_LATER, MP_T_CT_L_CLOCK,
    // m-files: the file screens and Safe Copies (their stopgap table, now here; MpMenuFiles shows them in its own windows)
    MP_T_KS_CANT_READ, MP_T_KS_CANT_READ_HINT, MP_T_KS_SP_CANT_READ_HINT, MP_T_KS_SLOTS_CANT_READ, MP_T_KS_NO_ROOM_COPY, MP_T_KS_FILE_NOT_READY, MP_T_KS_PLAY_MINE, MP_T_KS_SAVE_MINE, MP_T_KS_SAVE_MINE_NOSTARS, MP_T_KS_KEEP_TITLE, MP_T_KS_KEEP_ROW, MP_T_KS_KEEP_MAKE, MP_T_KS_KEEP_BACK, MP_T_KS_KEEP_H_MAKE, MP_T_KS_KEEP_H_BACK, MP_T_KS_KEEP_LIST_TITLE, MP_T_KS_KEEP_MADE, MP_T_KS_KEEP_NOTHING, MP_T_KS_KEEP_NO_SPACE, MP_T_KS_KEEP_FAILED, MP_T_KS_KEEP_BUSY, MP_T_KS_KS_NOTICE, MP_T_KS_BK_ACCOUNT, MP_T_KS_BK_ROLLED, MP_T_KS_BK_BAD, MP_T_KS_BK_NEWER, MP_T_KS_BK_NO_SPACE, MP_T_KS_BK_NOTHING,     MP_T_PLAY_ANYONE, MP_T_FALLS_Q, MP_T_FALLS_A_BUBBLE, MP_T_FALLS_A_TRY, MP_T_CHANGE_FILE, MP_T_KB_DIRECT_CODE, MP_T_KS_NO_COPY, MP_T_KS_TOO_MANY, MP_T_KS_KEEP_SHARE, MP_T_KS_KEEP_H_SHARE, MP_T_KS_MADE_SHARE, MP_T_KS_L_VERSION, MP_T_KS_L_BEFORE, MP_T_KS_L_MADE, MP_T_KS_H_HOLDS, MP_T_KS_ROW_TEAMFILES, MP_T_KS_ROW_ACCOUNT, MP_T_KS_ROW_SET_CONFIG, MP_T_KS_ROW_SET_FLOW, MP_T_KS_ROW_SET_COOP, MP_T_KS_H_SLOT_NEW, MP_T_KS_H_SLOT_HERE, MP_T_KS_H_ACCOUNT, MP_T_KS_Q_SLOT_REPLACE, MP_T_KS_Q_TEAM_DIFFERENT, MP_T_KS_Q_SET_CONFIG, MP_T_KS_Q_SET_FLOW, MP_T_KS_Q_SET_COOP, MP_T_KS_D_SLOT_BACK, MP_T_KS_D_SLOT_BACK_PLAIN, MP_T_KS_D_SLOT_SAME, MP_T_KS_D_ACCOUNT_DONE, MP_T_KS_D_ACCOUNT_SAME, MP_T_KS_D_SET_DONE,
    // conntest: the Connection Test screen. The verdict's headlines and lines are in the order of MpCtHeadline / MpCtLine (port/mp_conntest.h)
    // (the other two checks are named Same Wi-Fi and Internet, as in the Connection menu)
    
    // (a note, not a verdict line: relay only is on; the Details window)
    // touches: the host-rules card (an OK window: who chooses, and what a fall costs or how the race is won) and its toast version
    MP_T_RULES_HOST_T, MP_T_RULES_HOST_R, MP_T_RULES_FALL_BUBBLE, MP_T_RULES_FALL_TRY, 
    MP_T_RULES_RACE, MP_T_RULES_TOAST, MP_T_RULES_CHANGED, MP_T_RULES_P_BUBBLES, MP_T_RULES_P_TRY,
    
    // ---- message window texts (each has an "MP_..." string id, see MpMsgText::findDialog)
    MP_T_DLG_FIRST,
    MP_T_ERR_NO_WIFI = MP_T_DLG_FIRST, MP_T_ERR_SERVICE_DOWN, MP_T_ERR_UPDATE, MP_T_ERR_ROOM_NOT_FOUND, MP_T_ERR_ROOM_FULL,
    MP_T_ERR_VERSION, MP_T_ERR_TIMEOUT, MP_T_ERR_HOST_LEFT, MP_T_ERR_KICKED, MP_T_ERR_NAT, MP_T_ERR_NAME_TAKEN, MP_T_ERR_BAD_NAME,
    MP_T_ERR_RATE, MP_T_ERR_INTERNAL,
    MP_T_ERR_CLOCK,   // flow: MP_ERR_CLOCK (the console's date and time are wrong)
    MP_T_DLG_SETTINGS_RO,   // flow (savefix): mp.cfg is not written this run (a newer game's file, or a damaged one)
    MP_T_DLG_LEAVE_GUEST, MP_T_DLG_LEAVE_HOST, MP_T_DLG_CLOSE_ROOM, MP_T_DLG_KICK, MP_T_DLG_PLAY_MINE, MP_T_DLG_OVERWRITE,
    MP_T_DLG_KEEP, MP_T_DLG_KEPT, MP_T_DLG_KEEP_FAILED, MP_T_DLG_FRIEND_ADDED, MP_T_DLG_FRIEND_REMOVE,
    MP_T_DLG_BACK_DOME, MP_T_DLG_EVERYONE_LEFT, MP_T_DLG_POWER_SAVE, MP_T_DLG_SAVE_FAILED, MP_T_DLG_NO_ROOM_FILE,
    MP_T_DLG_FRIEND_FAILED,
    MP_T_DLG_SAVE_ASK, MP_T_DLG_FEWER_STARS, MP_T_DLG_RECOVERY, MP_T_DLG_SAVED, MP_T_DLG_SAVE_MINE, MP_T_DLG_IMPORT_FEWER,
    MP_T_DLG_SP_UNAVAILABLE,   // fix2
    MP_T_DLG_ADDR_NO_ANSWER,   // netx
    MP_T_DLG_REQ_BLOCK, MP_T_DLG_REQ_ACCEPTED, MP_T_DLG_REQ_BLOCKED, MP_T_DLG_UNBLOCKED, MP_T_DLG_REQ_FAILED,   // friendsui
    // fix3: the online service's own refusals (mp_service_last_code), worded here instead of "Something went wrong."
    MP_T_ERR_BANNED, MP_T_ERR_FRIENDS_FULL, MP_T_ERR_NOT_FRIENDS, MP_T_ERR_NO_PLAYER, MP_T_ERR_OWN_CODE, MP_T_ERR_NOT_MEMBER,
    MP_T_ERR_DAILY,
    MP_T_ERR_BUSY,   // quota32: the service (or Cloudflare in front of it) is out of quota / overloaded: "busy right now, try later" (code "service_busy")
    MP_T_DLG_SP_FEWER,   // gamefix
    MP_T_DLG_HOWTO, MP_T_DLG_REJOIN_GONE,   // flow
    MP_T_DLG_TELL_CODE, MP_T_DLG_CODE_BAD, MP_T_DLG_CANT_REACH,   // flow (one door): far away
    MP_T_DLG_CT_OFFER,   // conntest
    // release3: Save a report
    MP_T_ERR_DROPPED,   // uxfix
    MP_T_DLG_DATA_MISMATCH,   // uxfix
    MP_T_ERR_ROOM_STARTED,   // uxfix
    MP_T_KS_HOWTO_SAFE,   // menus4: How to Play's second page: Keep My Multiplayer Data Safe (Yes opens Safe Copies)
    MP_T_UPDATE_AVAILABLE,   // release3: toast, once per boot, when the online service names a newer game version (port/mp_update.h)
    MP_T_TEAM_TOTAL,         // costar: the star card's team line, the shared Star Bit count that feeds the Hungry Lumas
    // race: the results card's next step (host chooses), the head start line on the countdown
    MP_T_RACE_AGAIN, MP_T_NEXT_STAR, MP_T_LEAVE_RACE,
    MP_T_RACE_HOST_PICKS,    // "<S1> is choosing..." (a guest's results card while the host has not chosen)
    MP_T_RACE_ASK,           // lobby: "Race Again?" (a guest's results card: the row that asks the leader for a rematch)
    MP_T_RACE_ASKED,         // lobby: "Asked! Waiting for <S1>..." (that guest's line once the ask is sent)
    MP_T_RACE_WANTS,         // lobby: "<S1> wants to race again" (the leader's nudge when a guest asked)
    MP_T_HEAD_START,         // "Starts <N1> s late" (under the last winner's name on the countdown)
    MP_T_SERIES,             // "Series": the small heading over the results card's points column
    // wire: the Friends screens' words (words-wanted.md, m-friends), which showed in English in every language
    MP_T_MY_FRIEND_CODE, MP_T_FRIEND_MARK, MP_T_FRIEND_CODE_LINE, MP_T_REMOVE_FRIEND, MP_T_DESC_FRIEND_PAGE, MP_T_DESC_FRIEND_JOIN,
    MP_T_DESC_FRIEND_REMOVE, MP_T_SVC_BUSY, MP_T_SVC_OFFLINE,
    MP_T_G_REACHING,   // uxfix
    MP_T_RECONNECT_LONG,   // uxfix
    MP_T_G_CODE_LONG, MP_T_DLG_CODE_EXPIRED,   // code5: the long fallback code's line / a far-away code that is no longer open
    MP_T_WAIT_NAME,   // uxwait: the small banner "Waiting for <S1>..." (held behind a closed screen by another console)
    MP_T_REFRESH,     // uxwait: the Friends list's button: ask the service for the list again
    MP_T_DELETE_ACCOUNT,         // Friends' last row: "Delete My Online Account"
    MP_T_DESC_DELETE_ACCOUNT,    // its line: "Removes your online account and friends list."
    MP_T_DLG_DELETE_ACCOUNT,     // the Yes / No window
    MP_T_ACCOUNT_DELETED,        // the toast: "Your online account was deleted."
    MP_T_RACE_IDLE_LEFT,   // B6: toast on a guest whose leader chose nothing for two minutes: "No one chose, so you left the race."
    // beta diagnostics: Having Trouble? / Check Connection / Send a Problem Report (MpMenuReport.cpp). Own block: the rows are at the end of the three tables.
    MP_T_RP_HELP, MP_T_RP_DESC_HELP, MP_T_RP_CHECK, MP_T_RP_DESC_CHECK, MP_T_RP_SEND, MP_T_RP_DESC_SEND, MP_T_RP_INFO1, MP_T_RP_INFO2, MP_T_RP_ASK,
    MP_T_RP_ASK_NOTE, MP_T_RP_B_SEND, MP_T_RP_B_NOTE, MP_T_RP_B_CANCEL, MP_T_RP_B_INFO, MP_T_RP_KB_NOTE, MP_T_RP_SENDING, MP_T_RP_CHECKING,
    MP_T_RP_THANKS, MP_T_RP_E_REJECT, MP_T_RP_E_BIG, MP_T_RP_E_LIMIT, MP_T_RP_E_BUSY, MP_T_RP_E_OFFLINE, MP_T_RP_E_TIMEOUT, MP_T_RP_E_FAIL,
    MP_T_RP_AFTER_ERR, MP_T_RP_CK_DIRECT_OK, MP_T_RP_CK_DIRECT_RELAY, MP_T_RP_CK_DIRECT_BLOCKED, MP_T_RP_CK_RELAY_OK, MP_T_RP_CK_RELAY_FAIL,
    MP_T_RP_CK_RELAY_SKIP,
    // settings (native in-game Settings, 14 Nov): the entry pill, the pills' labels (L_) and values (V_), one band sentence each (D_), the disc's
    // language names in their own language (LANG_), the moves (M_) and the buttons that have no picture (B_), the leave question and the windows
    MP_T_SET_ENTRY,
    MP_T_SET_BAND, MP_T_SET_L_PICTURE, MP_T_SET_L_CAMERA, MP_T_SET_L_ZOOM, MP_T_SET_L_RSTICK, MP_T_SET_L_INVERT, MP_T_SET_L_LANGUAGE,
    MP_T_SET_L_CONTROLS, MP_T_SET_L_MORE, MP_T_SET_L_MOTION, MP_T_SET_L_REACH, MP_T_SET_L_RESET, MP_T_SET_L_TILT, MP_T_SET_L_TILT_POWER,
    MP_T_SET_L_POINTER, MP_T_SET_L_REAR, MP_T_SET_L_TAPJUMP, MP_T_SET_L_BUTTONS, MP_T_SET_V_SMOOTH, MP_T_SET_V_DETAILED, MP_T_SET_V_SYSTEM_LANG,
    MP_T_SET_V_CONSOLE, MP_T_SET_V_LEFT_STICK, MP_T_SET_V_CAMERA, MP_T_SET_V_POINTER, MP_T_SET_V_NORMAL, MP_T_SET_V_NONE, MP_T_SET_V_STEADY,
    MP_T_SET_V_QUICK, MP_T_SET_D_PICTURE_SMOOTH, MP_T_SET_D_PICTURE_DETAILED, MP_T_SET_D_CAMERA, MP_T_SET_D_ZOOM, MP_T_SET_D_RSTICK_CAMERA,
    MP_T_SET_D_RSTICK_POINTER, MP_T_SET_D_INVERT_ON, MP_T_SET_D_INVERT_OFF, MP_T_SET_D_LANG_OWN, MP_T_SET_D_LANG_NAMED, MP_T_SET_D_REACH,
    MP_T_SET_D_RESET, MP_T_SET_D_SESSION, MP_T_SET_D_NOT_RACE, MP_T_SET_D_RESTART_NEXT, MP_T_SET_D_HOLD, MP_T_SET_D_SESSION_MENU,
    MP_T_SET_D_MOTION_ON, MP_T_SET_D_MOTION_OFF, MP_T_SET_D_TILT_CONSOLE, MP_T_SET_D_TILT_STICK, MP_T_SET_D_POINTER_STEADY, MP_T_SET_D_POINTER_QUICK,
    MP_T_SET_D_REAR_ON, MP_T_SET_D_REAR_OFF, MP_T_SET_D_TAPJUMP_ON, MP_T_SET_D_TAPJUMP_OFF, MP_T_SET_D_CONTROLS, MP_T_SET_D_MORE,
    MP_T_SET_D_TILT_POWER, MP_T_SET_D_BUTTONS, MP_T_SET_D_CAPTURE, MP_T_SET_D_CAPTURE_TIMEOUT, MP_T_SET_TITLE_BTN_CONSOLE, MP_T_SET_TITLE_BTN_PAD,
    MP_T_SET_D_BUTTONS_NEXT, MP_T_SET_D_TEST_FILE, MP_T_SET_LANG_ENGLISH, MP_T_SET_LANG_FRENCH, MP_T_SET_LANG_SPANISH, MP_T_SET_LANG_GERMAN,
    MP_T_SET_LANG_ITALIAN, MP_T_SET_M_JUMP, MP_T_SET_M_STAR_BITS, MP_T_SET_M_SPIN, MP_T_SET_M_SPIN2, MP_T_SET_M_CROUCH, MP_T_SET_M_CAMERA,
    MP_T_SET_M_PAUSE, MP_T_SET_M_AIM, MP_T_SET_M_CAM_UP, MP_T_SET_M_CAM_DOWN, MP_T_SET_M_CAM_LEFT, MP_T_SET_M_CAM_RIGHT, MP_T_SET_MD_JUMP,
    MP_T_SET_MD_STAR_BITS, MP_T_SET_MD_SPIN, MP_T_SET_MD_SPIN2, MP_T_SET_MD_CROUCH, MP_T_SET_MD_CAMERA, MP_T_SET_MD_PAUSE, MP_T_SET_MD_AIM,
    MP_T_SET_MD_CAM_UP, MP_T_SET_MD_CAM_DOWN, MP_T_SET_MD_CAM_LEFT, MP_T_SET_MD_CAM_RIGHT, MP_T_SET_B_R, MP_T_SET_B_L2, MP_T_SET_B_R2, MP_T_SET_B_L3,
    MP_T_SET_B_R3, MP_T_SET_B_UP, MP_T_SET_B_DOWN, MP_T_SET_B_LEFT, MP_T_SET_B_RIGHT,
    MP_T_SET_B_CROSS, MP_T_SET_B_CIRCLE, MP_T_SET_B_SQUARE, MP_T_SET_B_TRIANGLE, MP_T_SET_B_L, MP_T_SET_B_SELECT, MP_T_SET_B_START,   // remapui31: the buttons in words (the font's pictures follow the player's buttons)
    MP_T_SET_LEAVE_Q, MP_T_SET_SAVE, MP_T_SET_SAVE_RESTART,
    MP_T_SET_DONT_SAVE, MP_T_SET_KEEP_EDITING, MP_T_SET_RESET, MP_T_SET_TOAST_SAVED, MP_T_SET_TOAST_SAVED_LATER, MP_T_SET_TOAST_RECOVERED,
    MP_T_SET_TOAST_UNREADABLE, MP_T_SET_TOAST_NO_ROOM, MP_T_SET_OPEN, MP_T_SET_CAP_LIST, MP_T_SET_L_REPORT, MP_T_SET_D_REPORT, MP_T_SET_TOAST_HOLD, MP_T_SET_DLG_RESTART_FAIL, MP_T_SET_DLG_CONFLICT, MP_T_SET_DLG_LOST, MP_T_SET_DLG_UNAVAILABLE,
    MP_T_SET_DLG_RESET, MP_T_SET_DLG_RESET_ALL,
    MP_T_SET_L_ASSIST, MP_T_SET_D_ASSIST_ON, MP_T_SET_D_ASSIST_OFF,
    MP_T_SET_L_ALLOW50, MP_T_SET_D_ALLOW50_ON, MP_T_SET_D_ALLOW50_OFF,   // mp30: the "Allow in-between frame rates (50 fps)" row (Smooth Motion only; starts the next time)
    MP_T_SET_D_RESTART_ASK, MP_T_SET_DLG_RESTART_PAUSE,
    MP_T_SET_RESTART_WAIT, MP_T_SET_RESTART_STUCK, MP_T_SET_RESTART_STUCK_LATER, MP_T_SET_START_NOW, MP_T_SET_LATER,   // setfix31b: "Save and Start Again" waiting for a save   // setapply31: from pause, a change that needs a start: the band, and "Start the game again now?"   // setnat31: the "60 Assist" row (Smooth Motion only; starts the next time)
    MP_T_SET_D_PICTURE_SMOOTH_NEW, MP_T_SET_D_PICTURE_DETAILED_NEW,   // modeux32: the Picture sentence once the choice differs from what is running (names the restart)
    MP_T_COUNT
};

namespace MpMsgText {

int languageIndex(const char* gamePrefix);   // "UsEnglish", "EuFrench", "UsSpanish", ... (others: English)
void setLanguage(int lang);
int language();
// German/Italian text of an id (MpMessagesExtra.cpp); null when the id has no row there (raw() then falls back to English).
const char16_t* extraText(int id, int lang);

// The raw text (markup included) in the current language; never null ("" for unknown ids).
const char16_t* raw(MpText id);
// The dialog id of a window text ("MP_ERR_NO_WIFI") and back. Unknown => MP_T_NONE / nullptr.
const char* dialogName(MpText id);
MpText dialogByName(const char* name, size_t len);
MpText errorText(int mpUiError);             // MpUiError -> MP_T_ERR_*
// fix3: the online service's error code (mp_service_last_code: "banned", "friends_full", "not_friends", "player_not_found",
// "own_code", "not_member", "daily_cap", "version_mismatch", ...) -> its window text; MP_T_NONE for "" / null / a code we do not
// word (the caller's own text then). A code that names a room ("bad_code") is not worded: it means different things by context.
MpText serviceText(const char* code);

// Formatting into `out` (cap units, always terminated): markup arguments replaced; pictures kept as U+E0xx. Returns the length.
struct Args {
    const char* str = nullptr;   // UTF-8
    const char* str2 = nullptr;  // UTF-8
    int num = 0;
    uint32_t turns = 0;          // time, logic turns at 60/s
};
int format(char16_t* out, int cap, MpText id, const Args& a);
int formatRaw(char16_t* out, int cap, const char16_t* text, const Args& a);
// UTF-8 -> UTF-16 (invalid bytes become U+FFFD; stops at cap-1). Returns the length.
int utf8To16(char16_t* out, int cap, const char* in, int inLen = -1);
// Time: turns at 60/s -> "m:ss.cc" (centiseconds rounded down). Returns the length.
int formatTime(char16_t* out, int cap, uint32_t turns);
// For a message window: pictures -> picture tags, the system windows' leading tag (0x1A 0x0106 0x0003) first. Returns the length.
int toWindowText(char16_t* out, int cap, const char16_t* text, bool leadingTag);

// The game's MessageData fallback ("MP_*" ids, plus "_Yes" / "_No"). Dynamic windows: "MP_DYN0" / "MP_DYN1" show what
// setDynamic put there (window text, already converted). Returns null when the id is not ours.
const char16_t* findDialog(const char* id);
void setDynamic(int slot, MpText id, const Args& a);   // slot 0 or 1; the Yes / No labels are the usual ones
void setDynamicLines(int slot, const MpText* ids, int count);
void setDynamicLinesArgs(int slot, const MpText* ids, const Args* args, int count);   // beta diagnostics: the same with an Args per line (Check Connection's "Relay: works (N ms)")   // conntest: up to three texts, one per line
// touches: the host-rules card. mode MP_MODE_*, falls = MpSettings.downed, bump = interaction != 0. Window: 3 lines (host + mode, the
// rules, "Show this next time?"); toast: one line "<host> is the host: Play Together, bubbles, bumping on."
struct Rules { const char* host; int mode; int falls; bool bump; };   // mode: 0 Play Together, 1 Star Race (MpMode order)
void setDynamicRules(int slot, const Rules& r);
int formatRulesToast(char16_t* out, int cap, const Rules& r, bool changed);
const char16_t* dynamicText(int slot);                 // (tests) the converted text

}  // namespace MpMsgText
