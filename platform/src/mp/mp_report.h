// Galaxy 3.0 multiplayer (release3): "Save a report" for bug reports.
//
// One small text file, written next to the logs (ux0:data/smg/report.txt on the Vita; $PETARI_DATA_DIR/report.txt, else ./report.txt on the
// desktop), that a player can attach to an issue. It holds: the build (app version, session fingerprint, protocol version), the
// multiplayer settings (no name, no server address, no console key), the last part of the log (only the multiplayer / network lines)
// and the last Connection Test result when there is one. Everything is passed through mp_scrub() first: addresses, tokens, the
// console key, friend and room codes, player names, host names in URLs and the user's folder name never reach the file.
// The scrubber works on text it is given; it does not know what is secret, it knows what secrets look like (see mp_report.cpp).
#ifndef PORT_MP_REPORT_H
#define PORT_MP_REPORT_H

#include <stddef.h>

extern "C" {

// Copies in[0..n) to out (NUL terminated, at most cap-1 bytes) replacing what must not leave the console. Line by line:
//   name=<text>, console=, friend=, room=, token=, key=, console_key=, code= (when it looks like a friend / room code)  -> <hidden>
//   a.b.c.d and a.b.c.d:port, IPv6 and MAC-like a:b::c                                                              -> <address>
//   8 hex digits after the word "address"; 32 or more hex digits; 40 or more letters+digits (a token)               -> <secret>
//   http(s)://host...                                                                                              -> http(s)://<host>
//   /home/<user>, /Users/<user>, C:\Users\<user>                                                                   -> .../<user>
// Returns the length written. Never reads past n, never writes past cap-1. Deterministic.
size_t mp_scrub(const char* in, size_t n, char* out, size_t cap);

// What goes into a report (all strings optional). Pure text assembly, no file access: the tests use this.
struct MpReportParts {
    const char* appVersion;         // "3.0.0"
    const char* buildTag;           // "Nov 10 2026 12:00:00"-style stamp, or any short id
    unsigned fingerprint;           // the session fingerprint (0 = unknown)
    int protocol;                   // MP_PROTO_VERSION
    const char* settings;           // "key = value" lines, no secrets (the caller leaves them out; the scrubber is a second guard)
    const char* log;                // raw tail of the log (may start mid-line)
    size_t logLen;
    bool logIsTail;                 // the log text does not start at the file's start: its first (partial) line is dropped
    const char* conntest;           // the last Connection Test result, text (null: none)
    size_t conntestLen;
};

// beta diagnostics: the uploaded report's "log": the allow-listed, scrubbed lines of the previous log (may be null) then the current one, the newest
// lines kept when more than cap - 1 bytes remain (whole lines). *IsTail: the text is the end of a larger file, its first line is dropped. Also hides
// e-mail addresses ("<email>"; mp_scrub does too). Returns the length written (NUL terminated).
size_t mp_report_log_payload(const char* prev, size_t prevLen, bool prevIsTail, const char* cur, size_t curLen, bool curIsTail, char* out, size_t cap);

// Builds the report text into out (NUL terminated). Log lines are kept only when they come from multiplayer / network code
// ("[mp]", "[np-net]", "[net-https]", "[vlink", "[online]"), then scrubbed. Returns the length. If cap is too small the oldest log lines are dropped first.
size_t mp_report_build(char* out, size_t cap, const MpReportParts& parts);

// Gathers everything on this console and writes the file. `dir` null = the data folder as above. On success path (if not null) gets the
// file's path and true is returned. Reads at most MP_REPORT_LOG_KB kilobytes from the end of log.txt. Game thread; blocks for a file write.
bool mp_report_save(const char* dir, char* path, size_t pathCap);

}  // extern "C"

#define MP_REPORT_LOG_KB 24        // last N kB of the log that are read
#define MP_REPORT_CONNTEST_KB 4    // at most this much of conntest.txt (the Connection Test's last result, in the multiplayer folder)

#endif  // PORT_MP_REPORT_H
