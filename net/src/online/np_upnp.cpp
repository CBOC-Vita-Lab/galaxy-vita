// Galaxy 3.0 (agent netx): UPnP IGD client. See np_upnp.h. Written from the UPnP / SSDP / SOAP standards for this project.
#include "np_upnp.h"

#include "../np_https_sys.h"   // nph::Sock*: non-blocking TCP on POSIX and sceNet

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace NpNet {

namespace {

const uint32_t kSsdpGroup = 0xEFFFFFFAu;   // 239.255.255.250
const uint16_t kSsdpPort = 1900;
const size_t kHeaderMax = 8192;             // an answer's header block longer than this is malformed
const int kMaxSsdpPerStep = 32;             // datagrams read per Step() (a flood cannot hold the helper thread)
const uint32_t kCollectMs = 300;            // after the first gateway answered, other answers are taken for this long
const uint32_t kRenewRetryMs = 60 * 1000;

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// First case-insensitive occurrence of `needle` in hay[0..len); SIZE_MAX if none.
size_t FindCI(const char* hay, size_t len, const char* needle, size_t from = 0) {
    const size_t n = strlen(needle);
    if (n == 0 || len < n) return SIZE_MAX;
    for (size_t i = from; i + n <= len; ++i) {
        size_t k = 0;
        while (k < n && Lower(hay[i + k]) == Lower(needle[k])) ++k;
        if (k == n) return i;
    }
    return SIZE_MAX;
}

bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Characters a value may have to be put into our own request text (header or XML): printable ASCII without blanks, quotes,
// angle brackets, backslash or ampersand.
bool SafeChar(char c) { return c > 0x20 && c < 0x7F && c != '"' && c != '<' && c != '>' && c != '\\' && c != '&' && c != '\''; }
bool SafeText(const char* s) {
    if (!s || !*s) return false;
    for (; *s; ++s)
        if (!SafeChar(*s)) return false;
    return true;
}

void CopyBounded(char* out, size_t cap, const char* in, size_t n) {
    if (!cap) return;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

// Copies [b, e) without surrounding blanks; false when it does not fit (or is empty).
bool CopyTrim(const char* b, const char* e, char* out, size_t cap) {
    while (b < e && IsBlank(*b)) ++b;
    while (e > b && IsBlank(e[-1])) --e;
    const size_t n = size_t(e - b);
    if (n == 0 || n >= cap) return false;
    memcpy(out, b, n);
    out[n] = 0;
    return true;
}

// The value of header `name` (at the start of a line, case-insensitive) in the header block text[0..len).
bool HeaderValue(const char* text, size_t len, const char* name, char* out, size_t cap) {
    const size_t nl = strlen(name);
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n') ++e;
        // line [i, e)
        if (e - i > nl && FindCI(text + i, nl, name) == 0 && text[i + nl] == ':') {
            const char* vb = text + i + nl + 1;
            const char* ve = text + e;
            return CopyTrim(vb, ve, out, cap);
        }
        i = e + 1;
    }
    return false;
}

const char* Ip4(uint32_t ip, char* out) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return out;
}

bool IsNameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; }

// The next element named `name` (any prefix) at or after `from`: inner text [*ib, *ie), position after its end tag in *after.
// Self-closing elements have an empty inner text. Case-sensitive (XML), bounded by len.
bool NextElement(const char* x, size_t len, const char* name, size_t from, size_t* ib, size_t* ie, size_t* after) {
    const size_t nl = strlen(name);
    for (size_t i = from; i < len; ++i) {
        if (x[i] != '<') continue;
        size_t p = i + 1;
        if (p < len && (x[p] == '/' || x[p] == '!' || x[p] == '?')) continue;
        // optional prefix "abc:"
        size_t q = p;
        while (q < len && IsNameChar(x[q]) && q - p < 32) ++q;
        if (q < len && x[q] == ':') p = q + 1;
        if (p + nl > len || memcmp(x + p, name, nl) != 0) continue;
        const size_t t = p + nl;
        if (t >= len) return false;
        if (!(x[t] == '>' || x[t] == '/' || IsBlank(x[t]))) continue;   // a longer name ("serviceList" for "service")
        size_t gt = t;
        while (gt < len && x[gt] != '>') ++gt;
        if (gt >= len) return false;
        if (x[gt - 1] == '/') {   // <name/>
            *ib = *ie = gt + 1;
            *after = gt + 1;
            return true;
        }
        // the end tag: "</" [prefix ":"] name [blanks] ">"
        for (size_t k = gt + 1; k + 1 < len; ++k) {
            if (x[k] != '<' || x[k + 1] != '/') continue;
            size_t a = k + 2, b = a;
            while (b < len && IsNameChar(x[b]) && b - a < 32) ++b;
            if (b < len && x[b] == ':') a = b + 1;
            if (a + nl > len || memcmp(x + a, name, nl) != 0) continue;
            size_t c = a + nl;
            while (c < len && IsBlank(x[c])) ++c;
            if (c < len && x[c] == '>') {
                *ib = gt + 1;
                *ie = k;
                *after = c + 1;
                return true;
            }
        }
        return false;   // never closed (a cut answer)
    }
    return false;
}

// Decodes the five predefined entities while copying; any other '&' sequence is refused.
bool CopyXmlText(const char* b, const char* e, char* out, size_t cap) {
    while (b < e && IsBlank(*b)) ++b;
    while (e > b && IsBlank(e[-1])) --e;
    size_t n = 0;
    while (b < e) {
        char c = *b++;
        if (c == '&') {
            static const struct { const char* name; char c; } kEnt[] = {{"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}};
            bool ok = false;
            for (const auto& en : kEnt) {
                const size_t l = strlen(en.name);
                if (size_t(e - b) >= l && memcmp(b, en.name, l) == 0) {
                    c = en.c;
                    b += l;
                    ok = true;
                    break;
                }
            }
            if (!ok) return false;
        }
        if (n + 1 >= cap) return false;
        out[n++] = c;
    }
    if (cap) out[n] = 0;
    return n > 0;
}

bool ParseUInt(const char* s, size_t n, uint32_t max, uint32_t* out) {
    if (n == 0 || n > 10) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + uint64_t(s[i] - '0');
    }
    if (v > max) return false;
    *out = uint32_t(v);
    return true;
}

}  // namespace

