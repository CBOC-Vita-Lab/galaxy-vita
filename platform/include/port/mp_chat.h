// Galaxy 3.0 multiplayer: messages between players (owner: chat). The CHAT message (0x3E) codec, the typed-text rules and the rate
// limit, shared by the port side (platform/src/mp/mp_core.cpp: the host's checks before it relays) and the game side
// (decomp/src/Game/MP/MpChat.cpp: send, show). Plain C++, header only, no game headers, no allocation.
//
// CHAT (0x3E, reliable, any -> host relays; DESIGN.md section 3):
//   kind u8 = 0 (phrase): id u8 (< MP_CHAT_PHRASES). Fixed 2 bytes. Receivers show the phrase in THEIR language (the id, not text).
//   kind u8 = 1 (text):   len u8 (1..MP_CHAT_TEXT_BYTES), then len bytes of clean UTF-8 (mp_chat_clean leaves it unchanged):
//                         1..MP_CHAT_TEXT_CHARS characters, all in the Basic Multilingual Plane, no control / format / private-use
//                         characters, no leading, trailing or double spaces. Anything else is rejected whole.
// Typed text travels in every kind of game, public rooms included (owner, Nov 11-12); the host's own Typed Messages switch can still turn it off.
// They are not filtered for words: a player hides another's messages with "Hide messages" (Mute), remembered per player for the session.
// What protects every console is enforced by every receiving console: the length (MP_CHAT_TEXT_CHARS, one balloon line), the character rules
// (mp_chat_char_ok) and a rate limit per sender (MP_CHAT_SHOW_*).
#ifndef PORT_MP_CHAT_H
#define PORT_MP_CHAT_H

#include <stddef.h>
#include <stdint.h>

#include "port/mp_proto.h"

#define MP_CHAT_PHRASE_BRB 11       // touches: "Be right back": the sender is away until it moves (MpAway)
#define MP_CHAT_PHRASES 12          // preset phrases (MpText MP_T_PHRASE_0..; 0..3 are the four old stamps, same words)
#define MP_CHAT_TEXT_CHARS 24       // typed message: characters (ingame: one line of the speech balloon on the Vita's screen in all five languages)
#define MP_CHAT_TEXT_BYTES 72       // typed message: UTF-8 bytes on the wire (24 x 3: every allowed character is <= 3 bytes)
#define MP_CHAT_MAX_PAYLOAD (2 + MP_CHAT_TEXT_BYTES)

enum MpChatKind : uint8_t { MP_CHAT_PHRASE = 0, MP_CHAT_TEXT = 1 };

// MpSettings.flags bits for chat (bits 0..2: drop-in, shared purple coins, shared star chips; mp_proto.h).
enum MpChatSettingsFlag : uint8_t {
    MP_SET_TEXT_OFF = 1 << 3,   // the host turned typed messages off (default: on, i.e. the bit is clear)
    MP_SET_PUBLIC = 1 << 4,     // a public room (Quick Match / open to anyone). Set by the original host when it opens the room and kept by the
                                // core through every SETTINGS change and host migration. It no longer limits typed messages (owner, 11 Nov).
};
// (owner, 11 Nov: typed messages in every kind of game, public ones included: only the host's "off" stops them; the rate limits and the
// text cleaner apply everywhere)
static inline bool mp_chat_typed_allowed(const MpSettings* s) { return s && (s->flags & MP_SET_TEXT_OFF) == 0; }
// wire (owner, 11-12 Nov): the rule is the host's off switch ONLY. A room this console knows is public (Quick Match, Play with Anyone) takes
// typed messages like any other (a player who does not want to read one hides that player: MpChatLogic::Mute). This helper used to
// refuse when knownPublic was set (gamefix 10) and its test said so, long after the send path, the host's relay and the Chat page
// stopped asking it; it now answers as they do, so nothing that calls it later can bring the old rule back. knownPublic is kept for callers.
static inline bool mp_chat_typed_allowed_here(const MpSettings* s, bool knownPublic) { (void)knownPublic; return mp_chat_typed_allowed(s); }

