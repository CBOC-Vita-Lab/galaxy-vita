// The Vita's system keyboard for the public leaderboard name (np_online_name.h: NpOnlineImeOpen / Poll / Suggestion).
// sceImeDialog is a common dialog: it is created, updated and drawn by the renderer's swap like the NetCheck dialog of the radio
// (vlink_vita_radio.cpp). The game thread only posts a request and polls an answer; np_vita_ime_dialog_render() runs on the renderer
// thread (called from np_vita_radio_dialog_render, before every swap) and np_vita_ime_dialog_active() tells the renderer that a
// common dialog is on screen (OR-ed into np_vita_radio_dialog_active, which also keeps the game's controller input away from the keyboard).
#if defined(NP_TARGET_VITA)  // the Vita target only (the np Makefile compiles every platform/ source for every target)

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/common_dialog.h>
#include "np_log.h"
#include "np_ime_utf8.h"
#include <psp2/ime_dialog.h>
#include <psp2/kernel/error.h>
#include <psp2/sysmodule.h>

#include "../include/np_glue.h"   // Galaxy: NpLanPlayerName (vlink_vita.cpp: the Vita user name); Strikers declared it from np_lan.h

extern "C" bool np_vita_radio_dialogs_ready(void);   // vlink_vita_radio.cpp: AppUtil and CommonDialog are configured

namespace {

constexpr unsigned kMaxChars = 200;      // Galaxy: the buffer limit (chat messages 40 characters, netx addresses "ip:port" 21, names 15, room codes 6); Strikers: 10
unsigned g_maxChars = 10;                // this request's limit (NpOnlineImeOpenMax), 1..kMaxChars
bool g_text = false;                     // Galaxy (chat): a message: the system's languages, any characters (names: basic Latin)
enum State : int { kIdle, kRequested, kRunning, kAccepted, kCancelled, kFailed };

std::atomic<int> g_state{kIdle};
std::atomic<bool> g_visible{false};
SceWChar16 g_title[64];
SceWChar16 g_initial[kMaxChars + 1];
SceWChar16 g_buffer[kMaxChars + 1];
char g_result[4 * kMaxChars + 1];
bool g_moduleTried = false;

// ASCII / BMP UTF-8 <-> UTF-16 (names are ASCII after validation; whatever else the keyboard returns is dropped by the sanitiser)
void ToUtf16(const char* in, SceWChar16* out, size_t capacity) {
    size_t n = 0;
    for (size_t i = 0; in && in[i] && n + 1 < capacity;) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) { out[n++] = c; ++i; }
        else if ((c & 0xE0) == 0xC0 && in[i + 1]) { out[n++] = SceWChar16(((c & 0x1F) << 6) | (in[i + 1] & 0x3F)); i += 2; }
        else if ((c & 0xF0) == 0xE0 && in[i + 1] && in[i + 2]) { out[n++] = SceWChar16(((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F)); i += 3; }
        else { ++i; }
    }
    out[n] = 0;
}

// UTF-16 to UTF-8 with surrogate pairs combined (np_ime_utf8.h, host-tested)
void ToUtf8(const SceWChar16* in, char* out, size_t capacity) { NpUtf16ToUtf8(reinterpret_cast<const uint16_t*>(in), out, capacity); }

}  // namespace