// ---- the HTTP exchange (one at a time) ------------------------------------------------------------------------------------------
struct Upnp::Http {
    nph::Sock s;
    int phase;           // 1 connecting, 2 sending, 3 receiving
    uint64_t deadline;
    size_t reqLen, sent, got;
    bool truncated;
    int status;
    size_t bodyOff, bodyLen;
    Http() : phase(0), deadline(0), reqLen(0), sent(0), got(0), truncated(false), status(0), bodyOff(0), bodyLen(0) {
        nph::SockInit(&s);
        s.pool = nph::SOCK_POOL_UPNP;   // netport31: its own socket budget (np_https_sys.h), apart from the HTTPS client's
    }
};

Upnp::Upnp()
    : state_(IDLE), udp_(0), nGw_(0), gwIdx_(0), nSvc_(0), svcIdx_(0), http_(0), kind_(REQ_DESC), buf_(0), req_(0), localIp_(0), extIp_(0),
      tryPort_(0), mappedPort_(0), triesLeft_(0), lease_(0), leaseRetried_(false), removeAfter_(false), mappingMayExist_(false), startMs_(0),
      firstAnswerMs_(0), nextSendMs_(0), renewAtMs_(0), leaseEndMs_(0), sends_(0), renewals_(0), answersSeen_(0) {
    error_[0] = 0;
    desc_[0] = 0;
    preCheck_ = false;
    removedOk_ = false;
    cleanup_ = 0;
    opPort_ = 0;
    memset(gw_, 0, sizeof gw_);
    memset(svc_, 0, sizeof svc_);
}

Upnp::~Upnp() {
    Abort();
    free(buf_);
    free(req_);
}

const char* Upnp::StateName(State s) {
    static const char* const k[] = {"idle", "searching", "describing", "asking-ip", "mapping", "checking", "mapped", "renewing", "removing", "removed", "failed"};
    return unsigned(s) <= unsigned(FAILED) ? k[s] : "?";
}

void Upnp::Log(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (cfg_.log) cfg_.log(cfg_.logCtx, line);
    else NpLogf("upnp", "%s\n", line);
}

void Upnp::Fail(const char* why) {
    CloseHttp();
    snprintf(error_, sizeof error_, "%s", why ? why : "failed");
    state_ = FAILED;
    Log("failed: %s", error_);
}

void Upnp::Abort() {
    CloseHttp();
    if (udp_) {
        udp_->Close();
        delete udp_;
        udp_ = 0;
    }
    if (state_ != FAILED && state_ != REMOVED) state_ = IDLE;
}

bool Upnp::Start(const UpnpConfig& cfg, uint64_t now, Udp* udp) {
    Abort();
    cfg_ = cfg;
    error_[0] = 0;
    nGw_ = gwIdx_ = nSvc_ = svcIdx_ = 0;
    extIp_ = 0;
    mappedPort_ = 0;
    renewals_ = answersSeen_ = sends_ = 0;
    removeAfter_ = mappingMayExist_ = leaseRetried_ = false;
    preCheck_ = false;
    removedOk_ = false;
    cleanup_ = 0;
    opPort_ = 0;
    state_ = IDLE;
    if (!cfg_.internalPort) {
        Fail("no port to forward");
        return false;
    }
    if (!cfg_.portTries) cfg_.portTries = 8;
    if (cfg_.portTries > 64) cfg_.portTries = 64;
    if (!cfg_.leaseSec) cfg_.leaseSec = 3600;
    if (!cfg_.searchMs) cfg_.searchMs = 3000;
    if (!cfg_.httpMs) cfg_.httpMs = 3000;
    // the description goes into our request: letters, digits, blanks, dots and dashes only
    {
        const char* d = cfg_.description ? cfg_.description : "Super Mario Galaxy";
        size_t n = 0;
        for (; *d && n + 1 < sizeof desc_; ++d) {
            const char c = *d;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-') desc_[n++] = c;
        }
        desc_[n] = 0;
        if (!n) snprintf(desc_, sizeof desc_, "Galaxy");
    }
    if (!buf_) buf_ = static_cast<char*>(malloc(kAnswerCap + 1));
    if (!req_) req_ = static_cast<char*>(malloc(kRequestCap));
    if (!buf_ || !req_) {
        Fail("out of memory");
        return false;
    }
    udp_ = udp ? udp : CreatePlatformUdp();
    if (!udp_ || !udp_->Open(0)) {
        Fail(udp_ && udp_->Error()[0] ? udp_->Error() : "no socket for the search");
        return false;
    }
    localIp_ = cfg_.internalIp ? cfg_.internalIp : udp_->LocalIp();
    if (!localIp_) {
        Fail("this console has no network address");
        return false;
    }
    tryPort_ = cfg_.externalPort ? cfg_.externalPort : cfg_.internalPort;
    triesLeft_ = cfg_.portTries;
    lease_ = cfg_.leaseSec;
    startMs_ = now;
    firstAnswerMs_ = 0;
    nextSendMs_ = now;
    state_ = SEARCHING;
    return true;
}

