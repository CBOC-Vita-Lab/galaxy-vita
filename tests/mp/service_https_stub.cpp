// Host test only: np_https without TLS (the service test talks plain http:// to the local fake server). Any https:// request is
// refused as UNAVAILABLE, like a build without mbedtls, except three test hosts that end like a console with a wrong clock would
// (service review): https://clock.invalid (NP_HTTPS_E_CLOCK), https://certdate.invalid (E_CERT, NOT_YET_VALID) and
// https://cert.invalid (E_CERT, UNTRUSTED: a certificate problem that is NOT the clock).
#include <string.h>

#include "np_https.h"

namespace {
int g_kind[8];   // per handle: 1 clock, 2 cert date, 3 cert untrusted
}

extern "C" {
NpHttpsHandle np_https_start(const NpHttpsRequest* rq, NpHttpsError* err) {
    int kind = 0;
    if (rq && rq->url) {
        if (!strncmp(rq->url, "https://clock.invalid", 21)) kind = 1;
        else if (!strncmp(rq->url, "https://certdate.invalid", 24)) kind = 2;
        else if (!strncmp(rq->url, "https://cert.invalid", 20)) kind = 3;
    }
    if (!kind) { if (err) *err = NP_HTTPS_E_UNAVAILABLE; return 0; }
    for (int h = 1; h < 8; ++h)
        if (!g_kind[h]) { g_kind[h] = kind; if (err) *err = NP_HTTPS_OK; return NpHttpsHandle(h); }
    if (err) *err = NP_HTTPS_E_BUSY;
    return 0;
}
int np_https_poll(NpHttpsHandle h, NpHttpsResult* res) {
    if (h <= 0 || h >= 8 || !g_kind[h]) return -1;
    memset(res, 0, sizeof *res);
    res->error = g_kind[h] == 1 ? NP_HTTPS_E_CLOCK : NP_HTTPS_E_CERT;
    res->cert_flags = g_kind[h] == 2 ? NP_HTTPS_CERT_NOT_YET_VALID : g_kind[h] == 3 ? NP_HTTPS_CERT_UNTRUSTED : 0;
    return 1;
}
void np_https_release(NpHttpsHandle h) { if (h > 0 && h < 8) g_kind[h] = 0; }
const char* np_https_error_name(NpHttpsError) { return "STUB"; }
const char* np_https_stage_name(NpHttpsStage) { return "QUEUED"; }
const char* np_https_error_text(NpHttpsError) { return "stub"; }
void np_https_cancel(NpHttpsHandle h) { if (h > 0 && h < 8) g_kind[h] = 0; }
int np_https_waiting(NpHttpsHandle) { return 0; }   // netport31 (np_http.cpp HttpOthersWaiting): the stub runs nothing in a queue
}
