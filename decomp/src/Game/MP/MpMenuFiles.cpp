// Galaxy 3.0 multiplayer: the menus, the file family: Which File?, My Files, copy / play on this file, which co-op file, keep / recovery offers, the save root at the file select (flow split of MpMenuSequence.cpp, 11 Nov).
// Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
#include "MpMenuInternal.hpp"

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)

// ---- m-files: what this family needs from the save layer and from keepsafe -------------------------------------------------------
// savefix: MpSaveGame::slotState (a file that cannot be read is not a file that is new) is in MpSaveGame.hpp since d1a7d8bb.
#include "port/mp_keepsafe.h"   // keepsafe (merged in b714ce15): the real header

namespace MpMenu {

namespace {

// ---- m-files: a file that could not be read is not a file that is new ---------------------------------------------------------
// The star caches hold: stars (>= 0, a file in use), -1 (no such file yet: "New"), kUnreadable (the file is there and could not be
// read). Only -1 can be written to as a new file; kUnreadable is never a place to write, never "New", never replaced.
constexpr int kUnreadable = -2;
inline bool Bad(int v) { return v == kUnreadable; }

// A single-player file: stars, -1 never created, kUnreadable.
int ReadSp(int file) {
    int stars = -1;
    const int r = MpSaveGame::slotState(false, file, &stars);
    if (r == MpSaveGame::SLOT_UNREADABLE) return kUnreadable;
    return r == MpSaveGame::SLOT_USED && stars >= 0 ? stars : -1;
}
// A co-op file: in use when coop.cfg says a session played on it / progress was kept there, or it has Power Stars (coop.cfg lost);
// otherwise "New" (-1) (flow, review 2 #3); a file that is there but cannot be read: kUnreadable.
int ReadCoop(int slot) {
    int stars = -1;
    const int r = MpSaveGame::slotState(true, slot, &stars);
    if (r == MpSaveGame::SLOT_UNREADABLE) return kUnreadable;
    if (r != MpSaveGame::SLOT_USED) return -1;
    if (stars < 0) return kUnreadable;
    MpCoopMeta m;
    memset(&m, 0, sizeof m);
    if (stars > 0 || (mp_coop_meta_get && mp_coop_meta_get(slot, &m))) return stars;
    return -1;
}

// ---- m-files: words this family needs that MpMessages does not have yet (share/flow/words-wanted.md) ----------------------------
// English placeholders. flow turns each into an MpText in five languages; until then they go through the same formatter the real
// texts use (S1 / S2 / N1 = the two string arguments and the number).
#define S1 u"\x01"
#define S2 u"\x02"
#define N1 u"\x03"
enum Word : uint8_t {
    W_CANT_READ, W_CANT_READ_HINT, W_SP_CANT_READ_HINT, W_SLOTS_CANT_READ, W_NO_ROOM_COPY, W_FILE_NOT_READY, W_PLAY_MINE, W_SAVE_MINE,
    W_SAVE_MINE_NOSTARS, W_KEEP_TITLE, W_KEEP_ROW, W_KEEP_MAKE, W_KEEP_BACK, W_KEEP_H_MAKE, W_KEEP_H_BACK, W_KEEP_LIST_TITLE, W_KEEP_MADE,
    W_KEEP_NOTHING, W_KEEP_NO_SPACE, W_KEEP_FAILED, W_KEEP_BUSY, W_KS_NOTICE, W_BK_ACCOUNT, W_BK_ROLLED, W_BK_BAD, W_BK_NEWER, W_BK_NO_SPACE,
    W_BK_NOTHING, W_KEEP_SHARE, W_KEEP_H_SHARE, W_MADE_SHARE, W_NO_COPY, W_TOO_MANY, W_L_VERSION, W_L_BEFORE, W_L_MADE, W_H_HOLDS,
    W_ROW_TEAMFILES, W_ROW_ACCOUNT, W_ROW_SET_CONFIG, W_ROW_SET_FLOW, W_ROW_SET_COOP, W_H_SLOT_NEW, W_H_SLOT_HERE, W_H_ACCOUNT, W_Q_SLOT_REPLACE,
    W_Q_TEAM_DIFFERENT, W_Q_SET_CONFIG, W_Q_SET_FLOW, W_Q_SET_COOP, W_D_SLOT_BACK, W_D_SLOT_BACK_PLAIN, W_D_SLOT_SAME, W_D_ACCOUNT_DONE,
    W_D_ACCOUNT_SAME, W_D_SET_DONE,
    W_COUNT
};
// (flow, words pass: the words are in MpMessages now, five languages; this maps the family's word numbers to them)
const MpText kWordIds[W_COUNT] = {
    MP_T_KS_CANT_READ, MP_T_KS_CANT_READ_HINT, MP_T_KS_SP_CANT_READ_HINT, MP_T_KS_SLOTS_CANT_READ, MP_T_KS_NO_ROOM_COPY, MP_T_KS_FILE_NOT_READY, MP_T_KS_PLAY_MINE, MP_T_KS_SAVE_MINE, MP_T_KS_SAVE_MINE_NOSTARS, MP_T_KS_KEEP_TITLE, MP_T_KS_KEEP_ROW, MP_T_KS_KEEP_MAKE, MP_T_KS_KEEP_BACK, MP_T_KS_KEEP_H_MAKE, MP_T_KS_KEEP_H_BACK, MP_T_KS_KEEP_LIST_TITLE, MP_T_KS_KEEP_MADE, MP_T_KS_KEEP_NOTHING, MP_T_KS_KEEP_NO_SPACE, MP_T_KS_KEEP_FAILED, MP_T_KS_KEEP_BUSY, MP_T_KS_KS_NOTICE, MP_T_KS_BK_ACCOUNT, MP_T_KS_BK_ROLLED, MP_T_KS_BK_BAD, MP_T_KS_BK_NEWER, MP_T_KS_BK_NO_SPACE, MP_T_KS_BK_NOTHING, MP_T_KS_KEEP_SHARE, MP_T_KS_KEEP_H_SHARE, MP_T_KS_MADE_SHARE, MP_T_KS_NO_COPY, MP_T_KS_TOO_MANY, MP_T_KS_L_VERSION, MP_T_KS_L_BEFORE, MP_T_KS_L_MADE, MP_T_KS_H_HOLDS, MP_T_KS_ROW_TEAMFILES, MP_T_KS_ROW_ACCOUNT, MP_T_KS_ROW_SET_CONFIG, MP_T_KS_ROW_SET_FLOW, MP_T_KS_ROW_SET_COOP, MP_T_KS_H_SLOT_NEW, MP_T_KS_H_SLOT_HERE, MP_T_KS_H_ACCOUNT, MP_T_KS_Q_SLOT_REPLACE, MP_T_KS_Q_TEAM_DIFFERENT, MP_T_KS_Q_SET_CONFIG, MP_T_KS_Q_SET_FLOW, MP_T_KS_Q_SET_COOP, MP_T_KS_D_SLOT_BACK, MP_T_KS_D_SLOT_BACK_PLAIN, MP_T_KS_D_SLOT_SAME, MP_T_KS_D_ACCOUNT_DONE, MP_T_KS_D_ACCOUNT_SAME, MP_T_KS_D_SET_DONE};
#undef S1
#undef S2
#undef N1
inline void Wd(char16_t* out, int cap, Word w, const Args& a = Args()) { txt::format(out, cap, kWordIds[w], a); }


// m-files (review finding 5): "Save to My File N" is offered only when the progress is known to come from that very file. The save
// layer's gate says so (LeaveOffer.single / RecoveryInfo.single: cleared when the player became the host by migration or took another
// console's snapshot); the menu also wants its own record to agree (this console started the game on that file, as the original host)
// and the file to read as a file in use. Returns the file number, 0 = not offered.
template <class Stars>
int SingleSource(uint8_t param, const MpSaveGame::LeaveOffer& l, const MpSaveGame::RecoveryInfo& r, bool, int, Stars stars) {
    int slot = 0;
    if (param == SP_LEAVE) slot = l.single ? l.singleSlot : 0;
    else if (param == SP_RECOVER) slot = r.single && r.sourceKind == 2 ? r.sourceSlot : 0;
    if (slot < 1 || slot > 6 || stars(slot) < 0) return 0;
    return slot;
}

// ---- keepsafe API (share/keep/API.md): Safe Copies --------------------------------------------------------------------------------
// Make a Copy / Make a Copy to Share / Bring Back a Copy (the list) / one copy (only what can be brought back: Team Files one at a
// time, the online account, the settings files). Nothing is cached across a call: after anything is brought back the copies are
// listed and the copy inspected again.
constexpr int kMaxCopies = 12;
constexpr int kTooMany = MP_KS_CONFIRM + 1;   // MP_KS_TOO_MANY (keepsafe branch keepfix 5; not in the header merged here yet)
constexpr int kNoticeNoSpace = 2;             // MP_KS_NOTICE_NO_SPACE (the same)
MpKsCopy sCopies[kMaxCopies];
int sCopyCount = 0;
char sPickName[96] = {0};       // the copy being looked at (by name: the list changes when an "Auto copy" is made)
MpKsCopyInfo sInfo;             // what inspect said about it

// "2026-11-11 14:20" (UTC: the game has no time zone; the same text everywhere)
void FormatDate(int64_t t, char* out, int cap) {
    if (t < 0) t = 0;
    int64_t days = t / 86400;
    const int secs = static_cast<int>(t % 86400);
    days += 719468;
    const int64_t era = days / 146097;
    const int doe = static_cast<int>(days - era * 146097);
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y0 = yoe + static_cast<int>(era) * 400;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int mp = (5 * doy + 2) / 153;
    const int d = doy - (153 * mp + 2) / 5 + 1;
    const int m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", y0 + (m <= 2 ? 1 : 0), m, d, secs / 3600, (secs / 60) % 60);
}

// The same, the day only (the message windows are narrow)
void FormatDay(int64_t t, char* out, int cap) {
    FormatDate(t, out, cap);
    if (cap > 10) out[10] = 0;
}

// The copies, newest first (keepsafe orders them). Read when a screen is pushed and after anything that changes them, never per frame.
void KeepLoad() {
    memset(sCopies, 0, sizeof sCopies);
    int n = mp_keepsafe_list_copies(sCopies, kMaxCopies);
    if (n < 0 || n > kMaxCopies) n = 0;
    sCopyCount = n;
}

// The copy `sPickName` against this console now. False: it cannot be used (the reason is in r).
bool KeepInspect(int* r) {
    memset(&sInfo, 0, sizeof sInfo);
    if (!sPickName[0]) {
        *r = MP_KS_NOT_FOUND;
        return false;
    }
    *r = mp_keepsafe_inspect(sPickName, &sInfo);
    return *r == MP_KS_OK;
}
// Which rows a copy offers: a Team File that is neither blank in the copy nor already the same here, the account, the settings.
uint8_t KeepSlotsOffered() { return static_cast<uint8_t>(~(sInfo.slotBlank | sInfo.slotSame) & 0x3F); }
bool KeepAnythingOffered() { return KeepSlotsOffered() || sInfo.account || (sInfo.settings & 7); }

// Bringing a copy back changes the Team Files on the card: the game's save image must not be a multiplayer one (a later autosave
// would write it over the new files), and no session or unanswered "keep this progress?" may be in the way.
bool KeepReady() {
    if (mp_active() && UiPhase() != MP_UI_LOBBY) return false;   // (Which File? is asked in the lobby: no Team File is loaded before Start, so it is safe there; never while playing)
    if ((MpSaveGame::isSessionRoot() || MpSaveGame::heldAfterSession()) && !MpSaveGame::leaveSessionRoot()) return false;
    return !MpSaveGame::leaveDecisionPending();
}

Word MakeError(int r) {
    return r == MP_KS_NOTHING ? W_KEEP_NOTHING : (r == MP_KS_BUSY ? W_KEEP_BUSY : W_KEEP_FAILED);   // (a full card, a failed write: "didn't work, your data was not changed")
}
Word BackError(int r) {
    switch (r) {
    case MP_KS_NEWER: return W_BK_NEWER;
    case MP_KS_BAD_FILE: case MP_KS_NOT_FOUND: return W_BK_BAD;
    case MP_KS_NO_SPACE: return W_BK_NO_SPACE;
    case MP_KS_NOTHING: return W_BK_NOTHING;
    case MP_KS_BUSY: return W_KEEP_BUSY;
    case kTooMany: return W_TOO_MANY;
    default: return W_BK_ROLLED;   // (IO, VERIFY: keepsafe has changed nothing)
    }
}

// A message window for one of the words above (flow: becomes ask(P, MP_T_..) when the ids exist). The window text goes into the
// dynamic slot the core's own dynamic windows use.
void RawWindow(Io* io, Dlg kind, Word w, const Args& a) {
    char16_t tmp[320];
    txt::format(tmp, 320, kWordIds[w], a);
    char16_t* dyn = const_cast<char16_t*>(txt::findDialog("MP_DYN0"));
    if (dyn) txt::toWindowText(dyn, 320, tmp, true);
    if (io) io->dialogOpen(kind, "MP_DYN0");
}
}  // namespace


namespace {
// One item brought back from the copy `sPickName` (what: 0 a Team File = arg, 1 the online account, 2 a settings file = arg). The game's side
// (the Team File reload, the menus' caches, the running settings, the account service) has run when this returns.
enum KeepWhat { KW_SLOT = 0, KW_ACCOUNT = 1, KW_SETTINGS = 2 };
int KeepDo(int what, int arg, int confirmed, char* before, size_t cap) {
    if (!KeepReady()) return MP_KS_BUSY;
    switch (what) {
    case KW_SLOT: return mp_keepsafe_bring_back_slot(sPickName, arg, confirmed, before, cap);
    case KW_ACCOUNT: return mp_keepsafe_use_account(sPickName, confirmed);
    default: return mp_keepsafe_bring_back_settings(sPickName, arg, before, cap);
    }
}
Word KeepResultWord(int what, int r, bool hadBefore) {
    if (r == MP_KS_OK) return what == KW_SLOT ? (hadBefore ? W_D_SLOT_BACK : W_D_SLOT_BACK_PLAIN) : (what == KW_ACCOUNT ? W_D_ACCOUNT_DONE : W_D_SET_DONE);
    if (r == MP_KS_NOTHING) return what == KW_SLOT ? W_D_SLOT_SAME : (what == KW_ACCOUNT ? W_D_ACCOUNT_SAME : W_BK_NOTHING);
    return BackError(r);
}
const char* const kKeepIds[3] = {"MP_KS_SLOT", "MP_KS_ACCOUNT", "MP_KS_SETTINGS"};
Word KeepSettingsQuestion(int bit) { return bit == MP_KS_SET_CONFIG ? W_Q_SET_CONFIG : (bit == MP_KS_SET_FLOW ? W_Q_SET_FLOW : W_Q_SET_COOP); }
}  // namespace

// keepsafe API: the question before a Team File that is in use here is replaced (the one that says this console's file has more is asked
// when the copy lacks progress this console's file has)
#define M_KEEP_ASK_SLOT(S)                                                                                         \
    do {                                                                                                          \
        Args qa_;                                                                                                 \
        qa_.num = (S);                                                                                            \
        M_ASK_WORD(P_BK_SLOT, "MP_KS_ASK_SLOT", (sInfo.slotLacks >> ((S) > 0 ? (S) - 1 : 0)) & 1 ? W_Q_TEAM_DIFFERENT : W_Q_SLOT_REPLACE, qa_, Dlg::YesNo, static_cast<uint8_t>(S)); \
    } while (0)

// keepsafe API: bring the item back and say what happened (a window with OK); the copy and the list are read again; a copy with nothing
// left to bring back goes back to the list. CONFIRMED 0 and the module asks for a confirmation: the question is asked (a Team File).
#define M_KEEP_DO(WHAT, ARG, CONFIRMED)                                                                            \
    do {                                                                                                          \
        char before_[96];                                                                                         \
        before_[0] = 0;                                                                                           \
        const int r_ = KeepDo((WHAT), (ARG), (CONFIRMED), before_, sizeof before_);                               \
        mp_log("ui: keep item %d/%d r=%d", (WHAT), (ARG), r_);                                                    \
        if (r_ == MP_KS_CONFIRM && !(CONFIRMED) && (WHAT) == KW_SLOT) {                                           \
            M_KEEP_ASK_SLOT(ARG);                                                                                 \
            break;                                                                                                \
        }                                                                                                         \
        mStarsValid = false;                                                                                      \
        KeepLoad();                                                                                               \
        int ri_ = 0;                                                                                              \
        if (!KeepInspect(&ri_) || !KeepAnythingOffered()) popTo(S_KEEPLIST);                                      \
        else top().cursor = 0;                                                                                    \
        Args ra_;                                                                                                 \
        ra_.num = (ARG);                                                                                          \
        if (mIo) mIo->sound(r_ == MP_KS_OK ? Se::Success : Se::Buzzer);                                           \
        M_ASK_WORD(P_BK_DONE, kKeepIds[(WHAT)], KeepResultWord((WHAT), r_, before_[0] != 0), ra_, Dlg::Ok, 0);    \
    } while (0)

// m-files: the window for a Word (the same bookkeeping as Sequence::ask)
#define M_ASK_WORD(PURPOSE, ID, W, ARGS, KIND, PARAM)                                                              \
    do {                                                                                                          \
        mDlg = (PURPOSE);                                                                                         \
        mDlgParam = (PARAM);                                                                                      \
        snprintf(mLastDialogId, sizeof mLastDialogId, "%s", ID);                                                  \
        mp_log("ui: window %s%s", ID, (KIND) == Dlg::YesNo ? " (yes/no)" : "");                                   \
        RawWindow(mIo, (KIND), (W), (ARGS));                                                                      \
    } while (0)

// m-files: "Save this progress to your File N?" names the file and the Power Stars it has now (read again now) and the progress's
#define M_ASK_SAVE_MINE(FILE)                                                                                         \
    do {                                                                                                              \
        const bool rec_ = top().param != SP_LEAVE;                                                                    \
        const int own_ = ReadSp(FILE), prog_ = rec_ ? mRecovery.stars : mLeave.stars;                                  \
        char sb_[12], pb_[12];                                                                                        \
        snprintf(sb_, sizeof sb_, "%d", own_);                                                                        \
        snprintf(pb_, sizeof pb_, "%d", prog_);                                                                       \
        Args a_;                                                                                                      \
        a_.num = (FILE);                                                                                              \
        a_.str = sb_;                                                                                                 \
        a_.str2 = pb_;                                                                                                \
        if (own_ < 0) {                                                                                               \
            if (mIo) mIo->sound(Se::Buzzer);                                                                          \
            M_ASK_WORD(P_INFO, "MP_FILE_NOT_READY", W_FILE_NOT_READY, Args(), Dlg::Ok, 0);                           \
        } else {                                                                                                      \
            M_ASK_WORD(P_SAVE_MINE, "MP_SAVE_MINE", W_SAVE_MINE, a_, Dlg::YesNo, 0);                                  \
        }                                                                                                             \
    } while (0)

// ------------------------------------------------------------------ stack
void Sequence::refreshStars() const {
    for (int i = 1; i <= 6; i++) {
        mCoopStarsCache[i - 1] = static_cast<int16_t>(ReadCoop(i));
        mSpStarsCache[i - 1] = static_cast<int16_t>(ReadSp(i));
    }
    mStarsValid = true;
    KeepLoad();   // menus4: the file screens offer "Bring Back a Copy" where a file cannot be read: read once per screen, with the files
}

int Sequence::spStars(int file) const {
    if (file < 1 || file > 6) return -1;
    if (!mStarsValid) refreshStars();   // populate cache on first miss so no screen can ever stall on per-frame disk reads
    return mSpStarsCache[file - 1];
}

int Sequence::coopStars(int slot) const {
    if (slot < 1 || slot > 6) return -1;
    if (!mStarsValid) refreshStars();   // flow (review 2 #2): read once into cache, never per frame on any screen
    if (mDepth > 0 && top().scr == S_SLOT && top().param == SP_LEAVE) {   // (the offer's own numbers; a file that cannot be read is in the cache)
        if (Bad(mCoopStarsCache[slot - 1])) return kUnreadable;
        return mLeave.coopStars[slot - 1];
    }
    return mCoopStarsCache[slot - 1];
}

// flow: the host's file is chosen (Which file?, one of my files, a co-op slot to copy into): the session starts on it now.
void Sequence::saveChosen() {
    mp_log("ui: save choice kind=%d coop=%d sp=%d", mSaveKind, mCoopSlot, mSpFile);
    startSession();
}

void Sequence::defaultCoopSave() {
    mSaveKind = SAVE_COOP;
    mCoopSlot = 0;
    const int last = FlowStore().lastCoop;   // flow: the co-op file played last, while it exists
    if (last >= 1 && last <= 6 && coopStars(last) >= 0) {
        mCoopSlot = last;
        mCoopContinue = true;
        return;
    }
    int empty = 0;
    for (int i = 1; i <= 6 && mCoopSlot == 0; i++) {
        if (coopStars(i) >= 0) mCoopSlot = i;
        else if (empty == 0 && !Bad(coopStars(i))) empty = i;   // (m-files: a file that could not be read is never the "new" one)
    }
    mCoopContinue = mCoopSlot != 0;
    if (mCoopSlot == 0) mCoopSlot = empty;
    if (mCoopSlot == 0) {   // m-files: none of the six can be used (all could not be read): no co-op choice at all; Which file? still offers My Files
        mSaveKind = SAVE_NONE;
        mCoopContinue = false;
    }
}

bool Sequence::prepareHostSave() {
    bool ok = false;
    // m-files: the files this choice would write to or play on are read again NOW, whatever the screen showed: a file that cannot be
    // read is never written to, replaced or played on (the guarantee sits here, at the write, not only in the rows)
    const bool coopTarget = mSaveKind == SAVE_COOP || mSaveKind == SAVE_COPY;
    const bool targetOk = !coopTarget || (mCoopSlot >= 1 && mCoopSlot <= 6 && !Bad(ReadCoop(mCoopSlot)));
    const bool sourceOk = mSaveKind == SAVE_NONE ? false : (mSaveKind == SAVE_COOP || (mSpFile >= 1 && mSpFile <= 6 && ReadSp(mSpFile) >= 0));
    if (!targetOk || !sourceOk) {
        mp_log("ui: host save refused: a file could not be read (kind=%d coop=%d sp=%d)", mSaveKind, mCoopSlot, mSpFile);
        mStarsValid = false;
        MpSaveGame::abandonHostChoice();
        return false;
    }
    switch (mSaveKind) {
    case SAVE_COOP: ok = MpSaveGame::enterSessionRoot(); break;
    case SAVE_COPY: ok = MpSaveGame::importSinglePlayerFile(mSpFile, mCoopSlot) && MpSaveGame::isSessionRoot(); break;
    case SAVE_MINE:
        // play on single-player file N: the single-player root stays; a backup first, then the save module knows
        ok = (!MpSaveGame::isSessionRoot() && !MpSaveGame::heldAfterSession()) || MpSaveGame::leaveSessionRoot();   // (a stale image is reloaded)
        ok = ok && MpSaveGame::backupSinglePlayerFile();
        if (ok) MpSaveGame::beginSinglePlayerSession(mSpFile);
        break;
    default: break;
    }
    mp_log("ui: host save kind=%d coop=%d sp=%d %s", mSaveKind, mCoopSlot, mSpFile, ok ? "ready" : "FAILED");
    mStarsValid = false;   // (an import / a root switch: read again)
    if (!ok) MpSaveGame::abandonHostChoice();   // gamefix: a failed preparation leaves no "Play on my file" behind
    return ok;
}

// Leaving (or the session ended): anyone can save the progress (MpSaveGame decides whether there is something to save).
void Sequence::offerSave() {
    memset(&mLeave, 0, sizeof mLeave);
    if (!MpSaveGame::offerSaveOnLeave(&mLeave) || !mLeave.offer) {
        if (!mFromFileSelect) close();
        return;
    }
    ask(P_SAVE_ASK, MP_T_DLG_SAVE_ASK, Args(), Dlg::YesNo);
}

// savebug: the player answered Yes to "this file has more stars" (P_SP_FEWER) for the save that follows. Only then does the save layer let a
// recovery file replace a single-player file that has more stars or was saved later (it checks again at the keep: a file that changed since
// the offer is refused, the player hears the buzzer and can try again).
static bool sSpFewerYes = false;

void Sequence::saveTo(int choice, int slot) {
    const bool recover = mDepth > 0 && top().scr == S_SLOT && top().param == SP_RECOVER;
    // m-files: read the target again at the write: a file that could not be read is never written to (a co-op file), and a
    // single-player file is only written back to while it still reads as a file in use
    const int single = recover ? mRecovery.sourceSlot : mLeave.singleSlot;
    const bool targetOk = choice == MpSaveGame::LEAVE_SAVE_COOP ? (slot >= 1 && slot <= 6 && !Bad(ReadCoop(slot)))
                                                                : (choice == MpSaveGame::LEAVE_SAVE_SINGLE ? single >= 1 && single <= 6 && ReadSp(single) >= 0 : true);
    if (!targetOk) {
        mp_log("ui: progress NOT saved: the file could not be read (choice=%d slot=%d)", choice, slot);
        mStarsValid = false;
        sSpFewerYes = false;
        if (mIo) mIo->sound(Se::Buzzer);
        M_ASK_WORD(P_INFO, "MP_FILE_NOT_READY", W_FILE_NOT_READY, Args(), Dlg::Ok, 0);   // (the question stays: another file can be picked)
        return;
    }
    const bool ok = recover ? MpSaveGame::recoveryKeep(slot, choice, choice == MpSaveGame::LEAVE_SAVE_SINGLE && sSpFewerYes) : MpSaveGame::saveOnLeave(choice, slot);
    sSpFewerYes = false;
    mStarsValid = false;   // (a file was written: read again)
    mp_log("ui: progress saved choice=%d slot=%d %s", choice, slot, ok ? "ok" : "FAILED");
    if (ok && choice == MpSaveGame::LEAVE_SAVE_COOP) {   // flow: the co-op file's side data follows what is in it now
        if (recover) {
            const int keep = mSessionCount;
            mSessionCount = 0;   // (a recovery file does not say who played: the row reads "Co-op File N")
            mLastGalaxy = -1;
            writeCoopMeta(slot);
            mSessionCount = keep;
        } else {
            writeCoopMeta(slot);
        }
    }
    if (mIo) mIo->sound(ok ? Se::Success : Se::Buzzer);
    if (recover || mFromFileSelect) pop();
    else close();
    Args a;
    a.num = slot;
    say(recover ? P_TIP_NEXT : P_KEPT, ok ? (choice == MpSaveGame::LEAVE_SAVE_COOP ? MP_T_DLG_KEPT : MP_T_DLG_SAVED) : MP_T_DLG_KEEP_FAILED, a);
}

// A co-op slot was chosen on the slot screen: warn (fewer stars / replace) or go.
void Sequence::slotChosen(int slot) {
    const int purpose = top().param;
    const int there = coopStars(slot);
    if (Bad(there) || Bad(ReadCoop(slot))) {   // m-files: a file that could not be read is never a place to write
        if (mIo) mIo->sound(Se::Buzzer);
        return;
    }
    Args a;
    a.num = slot;
    if (purpose == SP_COPY) {
        if (MpSaveGame::compareImportProgress(mSpFile, slot) < 0) {
            ask(P_IMPORT_FEWER, MP_T_DLG_IMPORT_FEWER, a, Dlg::YesNo, slot);
            return;
        }
        if (there >= 0) {
            ask(P_OVERWRITE, MP_T_DLG_OVERWRITE, a, Dlg::YesNo, slot);
            return;
        }
        mSaveKind = SAVE_COPY;
        mCoopSlot = slot;
        saveChosen();
        return;
    }
    const int mine = purpose == SP_LEAVE ? mLeave.stars : mRecovery.stars;
    if (there >= 0 && mine < there) {
        ask(P_FEWER, MP_T_DLG_FEWER_STARS, a, Dlg::YesNo, slot);
        return;
    }
    if (there >= 0) {
        ask(P_OVERWRITE, MP_T_DLG_OVERWRITE, a, Dlg::YesNo, slot);
        return;
    }
    saveTo(MpSaveGame::LEAVE_SAVE_COOP, slot);
}

void Sequence::writeCoopMeta(int slot) {
    if (!mp_coop_meta_set || slot < 1 || slot > 6) return;
    MpCoopMeta m;
    memset(&m, 0, sizeof m);
    m.count = static_cast<uint8_t>(mSessionCount);
    for (int i = 0; i < mSessionCount && i < 4; i++) {
        snprintf(m.names[i], sizeof m.names[i], "%s", mSessionNames[i]);
        m.looks[i] = mSessionLooks[i];
    }
    m.galaxy = static_cast<int8_t>(mLastGalaxy);
    mp_coop_meta_set(slot, &m);
}

void Sequence::onFileSelect() {
    if (mp_active()) return;
    mRootPending = false;
    MpSaveGame::abandonHostChoice();   // gamefix: back at the file select with no session: no stale "Play on my file"
    if (MpSaveGame::isSessionRoot() || MpSaveGame::heldAfterSession()) {
        if (MpSaveGame::leaveSessionRoot()) {
            mp_log("ui: back at the file select: single-player saves again");
        } else {
            // fix2: a save sequence still busy, or a single-player file that cannot be loaded: try again (fileSelectTick)
            mRootPending = true;
            mRootWarned = false;
            mRootSince = mRootTry = mFrame;
            mp_log("ui: back at the file select: save root switch failed, retrying");
        }
    }
    mWasPlaying = false;
    mOriginalHost = false;
}

// flow (review 2 #4): the entry screen's Single Player. A switch back to the single-player root that is still pending is tried now;
// if it still fails, the game's window says the file can't be opened and Single Player waits (the retries go on).
bool Sequence::singlePlayerGate() {
    if (mp_active() || mDlg || mKb) return mp_active() || mDlg || mKb;
    if (!mRootPending && !(MpSaveGame::isSessionRoot() || MpSaveGame::heldAfterSession())) return false;
    if (!(MpSaveGame::isSessionRoot() || MpSaveGame::heldAfterSession()) || MpSaveGame::leaveSessionRoot()) {
        mRootPending = false;
        mp_log("ui: single player: single-player saves again");
        return false;
    }
    mRootPending = true;
    mRootWarned = true;
    mRootTry = mFrame;
    mp_log("ui: single player blocked: single-player file unavailable");
    if (mIo) mIo->sound(Se::Buzzer);
    say(P_INFO, MP_T_DLG_SP_UNAVAILABLE);
    return true;
}

bool Sequence::fileSelectTick(bool canWarn) {
    if (!mRootPending || mp_active()) return false;
    // fix3: a warning that is due waits for a turn where the file select can hold its input (not on the title, not in the
    // middle of its own windows): the same A press must never also act on the files behind our window
    const bool warnDue = !mRootWarned && mFrame - mRootSince >= kRootWarn;
    if (mFrame - mRootTry >= (mRootWarned ? kRootRetrySlow : kRootRetry)) {
        mRootTry = mFrame;
        if (!(MpSaveGame::isSessionRoot() || MpSaveGame::heldAfterSession()) || MpSaveGame::leaveSessionRoot()) {
            mRootPending = false;
            mp_log("ui: save root switched after %u frames: single-player saves again", static_cast<unsigned>(mFrame - mRootSince));
            return false;
        }
    }
    if (warnDue && canWarn && !mDlg && !mKb && mDepth == 0) {
        // progress is never silently unsaved: the game's message window says so (and the retries go on, slower)
        mRootWarned = true;
        mp_log("ui: single-player file unavailable (warned)");
        if (mIo) mIo->sound(Se::Buzzer);
        say(P_INFO, MP_T_DLG_SP_UNAVAILABLE);
        return true;
    }
    return false;
}

bool Sequence::itemsFiles(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    auto anyUnreadable = [this] {
        for (int i = 1; i <= 6; i++)
            if (Bad(coopStars(i))) return true;
        return false;
    };
    switch (s) {
    case S_SAVE:
        {   // flow: Which file?: the co-op files in use, one new one, "Start from my own file..."
        int firstEmpty = 0;
        for (int i = 1; i <= 6; i++) {
            if (coopStars(i) >= 0 || Bad(coopStars(i))) add(I_COOP1 + i - 1);   // (m-files: one that could not be read stays in its place, shown as such)
            else if (firstEmpty == 0) firstEmpty = i;
        }
        if (firstEmpty) add(I_COOP1 + firstEmpty - 1);
        add(I_OWNFILE);
        // (uxfix #5: Safe Copies is a Triangle action of this screen, not one of its choices: pressFiles)
        if (sCopyCount > 0 && anyUnreadable()) add(I_KEEP_BACK);   // menus4: right where it is needed, and only when a copy exists
        break;
    }
    case S_KEEP:
        add(I_KEEP_MAKE), add(I_KEEP_SHARE);
        if (sCopyCount > 0) add(I_KEEP_BACK);   // flow (owner: no dead rows): absent while there is no copy
        break;
    case S_KEEPLIST:
        for (int i = 0; i < sCopyCount && i < kMaxCopies; i++) add(I_KEEP0 + i);
        break;
    case S_KEEPCOPY:   // only what can be brought back
        for (int i = 0; i < 6; i++)
            if (KeepSlotsOffered() & (1 << i)) add(I_KC_SLOT1 + i);
        if (sInfo.account) add(I_KC_ACCT);
        for (int b = 0; b < 3; b++)
            if (sInfo.settings & (1 << b)) add(I_KC_SET1 + b);
        break;
    case S_SPFILE:
        for (int i = 0; i < 6; i++) add(I_SPFILE1 + i);   // entry
        break;
    case S_FILEUSE:
        add(I_USE_COPY), add(I_USE_PLAY); break;               // flow
    case S_SLOT:
        for (int i = 0; i < 6; i++) add(I_SLOT1 + i);
        if (SingleSource(param, mLeave, mRecovery, mSaveKind == SAVE_MINE && mOriginalHost, mSpFile, [this](int f) { return spStars(f); })) add(I_SLOT_SINGLE);
        if (sCopyCount > 0 && (param == SP_RECOVER || anyUnreadable())) add(I_KEEP_BACK);   // menus4: the after-a-crash question and any unreadable Team File
        break;
    default: return false;
    }
    (void)param, (void)add;
    return true;
}

bool Sequence::chooseFiles(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    (void)decide;
    switch (item) {
    case I_KEEP:   // m-files: the Multiplayer menu's Safe Copies row
        decide();
        KeepLoad();
        push(S_KEEP);
        return true;
    case I_KEEP_MAKE:
    case I_KEEP_SHARE:
        {   // one press, no question: all the Team Files and the online account in one new file (keepsafe writes it, reads it back and checks it);
        // "to Share" leaves the account out
        decide();
        char name[96];
        name[0] = 0;
        const bool share = item == I_KEEP_SHARE;
        const int r = mp_keepsafe_make_copy(share ? MP_KS_COPY_NO_ACCOUNT : 0, name, sizeof name);
        mp_log("ui: safe copy made r=%d%s", r, share ? " (to share)" : "");
        KeepLoad();
        if (io) io->sound(r == MP_KS_OK ? Se::Success : Se::Buzzer);
        M_ASK_WORD(P_BK_DONE, share ? "MP_KEEP_MADE_SHARE" : "MP_KEEP_MADE", r == MP_KS_OK ? (share ? W_MADE_SHARE : W_KEEP_MADE) : MakeError(r), Args(), Dlg::Ok, 0);
        return true;
    }
    case I_KEEP_BACK:
        if (sCopyCount <= 0) {   // (nothing to bring back: the row is not offered; this is only a stale touch)
            if (io) io->sound(Se::Buzzer);
            return true;
        }
        decide();
        push(S_KEEPLIST);
        return true;
    case I_OWNFILE:
        {   // flow: "Start from my own file...": which one (the cursor on the one picked last, else the first that exists)
        decide();
        push(S_SPFILE);
        int at = mSpFile > 0 ? mSpFile - 1 : -1;
        for (int i = 0; at < 0 && i < 6; i++)
            if (spStars(i + 1) >= 0) at = i;
        top().cursor = static_cast<int8_t>(at < 0 ? 0 : at);
        fitScroll(top());
        return true;
    }
    case I_USE_COPY:
        {   // flow: a co-op copy of my file: into the first empty co-op file, else the co-op file to replace is asked
        decide();
        int empty = 0, writable = 0;
        for (int i = 1; i <= 6; i++) {
            if (!Bad(coopStars(i))) ++writable;
            if (!empty && coopStars(i) == -1) empty = i;   // (m-files: only a file that is really not there; one that could not be read is not "empty")
        }
        mp_log("ui: single-player file %d picked for copy", mSpFile);
        if (empty) {
            mSaveKind = SAVE_COPY;
            mCoopSlot = empty;
            saveChosen();
            return true;
        }
        if (!writable) {   // m-files: every Team File could not be read: nowhere to put a copy (nothing is offered, nothing is changed)
            if (io) io->sound(Se::Buzzer);
            M_ASK_WORD(P_INFO, "MP_NO_ROOM_COPY", W_NO_ROOM_COPY, Args(), Dlg::Ok, 0);
            return true;
        }
        push(S_SLOT, SP_COPY);
        return true;
    }
    case I_USE_PLAY:
        {   // flow: play on this file. m-files: the question names the file and its Power Stars (the file is read again now)
        const int stars = ReadSp(mSpFile);
        if (stars < 0) {   // (it could not be read just now, or is gone: nothing can start on it)
            if (io) io->sound(Se::Buzzer);
            mStarsValid = false;
            M_ASK_WORD(P_INFO, "MP_FILE_NOT_READY", W_FILE_NOT_READY, Args(), Dlg::Ok, 0);
            return true;
        }
        decide();
        mp_log("ui: single-player file %d picked for play", mSpFile);
        char sbuf[12];
        snprintf(sbuf, sizeof sbuf, "%d", stars);
        Args a;
        a.num = mSpFile;
        a.str = sbuf;
        M_ASK_WORD(P_PLAY_MINE, "MP_PLAY_MINE", W_PLAY_MINE, a, Dlg::YesNo, 0);
        return true;
    }
    default: break;
    }
    if (item >= I_KEEP0 && item < I_KEEP_END) {   // a copy: what it holds against this console is read now; only what can be brought back is shown
        const int i = item - I_KEEP0;
        if (i >= sCopyCount) return true;
        snprintf(sPickName, sizeof sPickName, "%s", sCopies[i].name);
        int r = 0;
        if (!KeepInspect(&r)) {   // (every byte is checked before anything is offered: a damaged copy, one from a newer game)
            mp_log("ui: safe copy %d refused r=%d", i, r);
            if (io) io->sound(Se::Buzzer);
            M_ASK_WORD(P_BK_DONE, "MP_KS_REFUSED", BackError(r), Args(), Dlg::Ok, 0);
            return true;
        }
        if (!KeepAnythingOffered()) {
            if (io) io->sound(Se::Buzzer);
            M_ASK_WORD(P_BK_DONE, "MP_KS_NOTHING_IN", W_BK_NOTHING, Args(), Dlg::Ok, 0);
            return true;
        }
        decide();
        push(S_KEEPCOPY);
        return true;
    }
    if (item >= I_KC_SLOT1 && item < I_KC_SLOT1 + 6) {   // a Team File of the copy: asked first when this console's file is in use, else brought back at once
        const int slot = item - I_KC_SLOT1 + 1;
        if (!(KeepSlotsOffered() & (1 << (slot - 1)))) return true;
        decide();
        if (sInfo.slotInUse & (1 << (slot - 1))) M_KEEP_ASK_SLOT(slot);
        else M_KEEP_DO(KW_SLOT, slot, 0);
        return true;
    }
    if (item == I_KC_ACCT) {   // always asked: the online account is never part of anything else
        if (!sInfo.account) return true;
        decide();
        M_ASK_WORD(P_BK_ACCOUNT, "MP_KS_ASK_ACCOUNT", W_BK_ACCOUNT, Args(), Dlg::YesNo, 0);
        return true;
    }
    if (item >= I_KC_SET1 && item < I_KC_SET1 + 3) {   // one settings file
        const int bit = 1 << (item - I_KC_SET1);
        if (!(sInfo.settings & bit)) return true;
        decide();
        M_ASK_WORD(P_BK_SET, "MP_KS_ASK_SETTINGS", KeepSettingsQuestion(bit), Args(), Dlg::YesNo, static_cast<uint8_t>(bit));
        return true;
    }
    if (item >= I_COOP1 && item < I_COOP1 + 6) {   // flow: Which file?: a co-op file (in use: continue; empty: a new one)
        if (Bad(coopStars(item - I_COOP1 + 1))) {   // m-files: a file that could not be read: no choice is made, nothing happens
            if (io) io->sound(Se::Buzzer);
            return true;
        }
        decide();
        mSaveKind = SAVE_COOP;
        mCoopSlot = item - I_COOP1 + 1;
        mCoopContinue = coopStars(mCoopSlot) >= 0;
        saveChosen();
        return true;
    }
    if (item >= I_SLOT1 && item < I_SLOT1 + 6) {
        decide();
        slotChosen(item - I_SLOT1 + 1);
        return true;
    }
    if (item >= I_SPFILE1 && item < I_SPFILE_END) {   // flow: one of my files (an empty one cannot be): copy it or play on it
        const int file = item - I_SPFILE1 + 1;
        if (spStars(file) < 0) {   // (never created, or unreadable: the game's own file select would not load it either)
            if (io) io->sound(Se::Buzzer);
            return true;
        }
        decide();
        mSpFile = file;
        push(S_FILEUSE);
        return true;
    }
    if (item == I_SLOT_SINGLE) {
        const bool rec = top().param != SP_LEAVE;
        const int file = rec ? mRecovery.sourceSlot : mLeave.singleSlot;
        if (!SingleSource(top().param, mLeave, mRecovery, mSaveKind == SAVE_MINE && mOriginalHost, mSpFile, [this](int f) { return spStars(f); })) {
            if (io) io->sound(Se::Buzzer);   // (m-files: not offered, so not chosen: a stale row)
            return true;
        }
        decide();
        sSpFewerYes = false;   // (a new choice: an earlier Yes does not carry over)
        Args a;
        a.num = file;
        // gamefix: a single-player file with more Power Stars, or saved after this progress was made, is asked about first
        if (MpSaveGate::singleOverwriteWarning(rec ? mRecovery.stars : mLeave.stars, rec ? mRecovery.spStars : mLeave.singleStars,
                                               rec && mRecovery.spNewer)) {
            ask(P_SP_FEWER, MP_T_DLG_SP_FEWER, a, Dlg::YesNo);
            return true;
        }
        M_ASK_SAVE_MINE(file);
        return true;
    }
    return false;
}

bool Sequence::dialogFiles(int purpose, bool yes, int param) {
    switch (purpose) {
    case P_PLAY_MINE:
        if (yes) {
            mSaveKind = SAVE_MINE;
            saveChosen();
        }
        return true;
    case P_IMPORT_FEWER:
    case P_OVERWRITE:
    case P_FEWER:
        if (!yes || mDepth == 0 || top().scr != S_SLOT) return true;
        if (top().param == SP_COPY) {
            mSaveKind = SAVE_COPY;
            mCoopSlot = param;
            saveChosen();
            return true;
        }
        saveTo(MpSaveGame::LEAVE_SAVE_COOP, param);
        return true;
    case P_SP_FEWER:
        // gamefix: Yes goes on to the usual "save to your file N? A backup is made first"
        if (yes && mDepth > 0 && top().scr == S_SLOT) {
            sSpFewerYes = true;
            const int file = top().param == SP_LEAVE ? mLeave.singleSlot : mRecovery.sourceSlot;
            M_ASK_SAVE_MINE(file);
        }
        return true;
    case P_SAVE_MINE:
        if (yes && mDepth > 0 && top().scr == S_SLOT) saveTo(MpSaveGame::LEAVE_SAVE_SINGLE, 0);
        return true;
    case P_SAVE_ASK:
        if (yes) {
            if (mDepth == 0) mFade = 0;
            if (!mFromFileSelect) mDepth = 0;
            push(S_SLOT, SP_LEAVE);
        } else {
            const bool ok = MpSaveGame::saveOnLeave(MpSaveGame::LEAVE_DONT_SAVE, 0);
            mp_log("ui: progress not saved (%s)", ok ? "discarded" : "FAILED");
            if (!mFromFileSelect) close();
        }
        return true;
    case P_RECOVERY:
        if (yes) {
            push(S_SLOT, SP_RECOVER);
        } else {
            const bool ok = MpSaveGame::recoveryDiscard();
            mp_log("ui: recovery discarded %s", ok ? "ok" : "FAILED");
            showTip();
        }
        return true;
    case P_KEPT:
        if (!mFromFileSelect) close();
        return true;
    case P_BK_SLOT:
        if (yes) M_KEEP_DO(KW_SLOT, param, 1);
        return true;
    case P_BK_ACCOUNT:
        if (yes) M_KEEP_DO(KW_ACCOUNT, 0, 1);
        return true;
    case P_BK_SET:
        if (yes) M_KEEP_DO(KW_SETTINGS, param, 1);
        return true;
    case P_BK_DONE:
    case P_BK_NOTICE:
        return true;
    case P_ENDED:
        if (param) offerSave();
        else if (!mFromFileSelect) close();
        return true;
    default: return false;
    }
    return true;
}

bool Sequence::keyboardFiles(int purpose, const char* text) {
    (void)purpose;
    (void)text;
    return false;
}

// keepsafe API (share/keep/API.md, "the exact order at entry"): every time the Multiplayer menu is entered, before its first frame is used for
// anything: the notice is taken ONCE and shown as a card the player presses OK on (a card, not a toast and not a hint row); then
// (uxfix #5: the line that "no copy from before this update" used to keep on the Multiplayer menu is gone: the card says it once.)
// (This runs from the file family's own tick, so the Main family needs no change.)
bool Sequence::tickFiles(Scr scr, const Input& in, bool* stop) {
    (void)in;
    static uint32_t sLastFrame = 0, sVisit = ~0u;
    if (mFrame < sLastFrame) sVisit = ~0u;   // (a new sequence: the menus were started again)
    sLastFrame = mFrame;
    if (scr != S_MAIN || mDlg || mKb || mp_active() || sVisit == mClosedAt) return false;
    sVisit = mClosedAt;
    const int notice = mp_keepsafe_notice_take();   // 1. once, first; the value is kept here
    if (notice == kNoticeNoSpace || notice == MP_KS_NOTICE_NO_COPY) {   // 2. a card with one OK
        mp_log("ui: keep notice %d", notice);
        M_ASK_WORD(P_BK_NOTICE, "MP_KS_NOTICE", notice == kNoticeNoSpace ? W_KS_NOTICE : W_NO_COPY, Args(), Dlg::Ok, 0);
        *stop = true;
        return true;
    }
    return false;
}

bool Sequence::labelFiles(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    (void)f;
    switch (item) {
    case I_USE_COPY:
        Text(r.label, kLabel, MP_T_USE_COPY); return true;
    case I_USE_PLAY:
        Text(r.label, kLabel, MP_T_USE_PLAY); return true;
    case I_OWNFILE:
        Text(r.label, kLabel, MP_T_OWN_FILE); r.icon = ICON_FILE; return true;
    case I_KEEP:   // m-files: the Multiplayer menu's row
        Wd(r.label, kLabel, W_KEEP_ROW); r.icon = ICON_FILE; return true;
    case I_KEEP_MAKE:
        Wd(r.label, kLabel, W_KEEP_MAKE);
        return true;
    case I_KEEP_SHARE:
        Wd(r.label, kLabel, W_KEEP_SHARE); return true;
    case I_KEEP_BACK:
        Wd(r.label, kLabel, W_KEEP_BACK); return true;
    case I_KC_ACCT:
        Wd(r.label, kLabel, W_ROW_ACCOUNT); r.icon = ICON_FILE; return true;
    default: break;
    }
    if (item >= I_KEEP0 && item < I_KEEP_END) {   // a copy: what made it (never a file name), its day
        const int i = item - I_KEEP0;
        if (i >= sCopyCount) return true;
        const MpKsCopy& c = sCopies[i];
        Args a;
        a.str = c.version;
        Wd(r.label, kLabel, c.kind == MP_KS_COPY_VERSION ? W_L_VERSION : (c.kind == MP_KS_COPY_BEFORE ? W_L_BEFORE : W_L_MADE), a);
        char d[24];
        FormatDay(c.created, d, sizeof d);
        txt::utf8To16(r.value, kValue, d);
        r.icon = ICON_FILE;
        return true;
    }
    if (item >= I_KC_SLOT1 && item < I_KC_SLOT1 + 6) {   // a Team File of the copy and its Power Stars
        const int s = item - I_KC_SLOT1;
        Num(r.label, kLabel, MP_T_COOP_FILE, s + 1);
        r.icon = ICON_FILE;
        if (sInfo.copyStars[s] >= 0) StarCount(r.value, kValue, sInfo.copyStars[s]);
        return true;
    }
    if (item >= I_KC_SET1 && item < I_KC_SET1 + 3) {
        const int b = item - I_KC_SET1;
        Wd(r.label, kLabel, b == 0 ? W_ROW_SET_CONFIG : (b == 1 ? W_ROW_SET_FLOW : W_ROW_SET_COOP));
        r.icon = ICON_FILE;
        return true;
    }
    if (item >= I_COOP1 && item < I_COOP1 + 6) {   // flow: Which file?: the players' names, the host's head, the stars
        const int slot = item - I_COOP1 + 1;
        const int stars = coopStars(slot);
        MpCoopMeta meta;
        memset(&meta, 0, sizeof meta);
        const bool known = stars >= 0 && mp_coop_meta_get && mp_coop_meta_get(slot, &meta) && meta.count > 0;
        if (Bad(stars)) {   // m-files: it is there and could not be read: not "New", not choosable
            Num(r.label, kLabel, MP_T_COOP_FILE, slot);
            r.icon = ICON_FILE;
            Wd(r.value, kValue, W_CANT_READ);
            r.marks |= MARK_DISABLED;
            return true;
        }
        if (stars < 0) {
            Text(r.label, kLabel, MP_T_NEW_COOP_FILE);
            r.icon = ICON_FILE;
        } else if (known) {
            char names[4 * (MP_NAME_BYTES + 3)];
            int n = 0;
            names[0] = 0;
            for (int i = 0; i < meta.count && i < 4; i++)
                n += snprintf(names + n, sizeof names - n, "%s%s", i ? (i + 1 == meta.count ? " & " : ", ") : "", meta.names[i]);
            txt::utf8To16(r.label, kLabel, names);
            r.icon = static_cast<uint8_t>(ICON_MARIO + (meta.looks[0] < MP_LOOK_COUNT ? meta.looks[0] : 0));
        } else {
            Num(r.label, kLabel, MP_T_COOP_FILE, slot);
            r.icon = ICON_FILE;
        }
        if (stars >= 0) StarCount(r.value, kValue, stars);
        if (slot == mCoopSlot) r.marks |= MARK_YOU;
        return true;
    }
    if (item >= I_SLOT1 && item < I_SLOT1 + 6) {
        const int slot = item - I_SLOT1 + 1;
        Num(r.label, kLabel, MP_T_COOP_FILE, slot);
        r.icon = ICON_FILE;
        const int stars = coopStars(slot);
        if (stars >= 0) StarCount(r.value, kValue, stars);
        else if (Bad(stars)) Wd(r.value, kValue, W_CANT_READ), r.marks |= MARK_DISABLED;   // m-files
        else Text(r.value, kValue, MP_T_NEW);
        return true;
    }
    if (item >= I_SPFILE1 && item < I_SPFILE_END) {   // entry: "File N", its stars or "New" (not choosable)
        const int file = item - I_SPFILE1 + 1;
        Num(r.label, kLabel, MP_T_SP_FILE, file);
        r.icon = ICON_FILE;
        const int stars = spStars(file);
        if (stars >= 0) {
            StarCount(r.value, kValue, stars);
        } else if (Bad(stars)) {   // m-files: said plainly, never "New"
            Wd(r.value, kValue, W_CANT_READ);
            r.marks |= MARK_DISABLED;
        } else {
            Text(r.value, kValue, MP_T_NEW);
            r.marks |= MARK_DISABLED;
        }
        if (file == mSpFile) r.marks |= MARK_YOU;
        return true;
    }
    if (item == I_SLOT_SINGLE) {
        Num(r.label, kLabel, MP_T_SAVE_TO_MY_FILE, f.param == SP_LEAVE ? mLeave.singleSlot : mRecovery.sourceSlot);
        r.icon = ICON_FILE;
        return true;
    }
    return false;
}

bool Sequence::modelFiles(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    auto label = [this, &f](int item, Row& r) { rowLabel(item, r, f); };
    (void)button, (void)hint, (void)label, (void)cursorItem, (void)count, (void)it;
    switch (f.scr) {
    case S_SAVE:
        {   // flow: Which file?
        Text(m->title, 48, MP_T_TITLE_SAVE);
        Text(m->subtitle, 64, ModeName(mMode));
        if (cursorItem >= I_COOP1 && cursorItem < I_COOP1 + 6) {
            const int slot = cursorItem - I_COOP1 + 1;
            MpCoopMeta meta;
            memset(&meta, 0, sizeof meta);
            if (Bad(coopStars(slot))) {   // m-files
                Wd(m->hint, 80, W_CANT_READ_HINT);
            } else if (coopStars(slot) < 0) {
                hint(MP_T_DESC_NEW_COOP);
            } else if (mIo && mp_coop_meta_get && mp_coop_meta_get(slot, &meta) && meta.galaxy >= 0) {
                char16_t g[40];
                g[0] = 0;
                mIo->galaxyName(meta.galaxy, g, 40);
                char16_t pre[48];
                Text(pre, 48, MP_T_LAST_PLAYED);
                int n = 0;
                for (const char16_t* q = pre; *q && n < 79; ++q) m->hint[n++] = *q;
                for (const char16_t* q = g; *q && n < 79; ++q) m->hint[n++] = *q;
                m->hint[n] = 0;
            } else {
                Num(m->hint, 80, MP_T_DESC_COOP_N, slot);
            }
        } else if (cursorItem == I_OWNFILE) {
            hint(MP_T_DESC_OWN_FILE);
        } else if (cursorItem == I_KEEP_BACK) {
            Wd(m->hint, 80, W_KEEP_H_BACK);
        }
        button(txt::PIC_CROSS, cursorItem == I_OWNFILE || cursorItem == I_KEEP_BACK ? MP_T_SELECT : MP_T_START), button(txt::PIC_TRIANGLE, MP_T_KS_KEEP_ROW), button(txt::PIC_CIRCLE, MP_T_BACK);   // uxfix #5: Triangle = Safe Copies
        break;
    }
    case S_SPFILE:
        // entry / flow: one of my files
        Text(m->title, 48, MP_T_TITLE_SPFILE);
        Text(m->subtitle, 64, MP_T_OWN_FILE);
        if (cursorItem >= I_SPFILE1 && cursorItem < I_SPFILE_END && Bad(spStars(cursorItem - I_SPFILE1 + 1))) Wd(m->hint, 80, W_SP_CANT_READ_HINT);   // m-files
        else hint(MP_T_DESC_SPFILE_PICK);
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    case S_FILEUSE:
        // flow: that file: a co-op copy (recommended) or play on it
        m->kind = KIND_BUTTONS;
        Num(m->title, 48, MP_T_SP_FILE, mSpFile);
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    case S_SLOT:
        Text(m->title, 48, MP_T_TITLE_SLOT);
        {   // m-files: on a file that could not be read, or when some could not be, the hint says so
            bool anyBad = false;
            for (int i = 1; i <= 6; i++) anyBad = anyBad || Bad(coopStars(i));
            if (cursorItem == I_KEEP_BACK) Wd(m->hint, 80, W_KEEP_H_BACK);
            else if (cursorItem >= I_SLOT1 && cursorItem < I_SLOT1 + 6 && Bad(coopStars(cursorItem - I_SLOT1 + 1))) Wd(m->hint, 80, W_CANT_READ_HINT);
            else if (anyBad) Wd(m->hint, 80, W_SLOTS_CANT_READ);
            else hint(f.param == SP_LEAVE ? MP_T_DESC_SLOT_LEAVE : (f.param == SP_RECOVER ? MP_T_DESC_SLOT_RECOVER : MP_T_DESC_SLOT));
        }
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    case S_KEEP:
        {   // Safe Copies: big buttons (Make a Copy, Make a Copy to Share, Bring Back a Copy when there is one); the line says what the one under the cursor does
        m->kind = KIND_BUTTONS;
        Wd(m->title, 48, W_KEEP_TITLE);
        if (cursorItem == I_KEEP_SHARE) Wd(m->hint, 80, W_KEEP_H_SHARE);
        else if (cursorItem == I_KEEP_BACK) Wd(m->hint, 80, W_KEEP_H_BACK);
        else Wd(m->hint, 80, W_KEEP_H_MAKE);
        GameLook(m);
        Copy16(m->line, 96, m->hint);
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    case S_KEEPLIST:
    case S_KEEPCOPY:
        {   // the copies, newest first (the line says what the one under the cursor holds); one copy: its items (the line says what the row does)
        Wd(m->title, 48, W_KEEP_LIST_TITLE);
        char16_t* line = m->hint;
        if (f.scr == S_KEEPLIST) {
            const int i = cursorItem - I_KEEP0;
            if (cursorItem >= I_KEEP0 && i < sCopyCount) {
                const MpKsCopy& c = sCopies[i];
                int n = 0;
                auto put = [&](const char16_t* t, bool comma) {
                    if (comma && n < 78) line[n++] = u',';
                    if (n > 0 && n < 78) line[n++] = u' ';
                    for (; *t && n < 79; ++t) line[n++] = *t;
                    line[n] = 0;
                };
                char16_t w[40];
                Wd(w, 40, W_H_HOLDS), put(w, false);
                bool any = false;
                if (c.teamFiles) Wd(w, 40, W_ROW_TEAMFILES), put(w, any), any = true;
                if (c.account) Wd(w, 40, W_ROW_ACCOUNT), put(w, any), any = true;
                if (c.settings) Wd(w, 40, W_ROW_SET_CONFIG), put(w, any), any = true;
                if (!any) line[0] = 0;
            }
        } else if (cursorItem >= I_KC_SLOT1 && cursorItem < I_KC_SLOT1 + 6) {
            const int s = cursorItem - I_KC_SLOT1;
            if (sInfo.slotInUse & (1 << s)) {
                char b[12];
                snprintf(b, sizeof b, "%d", sInfo.liveStars[s] < 0 ? 0 : sInfo.liveStars[s]);
                Args a;
                a.str = b;
                Wd(line, 80, W_H_SLOT_HERE, a);
            } else {
                Args a;
                a.num = s + 1;
                Wd(line, 80, W_H_SLOT_NEW, a);
            }
        } else if (cursorItem == I_KC_ACCT) {
            Wd(line, 80, W_H_ACCOUNT);
        }
        GameLook(m);
        Copy16(m->line, 96, m->hint);
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    default: return false;
    }
    // flow look (the first game pictures, 12 Nov: Which File? still had the title band, its line under it and two hands): the file
    // questions in the file select's look: the question in the bottom message, with the row's own line under it when it is short
    if (f.scr == S_SAVE || f.scr == S_SPFILE || f.scr == S_FILEUSE || f.scr == S_SLOT) GameLookFromTitle(m);
    return true;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressFiles(Scr scr, const Input& in, int item, int count, int stage) {
    if (stage == 1 && in.tri && scr == S_SAVE) {   // uxfix #5: Safe Copies, the one quiet place for it (Which File?'s Triangle)
        if (mIo) mIo->sound(Se::Decide);
        KeepLoad();
        push(S_KEEP);
        return true;
    }

    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveFiles(Scr scr, uint8_t param, int phase) {
    // flow: back out of choosing the host's file: no choice stays behind (the gate forgets an unused "Play on my file")
    const bool fileScreen = scr == S_SAVE || scr == S_SPFILE || scr == S_FILEUSE || (scr == S_SLOT && param == SP_COPY);
    if (phase == 1 && fileScreen) {
        MpSaveGame::abandonHostChoice();
        mp_log("ui: file choice left (%s)", screenName(scr));
    }
    if (phase == 2) {
        if (mDepth > 0 && !mStarsValid && (top().scr == S_SAVE || top().scr == S_SLOT || top().scr == S_GATHER)) refreshStars();   // menus4: back from the copies: the files are read again
        if (fileScreen && mDepth > 0 && top().scr == S_GATHER) {   // flow (one door): back to the gathering: Change File chose nothing
            if (!mFileChosen) defaultCoopSave();
            mFileOnly = false;
        }
        // (back from choosing where to save: the question again, never a silent discard)
        if (scr == S_SLOT && param == SP_LEAVE) ask(P_SAVE_ASK, MP_T_DLG_SAVE_ASK, Args(), Dlg::YesNo);
        if (scr == S_SLOT && param == SP_RECOVER) ask(P_RECOVERY, MP_T_DLG_RECOVERY, Args(), Dlg::YesNo);
    }
    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