void Upnp::SendSearch(uint64_t now) {
    // three rounds (0, 0.6, 1.5 s): SSDP is UDP and the router may be busy; each round asks for both IGD versions and the service
    static const char* const kTargets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                           "urn:schemas-upnp-org:service:WANIPConnection:1"};
    for (size_t i = 0; i < sizeof kTargets / sizeof kTargets[0]; ++i) {
        char msg[256];
        const int n = snprintf(msg, sizeof msg,
                               "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", kTargets[i]);
        if (n <= 0 || size_t(n) >= sizeof msg) continue;
        if (!cfg_.ssdpTarget.IsZero()) {
            udp_->SendTo(cfg_.ssdpTarget, msg, size_t(n));
        } else {
            udp_->SendTo(Addr(kSsdpGroup, kSsdpPort), msg, size_t(n));
            // routers that drop multicast from wireless clients still answer a unicast M-SEARCH: the usual gateway address
            const uint32_t guess = (localIp_ & 0xFFFFFF00u) | 1u;
            if (guess != localIp_) udp_->SendTo(Addr(guess, kSsdpPort), msg, size_t(n));
        }
    }
    ++sends_;
    static const uint32_t kNext[] = {600, 900, 0};
    nextSendMs_ = sends_ < 3 ? now + kNext[sends_ - 1] : UINT64_MAX;
}

void Upnp::StepSearch(uint64_t now) {
    if (now >= nextSendMs_) SendSearch(now);
    // corefix 8: the default-route gateway, where the platform can tell (tests aim the search at a fake gateway: no guess then)
    const uint32_t gwIp = cfg_.gatewayIp ? cfg_.gatewayIp : (udp_ && cfg_.ssdpTarget.IsZero() ? udp_->GatewayIp() : 0);
    uint8_t dg[1500];
    for (int i = 0; i < kMaxSsdpPerStep; ++i) {
        Addr from;
        const int n = udp_->RecvFrom(&from, dg, sizeof dg - 1);
        if (n <= 0) break;
        ++answersSeen_;
        char loc[256];
        if (!ParseSsdpAnswer(reinterpret_cast<const char*>(dg), size_t(n), loc, sizeof loc)) continue;
        Gateway g;
        if (!ParseUrl(loc, &g.ip, &g.port, g.path, sizeof g.path)) continue;
        if (g.ip != from.ip) continue;   // a description elsewhere than the answer's sender: not followed
        bool dup = false;
        for (int k = 0; k < nGw_; ++k)
            if (gw_[k].ip == g.ip && gw_[k].port == g.port && strcmp(gw_[k].path, g.path) == 0) dup = true;
        if (dup) continue;
        if (nGw_ >= kMaxGateways) {
            // (corefix 8: four answers of other devices must not push the default gateway's out)
            if (!gwIp || g.ip != gwIp) continue;
            gw_[kMaxGateways - 1] = g;
        } else {
            gw_[nGw_++] = g;
        }
        if (!firstAnswerMs_) firstAnswerMs_ = now;
    }
    // corefix 8: when the default-route gateway is known, its answer is the one to use: a device on the LAN that answers the M-SEARCH
    // faster (or on purpose) is only tried after it. The collection goes on until the gateway has answered, up to the end of the search.
    int gwAt = -1;
    for (int k = 0; gwIp && k < nGw_; ++k)
        if (gw_[k].ip == gwIp) { gwAt = k; break; }
    const bool waitForGw = gwIp != 0 && gwAt < 0 && SinceMs(now, startMs_) < cfg_.searchMs;
    const bool collected = firstAnswerMs_ && !waitForGw && (SinceMs(now, firstAnswerMs_) >= kCollectMs || nGw_ >= kMaxGateways);
    if (collected) {
        if (gwAt > 0) {   // the gateway's answer first, the others after it in the order they came
            const Gateway g = gw_[gwAt];
            for (int k = gwAt; k > 0; --k) gw_[k] = gw_[k - 1];
            gw_[0] = g;
            Log("preferring the answer of the default gateway %u.%u.%u.%u", unsigned(gwIp >> 24), unsigned((gwIp >> 16) & 255), unsigned((gwIp >> 8) & 255), unsigned(gwIp & 255));
        } else if (gwIp && gwAt < 0) {
            Log("the default gateway did not answer: using the answer of another device on the LAN");
        }
        udp_->Close();   // (the search socket is not needed any more)
        gwIdx_ = 0;
        state_ = DESCRIBING;
        if (!StartGet(now)) NextGateway(now, error_);
        return;
    }
    if (SinceMs(now, startMs_) >= cfg_.searchMs) {
        char why[96];
        snprintf(why, sizeof why, "no UPnP router answered in %u ms (UPnP off on the router?)", unsigned(cfg_.searchMs));
        Fail(why);
    }
}

// ---- HTTP ------------------------------------------------------------------------------------------------------------------------
void Upnp::CloseHttp() {
    if (!http_) return;
    nph::SockClose(&http_->s);
    delete http_;
    http_ = 0;
}

static bool StartExchange(nph::Sock* s, uint32_t ip, uint16_t port, int* phase) {
    int sys = 0;
    static const volatile int kNoCancel = 0;
    const int rc = nph::SockConnect(s, ip, port, 0, &kNoCancel, &sys);   // 0 ms: start the connect, never wait here
    if (rc == 0) { *phase = 2; return true; }
    if (rc == nph::NET_TIMEOUT) { *phase = 1; return true; }   // in progress
    return false;
}

bool Upnp::StartGet(uint64_t now) {
    CloseHttp();
    if (gwIdx_ >= nGw_) return false;
    const Gateway& g = gw_[gwIdx_];
    char ip[16];
    const int n = snprintf(req_, kRequestCap, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nUser-Agent: GalaxyVita/3 UPnP/1.1\r\n\r\n",
                           g.path, Ip4(g.ip, ip), unsigned(g.port));
    if (n <= 0 || size_t(n) >= kRequestCap) {
        snprintf(error_, sizeof error_, "description path too long");
        return false;
    }
    http_ = new Http;
    http_->reqLen = size_t(n);
    http_->deadline = now + cfg_.httpMs;
    kind_ = REQ_DESC;
    if (!StartExchange(&http_->s, g.ip, g.port, &http_->phase)) {
        snprintf(error_, sizeof error_, "cannot connect to the router at %s:%u", ip, unsigned(g.port));
        CloseHttp();
        return false;
    }
    return true;
}

