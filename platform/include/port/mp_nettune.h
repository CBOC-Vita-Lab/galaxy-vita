// Galaxy 3.0 multiplayer (netlog): the net timing knobs that decide how smooth remote players look, read once from the environment
// (env.txt on the console, the process environment on the desktop) when a session (or a scan) begins, clamped to safe ranges, and logged
// with their effective values ("[mp] net: tune ..."). Header only (no new source file for the host test builds). A key that is not set
// leaves the compiled-in default alone, bit for bit: `set` bit n says the key was given and valid, and only then does a consumer replace
// its constant. Written by the network thread when a session begins (idle -> not idle), read by the game thread when the session goes
// live and by the core; nothing changes while a session runs.
//
//   SMG_MP_NETLOG_SEC        5      0..300   seconds between the "[mp] net:" statistics lines (0 = off)
//   SMG_MP_NET_SEND_MS       22     10..100  shortest time between two STATE (pose) sends: 22 ms = every 2nd logic turn (30/s)
//   SMG_MP_NET_DELAY_MIN_MS  75     34..333  the least the remote players are drawn behind the newest pose they sent (jitter buffer floor)
//   SMG_MP_NET_DELAY_MAX_MS  333.3  75..333  the most (jitter buffer ceiling; the buffer holds 12 poses, 367 ms at 30/s)
//   SMG_MP_NET_DELAY_EXTRA_MS 25    0..100   added to the measured lateness when the buffer picks its delay
//   SMG_MP_NET_DELAY_PCT     95     50..99   the delay covers this share of the packets (the latest 100 - PCT % are left uncovered)
//   SMG_MP_NET_EXTRAP_MS     200    0..300   how long a puppet is guessed ahead past its newest pose before it holds still
//   SMG_MP_NET_KEEPALIVE_MS  50     20..200  a quiet peer link carries a datagram at least this often (Wi-Fi power save)
//   SMG_MP_NET_BUNDLE_MS     10     0..50    the host's poses wait at most this long for company in one datagram
//   SMG_MP_NET_HEARTBEAT_MS  500    100..2000 while other traffic flows, the ping heartbeat rides inside a datagram this often
//   SMG_MP_NET_POLL_MS       2      1..20    the network thread's sleep between passes (how often received datagrams are looked at)
#ifndef PORT_MP_NETTUNE_H
#define PORT_MP_NETTUNE_H

