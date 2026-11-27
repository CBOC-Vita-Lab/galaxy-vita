// tls_chain_test: does the game's HTTPS client accept a certificate chain? Offline, with the client's own code (no copy of its roots,
// profile or rules): net/src/np_https_tls_mbedtls.cpp built with NP_HTTPS_TEST_HOOKS against the same Mbed TLS archives the game links.
// Run by tests/mp/tls_chain_test.sh (which also makes the synthetic chains and asserts each verdict). Never contacts any server.
//
//   tls_chain_test verify CHAIN.pem HOST DATE [--extra-root ROOTS.pem]
//       CHAIN.pem: the chain a server sends, leaf first (openssl s_client -showcerts output is fine: only the PEM blocks are read).
//       DATE: YYYY-MM-DD (noon UTC) or YYYY-MM-DDTHH:MM:SSZ, the console clock to check the dates against.
//       --extra-root: test-only anchors added after the game's bundled roots (np_https_roots.h). Without it, the game's anchors only.
//       Prints ACCEPT or REFUSE with the library's flags; exit 0 accept, 1 refuse, 2 error.
//   tls_chain_test handshake PORT HOST DATE [--tls12|--tls13] [--extra-root ROOTS.pem]
//       The same verdict from a real handshake (tls::Open / Handshake, the game's code path) with a local test server on 127.0.0.1:PORT
//       (tests/mp/tls_chain_test.sh starts `openssl s_server` there with a synthetic chain). Proves verify == handshake.
//   tls_chain_test roots
//       The game's anchors (name, valid until, SHA-256), from np_https_roots.h.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "np_https.h"
#include "np_https_roots.h"
#include "np_https_sys.h"
#include "np_https_tls.h"

#include "mbedtls/x509.h"

using namespace nph;

namespace {

int64_t g_now;
int64_t FakeClock() { return g_now; }

bool ReadFile(const char* path, std::string* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

// Only the PEM certificate blocks, in order (s_client output has other lines around them).
std::string PemBlocks(const std::string& text) {
    std::string out;
    size_t at = 0;
    for (;;) {
        const size_t b = text.find("-----BEGIN CERTIFICATE-----", at);
        if (b == std::string::npos) break;
        const size_t e = text.find("-----END CERTIFICATE-----", b);
        if (e == std::string::npos) break;
        out.append(text, b, e + 25 - b);
        out += "\n";
        at = e + 25;
    }
    return out;
}

bool ParseDate(const char* s, int64_t* out) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, d, h = 12, mi = 0, se = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &se) != 6 && sscanf(s, "%d-%d-%d", &y, &mo, &d) != 3) return false;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    *out = (int64_t)timegm(&tm);
    return *out > 0;
}

void FlagNames(uint32_t f, char* out, size_t cap) {
    static const struct { uint32_t bit; const char* name; } kNames[] = {
        {MBEDTLS_X509_BADCERT_EXPIRED, "EXPIRED"},           {MBEDTLS_X509_BADCERT_REVOKED, "REVOKED"},
        {MBEDTLS_X509_BADCERT_CN_MISMATCH, "CN_MISMATCH"},   {MBEDTLS_X509_BADCERT_NOT_TRUSTED, "NOT_TRUSTED"},
        {MBEDTLS_X509_BADCRL_NOT_TRUSTED, "CRL_NOT_TRUSTED"}, {MBEDTLS_X509_BADCRL_EXPIRED, "CRL_EXPIRED"},
        {MBEDTLS_X509_BADCERT_MISSING, "MISSING"},           {MBEDTLS_X509_BADCERT_SKIP_VERIFY, "SKIP_VERIFY"},
        {MBEDTLS_X509_BADCERT_OTHER, "OTHER"},               {MBEDTLS_X509_BADCERT_FUTURE, "FUTURE"},
        {MBEDTLS_X509_BADCRL_FUTURE, "CRL_FUTURE"},          {MBEDTLS_X509_BADCERT_KEY_USAGE, "KEY_USAGE"},
        {MBEDTLS_X509_BADCERT_EXT_KEY_USAGE, "EXT_KEY_USAGE"}, {MBEDTLS_X509_BADCERT_NS_CERT_TYPE, "NS_CERT_TYPE"},
        {MBEDTLS_X509_BADCERT_BAD_MD, "BAD_MD"},             {MBEDTLS_X509_BADCERT_BAD_PK, "BAD_PK"},
        {MBEDTLS_X509_BADCERT_BAD_KEY, "BAD_KEY"},           {MBEDTLS_X509_BADCRL_BAD_MD, "CRL_BAD_MD"},
        {MBEDTLS_X509_BADCRL_BAD_PK, "CRL_BAD_PK"},          {MBEDTLS_X509_BADCRL_BAD_KEY, "CRL_BAD_KEY"},
    };
    out[0] = 0;
    size_t n = 0;
    for (const auto& k : kNames)
        if ((f & k.bit) && n < cap) n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? "|" : "", k.name);
}