bool Upnp::StartSoap(Kind kind, uint64_t now) {
    CloseHttp();
    if (svcIdx_ >= nSvc_) return false;
    const Service& s = svc_[svcIdx_];
    char args[512];
    const char* action = "";
    char ipText[16], local[16];
    Ip4(localIp_, local);
    switch (kind) {
    case REQ_IP: action = "GetExternalIPAddress"; args[0] = 0; break;
    case REQ_ADD:
        action = "AddPortMapping";
        snprintf(args, sizeof args,
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                 "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                 "<NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>%u</NewLeaseDuration>",
                 unsigned(tryPort_), unsigned(cfg_.internalPort), local, desc_, unsigned(lease_));
        break;
    case REQ_CHECK:
        action = "GetSpecificPortMappingEntry";
        snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
                 unsigned(opPort_));
        break;
    case REQ_DEL:
        action = "DeletePortMapping";
        snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
                 unsigned(opPort_));
        break;
    default: return false;
    }
    char body[1100];
    const int bl = snprintf(body, sizeof body,
                            "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
                            action, s.type, args, action);
    if (bl <= 0 || size_t(bl) >= sizeof body) {
        snprintf(error_, sizeof error_, "request too long");
        return false;
    }
    const int n = snprintf(req_, kRequestCap,
                           "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n"
                           "Content-Length: %d\r\nConnection: close\r\nUser-Agent: GalaxyVita/3 UPnP/1.1\r\n\r\n%s",
                           s.path, Ip4(s.ip, ipText), unsigned(s.port), s.type, action, bl, body);
    if (n <= 0 || size_t(n) >= kRequestCap) {
        snprintf(error_, sizeof error_, "request too long");
        return false;
    }
    http_ = new Http;
    http_->reqLen = size_t(n);
    http_->deadline = now + cfg_.httpMs;
    kind_ = kind;
    if (kind == REQ_ADD) mappingMayExist_ = true;
    if (!StartExchange(&http_->s, s.ip, s.port, &http_->phase)) {
        snprintf(error_, sizeof error_, "cannot connect to the router's control address %s:%u", ipText, unsigned(s.port));
        CloseHttp();
        return false;
    }
    return true;
}

bool Upnp::StartCheck(uint64_t now, uint16_t port) {
    opPort_ = port;
    return StartSoap(REQ_CHECK, now);
}

bool Upnp::StartDelete(uint64_t now, uint16_t port) {
    opPort_ = port;
    return StartSoap(REQ_DEL, now);
}

// corefix 8: the mapping starts with a look at the router's table: an AddPortMapping for an external port that another device of the LAN
// holds is refused by a router that follows the standard (718), but many simply overwrite it, and the other device loses its forward.
// So the port is asked about first (GetSpecificPortMappingEntry) and, when it maps to somebody else, the next port is tried. A router that
// does not know the action (or answers anything but an entry) is treated as "free" and gets the AddPortMapping as before.
void Upnp::BeginMapping(uint64_t now) {
    if (cfg_.staleExternalPort && cleanup_ == 0) {
        cleanup_ = 1;   // an earlier run's permanent mapping (lease 0) that was never deleted: look at it, delete it if it is ours
        state_ = CHECKING;
        preCheck_ = false;
        if (!StartCheck(now, cfg_.staleExternalPort)) {
            cleanup_ = 0;
            BeginMapping(now);
        }
        return;
    }
    preCheck_ = true;
    state_ = CHECKING;
    if (!StartCheck(now, tryPort_)) {
        preCheck_ = false;
        state_ = MAPPING;
        if (!StartSoap(REQ_ADD, now)) Fail(error_);
    }
}

int Upnp::StepHttp(uint64_t now) {
    Http& h = *http_;
    if (now >= h.deadline) {
        if (h.phase == 3 && h.got > 0) snprintf(error_, sizeof error_, "the router answered too slowly (%u ms)", unsigned(cfg_.httpMs));
        else snprintf(error_, sizeof error_, "no answer from the router in %u ms", unsigned(cfg_.httpMs));
        return -1;
    }
    int sys = 0;
    if (h.phase == 1) {
        const int w = nph::SockWait(&h.s, true, 0);
        if (w < 0) { snprintf(error_, sizeof error_, "connection to the router failed"); return -1; }
        if (w == 0) return 0;
        h.phase = 2;   // (a refused connect shows at the first send)
    }
    if (h.phase == 2) {
        while (h.sent < h.reqLen) {
            const int n = nph::SockSend(&h.s, req_ + h.sent, h.reqLen - h.sent, &sys);
            if (n < 0) { snprintf(error_, sizeof error_, "the router refused the connection (%d)", sys); return -1; }
            if (n == 0) return 0;
            h.sent += size_t(n);
        }
        h.phase = 3;
        h.got = 0;
    }
    // receiving: read what is there (bounded), then see whether the answer is complete
    bool eof = false;
    for (int i = 0; i < 16; ++i) {
        if (h.got >= size_t(kAnswerCap)) {
            h.truncated = true;   // longer than we keep: parsed as far as it goes
            break;
        }
        const int n = nph::SockRecv(&h.s, buf_ + h.got, size_t(kAnswerCap) - h.got, &sys);
        if (n == -2) { eof = true; break; }
        if (n < 0) {
            if (h.got) { eof = true; break; }   // a reset after the answer (some routers close hard)
            snprintf(error_, sizeof error_, "the connection to the router broke (%d)", sys);
            return -1;
        }
        if (n == 0) break;
        h.got += size_t(n);
    }
    buf_[h.got] = 0;
    const int r = ParseHttp(buf_, h.got, eof, h.truncated, &h.status, &h.bodyOff, &h.bodyLen);
    if (r < 0) { snprintf(error_, sizeof error_, "the router's answer is not HTTP"); return -1; }
    if (r == 0) {
        if (eof || h.truncated) { snprintf(error_, sizeof error_, "the router's answer was cut short"); return -1; }
        return 0;
    }
    if (h.bodyOff + h.bodyLen <= size_t(kAnswerCap)) buf_[h.bodyOff + h.bodyLen] = 0;
    return 1;
}