// ---- renderer thread ------------------------------------------------------------------------------------------------------------------
extern "C" void np_vita_ime_dialog_render(void) {
    const int state = g_state.load(std::memory_order_acquire);
    if (state == kRequested) {
        if (!g_moduleTried) { g_moduleTried = true; sceSysmoduleLoadModule(SCE_SYSMODULE_IME); }   // harmless when the firmware has it loaded
        SceImeDialogParam param;
        sceImeDialogParamInit(&param);
        if (!g_text) {
            param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
            param.languagesForced = SCE_TRUE;
            param.type = SCE_IME_TYPE_BASIC_LATIN;
            param.option = SCE_IME_OPTION_NO_ASSISTANCE;
        } else {
            param.type = SCE_IME_TYPE_DEFAULT;   // (the system's own languages and predictions; one line)
        }
        param.title = g_title;
        param.maxTextLength = g_maxChars;
        param.initialText = g_initial;
        param.inputTextBuffer = g_buffer;
        memset(g_buffer, 0, sizeof g_buffer);
        const int rc = sceImeDialogInit(&param);
        if (rc < 0) {
            NpLogf("online", "the keyboard did not start (0x%08X)\n", (unsigned)rc);
            g_state.store(kFailed, std::memory_order_release);
            return;
        }
        g_visible.store(true, std::memory_order_release);
        g_state.store(kRunning, std::memory_order_release);
        return;
    }
    if (state != kRunning) return;
    const SceCommonDialogStatus status = sceImeDialogGetStatus();
    if (status == SCE_COMMON_DIALOG_STATUS_RUNNING) return;
    if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult result;
        memset(&result, 0, sizeof result);
        sceImeDialogGetResult(&result);
        const bool entered = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
        if (entered) { g_buffer[g_maxChars] = 0; ToUtf8(g_buffer, g_result, sizeof g_result); }
        sceImeDialogTerm();
        g_visible.store(false, std::memory_order_release);
        g_state.store(entered ? kAccepted : kCancelled, std::memory_order_release);
        return;
    }
    if (status == SCE_COMMON_DIALOG_STATUS_NONE) {   // gone without a result
        g_visible.store(false, std::memory_order_release);
        g_state.store(kFailed, std::memory_order_release);
    }
}

extern "C" bool np_vita_ime_dialog_active(void) { return g_visible.load(std::memory_order_acquire); }

// ---- game thread ----------------------------------------------------------------------------------------------------------------------
extern "C" int NpOnlineImeAvailable(void) { return np_vita_radio_dialogs_ready() ? 1 : 0; }

namespace {
int OpenRequest(const char* initial, const char* title, int maxChars, bool text) {
    const int state = g_state.load(std::memory_order_acquire);
    if (state == kRequested || state == kRunning) return -1;
    g_maxChars = maxChars < 1 ? 1u : (maxChars > int(kMaxChars) ? kMaxChars : unsigned(maxChars));
    g_text = text;   // (before the release store below: the renderer thread reads it once it sees kRequested)
    ToUtf16(initial, g_initial, g_maxChars + 1);
    ToUtf16(title, g_title, sizeof g_title / sizeof g_title[0]);
    g_result[0] = 0;
    g_state.store(kRequested, std::memory_order_release);
    return 0;
}
}  // namespace

// Galaxy: the length limit per request (Strikers: always its name limit, 10)
extern "C" int NpOnlineImeOpenMax(const char* initial, const char* title, int maxChars) { return OpenRequest(initial, title, maxChars, false); }

extern "C" int NpOnlineImePoll(char* out, size_t size) {
    switch (g_state.load(std::memory_order_acquire)) {
    case kRequested:
    case kRunning: return 0;
    case kAccepted:
        snprintf(out, size, "%s", g_result);
        g_state.store(kIdle, std::memory_order_release);
        return 1;
    case kCancelled: g_state.store(kIdle, std::memory_order_release); return 2;
    case kFailed: g_state.store(kIdle, std::memory_order_release); return -1;
    default: return -1;
    }
}

extern "C" int NpOnlineImeOpen(const char* initial, const char* title) { return NpOnlineImeOpenMax(initial, title, 10); }

// Galaxy (chat): a message of up to maxChars characters in any of the system's languages (the caller cleans what comes back)
extern "C" int NpOnlineImeOpenText(const char* initial, const char* title, int maxChars) { return OpenRequest(initial, title, maxChars, true); }

// The Vita user name (it can be the player's real name: only a suggestion in the keyboard).
extern "C" const char* NpOnlineImeSuggestion(void) {
    const char* name = NpLanPlayerName();
    return name ? name : "";
}

#endif  // NP_TARGET_VITA