void NpNames(uint32_t f, char* out, size_t cap) {
    static const struct { uint32_t bit; const char* name; } kNames[] = {
        {NP_HTTPS_CERT_EXPIRED, "EXPIRED"}, {NP_HTTPS_CERT_NOT_YET_VALID, "NOT_YET_VALID"}, {NP_HTTPS_CERT_UNTRUSTED, "UNTRUSTED"},
        {NP_HTTPS_CERT_HOST_MISMATCH, "HOST_MISMATCH"}, {NP_HTTPS_CERT_BAD_SIGNATURE, "BAD_SIGNATURE"}, {NP_HTTPS_CERT_OTHER, "OTHER"}};
    out[0] = 0;
    size_t n = 0;
    for (const auto& k : kNames)
        if ((f & k.bit) && n < cap) n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? "|" : "", k.name);
}

void PrintPath(const tls::Details& d) {
    for (int i = 0; i < d.chain_len && i < tls::MAX_CHAIN; i++)
        printf("  path[%d] subject \"%s\" issuer \"%s\" valid %s .. %s verdict 0x%x\n", i, d.chain[i].subject, d.chain[i].issuer,
               d.chain[i].not_before, d.chain[i].not_after, (unsigned)d.chain[i].raw_flags);
}

// One line the shell script greps: "ACCEPT" or "REFUSE flags=0x... (NAMES) client=NAMES".
int Verdict(const char* what, bool accepted, uint32_t raw) {
    if (accepted) { printf("%s: ACCEPT\n", what); return 0; }
    char names[256], np[128];
    FlagNames(raw, names, sizeof names);
    NpNames(tls::MapFlags(raw), np, sizeof np);
    printf("%s: REFUSE flags=0x%x (%s) client=%s\n", what, (unsigned)raw, names[0] ? names : "-", np[0] ? np : "OTHER");
    return 1;
}

bool InitTls(const char* extra_path) {
    static std::string extra;
    if (extra_path) {
        std::string text;
        if (!ReadFile(extra_path, &text)) { fprintf(stderr, "cannot read %s\n", extra_path); return false; }
        extra = PemBlocks(text);
        tls::SetExtraTestRoots(extra.c_str(), extra.size() + 1);
    }
    char why[96] = {};
    int roots = 0;
    if (!tls::Init(why, sizeof why, &roots)) { fprintf(stderr, "tls init failed: %s\n", why); return false; }
    printf("anchors: %d bundled (np_https_roots.h, NP_HTTPS_ROOTS_DAY %lld)%s%s, %s\n", NP_HTTPS_ROOT_COUNT, (long long)NP_HTTPS_ROOTS_DAY,
           extra_path ? " + test anchors from " : "", extra_path ? extra_path : "", tls::BackendName());
    if (!extra_path && roots != NP_HTTPS_ROOT_COUNT) { fprintf(stderr, "expected %d anchors, have %d\n", NP_HTTPS_ROOT_COUNT, roots); return false; }
    return true;
}

// ---- handshake mode: a blocking socket under the game's Io ------------------------------------------------------------------
int SendFn(void* ctx, const uint8_t* b, size_t n) {
    const ssize_t r = send(*(int*)ctx, b, n, MSG_NOSIGNAL);
    return r > 0 ? (int)r : -1;
}
int RecvFn(void* ctx, uint8_t* b, size_t n) {
    const ssize_t r = recv(*(int*)ctx, b, n, 0);
    if (r > 0) return (int)r;
    return r == 0 ? -2 : -1;
}