void Upnp::NextGateway(uint64_t now, const char* why) {
    for (++gwIdx_; gwIdx_ < nGw_; ++gwIdx_) {
        state_ = DESCRIBING;
        if (StartGet(now)) return;
    }
    Fail(why && *why ? why : "the router offers no usable WAN connection service");
}

void Upnp::NextService(uint64_t now, const char* why) {
    for (++svcIdx_; svcIdx_ < nSvc_; ++svcIdx_) {
        state_ = ASKING_IP;
        if (StartSoap(REQ_IP, now)) return;
    }
    NextGateway(now, why);
}

void Upnp::OnAnswer(uint64_t now) {
    const char* body = buf_ + http_->bodyOff;
    const size_t blen = http_->bodyLen;
    const int status = http_->status;
    char ip[16], ip2[16];
    switch (state_) {
    case DESCRIBING: {
        const Gateway& g = gw_[gwIdx_];
        if (status != 200) {
            char why[64];
            snprintf(why, sizeof why, "the router's description answered HTTP %d", status);
            CloseHttp();
            NextGateway(now, why);
            return;
        }
        nSvc_ = ParseDescription(body, blen, g.ip, g.port, svc_, kMaxServices);
        CloseHttp();
        if (nSvc_ == 0) {
            NextGateway(now, "the router lists no WAN connection service (not an internet gateway)");
            return;
        }
        Log("gateway %s:%u, %d WAN service(s), first %s", Ip4(g.ip, ip), unsigned(g.port), nSvc_, svc_[0].type);
        svcIdx_ = 0;
        state_ = ASKING_IP;
        if (!StartSoap(REQ_IP, now)) NextService(now, error_);
        return;
    }
    case ASKING_IP: {
        char text[32];
        uint32_t a = 0;
        const bool ok = status == 200 && ElementText(body, blen, "NewExternalIPAddress", text, sizeof text) && ParseIp(text, &a) && a != 0;
        CloseHttp();
        if (!ok) {
            NextService(now, "the router does not know its public address (not connected to the internet?)");
            return;
        }
        extIp_ = a;
        BeginMapping(now);
        return;
    }
    case MAPPING:
    case RENEWING: {
        const bool renewing = state_ == RENEWING;
        if (status == 200) {
            CloseHttp();
            mappedPort_ = tryPort_;
            state_ = MAPPED;
            renewAtMs_ = lease_ ? now + uint64_t(lease_) * 500u : UINT64_MAX;
            leaseEndMs_ = lease_ ? now + uint64_t(lease_) * 1000u : UINT64_MAX;
            if (renewing) {
                ++renewals_;
                Log("renewed UDP %u (lease %u s)", unsigned(mappedPort_), unsigned(lease_));
            } else if (lease_ == 0) {
                Log("mapped UDP %s:%u -> %s:%u with NO lease (this router keeps only permanent mappings): it is deleted when the session ends, and at the next start if the game is killed first",
                    Ip4(extIp_, ip), unsigned(mappedPort_), Ip4(localIp_, ip2), unsigned(cfg_.internalPort));
            } else {
                Log("mapped UDP %s:%u -> %s:%u (lease %u s)%s", Ip4(extIp_, ip), unsigned(mappedPort_), Ip4(localIp_, ip2), unsigned(cfg_.internalPort),
                    unsigned(lease_), ExternalIsPrivate() ? " but the public address is private: another NAT is in front (not reachable)" : "");
            }
            if (removeAfter_) Remove(now);
            return;
        }
        const int code = SoapErrorCode(body, blen);
        CloseHttp();
        if (renewing) {
            if (now + kRenewRetryMs < leaseEndMs_) {
                Log("renewal refused (HTTP %d, UPnP error %d): retry in %u s", status, code, unsigned(kRenewRetryMs / 1000));
                state_ = MAPPED;
                renewAtMs_ = now + kRenewRetryMs;
            } else {
                Fail("the router no longer renews the port mapping");
            }
            return;
        }
        if ((code == 725 || code == 402) && lease_ != 0 && !leaseRetried_) {
            leaseRetried_ = true;   // routers that only keep permanent mappings (725), or reject a lease (some answer 402)
            lease_ = 0;
            if (!StartSoap(REQ_ADD, now)) Fail(error_);
            return;
        }
        if (code == 718) {   // ConflictInMappingEntry: maybe our own from an earlier run
            state_ = CHECKING;
            preCheck_ = false;
            if (!StartCheck(now, tryPort_)) Fail(error_);
            return;
        }
        if (code == 724 && tryPort_ != cfg_.internalPort) {
            Fail("the router needs the same external and internal port, and that port is taken");
            return;
        }
        char why[96];
        if (code >= 0) snprintf(why, sizeof why, "the router refused the port mapping (UPnP error %d)", code);
        else snprintf(why, sizeof why, "the router refused the port mapping (HTTP %d)", status);
        mappingMayExist_ = false;
        Fail(why);
        return;
    }
    case CHECKING: {
        char client[32], port[16];
        uint32_t cip = 0, cport = 0;
        const bool entry = status == 200 && ElementText(body, blen, "NewInternalClient", client, sizeof client) && ParseIp(client, &cip) &&
                           ElementText(body, blen, "NewInternalPort", port, sizeof port) && ParseUInt(port, strlen(port), 65535, &cport);
        const bool ours = entry && cip == localIp_ && cport == cfg_.internalPort;
        // an answer 200 that is not a readable entry (no client in it) is "something is mapped there": not free
        const bool taken = status == 200 && !ours;
        char left[16];
        uint32_t remaining = 0;
        if (status == 200 && ElementText(body, blen, "NewLeaseDuration", left, sizeof left)) ParseUInt(left, strlen(left), 0x7FFFFFFF, &remaining);
        CloseHttp();
        if (cleanup_ == 1) {
            // corefix 8: the permanent mapping an earlier run left: deleted if it is ours (and only then), then the normal mapping starts
            if (ours && !removeAfter_) {
                Log("deleting the permanent mapping of UDP %u an earlier run left behind", unsigned(cfg_.staleExternalPort));
                cleanup_ = 2;
                state_ = REMOVING;
                if (!StartDelete(now, cfg_.staleExternalPort)) {
                    cleanup_ = 3;
                    state_ = MAPPING;
                    BeginMapping(now);
                }
                return;
            }
            cleanup_ = 3;
            if (removeAfter_) {
                mappingMayExist_ = false;
                state_ = REMOVED;
                return;
            }
            BeginMapping(now);
            return;
        }
        if (removeAfter_ && !ours) {   // (Remove() came while we checked: no new mapping)
            mappingMayExist_ = false;
            state_ = REMOVED;
            return;
        }
        if (ours && !(preCheck_ && remaining == 0 && cfg_.leaseSec != 0 && lease_ != 0 && !removeAfter_)) {
            // an entry of an earlier session of this console: it is the mapping we want, renewed at half of what is left of its lease
            // (NewLeaseDuration = the remaining seconds; 0 or absent = permanent)
            mappedPort_ = tryPort_;
            state_ = MAPPED;
            preCheck_ = false;
            lease_ = remaining ? (remaining < cfg_.leaseSec ? cfg_.leaseSec : remaining) : 0;
            renewAtMs_ = remaining ? now + uint64_t(remaining) * 500u : UINT64_MAX;
            leaseEndMs_ = remaining ? now + uint64_t(remaining) * 1000u : UINT64_MAX;
            Log("mapped UDP %s:%u -> %s:%u (already ours from an earlier session)", Ip4(extIp_, ip), unsigned(mappedPort_), Ip4(localIp_, ip2),
                unsigned(cfg_.internalPort));
            if (removeAfter_) Remove(now);
            return;
        }
        if (preCheck_ && !taken) {
            // free (or ours, but permanent: asked again with a lease): ask for it
            preCheck_ = false;
            state_ = MAPPING;
            if (!StartSoap(REQ_ADD, now)) Fail(error_);
            return;
        }
        if (preCheck_ && ours) {   // (not reached: ours is handled above unless it is a permanent one we renew with a lease)
            preCheck_ = false;
            state_ = MAPPING;
            if (!StartSoap(REQ_ADD, now)) Fail(error_);
            return;
        }
        if (--triesLeft_ <= 0 || tryPort_ == 65535) {
            mappingMayExist_ = false;
            Fail("every external port tried is taken on the router");
            return;
        }
        if (preCheck_) Log("UDP %u is mapped to another device on the LAN: trying port %u", unsigned(tryPort_), unsigned(tryPort_ + 1));
        ++tryPort_;
        if (preCheck_) {
            if (!StartCheck(now, tryPort_)) {
                preCheck_ = false;
                state_ = MAPPING;
                if (!StartSoap(REQ_ADD, now)) Fail(error_);
            }
            return;
        }
        state_ = MAPPING;
        if (!StartSoap(REQ_ADD, now)) Fail(error_);
        return;
    }
    case REMOVING:
        CloseHttp();
        if (cleanup_ == 2) {   // corefix 8: the stale mapping is gone (or could not be deleted: the new request tells), the real mapping goes on
            Log("the stale mapping of UDP %u: %s", unsigned(cfg_.staleExternalPort), status == 200 ? "deleted" : "the router would not delete it");
            cleanup_ = 3;
            if (removeAfter_) {
                state_ = REMOVED;
                return;
            }
            BeginMapping(now);
            return;
        }
        removedOk_ = status == 200;
        Log(status == 200 ? "removed UDP %u" : "remove of UDP %u answered HTTP %d (it runs out with its lease)", unsigned(mappedPort_), status);
        mappedPort_ = 0;
        mappingMayExist_ = false;
        state_ = REMOVED;
        return;
    default:
        CloseHttp();
        return;
    }
}

