// The Vita's keyboard for np_keyboard.h: the system keyboard (sceImeDialog) of platform/sdk/dwc/np_online_ime_vita.cpp, the one helper the leaderboard
// name uses too. It takes a title and an initial text; the length limit there is the name limit (10), which also fits an invite code (6).
#if defined(NP_TARGET_VITA)  // the Vita target only (the np Makefile compiles every platform/ source for every target)

#include "np_keyboard.h"

extern "C" int NpOnlineImeAvailable(void);
extern "C" int NpOnlineImeOpenMax(const char* initial, const char* title, int maxChars);   // Galaxy: with the caller's limit
extern "C" int NpOnlineImeOpenText(const char* initial, const char* title, int maxChars);  // Galaxy: a message (any language)
extern "C" int NpOnlineImePoll(char* out, size_t size);
extern "C" const char* NpOnlineImeSuggestion(void);

extern "C" int NpKeyboardAvailable(void) { return NpOnlineImeAvailable(); }
extern "C" int NpKeyboardOpen(const char* title, const char* initial, int maxChars, int kind) {
    if (kind == kNpKeyboardText) return NpOnlineImeOpenText(initial, title, maxChars);   // Galaxy (chat)
    return NpOnlineImeOpenMax(initial, title, maxChars);
}
extern "C" int NpKeyboardPoll(char* out, size_t size) { return NpOnlineImePoll(out, size); }
extern "C" const char* NpKeyboardSuggestion(void) { return NpOnlineImeSuggestion(); }

#endif