struct MpChatMsg {
    uint8_t kind;                         // MpChatKind
    uint8_t phrase;                       // kind 0
    uint8_t len;                          // kind 1: bytes in text
    char text[MP_CHAT_TEXT_BYTES + 1];    // kind 1: NUL terminated copy
};

// ---- the typed-text rules ------------------------------------------------------------------------------------------------------
// Characters a message may hold: printable, in the BMP (the game's fonts are 16-bit), not private use (U+E000.. is the panels'
// picture markup), no control or format characters (C0, DEL, C1, zero-width, bidi overrides, line / paragraph separators, BOM),
// no noncharacters.
static inline bool mp_chat_char_ok(uint32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return false;          // C0, DEL, C1
    if (c == 0xAD) return false;                                     // soft hyphen (invisible)
    if (c >= 0x200B && c <= 0x200F) return false;                    // zero-width, LRM / RLM
    if (c >= 0x2028 && c <= 0x202E) return false;                    // line / paragraph separators, bidi embeddings / overrides
    if (c >= 0x2060 && c <= 0x206F) return false;                    // word joiner, invisible operators, bidi isolates
    if (c >= 0xD800 && c <= 0xDFFF) return false;                    // surrogates (never valid UTF-8 anyway)
    if (c >= 0xE000 && c <= 0xF8FF) return false;                    // private use
    if (c >= 0xFDD0 && c <= 0xFDEF) return false;                    // noncharacters
    if (c >= 0xFE00 && c <= 0xFE0F) return false;                    // variation selectors
    if (c == 0xFEFF || c >= 0xFFF0) return false;                    // BOM, specials, U+FFFE / U+FFFF
    return c <= 0xFFFF;
}

// One UTF-8 character at s[*i] (n bytes). Returns the code point, or 0xFFFFFFFF for an invalid / overlong / truncated sequence
// (then *i moves past the first byte only). Never reads past n.
static inline uint32_t mp_chat_utf8_next(const uint8_t* s, size_t n, size_t* i) {
    const uint8_t b = s[*i];
    uint32_t c;
    size_t len;
    if (b < 0x80) { (*i)++; return b; }
    else if (b >= 0xC2 && b <= 0xDF) { c = b & 0x1F; len = 2; }
    else if (b >= 0xE0 && b <= 0xEF) { c = b & 0x0F; len = 3; }
    else if (b >= 0xF0 && b <= 0xF4) { c = b & 0x07; len = 4; }
    else { (*i)++; return 0xFFFFFFFFu; }
    if (*i + len > n) { (*i)++; return 0xFFFFFFFFu; }
    for (size_t k = 1; k < len; k++) {
        const uint8_t t = s[*i + k];
        if ((t & 0xC0) != 0x80) { (*i)++; return 0xFFFFFFFFu; }
        c = (c << 6) | (t & 0x3F);
    }
    if ((len == 3 && c < 0x800) || (len == 4 && (c < 0x10000 || c > 0x10FFFF)) || (c >= 0xD800 && c <= 0xDFFF)) { (*i)++; return 0xFFFFFFFFu; }
    *i += len;
    return c;
}

