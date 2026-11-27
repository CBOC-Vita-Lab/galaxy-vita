// Galaxy 3.0 multiplayer: the menus, the Connection Test screen and its offer after a failed join (flow split of MpMenuSequence.cpp, 11 Nov;
// m-conntest rewrite, 11 Nov). Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
//
// The screen answers three questions a player actually has, in this order and in these terms:
//   "Can I play with people in this room?"  (same Wi-Fi / nearby)
//   "Can a friend far away join me?"        (the spoken code: needs a home network that lets a friend in)
//   "Can I play with anyone online?"        (the online service; the question is absent when the build has no service)
// Each gets one large verdict (Good / May be choppy / Not right now, with a signal picture) and, only when there is something to do, one
// short line under it. Nothing else is shown: no check names, no numbers, no network words (the details go to the log only).
#include "MpMenuInternal.hpp"

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)
// m-conntest: the last service check failed because the console's date and time are wrong (the TLS certificate dates cannot be judged).
// Wanted from the port (share/flow/core-wanted.md); until it exists the service's own code "clock" (mp_service_last_code) says the same.
extern "C" bool mp_conntest_clock_wrong(void) __attribute__((weak));

namespace MpMenu {

namespace {

// ------------------------------------------------------------------ the words (a stopgap table: flow moves them into MpMessages)
// Order of the languages: EN FR ES DE IT (MpMsgText::Lang). Register as share/words/terms.md: FR vous, ES tu, DE du, IT tu. No network
// word and no role word anywhere (conntest_test.cpp checks it in all five languages).
enum Word : int8_t {
    W_TITLE, W_Q_ROOM, W_Q_FAR, W_Q_ONLINE,
    W_V_GOOD, W_V_CHOPPY, W_V_NOT_NOW, W_CHECKING,
    W_AGAIN, W_WAIT_ROOM, W_WAIT_FAR, W_WAIT_ONLINE, W_TOOK_LONG,
    W_L_CLOSER, W_L_WIFI, W_L_SEE, W_L_FAR_MAYBE, W_L_FAR_NO, W_L_SLOW, W_L_DOWN, W_L_UPDATE, W_L_LATER, W_L_CLOCK,
    W_COUNT
};
constexpr int kLangs = 5;
// (flow, words pass: the words are in MpMessages now; this maps the screen's word numbers to them)
const MpText kWordIds[W_COUNT] = {
    MP_T_CT_TITLE, MP_T_CT_Q_ROOM, MP_T_CT_Q_FAR, MP_T_CT_Q_ONLINE, MP_T_CT_V_GOOD, MP_T_CT_V_CHOPPY, MP_T_CT_V_NOT_NOW, MP_T_CT_CHECKING, MP_T_CT_AGAIN, MP_T_CT_WAIT_ROOM, MP_T_CT_WAIT_FAR, MP_T_CT_WAIT_ONLINE, MP_T_CT_TOOK_LONG, MP_T_CT_L_CLOSER, MP_T_CT_L_WIFI, MP_T_CT_L_SEE, MP_T_CT_L_FAR_MAYBE, MP_T_CT_L_FAR_NO, MP_T_CT_L_SLOW, MP_T_CT_L_DOWN, MP_T_CT_L_UPDATE, MP_T_CT_L_LATER, MP_T_CT_L_CLOCK};

const char16_t* WordText(int w) { return txt::raw(kWordIds[w]); }

constexpr uint32_t kWatchdog = 15 * kFps;   // the engine's own budget is about 8 s; past this the screen stops waiting and says so
uint32_t sWaited = 0;                        // frames this run has been up (the screen is a singleton: one run at a time)
bool sTimedOut = false;

bool ClockWrong() {
    if (mp_conntest_clock_wrong && mp_conntest_clock_wrong()) return true;
    const char* code = mp_service_last_code ? mp_service_last_code() : nullptr;
    return code && !strcmp(code, "clock");
}
bool RelayOnly() { return mp_conntest_relay_only && mp_conntest_relay_only(); }

void Button(MpPanel::Model* m, int pic, const char16_t* label) {
    if (m->buttonCount >= 4) return;
    m->buttons[m->buttonCount].pic = static_cast<uint8_t>(pic);
    Copy16(m->buttons[m->buttonCount].label, 20, label);
    ++m->buttonCount;
}

CtAnswer Answer(CtVerdict v, int line = -1) { return CtAnswer{v, true, static_cast<int8_t>(line)}; }
}  // namespace

// ------------------------------------------------------------------ the judgement
CtAnswers ConnTestJudge(const MpCtView& v, bool serviceBuilt, bool clockWrong, bool relayOnly) {
    CtAnswers a;
    memset(&a, 0, sizeof a);
    a.room.line = a.far.line = a.online.line = -1;
    auto done = [&v](int i) { return v.state[i] == MPCT_ST_DONE; };
    const uint8_t w = v.result[MPCT_WIFI], l = v.result[MPCT_LOCAL], n = v.result[MPCT_INTERNET], h = v.result[MPCT_HOSTING], d = v.result[MPCT_DELAY];
    a.onlineAbsent = !serviceBuilt || (done(MPCT_INTERNET) && n == MPCT_R_NET_NOT_SET_UP);

    // 1. people in this room: the Vita's own radio (Nearby) needs no Wi-Fi, so only a weak Wi-Fi or one that hides consoles matters
    if (done(MPCT_WIFI) && done(MPCT_LOCAL)) {
        if (w == MPCT_R_WIFI_WEAK) a.room = Answer(CTV_CHOPPY, W_L_CLOSER);
        else if (w != MPCT_R_WIFI_NONE && l == MPCT_R_LOCAL_BLOCKED) a.room = Answer(CTV_CHOPPY, W_L_SEE);
        else a.room = Answer(CTV_GOOD);
    }

    // 2. a friend far away: the spoken code works when the home network lets a friend in
    if (done(MPCT_WIFI) && done(MPCT_INTERNET) && done(MPCT_HOSTING)) {
        if (w == MPCT_R_WIFI_NONE) {
            a.far = Answer(CTV_NOT_NOW, W_L_WIFI);
        } else {
            switch (h) {
            case MPCT_R_HOST_OPEN: a.far = w == MPCT_R_WIFI_WEAK ? Answer(CTV_CHOPPY, W_L_CLOSER) : Answer(CTV_GOOD); break;
            case MPCT_R_HOST_MAYBE: case MPCT_R_HOST_UNKNOWN: a.far = Answer(CTV_CHOPPY, W_L_FAR_MAYBE); break;
            case MPCT_R_HOST_CGNAT: a.far = Answer(CTV_NOT_NOW, W_L_FAR_NO); break;
            // not tested (no service to test against, or in a room): with the privacy setting on the code goes through the service
            default: a.far = relayOnly && n == MPCT_R_NET_OK ? Answer(CTV_GOOD) : CtAnswer{CTV_UNKNOWN, true, -1}; break;
            }
        }
    }

    // 3. anyone online: the online service (no row at all when the build has none)
    if (a.onlineAbsent) {
        a.online = CtAnswer{CTV_UNKNOWN, true, -1};
    } else if (done(MPCT_WIFI) && done(MPCT_INTERNET) && done(MPCT_DELAY)) {
        if (w == MPCT_R_WIFI_NONE) a.online = Answer(CTV_NOT_NOW, W_L_WIFI);
        else if (n == MPCT_R_NET_OK) a.online = w == MPCT_R_WIFI_WEAK ? Answer(CTV_CHOPPY, W_L_CLOSER)
                                                : (d == MPCT_R_DELAY_SLOW ? Answer(CTV_CHOPPY, W_L_SLOW) : Answer(CTV_GOOD));
        else if (clockWrong) a.online = Answer(CTV_NOT_NOW, W_L_CLOCK);
        else if (n == MPCT_R_NET_UPDATE) a.online = Answer(CTV_NOT_NOW, W_L_UPDATE);
        else if (n == MPCT_R_NET_OFF) a.online = Answer(CTV_NOT_NOW, W_L_LATER);
        else a.online = Answer(CTV_NOT_NOW, W_L_DOWN);
    }
    return a;
}

namespace {
bool Finished(const MpCtView& v) { return v.done && !v.running; }
CtAnswers JudgeNow(const MpCtView& v) { return ConnTestJudge(v, NetInfo().service, ClockWrong(), RelayOnly()); }
}  // namespace

CtVerdict ConnTestFarVerdict() {
    if (!mp_conntest_view) return CTV_UNKNOWN;
    MpCtView v;
    memset(&v, 0, sizeof v);
    mp_conntest_view(&v);
    if (!Finished(v)) return CTV_UNKNOWN;
    // a finished test describes the Wi-Fi it ran on: if the console's Wi-Fi has since come or gone, it says nothing
    if (NetInfo().wifiUp != (v.result[MPCT_WIFI] != MPCT_R_WIFI_NONE)) return CTV_UNKNOWN;
    const CtAnswers a = JudgeNow(v);
    return a.far.ready ? static_cast<CtVerdict>(a.far.verdict) : CTV_UNKNOWN;
}

bool ConnTestSteerFarToOnline() {
    if (ConnTestFarVerdict() != CTV_NOT_NOW) return false;
    MpCtView v;
    memset(&v, 0, sizeof v);
    mp_conntest_view(&v);
    const CtAnswers a = JudgeNow(v);
    return !a.onlineAbsent && a.online.ready && (a.online.verdict == CTV_GOOD || a.online.verdict == CTV_CHOPPY);
}

int ConnTestWordCount() { return W_COUNT; }
const char16_t* ConnTestWord(int index, int lang) {
    if (index < 0 || index >= W_COUNT || lang < 0 || lang >= kLangs) return nullptr;
    const int was = txt::language();
    txt::setLanguage(lang);
    const char16_t* t = txt::raw(kWordIds[index]);
    txt::setLanguage(was);
    return t;
}

namespace {
// which questions are listed: the room always; the far friend unless the finished test could not say; the online one unless the build has none
struct Listed { bool far, online; };
Listed ListedQuestions(const MpCtView& v, const CtAnswers& a) { return {!(v.done && a.far.verdict == CTV_UNKNOWN), !a.onlineAbsent}; }
const CtAnswer& AnswerOf(const CtAnswers& a, int item) { return item == MpMenu::I_CT_ROOM ? a.room : (item == MpMenu::I_CT_FAR ? a.far : a.online); }
}  // namespace

// ------------------------------------------------------------------ the Connection Test (conntest)
bool Sequence::openConnTest() {
    if (!mp_conntest_start || !mp_conntest_start(mp_active())) {
        if (mIo) mIo->sound(Se::Buzzer);
        return false;
    }
    mCtActive = true;
    mCtReported = false;
    sWaited = 0;
    sTimedOut = false;
    push(S_CONNTEST);
    top().cursor = 0;
    mp_log("ui: connection test opened");
    return true;
}

void Sequence::askLines(int purpose, const MpText* ids, int count, Dlg kind) {
    // (the verdict no longer opens a window: kept only because the public header declares it)
    mDlg = purpose;
    mDlgParam = 0;
    txt::setDynamicLines(0, ids, count);
    strncpy(mLastDialogId, "MP_DYN0", sizeof mLastDialogId - 1);
    if (mIo) mIo->dialogOpen(kind, "MP_DYN0");
}

// Per frame while the screen is up. Back is taken FIRST, in every state (running, finished, timed out, a verdict arriving this very frame).
void Sequence::tickConnTest(const Input& in) {
    MpCtView v;
    memset(&v, 0, sizeof v);
    if (mp_conntest_view) mp_conntest_view(&v);
    if (in.back) {
        mp_log("ui: connection test closed");
        if (mIo) mIo->sound(Se::Cancel);
        mCtActive = false;
        if (mp_conntest_cancel) mp_conntest_cancel();
        pop();
        return;
    }
    if (v.running) {
        // never wait for good: past the watchdog the run is stopped and the screen says it took too long (Check Again tries once more)
        if (++sWaited > kWatchdog) {
            mp_log("ui: connection test gave up waiting");
            sTimedOut = true;
            if (mp_conntest_cancel) mp_conntest_cancel();
            if (mp_conntest_view) mp_conntest_view(&v);
            if (mIo) mIo->sound(Se::Buzzer);
        }
    }
    // the questions are rows: the cursor moves over them (the line under the title says what to do about the one it is on)
    int it[kItems];
    const int count = items(S_CONNTEST, 0, it);
    Frame& f = top();
    if (f.cursor < 0 || f.cursor >= count) f.cursor = 0;
    if (v.done && !mCtReported) {
        // the run just finished: the details go to the log only; a weak link makes the Wi-Fi power-save tip due (shown with the menu, never here)
        mCtReported = true;
        const CtAnswers a = JudgeNow(v);
        mp_log("ui: connection test room=%d far=%d online=%d%s results=%d,%d,%d,%d,%d detail=%d,%d ms=%d clock=%d", a.room.verdict, a.far.verdict,
               a.online.verdict, a.onlineAbsent ? "(absent)" : "", v.result[0], v.result[1], v.result[2], v.result[3], v.result[4], v.detail[0], v.detail[1],
               v.detail[MPCT_DELAY], ClockWrong() ? 1 : 0);
        bool all = true;
        int firstLine = -1;
        for (int i = 0; i < count; i++) {
            const CtAnswer& q = AnswerOf(a, it[i]);
            if (q.verdict != CTV_GOOD) all = false;
            if (q.line >= 0 && firstLine < 0) firstLine = i;
        }
        if (mIo) mIo->sound(all ? Se::Success : Se::Decide);
        if (firstLine >= 0) f.cursor = static_cast<int8_t>(firstLine);   // (it starts on the first thing there is something to do about)
        fitScroll(f);
        if (v.verdict.weakLink) mTipWanted = true;
    }
    if (in.up) moveCursor(-1, count);
    else if (in.down) moveCursor(1, count);
    else if (in.ok && !v.running) {
        if (mIo) mIo->sound(Se::Decide);
        mDecidedRow = -1;
        mCtReported = false;
        sWaited = 0;
        sTimedOut = false;
        f.cursor = 0;
        if (!mp_conntest_start || !mp_conntest_start(mp_active())) {
            mCtActive = false;
            if (mIo) mIo->sound(Se::Buzzer);
        }
    }
}

bool Sequence::itemsConnTest(Scr s, uint8_t param, int* out, int* np) const {
    (void)param;
    if (s != S_CONNTEST) return false;
    MpCtView v;
    memset(&v, 0, sizeof v);
    if (mp_conntest_view) mp_conntest_view(&v);
    const CtAnswers a = JudgeNow(v);
    const Listed l = ListedQuestions(v, a);
    int n = 0;
    out[n++] = I_CT_ROOM;
    if (l.far) out[n++] = I_CT_FAR;
    if (l.online) out[n++] = I_CT_ONLINE;
    *np = n;
    return true;
}

bool Sequence::chooseConnTest(int item) {
    switch (item) {
    case I_CONNTEST:
        if (mIo) mIo->sound(Se::Decide);
        openConnTest();
        return true;
    case I_CT_ROOM: case I_CT_FAR: case I_CT_ONLINE:
        return true;   // (a question is only read: Cross on the screen is Check Again, handled in the tick)
    default: break;
    }
    return false;
}

bool Sequence::dialogConnTest(int purpose, bool yes, int param) {
    (void)param;
    switch (purpose) {
    case P_ERROR:
        // after a connection error, the one-line offer
        if (mOfferTest) {
            mOfferTest = false;
            ask(P_CT_OFFER, MP_T_DLG_CT_OFFER, Args(), Dlg::YesNo);
        } else {
            regatherAfterQuick();   // uxfix #3
        }
        return true;
    case P_CT_OFFER:
        if (yes) { mQuickRegather = false; openConnTest(); }
        else regatherAfterQuick();
        return true;
    case P_CT_RESULT: case P_CT_REPORT:
        return true;   // (no longer opened: the verdict is on the screen)
    default: return false;
    }
}

bool Sequence::keyboardConnTest(int purpose, const char* text) {
    (void)purpose;
    (void)text;
    return false;
}

bool Sequence::tickConnTest(Scr scr, const Input& in, bool* stop) {
    switch (scr) {
    case S_CONNTEST:
        tickConnTest(in);
        { *stop = true; return true; }
    default: return false;
    }
}

// A question's row: the question, its verdict as a word and a signal picture (full / half / nearly empty and red). Waiting its turn: dim.
bool Sequence::labelConnTest(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    if (f.scr != S_CONNTEST || item < I_CT_ROOM || item > I_CT_ONLINE) return false;
    MpCtView v;
    memset(&v, 0, sizeof v);
    if (mp_conntest_view) mp_conntest_view(&v);
    const CtAnswers a = JudgeNow(v);
    const CtAnswer& q = AnswerOf(a, item);
    Copy16(r.label, kLabel, WordText(item == I_CT_ROOM ? W_Q_ROOM : (item == I_CT_FAR ? W_Q_FAR : W_Q_ONLINE)));
    const bool running = item == I_CT_ROOM ? (v.state[MPCT_WIFI] == MPCT_ST_RUNNING || v.state[MPCT_LOCAL] == MPCT_ST_RUNNING)
                         : (item == I_CT_FAR ? v.state[MPCT_HOSTING] == MPCT_ST_RUNNING
                                             : (v.state[MPCT_INTERNET] == MPCT_ST_RUNNING || v.state[MPCT_DELAY] == MPCT_ST_RUNNING));
    if (q.ready && q.verdict != CTV_UNKNOWN) {
        const bool good = q.verdict == CTV_GOOD, choppy = q.verdict == CTV_CHOPPY;
        Copy16(r.value, kValue, WordText(good ? W_V_GOOD : (choppy ? W_V_CHOPPY : W_V_NOT_NOW)));
        r.ping = good ? 4 : (choppy ? 2 : 1);
        if (!good && !choppy) r.marks |= MARK_LAG;   // (a red, nearly empty signal)
    } else if (running) {
        Copy16(r.value, kValue, WordText(W_CHECKING));
    } else {
        r.marks |= MARK_DISABLED;   // (waiting its turn, or never finished)
    }
    return true;
}

bool Sequence::modelConnTest(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    (void)count, (void)it;
    if (f.scr != S_CONNTEST) return false;
    MpCtView v;
    memset(&v, 0, sizeof v);
    if (mp_conntest_view) mp_conntest_view(&v);
    const CtAnswers a = JudgeNow(v);
    Copy16(m->title, 48, WordText(W_TITLE));
    if (v.running) {
        // what is being checked right now (the screen never waits without saying what for)
        int w = W_WAIT_ROOM;
        for (int i = 0; i < MPCT_CHECKS; i++)
            if (v.state[i] == MPCT_ST_RUNNING) w = i <= MPCT_LOCAL ? W_WAIT_ROOM : (i == MPCT_HOSTING ? W_WAIT_FAR : W_WAIT_ONLINE);
        Copy16(m->subtitle, 64, WordText(w));
    } else if (!v.done && sTimedOut) {
        Copy16(m->subtitle, 64, WordText(W_TOOK_LONG));   // (stopped by the watchdog)
    } else if (cursorItem >= I_CT_ROOM && cursorItem <= I_CT_ONLINE) {
        // the one short line of what to do about the question the cursor is on (none when it is fine)
        const CtAnswer& q = AnswerOf(a, cursorItem);
        if (q.ready && q.line >= 0) Copy16(m->hint, 80, WordText(q.line));
    }
    // flow look: the list in the game's look; its one line (what is being checked, or what to do) in the game's bottom message
    GameLook(m);
    // (the first game pictures: with nothing to check or say the screen had no words at all: then its question is the line)
    Copy16(m->line, 96, m->subtitle[0] ? m->subtitle : (m->hint[0] ? m->hint : m->title));
    if (!v.running) Button(m, txt::PIC_CROSS, WordText(W_AGAIN));
    ModelButton(m, txt::PIC_CIRCLE, MP_T_BACK);
    return true;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressConnTest(Scr scr, const Input& in, int item, int count, int stage) {
    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveConnTest(Scr scr, uint8_t param, int phase) {
    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