void Upnp::Step(uint64_t now) {
    switch (state_) {
    case SEARCHING: StepSearch(now); return;
    case MAPPED:
        if (now >= renewAtMs_) {
            state_ = RENEWING;
            if (!StartSoap(REQ_ADD, now)) {
                state_ = MAPPED;
                renewAtMs_ = now + kRenewRetryMs;
            }
        }
        return;
    case DESCRIBING:
    case ASKING_IP:
    case MAPPING:
    case CHECKING:
    case RENEWING:
    case REMOVING: {
        if (!http_) return;
        const int r = StepHttp(now);
        if (r == 0) return;
        if (r > 0) {
            OnAnswer(now);
            return;
        }
        // the exchange failed
        char why[96];
        snprintf(why, sizeof why, "%s", error_);
        CloseHttp();
        if (state_ == DESCRIBING) NextGateway(now, why);
        else if (state_ == ASKING_IP) NextService(now, why);
        else if (state_ == RENEWING) {
            if (now + kRenewRetryMs < leaseEndMs_) {
                Log("renewal failed (%s): retry in %u s", why, unsigned(kRenewRetryMs / 1000));
                state_ = MAPPED;
                renewAtMs_ = now + kRenewRetryMs;
            } else {
                Fail(why);
            }
        } else if (state_ == REMOVING && cleanup_ == 2) {
            Log("the stale mapping of UDP %u could not be deleted (%s)", unsigned(cfg_.staleExternalPort), why);
            cleanup_ = 3;
            if (removeAfter_) state_ = REMOVED;
            else BeginMapping(now);
        } else if (state_ == CHECKING && (preCheck_ || cleanup_ == 1)) {
            // corefix 8: the look at the router's table failed (a router that does not answer GetSpecificPortMappingEntry): ask for the mapping
            Log("could not look at the router's table (%s): asking for the mapping directly", why);
            preCheck_ = false;
            if (cleanup_ == 1) cleanup_ = 3;
            if (removeAfter_) {
                state_ = REMOVED;
            } else {
                state_ = MAPPING;
                if (!StartSoap(REQ_ADD, now)) Fail(error_);
            }
        } else if (state_ == REMOVING) {
            Log("remove of UDP %u failed (%s): it runs out with its lease", unsigned(mappedPort_), why);
            state_ = REMOVED;
        } else {
            Fail(why);
        }
        return;
    }
    default: return;
    }
}

