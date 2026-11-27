// quota32: host test of the "service is busy / out of quota" classifier (platform/src/mp/mp_svc_busy.h) and of the message words for it.
#include <stdio.h>
#include <string.h>

#include <string>

#include "mp_svc_busy.h"
#include "Game/MP/MpMessages.hpp"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static bool Busy(int transport, int status, const std::string& body) { return mp_svc::IsBusyAnswer(transport, status, body.data(), body.size()); }

int main() {
    // Cloudflare's own answers (not our JSON): busy
    CHECK(Busy(0, 429, "error code: 1027"));
    CHECK(Busy(0, 429, "<!DOCTYPE html><html><head><title>Error 1027</title></head><body>Worker exceeded free tier daily request limit.</body></html>"));
    CHECK(Busy(0, 429, ""));                                     // an empty 429 (a proxy)
    CHECK(Busy(0, 429, "{\"message\":\"slow down\"}"));          // JSON, but not our envelope
    CHECK(Busy(0, 500, "<html>Error 1101 Worker threw exception</html>"));
    CHECK(Busy(0, 502, "<html>Bad gateway</html>"));
    CHECK(Busy(0, 503, "Service Unavailable"));
    CHECK(Busy(0, 504, ""));
    CHECK(Busy(0, 522, "<html>Connection timed out</html>"));
    CHECK(Busy(0, 530, "error code: 1016"));
    CHECK(Busy(0, 500, "   \n<html>starts with white space</html>"));
    CHECK(Busy(0, 500, "{\"error\":\"string, not an object\"}"));   // an "error" without a code
    // the service's own busy answers
    CHECK(Busy(0, 503, "{\"error\":{\"code\":\"over_budget\",\"message\":\"x\",\"retryAfterSec\":120}}"));
    CHECK(Busy(0, 503, "{\"error\": {\"message\":\"m\", \"code\": \"over_budget\"}}"));
    CHECK(Busy(0, 503, "{\"error\":{\"code\":\"service_busy\"}}"));
    // the service speaking: not "busy" (its own rate limit, refusals, bugs)
    CHECK(!Busy(0, 429, "{\"error\":{\"code\":\"rate_limited\",\"message\":\"slow down\",\"retryAfterSec\":30}}"));
    CHECK(!Busy(0, 429, "{\"error\":{\"code\":\"daily_cap\"}}"));
    CHECK(!Busy(0, 500, "{\"error\":{\"code\":\"internal\"}}"));
    CHECK(!Busy(0, 503, "{\"error\":{\"code\":\"busy\",\"message\":\"x\",\"retryAfterSec\":3}}"));   // fix33 (S2): the worker's transient answer: the service speaking, no menu gate
    CHECK(!Busy(0, 503, "{\"error\":{\"code\":\"service_disabled\",\"message\":\"off\"}}"));
    CHECK(!Busy(0, 403, "{\"error\":{\"code\":\"banned\"}}"));
    CHECK(!Busy(0, 404, "<html>not found</html>"));
    CHECK(!Busy(0, 401, "{\"error\":{\"code\":\"unauthorized\"}}"));
    CHECK(!Busy(0, 426, "{\"error\":{\"code\":\"update_required\"}}"));
    // successes, redirects, captive portals are other cases
    CHECK(!Busy(0, 200, "{\"ok\":true}"));
    CHECK(!Busy(0, 200, "<html>Sign in to the hotspot</html>"));
    CHECK(!Busy(0, 302, ""));
    // transport failures are not answers
    CHECK(!Busy(1, 0, ""));
    CHECK(!Busy(2, 429, "error code: 1027"));
    CHECK(!Busy(3, 0, ""));
    // hostile bodies: no crash, no read past the end (run under ASan), a huge page is only looked at in its first KiB
    CHECK(Busy(0, 429, std::string(100000, 'A')));
    CHECK(Busy(0, 500, std::string(2000, '{')));
    CHECK(Busy(0, 429, "{\"error\":{\"code\":\""));          // cut at the code: no usable envelope: an edge page
    CHECK(!Busy(0, 429, "{\"error\":{\"code\":\"rate_limited"));   // cut inside the code: still the service's
    char out[8];
    CHECK(mp_svc::EnvelopeCode("{\"error\":{\"code\":\"abcdefghijkl\"}}", 31, out, sizeof out) == 7 && !strcmp(out, "abcdefg"));   // capped, terminated
    CHECK(mp_svc::EnvelopeCode(nullptr, 0, out, sizeof out) == 0);
    CHECK(mp_svc::EnvelopeCode("", 0, out, sizeof out) == 0);

    // the words: the new message exists in every language of the game's own tables, is not the old "service down" line, and the service's own
    // rate limit keeps its own words
    const int langs[] = {MpMsgText::LANG_EN, MpMsgText::LANG_FR, MpMsgText::LANG_ES, MpMsgText::LANG_DE, MpMsgText::LANG_IT};
    std::u16string en;
    for (int lang : langs) {
        MpMsgText::setLanguage(lang);
        const char16_t* t = MpMsgText::raw(MP_T_ERR_BUSY);
        CHECK(t && t[0]);
        CHECK(std::u16string(t) != std::u16string(MpMsgText::raw(MP_T_ERR_SERVICE_DOWN)));
        if (lang == MpMsgText::LANG_EN) en = t; else CHECK(std::u16string(t) != en);   // translated, not the English fallback
    }
    MpMsgText::setLanguage(MpMsgText::LANG_EN);
    CHECK(MpMsgText::serviceText("service_busy") == MP_T_ERR_BUSY);
    CHECK(MpMsgText::serviceText("over_budget") == MP_T_ERR_BUSY);
    CHECK(MpMsgText::serviceText("rate_limited") == MP_T_ERR_RATE);

    if (g_fail) { fprintf(stderr, "svc_busy_test: %d check(s) failed\n", g_fail); return 1; }
    fprintf(stderr, "svc_busy_test: all checks passed\n");
    return 0;
}
