// The text keyboard of the platform, one helper for every typed text of the game's online menus: the invite code of a friend's room, and the public
// name of the player (the same helper serves the leaderboard name of the ranking client). On the Vita it is the system keyboard (sceImeDialog, a
// common dialog drawn by the renderer's swap: np_keyboard_vita.cpp); on the desktop harness a test keyboard reads the answer from the environment so that
// a scripted run can type (np_keyboard_host.cpp). Nothing here blocks: Open() posts the request, Poll() is called once a frame.
//
//   kind 0 = a name (letters, digits and a few symbols), kind 1 = an invite code (letters and digits; the caller upper-cases and strips it).
//   Desktop test keyboard: SMG_KEYBOARD_NAME=<text> | SMG_KEYBOARD_CODE=<text> | SMG_KEYBOARD_CODE_FILE=<path> (answers once the file has a line:
//   the joining process waits for the host's code) | SMG_KEYBOARD_CANCEL=name,code (those prompts are cancelled) | SMG_KEYBOARD_DELAY_FRAMES=<n>
//   (the keyboard stays open for n polls first). With none of them the keyboard is "not available" and Open() fails.
//   Galaxy adds SMG_KEYBOARD_TEXT=<text> (answers any prompt without an answer of its own) and SMG_KEYBOARD_TEXT_FILE=<path> (the same, from the
//   first line of a file, waiting until it has one; for codes SMG_KEYBOARD_CODE_FILE wins). A message (kind 2) is answered by SMG_KEYBOARD_TEXT
//   (or SMG_KEYBOARD_TEXT_FILE) and cancelled by SMG_KEYBOARD_CANCEL=text.
#ifndef NP_KEYBOARD_H
#define NP_KEYBOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { kNpKeyboardName = 0, kNpKeyboardCode = 1, kNpKeyboardText = 2 };   // Galaxy: 2 = a message (any language, up to 40 characters)
enum { kNpKeyboardRunning = 0, kNpKeyboardAccepted = 1, kNpKeyboardCancelled = 2, kNpKeyboardFailed = -1 };

int NpKeyboardAvailable(void);
// 0 = started (answer with NpKeyboardPoll), -1 = not possible now (one dialog at a time, no keyboard on this platform).
int NpKeyboardOpen(const char* title, const char* initial, int maxChars, int kind);
// 0 running, 1 accepted (the text, UTF-8, in `out`), 2 cancelled, -1 failed. After a non-zero answer the keyboard is idle again.
int NpKeyboardPoll(char* out, size_t size);
// The Vita user name (it can be the player's real name: only ever a suggestion to edit), or "".
const char* NpKeyboardSuggestion(void);

#ifdef __cplusplus
}
#endif
#endif