// Clean what a player typed (the IME's UTF-8, or anything) into a message: invalid UTF-8 bytes are dropped; tabs and line breaks
// become spaces; other control / format characters are dropped; characters outside the BMP and private-use ones become '?';
// spaces are trimmed and runs of spaces collapsed; more than 2 combining marks in a row are dropped; at most MP_CHAT_TEXT_CHARS characters. out gets cap bytes at most (NUL
// terminated; cap >= MP_CHAT_TEXT_BYTES + 1 never truncates). Returns the length in bytes (0 = nothing left to send).
static inline size_t mp_chat_clean(const char* in, size_t inLen, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!in) return 0;
    const uint8_t* s = (const uint8_t*)in;
    size_t i = 0, n = 0, chars = 0;
    bool pendingSpace = false;
    int marks = 0;   // smallfix: combining marks (U+0300..036F) in a row in the output; at most 2 stay (24 stacked marks were a tall smear)
    while (i < inLen && s[i] && chars < MP_CHAT_TEXT_CHARS) {
        uint32_t c = mp_chat_utf8_next(s, inLen, &i);
        if (c == 0xFFFFFFFFu) continue;
        if (c == '\t' || c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029 || c == 0x3000 || c == 0xA0) c = ' ';
        if ((c >= 0x10000 && c <= 0x10FFFF) || (c >= 0xE000 && c <= 0xF8FF)) c = '?';
        if (c == ' ') { pendingSpace = n > 0; continue; }
        if (!mp_chat_char_ok(c)) continue;
        const bool mark = c >= 0x300 && c <= 0x36F;
        if (mark && marks >= 2) continue;
        uint8_t enc[3];
        size_t el;
        if (c < 0x80) { enc[0] = (uint8_t)c; el = 1; }
        else if (c < 0x800) { enc[0] = (uint8_t)(0xC0 | (c >> 6)); enc[1] = (uint8_t)(0x80 | (c & 0x3F)); el = 2; }
        else { enc[0] = (uint8_t)(0xE0 | (c >> 12)); enc[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); enc[2] = (uint8_t)(0x80 | (c & 0x3F)); el = 3; }
        const size_t need = (pendingSpace ? 1 : 0) + el;
        if (n + need + 1 > cap || chars + (pendingSpace ? 2 : 1) > MP_CHAT_TEXT_CHARS) break;
        if (pendingSpace) { out[n++] = ' '; chars++; pendingSpace = false; }
        for (size_t k = 0; k < el; k++) out[n++] = (char)enc[k];
        chars++;
        marks = mark ? marks + 1 : 0;
    }
    out[n] = 0;
    return n;
}

// A received text is accepted only when it is already clean (cleaning would not change a byte): one canonical form on the wire.
static inline bool mp_chat_text_ok(const char* s, size_t len) {
    if (!s || len == 0 || len > MP_CHAT_TEXT_BYTES) return false;
    char tmp[MP_CHAT_TEXT_BYTES + 1];
    const size_t n = mp_chat_clean(s, len, tmp, sizeof tmp);
    if (n != len) return false;
    for (size_t k = 0; k < len; k++)
        if (tmp[k] != s[k]) return false;
    return true;
}

// ---- codec ---------------------------------------------------------------------------------------------------------------------
static inline void mp_write_chat(MpWriter& w, const MpChatMsg& m) {
    w.u8(m.kind);
    if (m.kind == MP_CHAT_PHRASE) {
        w.u8(m.phrase);
    } else {
        w.u8(m.len);
        w.bytes(m.text, m.len);
    }
}
static inline bool mp_read_chat(MpReader& r, MpChatMsg* m) {
    m->kind = r.u8();
    m->phrase = 0;
    m->len = 0;
    m->text[0] = 0;
    if (!r.ok()) return false;
    if (m->kind == MP_CHAT_PHRASE) {
        m->phrase = r.u8();
        return r.done() && m->phrase < MP_CHAT_PHRASES;
    }
    if (m->kind != MP_CHAT_TEXT) return false;
    m->len = r.u8();
    if (!r.ok() || m->len == 0 || m->len > MP_CHAT_TEXT_BYTES) return false;
    if (!r.bytes(m->text, m->len) || !r.done()) { m->len = 0; m->text[0] = 0; return false; }
    m->text[m->len] = 0;
    if (!mp_chat_text_ok(m->text, m->len)) { m->len = 0; m->text[0] = 0; return false; }
    return true;
}
// Build a text message from what was typed (cleaned). False when nothing is left.
static inline bool mp_chat_make_text(MpChatMsg* m, const char* typed, size_t len) {
    m->kind = MP_CHAT_TEXT;
    m->phrase = 0;
    const size_t n = mp_chat_clean(typed, len, m->text, sizeof m->text);
    m->len = (uint8_t)n;
    return n > 0;
}

