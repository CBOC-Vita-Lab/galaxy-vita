// HttpSignal: see np_signal_http.h.
#include "np_signal_http.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "np_app_version.h"
#include "np_http.h"
#include "np_json.h"

// release3: the game's "a newer version is available" notice (platform/src/mp/mp_update.cpp); weak so Strikers and host tests link without it
extern "C" bool mp_update_note_latest(const char* latest) __attribute__((weak));

// Galaxy (integ): the body of the room creation (POST /v1/rooms: mode, maxMembers, visibility), set by the session layer before
// NpNetHost (mp_conn_online.cpp). Empty = the service's defaults (a code-only coop room), as in Strikers.
static std::string g_createBody;
extern "C" void NpNetSetCreateBody(const char* json) { g_createBody = json ? json : ""; }

namespace NpNet {
namespace {

void CopyStr(char* dst, size_t cap, const std::string& src) {
    size_t i = 0;
    for (; i < src.size() && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// svcfix3: members[].label is the name, plus "#" and the last 4 characters of the friend code when the name is shared. It has to fit the 15 bytes
// the game keeps for a name; when it does not (a name of ten multi-byte characters) the name is cut (never inside a character), never the suffix.
void FitLabel(char* dst, size_t cap, const std::string& label) {
    if (label.size() < cap) { CopyStr(dst, cap, label); return; }
    const size_t hash = label.rfind('#');
    if (hash == std::string::npos || label.size() - hash > 6 || label.size() - hash >= cap) { CopyStr(dst, cap, label); return; }
    const std::string tail = label.substr(hash);
    size_t keep = cap - 1 - tail.size();
    while (keep > 0 && (static_cast<unsigned char>(label[keep]) & 0xC0) == 0x80) --keep;
    CopyStr(dst, cap, label.substr(0, keep) + tail);
}

const char* BusySentence() { return "Online play is very busy right now. Please try again in a little while."; }   // (svc3: over_budget)

// What the player reads for an error code of the service (PROTOCOL.md section 1); the code is never shown.
std::string Sentence(const std::string& code, const std::string& message, int status, const std::string& latest) {
    if (code == "room_not_found" || code == "bad_code") return "That invite code was not found. Check it and try again.";
    if (code == "room_full") return "That room is already full.";
    if (code == "room_expired" || code == "room_closed") return "That room is closed. Ask for a new invite code.";
    if (code == "own_room") return "That is your own room. Give the code to a friend.";
    if (code == "in_ranked_match") return "That player is in a ranked match.";
    if (code == "update_required" || code == "upgrade_required") return latest.empty() ? "Different game version: please update the game." : "Different game version: please update the game to " + latest + ".";
    if (code == "unauthorized") return "Your online account was not accepted. Sign in again.";
    if (code == "banned") return "This online account cannot be used.";
    if (code == "daily_cap") return "Daily limit reached. Try again tomorrow.";
    if (code == "rate_limited") return "Too many tries. Wait a moment and try again.";
    if (code == "service_disabled") return message.empty() ? "Online play is switched off for now." : message;
    if (code == "server_full" || code == "busy") return "The online service is busy. Try again in a moment.";
    if (code == "over_budget" || code == "service_busy") return BusySentence();   // (svc3; quota32: Cloudflare's own quota / rate page)
    if (code == "not_member") return "You are not in that room any more.";
    if (code == "account_elsewhere") return "Your online account is now in a room on another system.";   // Galaxy (service review): a copied account file
    if (code == "version_mismatch" || code == "game_mismatch") return "That room uses a different version of the game.";   // Galaxy (fix3)
    if (status >= 500) return "The online service had a problem. Try again later.";
    char st[24];
    snprintf(st, sizeof st, "status %d", status);
    return "The online service refused the request (" + (code.empty() ? std::string(st) : code) + ").";
}

std::string ClosedSentence(const std::string& reason, bool expired) {
    if (expired) return "The room timed out.";
    if (reason == "host_left") return "The host left the room.";
    if (reason == "guest_left") return "Your friend left the room.";
    if (reason == "replaced") return "The host opened another room.";
    if (reason == "match_over") return "The match is over.";
    return "The room was closed.";
}

// Galaxy (service review): every retry delay is spread over 75 %..125 % of its base, so consoles that failed together (the service was
// down or out of quota for everybody) do not come back together. xorshift32 seeded from the system's random source (the clock if none).
uint32_t Spread(uint32_t base) {
    static uint32_t x = 0;
    if (!x) {
        if (!SystemRandom(0, &x, sizeof x) || !x) x = uint32_t(SteadyNowMs(0)) * 2654435761u | 1u;
    }
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    const uint32_t span = base / 2;
    return base - base / 4 + (span ? x % (span + 1) : 0);
}

// A service-sent retryAfterSec as milliseconds: whole seconds, at most 10 minutes, 0 when absent or not a sane number (NaN, huge, < 0).
uint32_t RetryAfterMs(double sec) {
    if (!(sec > 0)) return 0;
    if (sec > 600) sec = 600;
    return uint32_t(sec) * 1000u;
}

// svc3 (review S7): the service said "over_budget" (503): it has no requests left for the day (or none for new sessions: the part kept for
// running sessions is still served). retryAfterSec runs to 00:00 UTC, so it is honoured with a cap: the decisive requests (info, create, join)
// do not retry at all (retrying within seconds cannot succeed and every try is a request the free plan counts), the console remembers it was
// told to wait (at most kBusyCapMs, then it asks again) and answers the next Host() / Join() itself with the same sentence, without a request.
// The clock is the signal's own (SetClock); ResetServiceBusy() clears it (tests).
const uint32_t kBusyCapMs = 600000;       // the most a "come back later" is believed locally (RetryAfterMs caps the same way)
const uint32_t kTurnWaitMinMs = 600000;   // after a relay limit (turn_room_limit / turn_player_limit / turn_budget) the console does not ask again for 10 minutes (an hour of 15 s retries is 240 requests a member)
const uint32_t kBusyMinMs = 30000;        // a 503 over_budget without a usable retryAfterSec still keeps the console quiet this long
const uint32_t kBusyPollCapMs = 60000;    // a running room's poll: waits at most this long (the service marks a silent member lost after 20 s)
const uint32_t kBusyCallCapMs = 30000;    // an endpoint / punch / started call that was refused: tries again after at most this long
uint64_t g_busyUntil = 0;

// svcfix2: /v1/info is asked once per session (one console boot), not at every Host() / Join(): what it tells (the STUN list, the server's clock
// against ours, the kill switches) hardly changes, and a room a switch has closed is refused by the create / join request itself. The server clock
// offset drifts with the console's clock, so the answer is believed for kInfoTtlMs (2 hours: well under a second of drift).
// The same goes for "this account's relay-only flag is already what we want" (GET / PUT /v1/me). Process-wide; ResetServiceInfoCache() clears (tests).
struct InfoCache {
    bool valid = false;
    uint64_t atMs = 0;
    int64_t offsetMs = 0;
    bool haveOffset = false;
    std::string stunList;
    bool meKnown = false, relayOnly = false;
    std::string token;   // the account it was learned for
};
InfoCache g_info;
const uint64_t kInfoTtlMs = 2 * 3600 * 1000;

// svcfix2: the bounds within which the client obeys the service's poll plan (rooms4.pollPlan on the server): a held poll up to kPlanMaxHoldSec, the
// wait after an answer between kPlanMinNextMs and kPlanMaxNextMs. Whatever the service says, a started room is asked about at least every
// 10 minutes and a forming one never faster than 3 times a second.
const uint32_t kPlanMaxHoldSec = 60;
const uint32_t kPlanMinNextMs = 300;
const uint32_t kPlanMaxNextMs = 600000;
const uint32_t kForcedPollGapMs = 1000;   // a loss / gain of a direct link makes the console ask at once, but not more than once a second

int NatFromName(const std::string& n) {
    if (n == "open") return NAT_OPEN;
    if (n == "cone" || n == "restricted" || n == "full" || n == "port") return NAT_CONE;
    if (n == "symmetric") return NAT_SYMMETRIC;
    return NAT_UNKNOWN;
}
const char* NatName(int nat) {
    switch (nat) {
    case NAT_OPEN: return "open";
    case NAT_CONE: return "cone";
    case NAT_SYMMETRIC: return "symmetric";
    default: return "unknown";
    }
}

class HttpSignal : public Signal {
public:
    explicit HttpSignal(const HttpSignalConfig& c)
        : cfg_(c), status_(IDLE), now_(SteadyNowMs), nowCtx_(0), baseIdx_(0), cur_(0), retryAt_(0), nextPoll_(0), infoDone_(false), started_(false),
          startedSent_(false), haveOffset_(false), peersQuiet_(false), offsetMs_(0), nonceGen_(0), haveNonce_(false), failStreak_(0), planValid_(false), planHoldSec_(0), planNextMs_(0), directMask_(0), haveMask_(false), pollNow_(false), lastForcedAt_(0), pollScale_(1.0), curAt_(0), host_(false), relayOnly_(false),
          relayOnlySet_(false), turnState_(TURN_NONE) {
        turnError_[0] = 0;
        // split the base URL list
        std::string all = cfg_.baseUrl;
        size_t p = 0;
        while (p <= all.size()) {
            size_t e = all.find(',', p);
            if (e == std::string::npos) e = all.size();
            std::string u = all.substr(p, e - p);
            while (!u.empty() && u[u.size() - 1] == '/') u.resize(u.size() - 1);
            // Galaxy: never the Strikers service (msc-online), whatever a configuration says: Galaxy has its own (smg-online)
            if (u.find("msc-online") != std::string::npos) {
                NPN_LOG("signaling: the Strikers online service is not Galaxy's: %s ignored\n", u.c_str());
                u.clear();
            }
            if (!u.empty()) bases_.push_back(u);
            p = e + 1;
        }
        error_[0] = 0;
        name_[0] = 0;
        code_[0] = 0;
        memset(nonce_, 0, sizeof nonce_);
    }
    ~HttpSignal() override {
        if (cur_) HttpCancel(cur_);
    }

    void SetClock(uint64_t (*now)(void*), void* ctx) override {
        now_ = now;
        nowCtx_ = ctx;
    }

    void Host(const char* name) override { Begin(name, 0); }
    void Join(const char* invite, const char* name) override { Begin(name, invite ? invite : ""); }

    void PublishCandidates(const SigCandidates& mine) override {
        mine_ = mine;
        if (!haveNonce_ || nonceGen_ != mine.gen) {   // a rebuilt link is a new attempt: the partner sees a new nonce and punches afresh
            if (!SystemRandom(0, nonce_, sizeof nonce_)) {   // a nonce from a dead generator repeats: no punching on it
                Fail(RandomFailureSentence());
                return;
            }
            nonceGen_ = mine.gen;
            haveNonce_ = true;
        }
        room_.members[room_.mySlot < 0 ? 0 : room_.mySlot].cands = mine;
        Job j;
        j.kind = K_ENDPOINT;
        j.method = "POST";
        j.path = "/v1/rooms/" + std::string(code_) + "/endpoint";
        j.body = EndpointBody();
        for (size_t i = 0; i < queue_.size(); ++i)
            if (queue_[i].kind == K_ENDPOINT) { queue_.erase(queue_.begin() + long(i)); break; }   // only the newest matters
        PushBeforeTurn(j);
    }

    void ReportPunch(const PunchReport& r) override {
        Job j;
        j.kind = K_PUNCH;
        j.method = "POST";
        j.path = "/v1/rooms/" + std::string(code_) + "/punch";
        const char* path = r.pathKind == 0 ? "lan" : (r.pathKind == 2 ? "relay" : "direct");
        char body[160];
        const unsigned ms = unsigned(r.ms > 120000 ? 120000 : r.ms);   // the service's limit: a larger value is a 400, which ends the signaling (a late report must not)
        // Galaxy (rooms of up to 4): "peer" names the member the attempt was with
        const unsigned peer = unsigned(r.peerSlot < kMaxSlots ? r.peerSlot : 0);
        if (r.ok) snprintf(body, sizeof body, "{\"result\":\"ok\",\"path\":\"%s\",\"ms\":%u,\"peer\":%u}", path, ms, peer);
        else snprintf(body, sizeof body, "{\"result\":\"fail\",\"path\":\"direct\",\"ms\":%u,\"peer\":%u}", ms, peer);
        j.body = body;
        PushBeforeTurn(j);
    }

    // svcfix3 (S5): credentials without a visible reason come out of a smaller slice of the day, so the service must have seen this console's
    // endpoint (a symmetric NAT) or punch verdict before it sees the request for credentials: a report that is made after the request was
    // queued goes in front of it (the requests are sent one at a time, in queue order).
    template <class J> void PushBeforeTurn(const J& j) {   // (Job is declared further down)
        size_t at = queue_.size();
        for (size_t i = 0; i < queue_.size(); ++i)
            if (queue_[i].kind == K_TURN) { at = i; break; }
        queue_.insert(queue_.begin() + long(at), j);
    }

    void SetRelayOnly(bool on) override {
        relayOnly_ = on;
        relayOnlySet_ = true;
    }

    void RequestTurn() override {
        if (status_ != IN_ROOM) return;
        if (turnBlockedUntil_ && now_ && now_(nowCtx_) < turnBlockedUntil_) {   // the service said "not now" (a limit): not asked again until its time is up
            turnState_ = TURN_FAILED;
            CopyStr(turnError_, sizeof turnError_, TurnSentence(turnLimitCode_, ""));
            return;
        }
        turnState_ = TURN_WORKING;
        turnError_[0] = 0;
        for (size_t i = 0; i < queue_.size(); ++i)
            if (queue_[i].kind == K_TURN) return;
        Job j;
        j.kind = K_TURN;
        j.method = "POST";
        j.path = "/v1/turn";
        j.body = "{\"room\":";
        JsonEscapeAppend(&j.body, code_);
        j.body += "}";
        queue_.push_back(j);
    }
    TurnStatus TurnState() const override { return turnState_; }
    const TurnInfo& Turn() const override { return turn_; }
    const char* TurnError() const override { return turnError_; }
    uint32_t TurnWaitMs() const override { return turnState_ == TURN_FAILED ? turnWaitMs_ : 0; }
    void ReportTurnUsage(uint64_t bytes) override {
        if (bytes == 0 || bases_.empty()) return;
        char body[64];
        // (netfix B8a: no %llu on the Vita: the decimal digits by hand)
        char digits[24];
        int nd = 0;
        for (uint64_t v = bytes; v > 0 && nd < 20; v /= 10) digits[nd++] = char('0' + int(v % 10));
        char num[24];
        for (int i = 0; i < nd; ++i) num[i] = digits[nd - 1 - i];
        num[nd] = 0;
        snprintf(body, sizeof body, "{\"bytes\":%s}", num);
        HttpFire("POST", bases_[baseIdx_] + "/v1/turn/usage", body, Headers(), 5000);
    }

    void Started() override {
        if (startedSent_ || status_ != IN_ROOM) return;
        startedSent_ = true;
        Job j;
        j.kind = K_STARTED;
        j.method = "POST";
        j.path = "/v1/rooms/" + std::string(code_) + "/started";
        queue_.push_back(j);
    }

    void SetDirectLinks(uint32_t mask) override {
        mask &= 0xf;
        const uint32_t changed = mask ^ directMask_;
        directMask_ = mask;
        if (!startedSent_ || status_ != IN_ROOM || !cfg_.pollWaitSec || !changed) return;
        // A link that went away (a peer lost) is told to the service AT ONCE: it may be the host, and a new host has to be found. A link that came
        // (a member joined or came back) is told at once too, since the host's settled rhythm waits for it. At most one such ask a second.
        const uint64_t now = now_(nowCtx_);
        if (haveMask_ && SinceMs(now, lastForcedAt_) < kForcedPollGapMs) return;
        lastForcedAt_ = now;
        pollNow_ = true;
        nextPoll_ = now;
    }

    void SetLeaveSummary(const char* json) override { leaveSummary_ = json ? json : ""; }

    void Leave() override {
        if (cur_) { HttpCancel(cur_); cur_ = 0; }
        queue_.clear();
        if (code_[0] && (status_ == IN_ROOM || status_ == WORKING) && !bases_.empty()) {
            // fire and forget: the room closes for a host, a guest leaves it (it reopens)
            std::string leaveBody = "{}";
            if (!leaveSummary_.empty()) leaveBody = "{\"summary\":" + leaveSummary_ + "}";   // beta diagnostics: the session's numbers
            HttpFire("POST", bases_[baseIdx_] + "/v1/rooms/" + code_ + "/leave", leaveBody, Headers(), 5000);
        }
        if (status_ != FAILED) status_ = CLOSED;
    }

    void Poll() override {
        HttpPump();
        if (status_ != WORKING && status_ != IN_ROOM) return;
        const uint64_t now = now_(nowCtx_);
        if (cur_) {
            HttpResponse r;
            const int rc = HttpPoll(cur_, &r);
            if (rc == 1) {
                cur_ = 0;
                OnResponse(r, now);
            } else if (rc < 0) {
                // the request is gone (the table lost it): that is a failed attempt like any other, not a silent hole in the queue
                cur_ = 0;
                HttpResponse t;
                t.error = 1;
                t.text = "The online service request was lost.";
                OnResponse(t, now);
            } else if (curJob_.kind == K_POLL && curJob_.waitMs && (!queue_.empty() || pollNow_ || HttpOthersWaiting(cur_))) {
                // a held poll must not hold back a request (our endpoint, a punch verdict), nor a loss of a direct link that the service must
                // hear about (svcfix2): drop it, send the request (or, for a link change, the poll again at once with the new report).
                // netport31: nor another part of the game's request (the menus, a race time, an invite): the console's HTTPS client runs one
                // request at a time, so it would wait for the whole hold (up to 55 s). The poll is asked again at once and queues behind it.
                HttpCancel(cur_);
                cur_ = 0;
                nextPoll_ = now;
                if (!queue_.empty()) {
                    Job j = queue_.front();
                    queue_.pop_front();
                    StartJob(j, now);
                }
            } else if (SinceMs(now, curAt_) > cfg_.requestTimeoutMs + curJob_.waitMs + 3000) {
                HttpCancel(cur_);
                cur_ = 0;
                HttpResponse t;
                t.error = 1;
                t.text = "The online service did not answer in time.";
                OnResponse(t, now);
            }
            return;
        }
        if (now < retryAt_) return;
        if (!queue_.empty()) {
            Job j = queue_.front();
            queue_.pop_front();
            StartJob(j, now);
            return;
        }
        if (status_ == IN_ROOM && now >= nextPoll_) {
            Job j;
            j.kind = K_POLL;
            j.method = "GET";
            char v[64];
            unsigned waitSec = cfg_.pollWaitSec > kPlanMaxHoldSec ? kPlanMaxHoldSec : unsigned(cfg_.pollWaitSec);
            const bool plan = cfg_.pollWaitSec > 0;   // svcfix2: held polls on = the service's poll plan is asked for (p=1) and obeyed
            if (plan && planValid_) waitSec = planHoldSec_;   // the service decides how long it may hold the next one
            else if (waitSec && pollScale_ > 1.0) {   // svc3: stretched, but under the service's quiet limit (12 s)
                const unsigned stretched = unsigned(double(waitSec) * pollScale_ + 0.5);
                if (stretched > waitSec) waitSec = stretched > 11u ? (waitSec > 11u ? waitSec : 11u) : stretched;
            }
            pollNow_ = false;
            if (room_.version) {
                int n = snprintf(v, sizeof v, "?v=%u", unsigned(room_.version));
                if (waitSec) n += snprintf(v + n, sizeof v - size_t(n), "&wait=%u", waitSec);
                if (plan) {
                    n += snprintf(v + n, sizeof v - size_t(n), "&p=1");
                    if (startedSent_) { n += snprintf(v + n, sizeof v - size_t(n), "&l=%x", unsigned(directMask_ & 0xf)); haveMask_ = true; }
                }
                j.waitMs = waitSec * 1000u;
            } else v[0] = 0;
            j.path = "/v1/rooms/" + std::string(code_) + (room_.version ? v : "");
            StartJob(j, now);
        }
    }

    Status GetStatus() const override { return status_; }
    const SigRoom& Room() const override { return room_; }
    const char* Error() const override { return error_; }
    const char* ErrorCode() const override { return status_ == FAILED || status_ == CLOSED ? errCode_ : ""; }
    const char* StunList() const override { return stunList_.c_str(); }

private:
    static const int kPollGiveUp = 9;   // consecutive failed polls (2+4+8+16+32+60+60+60 s, jittered: about 4 min)
    enum Kind { K_INFO, K_ME, K_ME_PUT, K_CREATE, K_JOIN, K_POLL, K_ENDPOINT, K_PUNCH, K_STARTED, K_TURN, K_ACCEPT };
    struct Job {
        Kind kind;
        std::string method, path, body;
        int tries;
        uint32_t waitMs;   // a held poll: how long the service may hold it (added to the request's time limit)
        Job() : kind(K_POLL), tries(0), waitMs(0) {}
    };

    void Begin(const char* name, const char* invite) {
        host_ = invite == 0;
        CopyStr(name_, sizeof name_, name ? name : "");
        error_[0] = 0;
        errCode_[0] = 0;
        queue_.clear();
        if (bases_.empty() || !cfg_.token[0]) {
            Fail(bases_.empty() ? "The online service address is not set." : "You are not signed in to the online service.");
            return;
        }
        if (g_busyUntil) {   // svc3: the service said it is out of requests for now: ask nothing until the time it named (capped)
            if (now_(nowCtx_) < g_busyUntil) {
                Fail(BusySentence());
                CopyStr(errCode_, sizeof errCode_, "over_budget");
                return;
            }
            g_busyUntil = 0;
        }
        status_ = WORKING;
        planValid_ = false; planHoldSec_ = 0; planNextMs_ = 0; directMask_ = 0; haveMask_ = false; pollNow_ = false;
        const bool cached = g_info.valid && g_info.token == cfg_.token && now_(nowCtx_) >= g_info.atMs && now_(nowCtx_) - g_info.atMs < kInfoTtlMs;
        if (cached) {   // svcfix2: /v1/info was answered this session (same account): use it
            offsetMs_ = g_info.offsetMs;
            haveOffset_ = g_info.haveOffset;
            stunList_ = g_info.stunList;
            infoDone_ = true;
        } else {
            g_info = InfoCache();
            Job info;
            info.kind = K_INFO;
            info.method = "GET";
            info.path = "/v1/info";
            queue_.push_back(info);
        }
        Job j;
        j.kind = host_ ? K_CREATE : K_JOIN;
        j.method = "POST";
        if (host_) {
            j.path = "/v1/rooms";
            j.body = g_createBody;   // Galaxy (integ): mode / visibility / maxMembers
        } else if (invite[0] == '@') {
            // "@CODE": a room the service already made with this player in it (a ranked pairing, PROTOCOL.md section 5: "continue exactly
            // like an invite room with room.code"). It cannot be joined by code; reading it is the entry (and the join acknowledgement).
            std::string code;
            for (const char* p = invite + 1; *p; ++p)
                if (isalnum(static_cast<unsigned char>(*p))) code.push_back(char(toupper(static_cast<unsigned char>(*p))));
            j.method = "GET";
            j.path = "/v1/rooms/" + code;
        } else {
            j.path = "/v1/rooms/join";
            std::string code;
            for (const char* p = invite; *p; ++p)
                if (*p != ' ' && *p != '-') code.push_back(char(toupper(static_cast<unsigned char>(*p))));
            j.body = "{\"code\":";
            JsonEscapeAppend(&j.body, code.c_str());
            j.body += "}";
        }
        pending_ = j;
        if (cached) {   // no /v1/info to wait for: the account's relay-only flag is checked (once per session), then the create / join goes out
            if (relayOnlySet_ && !(g_info.meKnown && g_info.relayOnly == relayOnly_)) {
                Job me;
                me.kind = K_ME;
                me.method = "GET";
                me.path = "/v1/me";
                queue_.push_back(me);
            } else {
                ReleasePending();
            }
        }
    }

    std::vector<std::string> Headers() const {
        std::vector<std::string> h;
        h.push_back(std::string("Authorization: Bearer ") + cfg_.token);
        h.push_back("X-Msc-Proto: 1");
        h.push_back("X-Msc-Game: smg");   // Galaxy: the service is shared code; this names the game on every request
        h.push_back(std::string("X-Msc-App: ") + AppVersionOrDefault(cfg_.appVersion));   // always: the production service refuses a request without it
        if (cfg_.build[0]) h.push_back(std::string("X-Msc-Build: ") + cfg_.build);
        return h;
    }

    std::string EndpointBody() const {
        std::string b = "{";
        if (!mine_.relayOnly) {
            // privacy: a relay-only account sends nothing but its relayed address and nonce (the service refuses anything else)
            const Addr pub = mine_.pub.IsZero() ? mine_.local : mine_.pub;   // "public" is required: without a STUN answer the LAN address is all there is
            b += "\"public\":\"";
            b += AddrStr(pub);
            b += "\",\"local\":\"";
            b += AddrStr(mine_.local);
            b += "\",";
        }
        if (!mine_.relay.IsZero()) {
            b += "\"relay\":\"";
            b += AddrStr(mine_.relay);
            b += "\",";
        }
        b += "\"nonce\":\"";
        b += Base64Encode(nonce_, sizeof nonce_);
        b += "\"";
        if (!mine_.relayOnly) {
            b += ",\"nat\":\"";
            b += NatName(mine_.nat);
            b += "\",\"candidates\":[";
            b += "{\"type\":\"host\",\"addr\":\"";
            b += AddrStr(mine_.local);
            b += "\"}";
            if (!mine_.pub.IsZero()) {
                b += ",{\"type\":\"srflx\",\"addr\":\"";
                b += AddrStr(mine_.pub);
                b += "\"}";
            }
            if (!mine_.pub2.IsZero() && mine_.pub2 != mine_.pub) {
                b += ",{\"type\":\"srflx\",\"addr\":\"";
                b += AddrStr(mine_.pub2);
                b += "\"}";
            }
            // Galaxy (netx): the UPnP-mapped port (the service keeps up to 6 candidates; this makes at most 4)
            if (!mine_.mapped.IsZero() && mine_.mapped != mine_.pub && mine_.mapped != mine_.pub2) {
                b += ",{\"type\":\"srflx\",\"addr\":\"";
                b += AddrStr(mine_.mapped);
                b += "\"}";
            }
            b += "]";
        }
        b += "}";
        return b;
    }

    void StartJob(Job& j, uint64_t now) {
        std::string err;
        const std::string url = bases_[baseIdx_] + j.path;
        // netport31 (Strikers heldpoll23): a held poll may go without data for its whole hold; on the console np_https otherwise ends any request
        // after 10 s without a byte ("no data for too long"), and the room's holds are 25 s (forming) and 55 s (a settled host): every held
        // poll failed, the failures counted towards kPollGiveUp and the room was given up after about 4 minutes. Ordinary requests keep 10 s.
        cur_ = HttpStart(j.method.c_str(), url, j.body, Headers(), cfg_.requestTimeoutMs + j.waitMs, j.waitMs ? HeldIoMs(j.waitMs) : 0, &err);
        curJob_ = j;
        curAt_ = now;
        if (!cur_) {
            HttpResponse t;
            t.error = 1;
            t.text = err;
            OnResponse(t, now);
        }
    }

    void Retry(uint64_t now, uint32_t minWaitMs = 0) {
        static const uint32_t backoff[] = {1000, 2000, 4000, 8000, 15000};
        const int n = curJob_.tries < 4 ? curJob_.tries : 4;
        uint32_t wait = Spread(backoff[n]);   // (service review: with jitter)
        if (wait < minWaitMs) wait = minWaitMs;
        curJob_.tries++;
        queue_.push_front(curJob_);
        retryAt_ = now + wait;
    }

    void Fail(const std::string& why) {
        CopyStr(error_, sizeof error_, why);
        status_ = FAILED;
        queue_.clear();
        NPN_LOG("signaling failed: %s\n", error_);
    }

    void OnResponse(const HttpResponse& r, uint64_t now) {
        const Job& j = curJob_;
        const bool decisive = j.kind == K_INFO || j.kind == K_ME || j.kind == K_ME_PUT || j.kind == K_CREATE || j.kind == K_JOIN;
        std::string code, message, latest, reason;
        Json err;
        uint32_t retryAfterMs = 0;
        if (!r.error && (r.status < 200 || r.status >= 300) && err.Parse(r.body.data(), r.body.size())) {
            const int e = err.Get(err.Root(), "error");
            code = err.GetStr(e, "code");
            message = err.GetStr(e, "message");
            latest = err.GetStr(e, "latestVersion");
            if (!latest.empty() && mp_update_note_latest) mp_update_note_latest(latest.c_str());   // release3: a failed join / call names the newest version
            reason = err.GetStr(e, "reason");
            retryAfterMs = RetryAfterMs(err.GetNum(e, "retryAfterSec", 0));   // (an int() of a huge double was undefined behaviour)
            if (code == "rate_limited" && err.GetStr(e, "reason") == "daily_cap") code = "daily_cap";   // a limit of the day: waiting a moment does not help
        }
        if (j.kind == K_TURN) {
            OnTurnResponse(r, now, code, message, retryAfterMs);
            return;
        }
        // quota32: an answer that is not the service's JSON error (Cloudflare's own page: "1027 Worker exceeded free tier daily request limit",
        // HTTP 429; a 1101 / 502 / 5xx page) is "busy" to the player: the same calm sentence and code as the service's own over_budget. A 429 of that
        // kind is treated like over_budget (the room-deciding calls do not retry it; a poll backs off); a 5xx keeps its retries.
        const bool edgePage = !r.error && code.empty() && (r.status == 429 || r.status >= 500);
        if (edgePage) code = "service_busy";
        if (!code.empty() && status_ != FAILED && status_ != CLOSED) CopyStr(errCode_, sizeof errCode_, code.c_str());   // Galaxy (fix3)
        const bool overBudget = (r.status == 503 && code == "over_budget") || (edgePage && r.status == 429);   // svc3 (+ quota32)
        if (overBudget && decisive) {
            uint32_t w = retryAfterMs > kBusyCapMs ? kBusyCapMs : retryAfterMs;
            if (w < kBusyMinMs) w = kBusyMinMs;
            g_busyUntil = now + w;
            Fail(BusySentence());
            return;
        }
        if (r.status == 429 && reason == "daily_cap") {
            // a per-account daily limit (rooms, matches, ...): retryAfterSec runs to midnight UTC, waiting it out is pointless. Terminal, with a sentence.
            Fail("Daily limit reached. Try again tomorrow.");
            return;
        }
        // (reason "too_early" on a result is an ordinary 429: waited out by the retry below / the result queue)
        const bool transient = r.error || r.status >= 500 || r.status == 429;
        if (transient) {
            ++failStreak_;
            if (decisive && j.tries + 1 >= cfg_.maxAttempts) {
                if (r.error) {
                    if (bases_.size() > 1 && j.kind == K_INFO) {   // another server of the list
                        baseIdx_ = (baseIdx_ + 1) % bases_.size();
                    }
                    Fail(r.text.empty() ? "The online service could not be reached." : r.text);
                } else {
                    Fail(Sentence(code, message, r.status, latest));
                }
                return;
            }
            if (!decisive && j.tries >= 6 && j.kind != K_POLL) return;   // a lost report is not worth more
            if (j.kind == K_POLL) {
                // the poll comes round again by itself, slower: 2, 4, 8, 16, 32, then 60 s, each with jitter; a 429's retryAfterSec is
                // honoured (at most 10 min). After kPollGiveUp failures in a row (about 4 minutes) the service is given up for this
                // room: np_online keeps a running match on its direct links (SignalingLost) and ends a room that has none. Thousands
                // of consoles therefore never poll a service that is out of quota or down for longer than that.
                if (failStreak_ >= kPollGiveUp) {
                    Fail(r.error ? "The online service cannot be reached." : Sentence(code, message, r.status, latest));
                    return;
                }
                const int sh = failStreak_ < 6 ? failStreak_ : 6;
                uint32_t wait = Spread(sh >= 6 ? 60000u : 1000u << sh);
                if (wait > 60000u + 15000u) wait = 75000u;
                if (r.status == 429 && retryAfterMs > wait) wait = retryAfterMs;
                if (overBudget && retryAfterMs > wait) wait = retryAfterMs > kBusyPollCapMs ? kBusyPollCapMs : retryAfterMs;   // svc3
                nextPoll_ = now + wait;
                return;
            }
            if (r.error && bases_.size() > 1 && j.kind == K_INFO) baseIdx_ = (baseIdx_ + 1) % bases_.size();
            Retry(now, r.status == 429 ? retryAfterMs : (overBudget ? (retryAfterMs > kBusyCallCapMs ? kBusyCallCapMs : retryAfterMs) : 0));
            return;
        }
        failStreak_ = 0;
        if (r.status < 200 || r.status >= 300) {
            // a refusal. Fatal for the requests that decide the room; a closed room for the others
            if (decisive || code == "unauthorized" || code == "update_required" || code == "upgrade_required" || code == "banned" || code == "service_disabled") {
                Fail(Sentence(code, message, r.status, latest));
                return;
            }
            if (code == "room_closed" || code == "room_expired" || code == "not_member" || code == "account_elsewhere" || r.status == 404 || r.status == 410) {
                status_ = CLOSED;
                // Galaxy (fix3): not_member = the service dropped this console from the room (silent > 60 s: asleep, a long stall):
                // the room may well go on without us (ErrorCode() says which, for a fresh join)
                CopyStr(error_, sizeof error_, code == "not_member" || code == "account_elsewhere" ? Sentence(code, message, r.status, latest)
                                                                     : ClosedSentence(code == "room_expired" ? "" : "", code == "room_expired"));
                NPN_LOG("signaling: %s closed the room for us (%d %s)\n", j.path.c_str(), r.status, code.c_str());
                return;
            }
            if (j.kind == K_ENDPOINT || j.kind == K_PUNCH || j.kind == K_STARTED) {
                // 409 room_not_ready: the partner's side is not there yet; asking again is right. Any other refusal of these will not change by
                // asking again, and a dropped endpoint would leave the host waiting for a partner who can never see it: the room fails, with a sentence.
                if (r.status == 409 && j.kind != K_ENDPOINT && j.tries < 5) {
                    Retry(now);
                    return;
                }
                Fail(Sentence(code, message, r.status, latest));
                return;
            }
            NPN_LOG("signaling: %s refused (%d %s)\n", j.path.c_str(), r.status, code.c_str());
            return;
        }
        Json doc;
        if (!doc.Parse(r.body.data(), r.body.size())) {
            NPN_LOG("signaling: unreadable answer to %s\n", j.path.c_str());
            if (decisive) Fail("The online service sent an answer the game does not understand.");
            return;
        }
        const int root = doc.Root();
        switch (j.kind) {
        case K_INFO: {
            if (mp_update_note_latest) mp_update_note_latest(doc.GetStr(doc.Get(root, "app"), "latest").c_str());   // release3: "a newer version is available"
            const int kill = doc.Get(root, "kill");
            for (int i = 0; i < doc.Size(kill); ++i) {
                const std::string& k = doc.Str(doc.At(kill, i));
                if (k == "all" || k == "rooms") {
                    const std::string& m = doc.GetStr(root, "message");
                    Fail(m.empty() ? "Online play is switched off for now." : m);
                    return;
                }
            }
            const double serverMs = doc.GetNum(root, "serverMs", 0);
            if (serverMs > 0) {
                // the server clock against ours, with half the round trip taken off (the answer is that old)
                offsetMs_ = int64_t(serverMs) - int64_t(now - SinceMs(now, curAt_) / 2);
                haveOffset_ = true;
            }
            const int stun = doc.Get(root, "stun");
            stunList_.clear();
            for (int i = 0; i < doc.Size(stun) && i < 4; ++i) {
                if (i) stunList_ += ",";
                stunList_ += doc.Str(doc.At(stun, i));
            }
            infoDone_ = true;
            g_info.valid = true;
            g_info.atMs = now;
            g_info.offsetMs = offsetMs_;
            g_info.haveOffset = haveOffset_;
            g_info.stunList = stunList_;
            g_info.token = cfg_.token;
            if (relayOnlySet_) {
                // the service filters addresses by the profile's relay-only flag, so the profile must say what this console does
                Job me;
                me.kind = K_ME;
                me.method = "GET";
                me.path = "/v1/me";
                queue_.push_back(me);
            } else {
                ReleasePending();
            }
            break;
        }
        case K_ME: {
            const bool has = doc.GetBool(root, "relayOnly", false);
            g_info.meKnown = true;
            g_info.relayOnly = has;
            if (has != relayOnly_) {
                Job put;
                put.kind = K_ME_PUT;
                put.method = "PUT";
                put.path = "/v1/me";
                put.body = std::string("{\"relayOnly\":") + (relayOnly_ ? "true" : "false") + "}";
                queue_.push_back(put);
            } else {
                ReleasePending();
            }
            break;
        }
        case K_ACCEPT:
            nextPoll_ = now + 300;   // the joiner's endpoint may be visible now
            break;
        case K_ME_PUT:
            g_info.meKnown = true;
            g_info.relayOnly = relayOnly_;
            ReleasePending();
            break;
        case K_TURN:
            break;
        case K_CREATE:
        case K_JOIN:
        case K_POLL:
        case K_ENDPOINT:
        case K_PUNCH:
        case K_STARTED: {
            const uint32_t before = room_.version;
            ApplyRoom(doc, root, now);
            AcceptPending();
            {   // svc3: pace hint, 1 when absent (a service without pacing, or one with budget to spare)
                double sc = doc.GetNum(root, "pollScale", 1.0);
                if (!(sc >= 1.0)) sc = 1.0;
                pollScale_ = sc > 1.4 ? 1.4 : sc;
            }
            if (j.kind == K_ENDPOINT) nextPoll_ = now + 300;   // the partner's endpoint may be there already
            bool planned = false;
            if (cfg_.pollWaitSec) {   // svcfix2: the service's plan: { holdSec, nextSec }, obeyed within bounds
                const int pl = doc.Get(root, "poll");
                if (pl >= 0 && doc.TypeOf(pl) == Json::OBJ) {
                    double hold = doc.GetNum(pl, "holdSec", -1), next = doc.GetNum(pl, "nextSec", -1);
                    if (hold >= 0 && next >= 0) {
                        planValid_ = true;
                        planHoldSec_ = hold > kPlanMaxHoldSec ? kPlanMaxHoldSec : unsigned(hold);
                        const double ms = next * 1000.0;
                        planNextMs_ = ms < kPlanMinNextMs ? kPlanMinNextMs : ms > kPlanMaxNextMs ? kPlanMaxNextMs : uint32_t(ms);
                        planned = true;
                    }
                }
            }
            if (j.kind == K_POLL) {
                if (planned) nextPoll_ = pollNow_ ? now : now + (planNextMs_ >= 30000 ? Spread(planNextMs_) : planNextMs_);   // (a long sleep is spread: consoles that began together do not ask together)
                else if (!j.waitMs) nextPoll_ = now + PollInterval();
                else if (room_.version != before) nextPoll_ = now + 300;                       // a change: look again soon (held)
                // held (for any part of the time: the service also answers early when it looks at its rooms): poll again soon
                else if (SinceMs(now, curAt_) >= 1500 && !peersQuiet_) nextPoll_ = now + (startedSent_ && !PollBusy() ? uint32_t(4000.0 * pollScale_) : 300);
                else nextPoll_ = now + PollInterval();   // answered at once without a change: a service without held polls (or peersQuiet)
            }
            break;
        }
        }
    }

    // Free-plan budget (server/README.md): a started room polls every pollStartedMs (8 s) instead of every 1.7 s. The peers heartbeat each other
    // directly; the service is only needed for host migration and for members who join or rejoin. So the fast rhythm comes back whenever
    // something is going on: a member of one of OUR pairs (the host for a guest, every guest for the host) has no endpoint yet (a drop-in or
    // a rejoin in progress), or the service said that a member went quiet (peersQuiet: a host that may be about to be replaced).
    bool PollBusy() const {
        if (peersQuiet_) return true;
        for (int i = 0; i < kMaxSlots; ++i) {
            const SigMember& m = room_.members[i];
            if (!m.present || i == room_.mySlot) continue;
            if (m.link != 0) return true;
            const bool myPair = room_.mySlot == room_.hostSlot || i == room_.hostSlot;
            if (myPair && !m.cands.valid) return true;
        }
        return false;
    }
    uint32_t PollInterval() const {
        if (!startedSent_ || PollBusy()) return uint32_t(double(cfg_.pollMs) * pollScale_);
        const uint32_t t = uint32_t(double(cfg_.pollStartedMs) * pollScale_);   // svc3: stretched, but under the quiet limit
        return t > cfg_.pollStartedMs && t > 11000 ? (cfg_.pollStartedMs > 11000 ? cfg_.pollStartedMs : 11000) : t;
    }

    void ReleasePending() {
        if (pending_.kind == K_CREATE || pending_.kind == K_JOIN) {
            queue_.push_back(pending_);
            pending_ = Job();
        }
    }

    void OnTurnResponse(const HttpResponse& r, uint64_t now, const std::string& code, const std::string& message, uint32_t retryAfterMs) {
        const Job& j = curJob_;
        auto failTurn = [&](const std::string& sentence) {
            turnState_ = TURN_FAILED;
            CopyStr(turnError_, sizeof turnError_, sentence);
            NPN_LOG("relay credentials refused: %s\n", turnError_);
        };
        const bool again = r.error || r.status >= 500 || r.status == 429;
        // svcfix3: turn_room_limit / turn_player_limit (429) and turn_budget (503) mean "no relay right now": one answer ends it (no retries: every one
        // is a request the service counts), the sentence is calm, and the same room is not asked again for a while (turnBlockedUntil_)
        const bool limit = code == "turn_room_limit" || code == "turn_player_limit" || code == "turn_budget";
        const bool hard = limit || code == "turn_unavailable" || code == "service_disabled" || code == "daily_cap";
        turnWaitMs_ = 0;
        if (limit) {
            turnWaitMs_ = retryAfterMs < kTurnWaitMinMs ? kTurnWaitMinMs : (retryAfterMs > kBusyCapMs ? kBusyCapMs : retryAfterMs);
            turnBlockedUntil_ = now + turnWaitMs_;
            CopyStr(turnLimitCode_, sizeof turnLimitCode_, code);
        }
        if (!r.error && r.status >= 200 && r.status < 300) {
            Json doc;
            if (!doc.Parse(r.body.data(), r.body.size())) { failTurn("The relay service sent an answer the game does not understand."); return; }
            const int root = doc.Root();
            TurnInfo t;
            t.ttlSec = uint32_t(doc.GetNum(root, "ttlSec", 3600));
            const int servers = doc.Get(root, "servers");
            for (int i = 0; i < doc.Size(servers); ++i) {
                const int sv = doc.At(servers, i);
                const std::string& user = doc.GetStr(sv, "username");
                if (user.empty()) continue;   // the plain STUN entry
                CopyStr(t.user, sizeof t.user, user);
                CopyStr(t.pass, sizeof t.pass, doc.GetStr(sv, "credential"));
                const int urls = doc.Get(sv, "urls");
                for (int u = 0; u < doc.Size(urls) && t.nUrls < 4; ++u) {
                    const std::string& url = doc.Str(doc.At(urls, u));
                    if (url.compare(0, 5, "turn:") != 0 || url.find("transport=tcp") != std::string::npos) continue;   // UDP only: the Vita has no TCP
                    CopyStr(t.urls[t.nUrls++], sizeof t.urls[0], url);
                }
                break;
            }
            if (t.nUrls == 0 || !t.user[0]) { failTurn(TurnSentence("turn_unavailable", "")); return; }
            turn_ = t;
            turnState_ = TURN_READY;
            NPN_LOG("relay credentials received (%d server address(es), valid %u s)\n", t.nUrls, unsigned(t.ttlSec));
            return;
        }
        if (again && !hard) {
            if (j.tries >= 2) {
                failTurn(r.error ? std::string("The relay service could not be reached.") : TurnSentence(code, message));
                return;
            }
            Retry(now, r.status == 429 ? retryAfterMs : 0);
            return;
        }
        failTurn(TurnSentence(code, message));
    }

    static bool ParseIp(const std::string& s, uint32_t* ip) {
        Addr a;
        if (!ParseAddr(s.c_str(), 0, &a)) return false;
        *ip = a.ip;
        return true;
    }

    void ApplyRoom(const Json& d, int v, uint64_t now) {
        if (!d.Has(d.Get(v, "state"))) return;   // not a room view
        if (d.Has(d.Get(v, "changed")) && !d.GetBool(v, "changed", true)) {
            peersQuiet_ = d.GetBool(v, "peersQuiet", false);   // (Galaxy service: some other member has not called for a while)
            const std::string& st = d.GetStr(v, "state");
            if (st == "closed" || st == "expired") CloseWith(st == "expired", "");
            return;
        }
        const std::string& state = d.GetStr(v, "state");
        const double slotNum = d.GetNum(v, "slot", 0);
        if (!(slotNum >= 0 && slotNum < kMaxSlots)) return;   // Galaxy: up to 4 members (Strikers: 2); NaN fails too
        const int slot = int(slotNum);
        SigRoom r;
        CopyStr(r.invite, sizeof r.invite, d.GetStr(v, "code"));
        CopyStr(code_, sizeof code_, d.GetStr(v, "code"));
        r.mySlot = slot;
        r.version = uint32_t(d.GetNum(v, "version", 0));
        {   // Galaxy host migration (DESIGN.md section 8): the room's host now and the move count; absent = slot 0, epoch 0 (a two-player view)
            // (the coordinator's contract names them host_slot / host_epoch; the smg service sends hostSlot / hostEpoch: both are read)
            const int hsNode = d.Has(d.Get(v, "host_slot")) ? d.Get(v, "host_slot") : d.Get(v, "hostSlot");
            const int heNode = d.Has(d.Get(v, "host_epoch")) ? d.Get(v, "host_epoch") : d.Get(v, "hostEpoch");
            const double hs = d.Num(hsNode, 0), he = d.Num(heNode, 0);
            r.hostSlot = (hs >= 0 && hs < kMaxSlots) ? int(hs) : room_.hostSlot;   // a bad value keeps the host we know
            r.hostEpoch = (he >= 0 && he < 4294967296.0) ? uint32_t(he) : room_.hostEpoch;
        }
        const int you = d.Get(v, "you");
        SigMember& me = r.members[slot];
        me.present = true;
        me.slot = uint8_t(slot);
        CopyStr(me.name, sizeof me.name, d.GetStr(you, "name"));
        ParseIp(d.GetStr(you, "vip"), &me.vip);
        me.cands = mine_;
        // Galaxy: the members of a room of up to 4 ("members": [{slot, pid, name, vip, link, endpoint|null, punchAtMs, steer:{relay, reason}}]), or
        // the two-player view's single "peer" (Strikers' shape, still accepted). Each entry is checked on its own: a bad slot, a repeated slot or our
        // own slot is skipped, and at most 8 entries are looked at.
        const int members = d.Get(v, "members");
        if (d.TypeOf(members) == Json::ARR) {
            const int count = d.Size(members) < 8 ? d.Size(members) : 8;
            for (int i = 0; i < count; ++i) {
                const int e = d.At(members, i);
                if (d.TypeOf(e) != Json::OBJ) continue;
                const double sn = d.GetNum(e, "slot", -1);
                if (!(sn >= 0 && sn < kMaxSlots)) continue;
                const int ps = int(sn);
                if (ps == slot || r.members[ps].present) continue;
                ParseMember(d, e, ps, &r.members[ps], now);
            }
        } else {
            const int peer = d.Get(v, "peer");
            if (d.TypeOf(peer) == Json::OBJ) {
                const double sn = d.GetNum(peer, "slot", double(1 - slot));
                const int ps = (sn >= 0 && sn < kMaxSlots) ? int(sn) : -1;
                if (ps >= 0 && ps != slot) ParseMember(d, peer, ps, &r.members[ps], now);
            }
        }
        const int sess = d.Get(v, "session");
        if (!d.Has(sess)) {
            memcpy(r.key, room_.key, 32);   // a view without a session (the room ended, an older answer) does not take the match key away
            r.cookie = room_.cookie;
        } else {
            uint8_t key[64];
            const int n = Base64Decode(d.GetStr(sess, "key"), key, sizeof key);
            bool nonzero = false;
            for (int i = 0; i < n && i < 32; ++i)
                if (key[i]) nonzero = true;
            if (n != 32 || !nonzero) {   // never an empty / short key: it would be a key everybody knows
                Fail("The online service sent an unusable key for this match. Try again.");
                return;
            }
            memcpy(r.key, key, 32);
            r.cookie = uint32_t(d.GetNum(sess, "cookie", 0));
            const int steer = d.Get(sess, "steer");
            r.steerRelay = d.GetBool(steer, "relay", false);
            CopyStr(r.steerReason, sizeof r.steerReason, d.GetStr(steer, "reason"));
            r.punchAtMs = LocalTime(d.GetNum(sess, "punchAtMs", 0), now);
        }
        room_ = r;
        peersQuiet_ = false;   // (a full view carries each member's link)
        status_ = IN_ROOM;
        const std::string& reason = d.GetStr(v, "closedReason");
        if (state == "closed" || state == "expired") CloseWith(state == "expired", reason);
    }

    // Galaxy: the server time `serverMs` on the transport's clock; 0 when none, unknown or far off (a time far ahead is not waited for)
    uint64_t LocalTime(double serverMs, uint64_t now) const {
        if (!(serverMs > 0 && serverMs < 9.0e15) || !haveOffset_) return 0;
        const int64_t local = int64_t(serverMs) - offsetMs_;
        return (local > int64_t(now) && local < int64_t(now) + 6000) ? uint64_t(local) : 0;
    }

    // Galaxy: one member of the room view (a members[] entry or the legacy "peer"), the fields of Strikers' "peer" plus pid, punchAtMs and steer.
    // svcfix2 item 8: as the host, tell the service which waiting members this console accepts (once per joiner; the policy may say no).
    void AcceptPending() {
        if (room_.mySlot < 0 || room_.mySlot != room_.hostSlot) { acceptAsked_ = 0; return; }
        for (int i = 0; i < kMaxSlots; ++i) {
            const SigMember& m = room_.members[i];
            if (!m.present || !m.pending) { acceptAsked_ &= ~(1u << i); continue; }
            if (i == room_.mySlot || (acceptAsked_ & (1u << i))) continue;
            acceptAsked_ |= 1u << i;
            if (acceptFn_ && !acceptFn_(acceptCtx_, i, m.name)) continue;
            Job j;
            j.kind = K_ACCEPT;
            j.method = "POST";
            j.path = "/v1/rooms/" + std::string(code_) + "/accept";
            char body[32];
            snprintf(body, sizeof body, "{\"slot\":%d}", i);
            j.body = body;
            queue_.push_back(j);
        }
    }
    void SetAcceptPolicy(AcceptFn fn, void* ctx) override { acceptFn_ = fn; acceptCtx_ = ctx; }

    void ParseMember(const Json& d, int e, int ps, SigMember* out, uint64_t now) {
        SigMember& pm = *out;
        pm.present = true;
        pm.slot = uint8_t(ps);
        const double pid = d.GetNum(e, "pid", 0);
        pm.pid = (pid > 0 && pid < 4294967296.0) ? uint32_t(pid) : 0;
        CopyStr(pm.name, sizeof pm.name, d.GetStr(e, "name"));
        pm.nameDup = d.GetBool(e, "nameDup", false);   // svcfix3: another member has the same name: "label" tells them apart
        FitLabel(pm.label, sizeof pm.label, d.GetStr(e, "label"));   // ("" from an older service: the name is shown)
        ParseIp(d.GetStr(e, "vip"), &pm.vip);
        SigPlayerCode(d.GetStr(e, "code").c_str(), pm.code);   // Galaxy (core2): exactly XXXX-XXXX of the code alphabet, else ""
        const std::string& link = d.GetStr(e, "link");
        pm.link = link == "lost" ? 2 : (link == "quiet" ? 1 : 0);
        pm.pending = !d.GetBool(e, "accepted", true);   // (an older service: always accepted)
        pm.punchAtMs = LocalTime(d.GetNum(e, "punchAtMs", 0), now);
        const int steer = d.Get(e, "steer");
        if (d.TypeOf(steer) == Json::OBJ) {
            pm.steerRelay = d.GetBool(steer, "relay", false);
            CopyStr(pm.steerReason, sizeof pm.steerReason, d.GetStr(steer, "reason"));
        }
        const int ep = d.Get(e, "endpoint");
        if (d.TypeOf(ep) == Json::OBJ) {
            ParseAddr(d.GetStr(ep, "public").c_str(), 0, &pm.cands.pub);
            ParseAddr(d.GetStr(ep, "local").c_str(), 0, &pm.cands.local);
            pm.cands.nat = uint8_t(NatFromName(d.GetStr(ep, "nat")));
            pm.stale = d.GetBool(ep, "stale", false);
            uint8_t nonce[32];
            const int n = Base64Decode(d.GetStr(ep, "nonce"), nonce, sizeof nonce);
            uint32_t gen = 1;
            for (int i = 0; i < n && i < 4; ++i) gen = gen * 257u + nonce[i];   // a republish with a new nonce is a new generation
            pm.cands.gen = gen ? gen : 1;
            ParseAddr(d.GetStr(ep, "relay").c_str(), 0, &pm.cands.relay);
            // Galaxy (netx): the last "srflx" candidate that is not the public address is probed too: we publish the UPnP-mapped port
            // last (after a second STUN answer, which is harmless to probe as well)
            {
                const int list = d.Get(ep, "candidates");
                for (int i = 0; i < d.Size(list) && i < 8; ++i) {
                    const int c = d.At(list, i);
                    Addr a;
                    if (d.GetStr(c, "type") != "srflx" || !ParseAddr(d.GetStr(c, "addr").c_str(), 0, &a) || a.ip == 0 || a.port == 0) continue;
                    if (a != pm.cands.pub) pm.cands.mapped = a;
                }
            }
            pm.cands.valid = !pm.cands.pub.IsZero() || !pm.cands.local.IsZero() || !pm.cands.relay.IsZero();
        }
    }

    void CloseWith(bool expired, const std::string& reason) {
        if (status_ == CLOSED || status_ == FAILED) return;
        CopyStr(error_, sizeof error_, ClosedSentence(reason, expired));
        NPN_LOG("room closed: %s\n", error_);
        status_ = CLOSED;
    }

    HttpSignalConfig cfg_;
    Status status_;
    uint64_t (*now_)(void*);
    void* nowCtx_;
    std::vector<std::string> bases_;
    size_t baseIdx_;
    std::deque<Job> queue_;
    Job pending_, curJob_;
    int cur_;
    uint64_t retryAt_, nextPoll_;
    bool infoDone_, started_, startedSent_, haveOffset_, peersQuiet_;
    int64_t offsetMs_;
    uint8_t nonce_[8];
    uint32_t nonceGen_;
    bool haveNonce_;
    int failStreak_;
    // svcfix2: the service's plan for the next poll (room answers carry poll:{holdSec,nextSec}); planValid_ false = an older service: the old rhythm
    bool planValid_;
    uint32_t planHoldSec_, planNextMs_;
    uint32_t directMask_;   // SetDirectLinks: the members this console heartbeats with now
    bool haveMask_, pollNow_;
    uint64_t lastForcedAt_;
    double pollScale_;   // svc3: the service asks the clients to stretch their polls (1 .. 1.4) while its day's budget runs low
    uint64_t curAt_;
    bool host_;
    SigCandidates mine_;
    SigRoom room_;
    std::string stunList_;
    bool relayOnly_, relayOnlySet_;
    std::string leaveSummary_;   // beta diagnostics: see SetLeaveSummary
    uint32_t acceptAsked_ = 0;          // svcfix2 item 8: slots whose accept was decided already
    AcceptFn acceptFn_ = nullptr;
    void* acceptCtx_ = nullptr;
    TurnStatus turnState_;
    TurnInfo turn_;
    char turnError_[200];
    uint32_t turnWaitMs_ = 0;           // svcfix3: how long a relay limit asks for patience (TurnWaitMs)
    uint64_t turnBlockedUntil_ = 0;
    char turnLimitCode_[24] = {};
    char error_[96], name_[16], code_[16];
    char errCode_[32] = {};   // Galaxy (fix3): the service's code behind the last refusal (ErrorCode)
};

}  // namespace

std::string TurnSentence(const std::string& code, const std::string& message) {
    // svcfix3: the three limits of the relay (a room, a player, the day) read the same: no relay right now, nothing the player did wrong
    if (code == "turn_budget" || code == "turn_player_limit" || code == "turn_room_limit")
        return "Relay play is not available right now. Try again later, or play from a network that allows direct connections.";
    if (code == "daily_cap") return "Daily limit reached. Relay play is available again tomorrow, or play from a network that allows direct connections.";
    if (code == "turn_unavailable") return "Relay play is not available on this online service.";
    if (code == "service_disabled") return message.empty() ? "Online play is switched off for now." : message;
    if (code == "turn_upstream") return "The relay service did not answer. Try again in a moment.";
    if (code == "turn_not_allowed" || code == "not_member") return "The relay can only be used inside a room with both players.";
    if (code == "unauthorized") return "Your online account was not accepted. Sign in again.";
    return "The relay could not be set up (" + (code.empty() ? std::string("no answer") : code) + ").";
}

Signal* CreateHttpSignal(const HttpSignalConfig& config) { return new HttpSignal(config); }

void ResetServiceBusy() { g_busyUntil = 0; }
void ResetServiceInfoCache() { g_info = InfoCache(); }

}  // namespace NpNet
