// The desktop test keyboard of np_keyboard.h: the answers come from the environment, so a scripted harness run can "type" a name or an invite code.
#if !defined(NP_TARGET_VITA)

#include "np_keyboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

bool g_open = false;
int g_kind = 0;
int g_polls = 0;
int g_delay = 0;
char g_title[96];

bool Listed(const char* list, const char* word) {
    const size_t n = strlen(word);
    for (const char* p = list; p && *p;) {
        const char* e = strchr(p, ',');
        const size_t len = e ? size_t(e - p) : strlen(p);
        if (len == n && !strncmp(p, word, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

// The text for this prompt: "" = not given yet (a file that has no line), null = no answer exists at all.
const char* Answer(int kind, char* buf, size_t size) {
    buf[0] = 0;
    const char* env = kind == kNpKeyboardText ? nullptr : getenv(kind == kNpKeyboardName ? "SMG_KEYBOARD_NAME" : "SMG_KEYBOARD_CODE");
    if (env && *env) {
        snprintf(buf, size, "%s", env);
        return buf;
    }
    // Galaxy: SMG_KEYBOARD_TEXT answers any prompt (name or code) that has no answer of its own
    const char* any = getenv("SMG_KEYBOARD_TEXT");
    if (any && *any) {
        snprintf(buf, size, "%s", any);
        return buf;
    }
    if (kind == kNpKeyboardCode || getenv("SMG_KEYBOARD_TEXT_FILE")) {
        const char* file = kind == kNpKeyboardCode && getenv("SMG_KEYBOARD_CODE_FILE") ? getenv("SMG_KEYBOARD_CODE_FILE") : getenv("SMG_KEYBOARD_TEXT_FILE");
        if (file && *file) {
            if (FILE* f = fopen(file, "r")) {
                if (fgets(buf, int(size), f)) {
                    size_t n = strlen(buf);
                    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = 0;
                }
                fclose(f);
            }
            return buf;
        }
    }
    return 0;
}

}  // namespace

extern "C" int NpKeyboardAvailable(void) {
    char b[64];
    return Answer(kNpKeyboardName, b, sizeof b) || Answer(kNpKeyboardCode, b, sizeof b) || getenv("SMG_KEYBOARD_CANCEL") ? 1 : 0;
}

extern "C" int NpKeyboardOpen(const char* title, const char* initial, int maxChars, int kind) {
    (void)initial;
    (void)maxChars;
    if (g_open) return -1;
    char b[64];
    const char* cancel = getenv("SMG_KEYBOARD_CANCEL");
    if (!Answer(kind, b, sizeof b) && !(cancel && Listed(cancel, kind == kNpKeyboardName ? "name" : (kind == kNpKeyboardText ? "text" : "code")))) return -1;
    g_open = true;
    g_kind = kind;
    g_polls = 0;
    const char* d = getenv("SMG_KEYBOARD_DELAY_FRAMES");
    g_delay = d && *d ? atoi(d) : 0;
    snprintf(g_title, sizeof g_title, "%s", title ? title : "");
    fprintf(stderr, "[kbd] open: %s (%s)\n", g_title, kind == kNpKeyboardName ? "name" : (kind == kNpKeyboardText ? "text" : "code"));
    return 0;
}

extern "C" int NpKeyboardPoll(char* out, size_t size) {
    if (!g_open) return kNpKeyboardFailed;
    if (++g_polls <= g_delay) return kNpKeyboardRunning;
    const char* cancel = getenv("SMG_KEYBOARD_CANCEL");
    if (cancel && Listed(cancel, g_kind == kNpKeyboardName ? "name" : (g_kind == kNpKeyboardText ? "text" : "code"))) {
        g_open = false;
        fprintf(stderr, "[kbd] cancelled\n");
        return kNpKeyboardCancelled;
    }
    char b[816];   // beta diagnostics: a problem report note is up to 200 characters
    const char* a = Answer(g_kind, b, sizeof b);
    if (!a || !*a) return kNpKeyboardRunning;   // a file that is not there yet: the player is still typing
    snprintf(out, size, "%s", a);
    g_open = false;
    fprintf(stderr, "[kbd] typed %d characters\n", int(strlen(out)));
    return kNpKeyboardAccepted;
}

extern "C" const char* NpKeyboardSuggestion(void) {
    const char* n = getenv("SMG_LINK_NAME");
    return n ? n : "";
}

#endif
