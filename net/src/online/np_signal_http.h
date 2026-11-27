// Signaling over HTTPS to the online service (server/PROTOCOL.md section 4, "Invite rooms and signaling"). One Signal object per room.
//
//   Host()  : GET /v1/info (reachability, kill switch, server clock, STUN list), POST /v1/rooms
//   Join()  : GET /v1/info, POST /v1/rooms/join {"code"}
//   in the room: GET /v1/rooms/{code}?v=<version> every 1.7 s (it is also the presence ping the server's link states come from; every 8 s once
//            the match started and nothing is pending: the peers heartbeat each other, the service only matters for host migration and rejoin;
//            1.7 s again while a member of one of our pairs has no endpoint yet or the service reports a quiet member), POST .../endpoint when the candidates change, POST .../punch with each attempt's verdict, POST .../started once
//            the link is confirmed both ways, POST .../leave when the session ends.
// Every request is retried with the protocol's backoff (1, 2, 4, 8 s, then 15 s) for network failures and 5xx; 429 waits `retryAfterSec`; other
// 4xx end the attempt with a sentence for the player. Nothing blocks: requests run through np_http (worker threads), Poll() moves results over.
#ifndef NP_NET_SIGNAL_HTTP_H
#define NP_NET_SIGNAL_HTTP_H

#include <string>

#include "np_signal.h"

namespace NpNet {

struct HttpSignalConfig {
    char baseUrl[320];          // "https://host" (no trailing slash); several, comma separated, are tried in turn when one does not answer
    char token[64];             // the account's bearer token (POST /v1/register, kept in the save file)
    char appVersion[24];        // X-Msc-App: the game version "1.2.0"
    char build[72];             // X-Msc-Build: hex build / rule fingerprint hash, "" = none
    uint32_t pollMs;            // room poll interval while the match has not started (the protocol asks for 1.5 to 2 s)
    uint32_t pollStartedMs;     // ... once it has and nothing is pending (the Galaxy service marks a member lost after 20 s without a call and quiet after 12 s: stay well under)
    uint32_t requestTimeoutMs;
    int maxAttempts;            // for the requests that decide whether the room exists (info, create, join)
    // Galaxy (service review, request budget): held polls. >0: the room poll asks the service to hold its answer up to this many seconds
    // until the room changes (GET ...?v=N&wait=S&p=1[&l=mask]); the SERVICE's plan in the answer (poll:{holdSec,nextSec}) then decides how long the
    // next poll may be held and when it is sent (svcfix2: 25 s while the room forms, one held 55 s poll a minute from the host and a poll every 5 minutes from
    // a guest once every peer heartbeats directly); an older service sends no plan: the next poll follows 0.3 s after an answer (4 s once the match started and nothing is
    // pending), and a queued request (endpoint, punch, started) cancels a held poll instead of waiting behind it. A service that answers
    // at once (one without held polls) is polled at pollMs / pollStartedMs as before. 0 = plain polls (the default of this struct; the
    // game sets 8, vlink_online.cpp: SMG_NET_POLL_WAIT).
    uint32_t pollWaitSec;
    HttpSignalConfig() : pollMs(1700), pollStartedMs(8000), requestTimeoutMs(10000), maxAttempts(3), pollWaitSec(0) {
        baseUrl[0] = token[0] = appVersion[0] = build[0] = 0;
    }
};

Signal* CreateHttpSignal(const HttpSignalConfig& config);

// svc3: forget that the service said "over_budget" (the console remembers it for at most 10 minutes and asks nothing meanwhile). Tests.
void ResetServiceBusy();
// svcfix2: /v1/info (and the relay-only sync) is asked once per session; forget the answer (tests).
void ResetServiceInfoCache();

// The sentence for a relay (POST /v1/turn) refusal: error code of the service and its message. Shared with the test fakes.
std::string TurnSentence(const std::string& code, const std::string& message);

}  // namespace NpNet

#endif
