// Galaxy 3.0 multiplayer (core2): the spoken code of a Play by Address room: the host's IPv4 address, its UDP port (absent = the
// default MP_DIRECT_DEFAULT_PORT) and a room secret, in the room-code alphabet (no I L O 0 1), so a friend far away can be told it.
// Plain C API, no game headers. The secret is the room's pre-shared key: without it a stranger who knows the address cannot join
// or learn anything (DESIGN.md section 10.3).
//
// Layout: a payload number in base 31 (most significant character first) and one check character at the end.
//   payload bits, least significant first: secret 36 (40 with a port) | ip 32 | [port 16] | flag 1 (1 = a port follows)
//   14 payload characters without a port (2^69.4 >= 69 bits), 18 with one (2^89.2 >= 89 bits); + 1 check = 15 / 19.
//   check = sum of (position + 1) x value over the payload characters, mod 31 (31 is prime: every single wrong character and every
//   swap of two neighbours is caught).
// Shown as groups of 5: "K7QMD-4DXTA-RS2PQ" (15) or "K7QMD-4DXTA-RS2PQ-9HXA" (19).
// Decoding forgives how people read codes aloud: lower case, spaces / dashes / dots anywhere or none; the look-alikes the alphabet
// leaves out (0 / O, 1 / I / L) are not guessed at (no mapping is safe both ways): they are REJECTED with
// MP_CODE_LOOKALIKE (the menu says "That code didn't work. Please check it.").
#ifndef PORT_MP_DIRECT_CODE_H
#define PORT_MP_DIRECT_CODE_H

#include <stddef.h>
#include <stdint.h>

#define MP_DIRECT_DEFAULT_PORT 41100     // VLink UDP port of the game (DESIGN.md section 3)
#define MP_DIRECT_SECRET_BYTES 5         // 36 bits used without a port, 40 with one
#define MP_DIRECT_CODE_MAX 24            // longest text incl. separators and NUL ("XXXXX-XXXXX-XXXXX-XXXX" = 22 + NUL)

enum MpCodeResult : int {
    MP_CODE_OK = 0,
    MP_CODE_EMPTY = 1,        // nothing typed
    MP_CODE_LENGTH = 2,       // not 15 or 19 code characters
    MP_CODE_LOOKALIKE = 3,    // 0, O, 1, I or L: not in the alphabet (the code has none)
    MP_CODE_CHARACTER = 4,    // another character
    MP_CODE_CHECK = 5,        // a mistyped character (the check does not match)
    MP_CODE_ADDRESS = 6,      // decodes, but to no usable address (0.0.0.0, broadcast, multicast, port 0)
};

extern "C" {
// Writes the code with dashes into out (cap >= MP_DIRECT_CODE_MAX); returns its length, 0 on failure. port 0 or the default: the
// short form. Only the low 36 bits of the secret are used in the short form (secret[4] & 0x0F), all 40 in the long one.
int mp_direct_code_encode(uint32_t ip, uint16_t port, const uint8_t secret[MP_DIRECT_SECRET_BYTES], char* out, size_t cap);
// MP_CODE_OK with ip (host byte order), port (the default for a short code) and secret (5 bytes; unused bits 0).
int mp_direct_code_decode(const char* code, uint32_t* ip, uint16_t* port, uint8_t secret[MP_DIRECT_SECRET_BYTES]);
}

#endif
