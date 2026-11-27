// Galaxy 3.0 multiplayer: the text keyboard and the "system dialog on screen" query of the bridge (mp_bridge.h), on top of net/'s np_keyboard.
// Vita: the system keyboard (sceImeDialog, a common dialog drawn by the renderer's swap: net/src/np_online_ime_vita.cpp and the MP3 hooks in
// platform/vgx/vita/vgx_host_galaxy_vita.cpp). Desktop: net/'s test keyboard, answered from the environment so scripted runs can type:
//   SMG_KEYBOARD_TEXT=<text> (any prompt), SMG_KEYBOARD_NAME / SMG_KEYBOARD_CODE (one kind), SMG_KEYBOARD_TEXT_FILE / SMG_KEYBOARD_CODE_FILE=<path>
//   (the first line, once the file has one), SMG_KEYBOARD_CANCEL=name,code, SMG_KEYBOARD_DELAY_FRAMES=<n>.
// Game thread only. Nothing blocks: open posts the request, poll is called once a frame. The text handed back is cleaned: no control
// characters, never longer than the caller's limit, a code is upper-case letters and digits only.
// integ: port-side storage must never come from the game's JKR heaps (operator new on a game thread uses the current scene heap,
// which is destroyed at the next scene change while the session's objects live on): every entry point the game calls is a host
// allocation scope (port/heap_routing.h; referenced weakly so the host tests link without the port).
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct MpHostAllocScope {
    MpHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~MpHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define MP_HOST_ALLOC MpHostAllocScope mpHostAllocScope_
#include "port/mp_bridge.h"

#include "../../../net/include/np_keyboard.h"

#include <string.h>

#if defined(__vita__)
// net/src/vlink_vita_radio.cpp: a common dialog (ad hoc NetCheck or the keyboard) is on screen. Weak: a build without net/ has none.
extern "C" bool np_vita_radio_dialog_active(void) __attribute__((weak));
#endif

namespace {

bool sOpen = false;      // a keyboard request is out (between open and its answer)
bool sCode = false;      // the request is for a room code
int sMaxChars = 15;
constexpr int kNameMax = 21, kTextMax = 200;   // names 15, netx addresses 21 ("255.255.255.255:65535"); chat: a longer request is a message; kTextMax is only the ceiling (40), the chat asks for MP_CHAT_TEXT_CHARS (24), any language

// Keeps whole UTF-8 sequences, drops control characters, stops at `maxChars` characters or `cap - 1` bytes.
int CleanText(const char* in, char* out, int cap, int maxChars, bool code) {
    int n = 0, chars = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p && chars < maxChars;) {
        const unsigned char c = *p;
        if (code) {
            ++p;
            char k = 0;
            if (c >= 'a' && c <= 'z') k = char(c - 'a' + 'A');
            else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) k = char(c);
            if (!k) continue;   // spaces, dashes and anything else are not part of a code
            if (n + 1 >= cap) break;
            out[n++] = k;
            ++chars;
            continue;
        }
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len != 0 && !(len == 1 && (c < 0x20 || c == 0x7F));
        for (int i = 1; ok && i < len; ++i) ok = (p[i] & 0xC0) == 0x80;
        if (!ok) { ++p; continue; }   // a control character or a broken sequence
        if (n + len >= cap) break;
        memcpy(out + n, p, size_t(len));
        n += len;
        p += len;
        ++chars;
    }
    out[n] = 0;
    return n;
}

}  // namespace

extern "C" {

bool mp_keyboard_open(const char* titleUtf8, const char* initialUtf8, int maxChars, bool codeOnly) { MP_HOST_ALLOC;
    if (sOpen) return false;   // one keyboard at a time
    if (!NpKeyboardAvailable()) return false;
    const bool text = !codeOnly && maxChars > kNameMax;
    sMaxChars = maxChars < 1 ? 1 : (maxChars > (text ? kTextMax : kNameMax) ? (text ? kTextMax : kNameMax) : maxChars);
    sCode = codeOnly;
    if (NpKeyboardOpen(titleUtf8 ? titleUtf8 : "", initialUtf8 ? initialUtf8 : "", sMaxChars,
                       codeOnly ? kNpKeyboardCode : (text ? kNpKeyboardText : kNpKeyboardName)) != 0)
        return false;
    sOpen = true;
    mp_log("keyboard open kind=%s max=%d", codeOnly ? "code" : (text ? "text" : "name"), sMaxChars);
    return true;
}

int mp_keyboard_poll(char* outUtf8, int cap) { MP_HOST_ALLOC;
    if (!sOpen) return -1;
    char raw[4 * kTextMax + 8];
    const int r = NpKeyboardPoll(raw, sizeof raw);
    if (r == kNpKeyboardRunning) return 0;
    sOpen = false;
    if (r == kNpKeyboardAccepted && outUtf8 && cap > 0) {
        const int n = CleanText(raw, outUtf8, cap, sMaxChars, sCode);
        mp_log("keyboard done chars=%d", n);
        return 1;
    }
    if (outUtf8 && cap > 0) outUtf8[0] = 0;
    mp_log("keyboard %s", r == kNpKeyboardCancelled ? "cancelled" : "failed");
    return -1;
}

bool mp_dialog_active(void) { MP_HOST_ALLOC;
#if defined(__vita__)
    return np_vita_radio_dialog_active && np_vita_radio_dialog_active();
#else
    return sOpen;   // the desktop's test keyboard stands in for the dialog: game input is held while it is "open"
#endif
}

}  // extern "C"