// ---- rate limit: a token bucket ------------------------------------------------------------------------------------------------
// Senders: one message per 2 s with bursts of 3 (kSend*). The host relays with a little slack for network jitter and re-sends that
// arrive together (kRelay*); receivers allow more again (a host they do not control). Time in ms; the first call fills the bucket.
struct MpChatBucket {
    uint64_t credit;  // ms of credit: one message costs one period; capped at burst periods
    uint64_t last;    // ms
    bool init;
};
enum : uint32_t { MP_CHAT_SEND_PERIOD_MS = 2000, MP_CHAT_SEND_BURST = 3, MP_CHAT_RELAY_PERIOD_MS = 1500, MP_CHAT_RELAY_BURST = 4,
                  MP_CHAT_SHOW_PERIOD_MS = 1500, MP_CHAT_SHOW_BURST = 4 };   // (ingame: every receiver holds each sender to the host's relay rate)
static inline void mp_chat_bucket_reset(MpChatBucket* b) { b->credit = 0; b->last = 0; b->init = false; }
static inline void mp_chat_bucket_refill(MpChatBucket* b, uint64_t nowMs, uint32_t periodMs, uint32_t burst) {
    const uint64_t cap = (uint64_t)burst * periodMs;
    if (!b->init) { b->init = true; b->credit = cap; b->last = nowMs; return; }
    if (nowMs > b->last) {
        const uint64_t dt = nowMs - b->last;
        b->credit = dt >= cap || b->credit + dt >= cap ? cap : b->credit + dt;
    }
    b->last = nowMs;   // (a clock that went back: no credit, no debt)
}
static inline bool mp_chat_bucket_take(MpChatBucket* b, uint64_t nowMs, uint32_t periodMs, uint32_t burst) {
    mp_chat_bucket_refill(b, nowMs, periodMs, burst);
    if (periodMs == 0 || b->credit < periodMs) return periodMs == 0;
    b->credit -= periodMs;
    return true;
}
// Would a take succeed now (no change)?
static inline bool mp_chat_bucket_ready(const MpChatBucket* b, uint64_t nowMs, uint32_t periodMs, uint32_t burst) {
    MpChatBucket c = *b;
    return mp_chat_bucket_take(&c, nowMs, periodMs, burst);
}

// ---- the host's decision for one CHAT it received (mp_core.cpp) ----------------------------------------------------------------
enum MpChatVerdict : uint8_t { MP_CHAT_OK = 0, MP_CHAT_BAD = 1, MP_CHAT_TYPED_OFF = 2, MP_CHAT_RATE = 3 };
static inline const char* mp_chat_verdict_name(uint8_t v) {
    switch (v) {
    case MP_CHAT_OK: return "ok";
    case MP_CHAT_BAD: return "bad";
    case MP_CHAT_TYPED_OFF: return "typed-off";
    case MP_CHAT_RATE: return "rate";
    default: return "?";
    }
}
// publicRoom: what the host itself knows (it opened the room for anyone): kept for callers, it no longer matters (owner, 11 Nov: typed messages
// in every kind of game). settings: the session's (MP_SET_* bits): only the host's Typed Messages switch can turn them off.
static inline uint8_t mp_chat_host_check(const uint8_t* data, uint16_t len, bool reliable, const MpSettings* settings, bool publicRoom,
                                         MpChatBucket* bucket, uint64_t nowMs, MpChatMsg* out) {
    MpReader r(data, len);
    if (!reliable || !mp_read_chat(r, out)) return MP_CHAT_BAD;
    (void)publicRoom;
    if (out->kind == MP_CHAT_TEXT && !mp_chat_typed_allowed(settings)) return MP_CHAT_TYPED_OFF;   // only the host's switch
    if (!mp_chat_bucket_take(bucket, nowMs, MP_CHAT_RELAY_PERIOD_MS, MP_CHAT_RELAY_BURST)) return MP_CHAT_RATE;
    return MP_CHAT_OK;
}

#endif  // PORT_MP_CHAT_H
