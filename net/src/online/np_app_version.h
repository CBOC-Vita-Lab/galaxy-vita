// The game version every request to the online service carries (X-Msc-App). The production service requires it on every call and accepts dotted
// numbers only; this is the one definition the signaling client, the menu flow and the ranking client share.
#ifndef NP_NET_APP_VERSION_H
#define NP_NET_APP_VERSION_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace NpNet {

// "1.2.0"-style check: digits and single dots, 1..4 parts, no empty part.
inline bool AppVersionValid(const char* v) {
    if (!v || !*v) return false;
    int parts = 1;
    for (const char* c = v; *c; ++c) {
        if (*c == '.') {
            if (c == v || c[1] == 0 || c[1] == '.') return false;
            ++parts;
        } else if (*c < '0' || *c > '9') return false;
    }
    return parts <= 4 && strlen(v) < 24;
}

// This game's version as built: the release build's -DNP_ONLINE_APP_VERSION, else the version of this source tree. Raise it with every release: it is also
// part of the multiplayer fingerprint. No environment switch reaches this one. Galaxy 3.0: "3.0.0" (Strikers: "1.3.0").
inline const char* BuiltAppVersion() {
#ifdef NP_ONLINE_APP_VERSION
    return AppVersionValid(NP_ONLINE_APP_VERSION) ? NP_ONLINE_APP_VERSION : "3.0.0";
#else
    return "3.0.0";
#endif
}

// The version to send: SMG_ONLINE_APP_VERSION (tests), else the built version. Never empty, always valid.
inline const char* DefaultAppVersion() {
    if (const char* e = getenv("SMG_ONLINE_APP_VERSION"))
        if (*e && AppVersionValid(e)) return e;
    return BuiltAppVersion();
}

// `wanted` when it is a valid version, else the default.
inline const char* AppVersionOrDefault(const char* wanted) { return AppVersionValid(wanted) ? wanted : DefaultAppVersion(); }

}  // namespace NpNet

#endif
