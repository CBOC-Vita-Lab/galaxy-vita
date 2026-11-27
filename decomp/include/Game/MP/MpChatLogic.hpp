// Galaxy 3.0 multiplayer: messages between players, the pure parts (owner: chat). No game headers: the host tests build it as is.
//
//  - Grid: the Messages page's order of the phrases (3 columns x 4 rows) and its moves.
//  - Ring: the four phrases of the quick ring (hold Select in play), the most used ones. A phrase that becomes more used than
//    one in the ring takes that one's place; the others never move (thumbs learn the directions).
//  - Mute ("Hide messages"): per player, on this console only, for the whole session: a player who reconnects (into the same or
//    another slot, under any name: the key is the core's stable id) stays hidden; a new player in a freed slot is not.
//  - Shown text: UTF-8 -> UTF-16 for the game's fonts, anything the font cannot draw shows as nothing (never a box or a '?').
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "port/mp_chat.h"

namespace MpChatLogic {

// ---- the Messages page: phrase ids in reading order (3 x 4): the ones that go together sit together
constexpr int kGridCols = 3;
constexpr int kGridCount = MP_CHAT_PHRASES;
constexpr uint8_t kGridOrder[kGridCount] = {0, 2, 3,     // Over here!   Wait for me!   Let's go!
                                            4, 1, 8,     // Hello!       Thanks!        Sorry!
                                            6, 9, 5,     // Yes!         No!            Help!
                                            7, 10, 11};  // Which star?  Let's do this one!   Be right back!   // Thanks!      Nice!        Good game!
// The cursor moves on the grid; `cells` = kGridCount (+1 when the wide "Write a Message" cell sits under the grid).
// dir: 0 up, 1 right, 2 down, 3 left. Left / right wrap within a row; up / down stop at the edges (the wide cell is one below
// the middle column, and from it up goes back to the middle).
inline int gridMove(int cursor, int dir, int cells) {
    const int rows = (kGridCount + kGridCols - 1) / kGridCols;
    if (cursor < 0) return 0;
    if (cursor >= kGridCount) {   // the wide cell
        return dir == 0 ? (rows - 1) * kGridCols + kGridCols / 2 : cursor;
    }
    const int r = cursor / kGridCols, c = cursor % kGridCols;
    switch (dir) {
    case 0: return r > 0 ? cursor - kGridCols : cursor;
    case 2: return r + 1 < rows ? cursor + kGridCols : (cells > kGridCount ? kGridCount : cursor);
    case 1: return r * kGridCols + (c + 1) % kGridCols;
    case 3: return r * kGridCols + (c + kGridCols - 1) % kGridCols;
    default: return cursor;
    }
}

// ---- the quick ring
// ingame: the left of the ring is "Write a Message" (the console's own keyboard, a short message); the three other places hold the
// most used phrases.
constexpr int kWriteDir = 3;
struct Ring {
    uint8_t slot[4];                 // phrase at up, right, down (left: kWriteDir, slot[3] unused)
    uint16_t uses[MP_CHAT_PHRASES];  // phrases sent (from the page or the ring) since the game started
    void reset() {
        slot[0] = 0;   // up: Over here!
        slot[1] = 3;   // right: Let's go!
        slot[2] = 2;   // down: Wait for me!
        slot[3] = 1;   // left: Thanks!
        memset(uses, 0, sizeof uses);
        for (int i = 0; i < 4; i++) uses[slot[i]] = 1;   // (a head start: another phrase takes a place once it is used twice)
    }
    int indexOf(int phrase) const {
        for (int i = 0; i < 4; i++)
            if (i != kWriteDir && slot[i] == phrase) return i;
        return -1;
    }
    // A phrase was sent. Returns the ring position it took (-1: not in the ring).
    int used(int phrase) {
        if (phrase < 0 || phrase >= MP_CHAT_PHRASES) return -1;
        if (uses[phrase] < 0xFFFF) uses[phrase]++;
        int at = indexOf(phrase);
        if (at >= 0) return at;
        // the least used in the ring gives way to a phrase used more often than it (on a tie the last position: up, the first
        // place a thumb goes, changes last)
        int low = 2;   // (ingame: among the three phrase places; on a tie the last of them, down)
        for (int i = 1; i >= 0; i--)
            if (uses[slot[i]] < uses[slot[low]]) low = i;
        if (uses[phrase] > uses[slot[low]]) {
            slot[low] = static_cast<uint8_t>(phrase);
            at = low;
        }
        return at;
    }
};

// ---- mute (this console only)
struct Mute {
    // ingame: remembered by the player, not the slot: the hidden players' keys (the core's stable player id `sid`, else FNV-1a of the name
    // the session shows), for the session. The id survives the player's rename and rejoin and tells two players of one name apart. (reset() when it ends). kKeys covers everyone a session can hold.
    static constexpr int kKeys = 16;
    bool on[MP_MAX_PLAYERS];
    uint32_t slotKey[MP_MAX_PLAYERS];   // the key of the player in each slot as last seen (0 none)
    uint32_t keys[kKeys];
    int keyCount;
    void reset() { memset(on, 0, sizeof on); memset(slotKey, 0, sizeof slotKey); memset(keys, 0, sizeof keys); keyCount = 0; }
    static uint32_t keyOf(const char* name, uint32_t sid = 0) {
        if (sid) return sid;   // the stable id wins (0 = unknown / own slot: the name key)
        uint32_t h = 2166136261u;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name ? name : ""); *p; ++p) h = (h ^ *p) * 16777619u;
        return h ? h : 1u;
    }
    bool hiddenKey(uint32_t k) const {
        for (int i = 0; i < keyCount; i++) if (keys[i] == k) return true;
        return false;
    }
    bool muted(int slot) const { return slot >= 0 && slot < MP_MAX_PLAYERS && on[slot]; }
    // `name`: that slot's player (as shown); the choice is remembered for that player
    void set(int slot, bool v, const char* name = nullptr, uint32_t sid = 0) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS) return;
        on[slot] = v;
        const uint32_t k = name ? keyOf(name, sid) : slotKey[slot];
        if (name) slotKey[slot] = k;
        if (k == 0) return;
        if (v && !hiddenKey(k)) {
            if (keyCount < kKeys) keys[keyCount++] = k;
            else { memmove(&keys[0], &keys[1], sizeof(uint32_t) * (kKeys - 1)); keys[kKeys - 1] = k; }
        } else if (!v) {
            for (int i = 0; i < keyCount; i++)
                if (keys[i] == k) { keys[i] = keys[--keyCount]; break; }
        }
    }
    // once a turn: each slot follows its player: a slot whose player left is free (a new player there is not hidden); a player the
    // session's hidden list knows is hidden in whatever slot they come back to.
    template <typename Present, typename NameOf, typename SidOf> void sync(Present present, NameOf nameOf, SidOf sidOf) {
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (!present(s)) { on[s] = false; slotKey[s] = 0; continue; }
            const uint32_t k = keyOf(nameOf(s), sidOf(s));
            slotKey[s] = k;
            on[s] = hiddenKey(k);
        }
    }
    template <typename Present, typename NameOf> void sync(Present present, NameOf nameOf) {
        sync(present, nameOf, [](int) { return 0u; });
    }
    // (the old per-slot rule, kept for callers without names)
    template <typename Present> void forgetGone(Present present) {
        for (int s = 0; s < MP_MAX_PLAYERS; s++)
            if (on[s] && !present(s)) on[s] = false;
    }
};