void Upnp::Remove(uint64_t now) {
    switch (state_) {
    case MAPPED:
        state_ = REMOVING;
        if (!StartDelete(now, mappedPort_ ? mappedPort_ : tryPort_)) state_ = REMOVED;
        return;
    case MAPPING:
    case CHECKING:
    case RENEWING:
        removeAfter_ = true;   // the request in flight may create the mapping: delete it once it answered
        return;
    case REMOVING:
        if (cleanup_ == 2) removeAfter_ = true;   // (corefix 8: deleting the stale mapping: no new mapping after it)
        return;
    case REMOVED:
        return;
    default:
        CloseHttp();
        if (udp_) udp_->Close();
        if (state_ != FAILED) state_ = REMOVED;
        return;
    }
}

// ---- parsers ---------------------------------------------------------------------------------------------------------------------
bool Upnp::ParseIp(const char* text, uint32_t* ip) {
    if (!text) return false;
    while (IsBlank(*text)) ++text;
    uint32_t v = 0;
    for (int part = 0; part < 4; ++part) {
        int digits = 0;
        uint32_t x = 0;
        while (*text >= '0' && *text <= '9') {
            x = x * 10 + uint32_t(*text - '0');
            if (++digits > 3 || x > 255) return false;
            ++text;
        }
        if (!digits) return false;
        v = (v << 8) | x;
        if (part < 3) {
            if (*text != '.') return false;
            ++text;
        }
    }
    while (IsBlank(*text)) ++text;
    if (*text) return false;
    *ip = v;
    return true;
}

bool Upnp::ParseSsdpAnswer(const char* data, size_t len, char* location, size_t cap) {
    if (!data || len < 12 || !location || !cap) return false;
    if (len > 2048) len = 2048;
    location[0] = 0;
    // "HTTP/1.1 200 OK" (a NOTIFY or another M-SEARCH on the group is not an answer)
    if (FindCI(data, 9, "HTTP/1.") != 0) return false;
    size_t i = 8;
    while (i < len && data[i] == ' ') ++i;
    if (i + 3 > len || memcmp(data + i, "200", 3) != 0) return false;
    // the header block ends at the blank line (or the datagram)
    size_t end = FindCI(data, len, "\r\n\r\n");
    if (end == SIZE_MAX) end = len;
    char st[160] = {0}, usn[200] = {0};
    HeaderValue(data, end, "ST", st, sizeof st);
    HeaderValue(data, end, "USN", usn, sizeof usn);
    static const char* const kKinds[] = {"InternetGatewayDevice", "WANIPConnection", "WANPPPConnection", "WANConnectionDevice"};
    bool gateway = false;
    for (const char* k : kKinds)
        if (FindCI(st, strlen(st), k) != SIZE_MAX || FindCI(usn, strlen(usn), k) != SIZE_MAX) gateway = true;
    if (!gateway) return false;
    char loc[256];
    if (!HeaderValue(data, end, "LOCATION", loc, sizeof loc)) return false;
    if (strlen(loc) >= cap) return false;
    snprintf(location, cap, "%s", loc);
    return true;
}

bool Upnp::ParseUrl(const char* url, uint32_t* ip, uint16_t* port, char* path, size_t pathCap) {
    if (!url || !ip || !port || !path || pathCap < 2) return false;
    if (FindCI(url, strlen(url), "http://") != 0) return false;
    const char* p = url + 7;
    const char* hostEnd = p;
    while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') ++hostEnd;
    const size_t hl = size_t(hostEnd - p);
    if (hl == 0 || hl > 15) return false;
    char host[16];
    memcpy(host, p, hl);
    host[hl] = 0;
    if (!ParseIp(host, ip) || *ip == 0 || *ip == 0xFFFFFFFFu) return false;
    p = hostEnd;
    uint32_t pv = 80;
    if (*p == ':') {
        ++p;
        const char* e = p;
        while (*e >= '0' && *e <= '9') ++e;
        if (!ParseUInt(p, size_t(e - p), 65535, &pv) || pv == 0) return false;
        p = e;
    }
    *port = uint16_t(pv);
    if (*p == 0) {
        snprintf(path, pathCap, "/");
        return true;
    }
    if (*p != '/') return false;
    const size_t pl = strlen(p);
    if (pl >= pathCap) return false;
    for (size_t i = 0; i < pl; ++i)
        if (!(SafeChar(p[i]) || p[i] == '&')) return false;   // ('&' only from a decoded "&amp;" of a query string)
    memcpy(path, p, pl + 1);
    return true;
}

bool Upnp::ElementText(const char* xml, size_t len, const char* name, char* out, size_t cap) {
    if (!xml || !out || !cap) return false;
    out[0] = 0;
    size_t ib, ie, after;
    if (!NextElement(xml, len, name, 0, &ib, &ie, &after)) return false;
    return CopyXmlText(xml + ib, xml + ie, out, cap);
}

int Upnp::SoapErrorCode(const char* body, size_t len) {
    char text[16];
    uint32_t v = 0;
    if (!ElementText(body, len, "errorCode", text, sizeof text) || !ParseUInt(text, strlen(text), 9999, &v)) return -1;
    return int(v);
}

