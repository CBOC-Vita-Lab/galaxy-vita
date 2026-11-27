// Galaxy 3.0 multiplayer (beta diagnostics): host tests of the problem report's body: masking (IPv4, IPv6, ip:port, e-mail, 32+ hex), the
// 48,000-byte cap with the newest lines kept, JSON escaping, the summary and check fields, the body limit. Build and run: tests/mp/diag_build.sh
#include "mp_report.h"
#include "port/mp_diag.h"
#include "port/mp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int sFail = 0;
#define CHECK(c) do { if (!(c)) { ++sFail; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define HAS(s, sub) (std::string(s).find(sub) != std::string::npos)

// A small JSON checker (strings with escapes, objects, arrays, numbers, true / false / null): the body must parse.
struct Json {
    const char* p;
    void ws() { while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p; }
    bool str() {
        if (*p != '"') return false;
        ++p;
        while (*p && *p != '"') {
            const unsigned char c = (unsigned char)*p;
            if (c < 0x20) return false;
            if (c == '\\') {
                ++p;
                if (*p == 'u') { for (int i = 0; i < 4; i++) { ++p; if (!isxdigit((unsigned char)*p)) return false; } }
                else if (!strchr("\"\\/bfnrt", *p) || !*p) return false;
            }
            ++p;
        }
        if (*p != '"') return false;
        ++p;
        return true;
    }
    bool val() {
        ws();
        if (*p == '"') return str();
        if (*p == '{') {
            ++p; ws();
            if (*p == '}') { ++p; return true; }
            for (;;) { ws(); if (!str()) return false; ws(); if (*p++ != ':') return false; if (!val()) return false; ws(); if (*p == ',') { ++p; continue; } if (*p == '}') { ++p; return true; } return false; }
        }
        if (*p == '[') {
            ++p; ws();
            if (*p == ']') { ++p; return true; }
            for (;;) { if (!val()) return false; ws(); if (*p == ',') { ++p; continue; } if (*p == ']') { ++p; return true; } return false; }
        }
        if (!strncmp(p, "true", 4)) { p += 4; return true; }
        if (!strncmp(p, "false", 5)) { p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char* e; strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    }
    static bool valid(const std::string& s) { Json j{s.c_str()}; if (!j.val()) return false; j.ws(); return *j.p == 0; }
};

static std::string Payload(const std::string& cur, size_t cap = 200000) {
    std::vector<char> out(cap);
    const size_t n = mp_report_log_payload(nullptr, 0, false, cur.data(), cur.size(), false, out.data(), cap);
    return std::string(out.data(), n);
}

static void TestMasking() {
    const std::string log =
        "[mp] net: peer 192.168.1.23 ok\n"
        "[mp] net: peer 10.0.0.7:5555 ok\n"
        "[mp] net: v6 2001:db8::1 and [2001:db8:1:2::9]:4000 and fe80::1ff:fe23:4567:890a\n"
        "[mp] net: mail me at someone.name+tag@example.com now\n"
        "[mp] net: key 0123456789abcdef0123456789abcdef01234567 end\n"
        "[mp] net: keep 3 slots 12 ms\n"
        "[mp] secret: not on the allow-list 1234\n";
    const std::string p = Payload(log);
    CHECK(!HAS(p, "192.168.1.23"));
    CHECK(!HAS(p, "10.0.0.7"));
    CHECK(!HAS(p, "2001:db8"));
    CHECK(!HAS(p, "fe80::"));
    CHECK(!HAS(p, "someone.name"));
    CHECK(!HAS(p, "example.com"));
    CHECK(!HAS(p, "0123456789abcdef0123456789abcdef"));
    CHECK(HAS(p, "keep 3 slots 12 ms"));
    CHECK(!HAS(p, "not on the allow-list"));
    CHECK(HAS(p, "<email>"));
}

static void TestCap() {
    std::string log;
    for (int i = 0; i < 5000; i++) { char b[96]; snprintf(b, sizeof b, "[mp] net: line %05d padding padding padding\n", i); log += b; }
    const std::string p = Payload(log, MP_DIAG_LOG_MAX + 1);
    CHECK(p.size() <= (size_t)MP_DIAG_LOG_MAX);
    CHECK(p.size() > (size_t)MP_DIAG_LOG_MAX - 200);   // (filled, not cut short)
    CHECK(HAS(p, "line 04999"));                          // the newest kept
    CHECK(!HAS(p, "line 00000"));                         // the oldest dropped
    CHECK(p.empty() || p[0] == '[');                      // whole lines only
    // the previous log comes first, the current one after
    const std::string a = "[mp] net: PREVIOUS\n", b = "[mp] net: CURRENT\n";
    std::vector<char> out(4096);
    const size_t n = mp_report_log_payload(a.data(), a.size(), false, b.data(), b.size(), false, out.data(), out.size());
    const std::string q(out.data(), n);
    CHECK(q.find("PREVIOUS") < q.find("CURRENT"));
}

static void TestEscape() {
    const char in[] = "a\"b\\c\n\r\t\x01 \xC3\xA9 \xF0\x9F\x98\x80 bad:\xFF end";
    char out[256];
    const size_t n = mp_diag_json_escape(in, sizeof in - 1, out, sizeof out);
    CHECK(n == strlen(out));
    const std::string j = std::string("\"") + out + "\"";
    CHECK(Json::valid(j));
    CHECK(HAS(out, "\\\"") && HAS(out, "\\\\") && HAS(out, "\\n") && HAS(out, "\\r") && HAS(out, "\\t") && HAS(out, "\\u0001"));
    CHECK(HAS(out, "\xC3\xA9"));          // valid UTF-8 kept
    CHECK(!HAS(out, "\xFF"));             // an invalid byte never reaches the JSON
    // a too-small buffer: terminated, and the needed length is told
    char small[8];
    const size_t need = mp_diag_json_escape(in, sizeof in - 1, small, sizeof small);
    CHECK(need > sizeof small - 1);
    CHECK(strlen(small) < sizeof small);
}

static MpDiagSummary Sum() {
    MpDiagSummary s;
    memset(&s, 0, sizeof s);
    s.valid = true;
    snprintf(s.transport, sizeof s.transport, "relay");
    s.durS = 321; s.members = 3;
    snprintf(s.ended, sizeof s.ended, "error:sync");
    s.rxRate = 58.5f; s.gapMeanMs = 17.25f; s.gapMaxMs = 420.0f; s.bufMeanMs = 66.0f; s.bufMaxMs = 140.0f;
    s.extrap = 5; s.hold = 6; s.snap = 7; s.late = 8; s.stall100 = 9; s.txFail = 2; s.relayUsed = true;
    snprintf(s.app, sizeof s.app, "3.0.0");
    return s;
}

static void TestSummary() {
    char out[1024];
    const MpDiagSummary s = Sum();
    const size_t n = mp_diag_summary_json(&s, out, sizeof out);
    CHECK(n == strlen(out));
    CHECK(Json::valid(out));
    const char* want[] = {"\"transport\":\"relay\"", "\"dur_s\":321", "\"members\":3", "\"ended\":\"error:sync\"", "\"rx_rate\":58.5", "\"gap_mean_ms\":17.2",
                          "\"gap_max_ms\":420.0", "\"buf_mean_ms\":66.0", "\"buf_max_ms\":140.0", "\"extrap\":5", "\"hold\":6", "\"snap\":7", "\"late\":8",
                          "\"stall100\":9", "\"tx_fail\":2", "\"relay_used\":true", "\"app\":\"3.0.0\""};
    for (const char* w : want) if (!HAS(out, w)) { ++sFail; fprintf(stderr, "FAIL summary lacks %s in %s\n", w, out); }
    // nothing that names a player or a place
    CHECK(!HAS(out, "name") && !HAS(out, "addr") && !HAS(out, "\"id\""));
    MpDiagSummary nan = s;
    nan.rxRate = 0.0f / 0.0f;
    mp_diag_summary_json(&nan, out, sizeof out);
    CHECK(Json::valid(out));
    MpDiagCheck c;
    memset(&c, 0, sizeof c);
    c.valid = true;
    snprintf(c.nat, sizeof c.nat, "cone"); c.stunMs = 41; snprintf(c.relay, sizeof c.relay, "ok"); c.relayRttMs = 87;
    mp_diag_check_json(&c, out, sizeof out);
    CHECK(Json::valid(out));
    CHECK(HAS(out, "\"nat\":\"cone\"") && HAS(out, "\"stun_ms\":41") && HAS(out, "\"relay\":\"ok\"") && HAS(out, "\"relay_rtt_ms\":87") && HAS(out, "\"err\""));
}

static void TestBody() {
    const MpDiagSummary s = Sum();
    MpDiagCheck c;
    memset(&c, 0, sizeof c);
    c.valid = true;
    snprintf(c.nat, sizeof c.nat, "symmetric"); c.stunMs = 30; snprintf(c.relay, sizeof c.relay, "skipped"); c.relayRttMs = -1; snprintf(c.err, sizeof c.err, "service");
    std::string log;
    for (int i = 0; i < 4000; i++) { char b[96]; snprintf(b, sizeof b, "[mp] net: \"q\"\\%05d\ttab\n", i); log += b; }   // (every line needs escaping)
    std::string note(300, 'x');
    note.replace(0, 3, "\"\\\n");   // (a note with quotes, a backslash and a newline, cut to 200 characters)
    MpDiagBodyIn in;
    memset(&in, 0, sizeof in);
    in.kind = "after_error"; in.where = "post_session"; in.note = note.c_str(); in.check = &c; in.stats = &s;
    in.logCur = log.data(); in.logCurLen = log.size();
    std::vector<char> out(MP_DIAG_BODY_MAX + 8);
    size_t logBytes = 0;
    const size_t n = mp_diag_build_body(&in, out.data(), out.size(), MP_DIAG_BODY_MAX, &logBytes);
    const std::string body(out.data(), n);
    CHECK(n > 0 && n <= (size_t)MP_DIAG_BODY_MAX);
    CHECK(Json::valid(body));
    CHECK(logBytes <= (size_t)MP_DIAG_LOG_MAX && logBytes > 1000);
    CHECK(HAS(body, "{\"v\":1,\"kind\":\"after_error\",\"where\":\"post_session\""));
    CHECK(HAS(body, "\"check\":{") && HAS(body, "\"stats\":{") && HAS(body, "\"log\":\""));
    // a tight body limit: the log shrinks until the escaped body fits, still valid JSON, still the newest lines
    const size_t tight = 6000;
    const size_t m = mp_diag_build_body(&in, out.data(), out.size(), tight, &logBytes);
    const std::string b2(out.data(), m);
    CHECK(m > 0 && m <= tight);
    CHECK(Json::valid(b2));
    if (!HAS(b2, "03999")) fprintf(stderr, "tight body (%d bytes) tail: %s\n", (int)m, b2.substr(b2.size() > 300 ? b2.size() - 300 : 0).c_str());
    CHECK(HAS(b2, "03999"));
    // no check and no stats: null
    in.check = nullptr; in.stats = nullptr; in.note = nullptr; in.kind = "user"; in.where = "mp_menu";
    const size_t k = mp_diag_build_body(&in, out.data(), out.size(), MP_DIAG_BODY_MAX, &logBytes);
    const std::string b3(out.data(), k);
    CHECK(Json::valid(b3));
    CHECK(HAS(b3, "\"check\":null") && HAS(b3, "\"stats\":null") && HAS(b3, "\"kind\":\"user\""));
    // the note is cut to 200 characters (multi-byte characters count once)
    std::string kana;
    for (int i = 0; i < 250; i++) kana += "\xE3\x81\x82";
    in.note = kana.c_str();
    const size_t z = mp_diag_build_body(&in, out.data(), out.size(), MP_DIAG_BODY_MAX, &logBytes);
    const std::string b4(out.data(), z);
    CHECK(Json::valid(b4));
    size_t cnt = 0;
    for (size_t at = b4.find("\xE3\x81\x82"); at != std::string::npos; at = b4.find("\xE3\x81\x82", at + 3)) ++cnt;
    CHECK(cnt == 200);
}

static void TestErrorOffer() {
    // every MpUiError, classified: a new code must be added here (the static_assert on the last value) and in mp_diag_error_offers_report
    static_assert(MP_ERR_CLOCK == 15, "a new MpUiError: classify it in mp_diag_error_offers_report and list it here");
    const struct { int err; bool offer; } t[] = {
        {MP_ERR_NONE, false}, {MP_ERR_NO_WIFI, false}, {MP_ERR_SERVICE_DOWN, true}, {MP_ERR_UPDATE_REQUIRED, false}, {MP_ERR_ROOM_NOT_FOUND, false},
        {MP_ERR_ROOM_FULL, false}, {MP_ERR_DIFFERENT_VERSION, false}, {MP_ERR_TIMEOUT, true}, {MP_ERR_HOST_LEFT, false}, {MP_ERR_KICKED, false},
        {MP_ERR_NAT, true}, {MP_ERR_NAME_TAKEN, false}, {MP_ERR_BAD_NAME, false}, {MP_ERR_RATE_LIMITED, false}, {MP_ERR_INTERNAL, true}, {MP_ERR_CLOCK, false}};
    CHECK(sizeof t / sizeof t[0] == 16);
    for (int i = 0; i < 16; i++) { CHECK(t[i].err == i); CHECK(mp_diag_error_offers_report(t[i].err) == t[i].offer); }
    CHECK(mp_diag_error_offers_report(99));   // unknown = internal
}

static void TestProbeSource() {
    CHECK(mp_diag_probe_source(0x01020304u, 0x01020304u, true) == 1);    // same public IP: one permission is enough
    CHECK(mp_diag_probe_source(0x01020304u, 0x05060708u, true) == 2);    // another IP (address pooling): a second permission
    CHECK(mp_diag_probe_source(0x01020304u, 0x05060708u, false) == 0);   // B's STUN got no answer: today's behaviour
    CHECK(mp_diag_probe_source(0x01020304u, 0, true) == 0);              // an answer without an address
}

int main() {
    TestProbeSource();
    TestErrorOffer();
    TestMasking();
    TestCap();
    TestEscape();
    TestSummary();
    TestBody();
    if (sFail) { fprintf(stderr, "diag_test: %d failure(s)\n", sFail); return 1; }
    printf("diag_test: all passed\n");
    return 0;
}