// What a received message does on this console.
struct Show {
    bool toast, balloon;
};
inline Show decide(uint8_t kind, bool mutedSender) {
    Show s;
    (void)kind;
    s.toast = false;            // ingame: the balloon is the message: never a toast line
    s.balloon = !mutedSender;   // a hidden player shows nothing: neither typed nor quick messages
    return s;
}

// ---- the text a balloon / toast shows ------------------------------------------------------------------------------------------
// UTF-8 (clean, mp_chat_clean) -> UTF-16. `has` (may be null) says whether the game's font has a glyph; a missing one shows '?'.
// Returns the length (out always terminated).
template <typename Has> inline int toShown(const char* utf8, size_t len, char16_t* out, int cap, Has has) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    size_t i = 0;
    const uint8_t* s = reinterpret_cast<const uint8_t*>(utf8);
    while (utf8 && i < len && s[i] && n < cap - 1) {
        uint32_t c = mp_chat_utf8_next(s, len, &i);
        if (c == 0xFFFFFFFFu) continue;
        if (c != ' ' && (!mp_chat_char_ok(c) || c > 0xFFFF)) continue;   // ingame: not allowed: nothing (never a '?' or a box)
        const char16_t u = static_cast<char16_t>(c);
        if (u != u' ' && !has(u)) continue;                                // the game's font cannot draw it: nothing
        if (u == u' ' && (n == 0 || out[n - 1] == u' ')) continue;         // (no leading or double space left by a dropped one)
        out[n++] = u;
        if (n >= MP_CHAT_TEXT_CHARS) break;   // (ingame: never more than one balloon line, whatever came in)
    }
    while (n > 0 && out[n - 1] == u' ') n--;
    out[n] = 0;
    return n;
}
// One line break for a balloon: a text longer than `maxLine` characters gets '\n' at the space nearest its middle (or, with no
// space that leaves both lines within maxLine, else at maxLine). In place; cap >= len + 2. Returns the number of lines (1 or 2).
inline int wrapTwoLines(char16_t* t, int cap, int maxLine) {
    int n = 0;
    while (t[n]) n++;
    if (n <= maxLine) return 1;
    int best = -1;
    for (int k = 1; k < n - 1; k++)
        if (t[k] == u' ' && (best < 0 || (k > n / 2 ? k - n / 2 : n / 2 - k) < (best > n / 2 ? best - n / 2 : n / 2 - best))) best = k;
    if (best > 0 && best <= maxLine && n - best - 1 <= maxLine) {   // (13 Nov: no slack: no line is ever longer than maxLine, so the balloon's box never cuts)
        t[best] = u'\n';
        return 2;
    }
    if (n + 2 > cap) return 1;
    const int at = maxLine < n ? maxLine : n / 2;
    for (int k = n; k >= at; k--) t[k + 1] = t[k];
    t[at] = u'\n';
    return 2;
}

// How long a balloon stays (turns at 60/s): phrases 3.5 s, typed text a little longer the longer it is (at most 6 s).
inline int balloonTurns(uint8_t kind, int chars) {
    if (kind == MP_CHAT_PHRASE) return 210;
    const int t = 150 + chars * 6;
    return t > 360 ? 360 : t;
}

}  // namespace MpChatLogic