int Upnp::ParseDescription(const char* xml, size_t len, uint32_t descIp, uint16_t descPort, Service* out, int max) {
    if (!xml || !out || max <= 0) return 0;
    // URLBase (UPnP 1.0, optional): relative control URLs are relative to it, else to the description's own address
    uint32_t baseIp = descIp;
    uint16_t basePort = descPort;
    {
        char base[256], basePath[kPathCap];
        uint32_t bip;
        uint16_t bport;
        if (ElementText(xml, len, "URLBase", base, sizeof base) && ParseUrl(base, &bip, &bport, basePath, sizeof basePath) && bip == descIp) {
            baseIp = bip;
            basePort = bport;
        }
    }
    int n = 0;
    size_t pos = 0;
    for (int blocks = 0; blocks < 32 && n < max; ++blocks) {
        size_t ib, ie, after;
        if (!NextElement(xml, len, "service", pos, &ib, &ie, &after)) break;
        pos = after;
        const char* blk = xml + ib;
        const size_t bl = ie - ib;
        Service s;
        memset(&s, 0, sizeof s);
        if (!ElementText(blk, bl, "serviceType", s.type, sizeof s.type)) continue;
        if (!SafeText(s.type)) continue;
        if (FindCI(s.type, strlen(s.type), ":service:WANIPConnection:") == SIZE_MAX &&
            FindCI(s.type, strlen(s.type), ":service:WANPPPConnection:") == SIZE_MAX)
            continue;
        char ctl[256];
        if (!ElementText(blk, bl, "controlURL", ctl, sizeof ctl)) continue;
        if (FindCI(ctl, strlen(ctl), "http://") == 0) {
            if (!ParseUrl(ctl, &s.ip, &s.port, s.path, sizeof s.path) || s.ip != descIp) continue;   // never another host
        } else {
            char abs[300];
            snprintf(abs, sizeof abs, "http://%u.%u.%u.%u:%u%s%s", baseIp >> 24, (baseIp >> 16) & 255, (baseIp >> 8) & 255, baseIp & 255,
                     unsigned(basePort), ctl[0] == '/' ? "" : "/", ctl);
            if (!ParseUrl(abs, &s.ip, &s.port, s.path, sizeof s.path)) continue;
        }
        bool dup = false;
        for (int k = 0; k < n; ++k)
            if (strcmp(out[k].type, s.type) == 0 && strcmp(out[k].path, s.path) == 0) dup = true;
        if (!dup) out[n++] = s;
    }
    return n;
}

namespace {
// Chunked transfer coding (RFC 7230 section 4.1) over in[0..n): with out == null only checks; returns the decoded length, or -1
// malformed, -2 incomplete. out may be `in` (the copy only ever moves bytes towards the start).
long Dechunk(char* in, size_t n, char* out, size_t cap) {
    size_t r = 0, w = 0;
    for (int chunks = 0; chunks < 100000; ++chunks) {
        size_t v = 0;
        int digits = 0;
        while (r < n) {
            const char c = in[r];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            if (++digits > 7) return -1;
            v = v * 16 + size_t(d);
            ++r;
        }
        if (r >= n) return -2;
        if (!digits) return -1;
        // chunk extensions up to the line end
        size_t e = r;
        while (e < n && in[e] != '\n' && e - r < 256) ++e;
        if (e >= n) return -2;
        if (in[e] != '\n') return -1;
        r = e + 1;
        if (v == 0) return long(w);   // (trailers are not needed)
        if (n - r < v + 2) return -2;
        if (out) {
            if (w + v > cap) return -1;
            memmove(out + w, in + r, v);
        }
        w += v;
        r += v;
        if (in[r] == '\r') ++r;
        if (r >= n) return -2;
        if (in[r] != '\n') return -1;
        ++r;
    }
    return -1;
}
}  // namespace

int Upnp::ParseHttp(char* buf, size_t len, bool eof, bool truncated, int* status, size_t* bodyOff, size_t* bodyLen) {
    if (len < 12) return eof || truncated ? -1 : 0;
    if (FindCI(buf, 7, "HTTP/1.") != 0) return -1;
    size_t i = 8;
    while (i < len && buf[i] == ' ') ++i;
    uint32_t st = 0;
    if (i + 3 > len || !ParseUInt(buf + i, 3, 999, &st)) return -1;
    size_t hdrEnd = FindCI(buf, len < kHeaderMax ? len : kHeaderMax, "\r\n\r\n");
    size_t sep = 4;
    if (hdrEnd == SIZE_MAX) {
        hdrEnd = FindCI(buf, len < kHeaderMax ? len : kHeaderMax, "\n\n");   // (bare LF answers)
        sep = 2;
    }
    if (hdrEnd == SIZE_MAX) return (len >= kHeaderMax || eof || truncated) ? -1 : 0;
    const size_t body = hdrEnd + sep;
    *status = int(st);
    *bodyOff = body;
    char v[64];
    if (HeaderValue(buf, hdrEnd, "Transfer-Encoding", v, sizeof v) && FindCI(v, strlen(v), "chunked") != SIZE_MAX) {
        const long r = Dechunk(buf + body, len - body, 0, 0);
        if (r == -1) return -1;
        if (r == -2) return 0;   // (more to come; with eof the caller reports a cut answer)
        const long w = Dechunk(buf + body, len - body, buf + body, len - body);
        if (w < 0) return -1;
        *bodyLen = size_t(w);
        return 1;
    }
    if (HeaderValue(buf, hdrEnd, "Content-Length", v, sizeof v)) {
        uint32_t cl = 0;
        if (!ParseUInt(v, strlen(v), 0x7FFFFFFF, &cl)) return -1;
        if (len - body >= cl) {
            *bodyLen = cl;
            return 1;
        }
        if (truncated) {   // longer than we keep: what we have
            *bodyLen = len - body;
            return 1;
        }
        return 0;   // (more to come; with eof the caller reports a cut answer)
    }
    if (!(eof || truncated)) return 0;   // the body ends when the router closes
    *bodyLen = len - body;
    return 1;
}

}  // namespace NpNet
