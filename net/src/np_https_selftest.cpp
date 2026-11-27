// SMG_NET_SELFTEST=https://host/path : one GET through np_https a few seconds after start-up, the outcome in the log:
//   [net-https] selftest GET https://host/path
//   [net-https] selftest result: OK status=200 tls=1.3 123 ms 57 bytes   (or the error, stage, certificate reasons, codes)
// Nothing else in the game uses np_https yet; without the variable this file does nothing (no thread, no network use).
// The delay lets the Wi-Fi connection and the log settle; the request is made from a thread of its own, polled like the game would.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_https.h"
#include "np_https_sys.h"

namespace {

void SelfTestMain(void*) {
    const char* url = getenv("SMG_NET_SELFTEST");
    if (!url || strncmp(url, "https://", 8) != 0) {
        if (url && *url) nph::Log("selftest: SMG_NET_SELFTEST must start with https:// (got \"%s\")", url);
        return;
    }
    nph::SleepMs(4000);
    char clock[96];
    np_https_clock_text(clock, sizeof clock);
    nph::Log("selftest GET %s", url);
    nph::Log("selftest backend %s, network %s, console clock %s (%s)", np_https_backend_name(), nph::NetworkUp() ? "up" : "DOWN", clock,
             np_https_clock_ok() == 1 ? "ok" : (np_https_clock_ok() == 0 ? "BEFORE THIS BUILD: set the date and time" : "unknown"));
    NpHttpsRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    NpHttpsError err = NP_HTTPS_OK;
    const NpHttpsHandle h = np_https_start(&rq, &err);
    if (!h) {
        nph::Log("selftest result: %s (%s)", np_https_error_name(err), np_https_error_text(err));
        return;
    }
    NpHttpsResult r;
    int polls = 0;
    NpHttpsStage last = NP_HTTPS_S_QUEUED;
    while (np_https_poll(h, &r) == 0) {
        if (r.stage != last) { nph::Log("selftest stage %s (%u ms)", np_https_stage_name(r.stage), (unsigned)r.elapsed_ms); last = r.stage; }
        if (++polls > 4000) { np_https_cancel(h); }   // (the request's own limits end it long before; this is the belt)
        nph::SleepMs(16);   // the game thread's frame
    }
    nph::Log("selftest result: %s%s%s | HTTP %d | stage %s | tls %s | %u ms (dns %u, tcp %u, tls %u) | %u bytes", np_https_error_name(r.error),
             r.message[0] ? " - " : "", r.message, r.http_status, np_https_stage_name(r.stage),
             r.tls_version == 0x0304 ? "1.3" : (r.tls_version == 0x0303 ? "1.2" : "-"), (unsigned)r.elapsed_ms, (unsigned)r.dns_ms, (unsigned)r.connect_ms,
             (unsigned)r.tls_ms, (unsigned)r.body_len);
    if (r.cipher[0]) nph::Log("selftest cipher %s", r.cipher);
    if (r.cert_detail[0]) nph::Log("selftest certificate %s", r.cert_detail);
    if (r.error == NP_HTTPS_E_CERT) nph::Log("selftest certificate flags 0x%x (library 0x%x)", (unsigned)r.cert_flags, (unsigned)r.cert_raw);
    if (r.error != NP_HTTPS_OK) nph::Log("selftest codes: tls %d (-0x%x) sys 0x%x | %s", r.tls_code, (unsigned)-r.tls_code, (unsigned)r.sys_code, np_https_error_text(r.error));
    if (r.body_len) {
        char shown[161];
        const size_t n = r.body_len < 160 ? r.body_len : 160;
        for (size_t i = 0; i < n; i++) shown[i] = (r.body[i] >= 0x20 && r.body[i] < 0x7F) ? (char)r.body[i] : '.';
        shown[n] = 0;
        nph::Log("selftest body: %s", shown);
    }
    np_https_release(h);
}

struct SelfTestStarter {
    SelfTestStarter() {
        const char* url = getenv("SMG_NET_SELFTEST");
        if (url && *url) nph::ThreadStart(SelfTestMain, nullptr);
    }
};
SelfTestStarter g_starter;

}  // namespace