#include "port/mp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MpNetTune {
    enum { SEND = 1, DELAY_MIN = 2, DELAY_MAX = 4, DELAY_EXTRA = 8, DELAY_PCT = 16, EXTRAP = 32, KEEPALIVE = 64, BUNDLE = 128, PIGGY = 256, POLL = 512 };
    unsigned set;          // bits above: the key was given
    int logSec;            // 5
    int sendMs;            // 22
    float delayMinMs;      // 75
    float delayMaxMs;      // 333.3
    float delayExtraMs;    // 25
    int delayPct;          // 95
    float extrapMs;        // 200
    int keepAliveMs;       // 50
    int bundleMs;          // 10
    int piggyMs;           // 500
    int pollMs;            // 5

    static float msToTurns(float ms) { return ms * 0.06f; }   // 60 logic turns a second

    // The one place the process keeps it.
    static MpNetTune& cur() {
        static MpNetTune t = defaults();
        return t;
    }
    static MpNetTune defaults() {
        MpNetTune t;
        memset(&t, 0, sizeof t);
        t.logSec = 5;
        t.sendMs = 22;
        t.delayMinMs = 75.0f;
        t.delayMaxMs = 20.0f * 1000.0f / 60.0f;
        t.delayExtraMs = 25.0f;
        t.delayPct = 95;
        t.extrapMs = 200.0f;
        t.keepAliveMs = 50;
        t.bundleMs = 10;
        t.piggyMs = 500;
        t.pollMs = 2;
        return t;
    }

    // Reads the keys and logs the effective values (one line, plus one per key that was refused or clamped). Not for the game thread's
    // turn: once per session start.
    static const MpNetTune& load() {
        MpNetTune t = defaults();
        char note[160] = "";
        size_t nn = 0;
        t.logSec = (int)getNum("SMG_MP_NETLOG_SEC", 0, 300, t.logSec, 0, note, &nn);
        t.sendMs = (int)getNum("SMG_MP_NET_SEND_MS", 10, 100, t.sendMs, SEND, note, &nn, &t.set);
        t.delayMinMs = getNum("SMG_MP_NET_DELAY_MIN_MS", 34, 333, t.delayMinMs, DELAY_MIN, note, &nn, &t.set);
        t.delayMaxMs = getNum("SMG_MP_NET_DELAY_MAX_MS", 75, 333, t.delayMaxMs, DELAY_MAX, note, &nn, &t.set);
        t.delayExtraMs = getNum("SMG_MP_NET_DELAY_EXTRA_MS", 0, 100, t.delayExtraMs, DELAY_EXTRA, note, &nn, &t.set);
        t.delayPct = (int)getNum("SMG_MP_NET_DELAY_PCT", 50, 99, (float)t.delayPct, DELAY_PCT, note, &nn, &t.set);
        t.extrapMs = getNum("SMG_MP_NET_EXTRAP_MS", 0, 300, t.extrapMs, EXTRAP, note, &nn, &t.set);
        t.keepAliveMs = (int)getNum("SMG_MP_NET_KEEPALIVE_MS", 20, 200, (float)t.keepAliveMs, KEEPALIVE, note, &nn, &t.set);
        t.bundleMs = (int)getNum("SMG_MP_NET_BUNDLE_MS", 0, 50, (float)t.bundleMs, BUNDLE, note, &nn, &t.set);
        t.piggyMs = (int)getNum("SMG_MP_NET_HEARTBEAT_MS", 100, 2000, (float)t.piggyMs, PIGGY, note, &nn, &t.set);
        t.pollMs = (int)getNum("SMG_MP_NET_POLL_MS", 1, 20, (float)t.pollMs, POLL, note, &nn, &t.set);
        if (t.delayMaxMs < t.delayMinMs) t.delayMaxMs = t.delayMinMs;   // (a floor above the ceiling: the ceiling gives way, both stay in range)
        if (memcmp(&t, &cur(), sizeof t) != 0) cur() = t;   // (no write when nothing changed: several cores in one test process load at once; the struct has no padding)
        char keys[96] = "";
        size_t k = 0;
        static const struct { unsigned bit; const char* name; } names[] = {
            {SEND, "send"}, {DELAY_MIN, "delaymin"}, {DELAY_MAX, "delaymax"}, {DELAY_EXTRA, "delayextra"}, {DELAY_PCT, "delaypct"},
            {EXTRAP, "extrap"}, {KEEPALIVE, "keepalive"}, {BUNDLE, "bundle"}, {PIGGY, "heartbeat"}, {POLL, "poll"}};
        for (const auto& n : names)
            if ((t.set & n.bit) && k + 16 < sizeof keys) k += (size_t)snprintf(keys + k, sizeof keys - k, "%s%s", k ? "," : "", n.name);
        mp_log("net: tune sendms=%d delaymin=%.1f delaymax=%.1f delayextra=%.1f delaypct=%d extrapms=%.1f keepalive=%d bundle=%d heartbeat=%d poll=%d logsec=%d changed=%s",
               t.sendMs, t.delayMinMs, t.delayMaxMs, t.delayExtraMs, t.delayPct, t.extrapMs, t.keepAliveMs, t.bundleMs, t.piggyMs, t.pollMs, t.logSec,
               k ? keys : "none");
        if (nn) mp_log("net: tune notes:%s", note);
        return cur();
    }

private:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
// fmt1131: fmt is always one of this file's own literals ("%s=%g" style)
    static void addNote(char* note, size_t* nn, const char* fmt, const char* key, double v) {
        if (*nn + 60 >= 160) return;   // (the buffer holds a few notes; the rest are dropped)
        *nn += (size_t)snprintf(note + *nn, 160 - *nn, fmt, key, v);
    }
#pragma GCC diagnostic pop
    // key unset / empty: `def`, bit not set. Not a number: `def` and a note. Out of range: clamped, bit set, a note.
    static float getNum(const char* key, float lo, float hi, float def, unsigned bit, char* note, size_t* nn, unsigned* set = nullptr) {
        const char* e = getenv(key);
        if (!e || !*e) return def;
        char* end = nullptr;
        const double v = strtod(e, &end);
        if (end == e || (*end != 0 && *end != ' ' && *end != '\r' && *end != '\n')) {
            addNote(note, nn, " %s ignored (not a number), using %g;", key, (double)def);
            return def;
        }
        float f = (float)v;
        if (!(f >= lo)) {
            f = lo;
            addNote(note, nn, " %s clamped to %g;", key, (double)lo);
        } else if (f > hi) {
            f = hi;
            addNote(note, nn, " %s clamped to %g;", key, (double)hi);
        }
        if (set && bit) *set |= bit;
        return f;
    }
};

#endif  // PORT_MP_NETTUNE_H
