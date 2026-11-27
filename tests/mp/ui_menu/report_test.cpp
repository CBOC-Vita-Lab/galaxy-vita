// The menus' host tests: beta diagnostics (Having Trouble?: Check Connection, Send a Problem Report, the after-error offer). Shared helpers: common.hpp.
#include "common.hpp"
#include "port/mp_diag.h"

namespace {
void toHelp() { openMain(); press(7); frames(2); }
void toConsent() { toHelp(); down(); ok(); frames(4); }   // (the two "what is sent" windows are answered by the fake)
bool shown(const char* sub) { for (const std::string& t : io.texts) if (t.find(sub) != std::string::npos) return true; return false; }
}

void testReport() {
    // Having Trouble? is Main's Start button, only where the online service is; two rows
    fresh();
    fake::service = false;
    openMain();
    press(7), frames(2);
    CHECK(S().screen() == Scr::S_MAIN);   // (no service: no Start button)
    fresh();
    openMain();
    CHECK(S().itemCount() == 3);          // (no fifth big button: a guide button)
    press(7), frames(2);
    CHECK(S().screen() == Scr::S_HELP && S().itemCount() == 2);
    back(), frames(2);
    CHECK(S().screen() == Scr::S_MAIN);

    // Send: the two windows say what is sent, then Send / Add a Note / Cancel
    fresh();
    const int opened0 = io.opened;
    toConsent();
    CHECK(S().screen() == Scr::S_REPORT && S().itemCount() == 3);
    CHECK(io.opened - opened0 == 2);   // (what is sent, what is not: two windows; the words themselves: the five-language checks)
    // Cancel closes it, nothing was sent
    down(2), ok(), frames(2);
    CHECK(S().screen() == Scr::S_HELP && fake::dgBegins == 0);

    // a note through the keyboard, then Send; the report asks for kind "user" from the Multiplayer menu
    fresh();
    toConsent();
    down(), ok(), frames(2);
    CHECK(fake::kbMax == 200);
    fake::kbResult = 1;
    fake::kbOnce = true;
    snprintf(fake::kbText, sizeof fake::kbText, "it froze after the star");
    frames(4);
    CHECK(S().screen() == Scr::S_REPORT);
    press(8), ok(), frames(3);   // (up to Send)
    CHECK(S().screen() == Scr::S_RPWAIT && fake::dgBegins == 1 && fake::dgLastKind == 0 && fake::dgLastWhere == 0);
    CHECK(std::string(fake::dgNote) == "it froze after the star");
    // pending: nothing happens; Circle cancels
    frames(30);
    CHECK(S().screen() == Scr::S_RPWAIT);
    back(), frames(2);
    CHECK(fake::dgCancels == 1 && S().screen() == Scr::S_REPORT);

    // sent: "Thanks!" with the id, back where it began
    fake::dgReportResult = 0;
    io.texts.clear();
    ok(), frames(3);
    fake::dgReportResult = 1;   // MP_DIAG_OK
    frames(3);
    CHECK(shown("K7QM2XPA"));
    CHECK(S().screen() == Scr::S_HELP);

    // every failure shows a window and stays on the consent screen (try again, or cancel)
    const int fails[] = {MP_DIAG_OFFLINE, MP_DIAG_TIMEOUT, MP_DIAG_BAD_REQUEST, MP_DIAG_TOO_LARGE, MP_DIAG_RATE_LIMITED, MP_DIAG_BUSY, MP_DIAG_FAILED, MP_DIAG_CLOCK};
    for (int f : fails) {
        fresh();
        toConsent();
        ok(), frames(3);
        const int opened = io.opened;
        fake::dgReportResult = f;
        frames(3);
        CHECK(io.opened == opened + 1);
        CHECK(S().screen() == Scr::S_REPORT);
        CHECK(io.count(MpMenu::Se::Buzzer) >= 1);
    }
    // a send that cannot start says so
    fresh();
    toConsent();
    fake::dgBeginOk = false;
    const int o2 = io.opened;
    ok(), frames(3);
    CHECK(io.opened == o2 + 1 && S().screen() == Scr::S_REPORT);
    // a port that never answers: the wait ends by itself
    fresh();
    toConsent();
    ok(), frames(3);
    const int o3 = io.opened;
    frames(41 * 60);
    CHECK(io.opened == o3 + 1 && S().screen() == Scr::S_REPORT && fake::dgCancels == 1);

    // no account: the sign-up runs first (the row is chosen again after it), then the windows
    fresh();
    fake::dgAccount = false;
    fake::ensureWork = 200;
    toHelp();
    down(), ok(), frames(2);
    CHECK(S().screen() == Scr::S_BUSY);
    fake::ensureWork = 0;
    frames(10);
    CHECK(S().screen() == Scr::S_REPORT);

    // Check Connection: waits, then a window with the two lines; Circle cancels
    fresh();
    toHelp();
    ok(), frames(3);
    CHECK(S().screen() == Scr::S_RPWAIT && fake::dgCheckBegins == 1);
    back(), frames(2);
    CHECK(fake::dgCheckCancels == 1 && S().screen() == Scr::S_HELP);
    io.texts.clear();
    ok(), frames(3);
    fake::dgCheckResult = 1;
    frames(3);
    CHECK(shown("Backup route: works") && S().screen() == Scr::S_HELP);
    const char* nats[][2] = {{"open", "ok"}, {"symmetric", "fail"}, {"blocked", "skipped"}, {"unknown", "skipped"}};
    for (auto& n : nats) {
        fresh();
        snprintf(fake::dgCheck.nat, sizeof fake::dgCheck.nat, "%s", n[0]);
        snprintf(fake::dgCheck.relay, sizeof fake::dgCheck.relay, "%s", n[1]);
        toHelp();
        ok(), frames(3);
        const int o = io.opened;
        fake::dgCheckResult = 1;
        frames(3);
        CHECK(io.opened == o + 1);
    }
    // a check that cannot start
    fresh();
    fake::dgCheckBeginOk = false;
    toHelp();
    const int o4 = io.opened;
    ok(), frames(3);
    CHECK(io.opened == o4 + 1 && S().screen() == Scr::S_HELP);

    // after an error: the Multiplayer menu offers once; Yes goes to the windows (kind after_error, where post_session); No sends nothing
    fresh();
    fake::dgErrPending = true;
    io.answer = MpMenu::DlgState::No;
    openMain();
    CHECK(fake::dgErrClears == 1 && io.opened >= 1);
    CHECK(S().screen() == Scr::S_MAIN && fake::dgBegins == 0 && S().itemCount() == 3);
    io.opened = 0;
    back(), frames(2);
    openMain();
    CHECK(io.opened == 0 && fake::dgErrClears == 1);   // once
    fresh();
    fake::dgErrPending = true;
    io.answer = MpMenu::DlgState::Yes;
    openMain();
    frames(6);
    CHECK(S().screen() == Scr::S_REPORT);
    ok(), frames(3);
    CHECK(fake::dgBegins == 1 && fake::dgLastKind == 1 && fake::dgLastWhere == 2);
    // no service: no offer
    fresh();
    fake::service = false;
    fake::dgErrPending = true;
    openMain();
    CHECK(fake::dgErrClears == 0);
}