int Handshake(int port, const char* host, int* fd_out, tls::Conn** conn_out) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(0x7F000001u);   // 127.0.0.1 only
    if (connect(fd, (sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
    *fd_out = fd;
    static int s_fd;
    s_fd = fd;
    tls::Io io = {SendFn, RecvFn, &s_fd};
    tls::Conn* c = tls::Open(host, io);
    *conn_out = c;
    if (!c) return -1;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "roots")) {
        for (int i = 0; i < NP_HTTPS_ROOT_COUNT; i++) {
            const NpHttpsRootEntry& r = np_https_roots[i];
            char until[24];
            const time_t t = (time_t)r.not_after;
            strftime(until, sizeof until, "%Y-%m-%d", gmtime(&t));
            printf("%-42s until %s sha256 ", r.name, until);
            for (int k = 0; k < 32; k++) printf("%02x", r.sha256[k]);
            printf("\n");
        }
        return 0;
    }
    const char* extra = nullptr;
    int want = 0;   // 0 any, 12 / 13
    int nargs = 0;
    const char* args[8] = {};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--extra-root") && i + 1 < argc) extra = argv[++i];
        else if (!strcmp(argv[i], "--tls12")) want = 12;
        else if (!strcmp(argv[i], "--tls13")) want = 13;
        else if (nargs < 8) args[nargs++] = argv[i];
    }
    if (nargs != 4 || (strcmp(args[0], "verify") && strcmp(args[0], "handshake"))) {
        fprintf(stderr, "usage: %s verify CHAIN.pem HOST DATE [--extra-root R.pem]\n       %s handshake PORT HOST DATE [--tls12|--tls13] [--extra-root R.pem]\n"
                        "       %s roots\n", argv[0], argv[0], argv[0]);
        return 2;
    }
    if (!ParseDate(args[3], &g_now)) { fprintf(stderr, "bad date %s\n", args[3]); return 2; }
    g_wall_override = FakeClock;   // the certificate dates are checked against this, as the client checks them against the Vita's RTC
    if (!InitTls(extra)) return 2;
    const char* host = args[2];
    if (!strcmp(args[0], "verify")) {
        std::string text;
        if (!ReadFile(args[1], &text)) { fprintf(stderr, "cannot read %s\n", args[1]); return 2; }
        const std::string pem = PemBlocks(text);
        int count = 0;
        for (size_t at = 0; (at = pem.find("BEGIN CERTIFICATE", at)) != std::string::npos; at++) count++;
        printf("chain: %s, %d certificate(s) as served; host %s; clock %s\n", args[1], count, host, args[3]);
        uint32_t raw = 0;
        tls::Details d;
        const tls::Result r = tls::VerifyChain(pem.c_str(), pem.size() + 1, host, &raw, &d);
        if (r == tls::T_FAIL) { printf("verify: ERROR (the chain did not parse)\n"); return 2; }
        PrintPath(d);
        return Verdict("verify", r == tls::T_OK, raw);
    }
    // handshake
    const int port = atoi(args[1]);
    if (port <= 0 || port > 65535) return 2;
    int fd = -1;
    tls::Conn* c = nullptr;
    if (Handshake(port, host, &fd, &c) != 0) { printf("handshake: ERROR (no local test server on port %d)\n", port); if (fd >= 0) close(fd); return 2; }
    int code = 0;
    tls::Result r;
    for (;;) {
        r = tls::Handshake(c, &code);
        if (r == tls::T_WANT_READ || r == tls::T_WANT_WRITE) continue;   // (blocking socket: only a time-out lands here twice)
        break;
    }
    tls::Details d;
    tls::GetDetails(c, &d);
    uint32_t raw = 0;
    tls::CertFlags(c, &raw);
    int rc;
    if (r == tls::T_OK) {
        if (want && d.version != (want == 13 ? 0x0304u : 0x0303u)) { printf("handshake: ERROR (negotiated 0x%x)\n", (unsigned)d.version); rc = 2; }
        else {
            // one request through the game's Read / Write: in TLS 1.3 the server's NewSessionTicket messages arrive here first (this
            // build has no ticket support and must skip them, as against Cloudflare)
            static const char kReq[] = "GET / HTTP/1.0\r\n\r\n";
            size_t done = 0;
            std::string reply;
            tls::Result w = tls::Write(c, (const uint8_t*)kReq, sizeof kReq - 1, &done, &code);
            for (int i = 0; w == tls::T_OK && i < 200; i++) {
                uint8_t buf[2048];
                size_t got = 0;
                const tls::Result rr = tls::Read(c, buf, sizeof buf, &got, &code);
                if (rr == tls::T_OK) { reply.append((const char*)buf, got); continue; }
                if (rr == tls::T_WANT_READ) continue;
                break;   // closed / EOF
            }
            if (w != tls::T_OK || reply.compare(0, 12, "HTTP/1.0 200") != 0) {
                printf("handshake: ERROR (the request after the handshake failed: write %d, %zu bytes back, code -0x%04x)\n", (int)w, reply.size(),
                       (unsigned)-code);
                rc = 2;
            } else {
                printf("handshake: TLS %s %s, request answered (%zu bytes)\n", d.version == 0x0304 ? "1.3" : "1.2", d.cipher, reply.size());
                rc = Verdict("handshake", true, 0);
            }
        }
        tls::CloseNotify(c);
    } else if (r == tls::T_CERT) {
        PrintPath(d);
        rc = Verdict("handshake", false, raw);
    } else {
        printf("handshake: ERROR (result %d, library code -0x%04x)\n", (int)r, (unsigned)-code);
        rc = 2;
    }
    tls::Free(c);
    close(fd);
    return rc;
}
