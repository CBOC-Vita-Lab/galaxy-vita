// Galaxy 3.0 multiplayer: a game message without its tags, for the menus' own text boxes (pure code: host tested in tests/mp/ui_menu).
// A tag is 0x1A, then a unit whose low byte is the tag's size in bytes (the 0x1A unit included); a tag cut short by the end of the
// message ends the text there: nothing past the terminating 0 is ever read.
#pragma once

#include <stdint.h>

namespace MpTextStrip {

inline int strip(const uint16_t* s, char16_t* out, int cap) {
    if (cap <= 0) return 0;
    int n = 0;
    if (!s) {
        out[0] = 0;
        return 0;
    }
    for (int i = 0; s[i] && n < cap - 1 && i < 512; i++) {
        if (s[i] == 0x1A) {
            if (!s[i + 1]) break;   // (the tag's size unit is the end: a short message)
            const unsigned size = s[i + 1] & 0xFF;
            const unsigned units = size >= 4 ? size / 2 : 2;
            // skip the tag's units one by one: a tag longer than what is left stops at the end
            unsigned k = 1;
            while (k < units && s[i + k]) ++k;
            if (k < units) break;
            i += static_cast<int>(units) - 1;
            continue;
        }
        out[n++] = s[i] == u'\n' ? u' ' : static_cast<char16_t>(s[i]);
    }
    out[n] = 0;
    return n;
}

}  // namespace MpTextStrip
