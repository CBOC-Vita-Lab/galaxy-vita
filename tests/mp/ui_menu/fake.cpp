// The fake port, save and chat modules the menus link against in the host tests (flow split).
#include "common.hpp"
#include "port/mp_core_game.h"   // wire: MpPlayerInfo

int g_zoomCapable = 0;   // mppause: the handheld port's zoom row in the session menu (perf.cpp port_zoom_menu_capable; weak in the menu code)
extern "C" {
int port_zoom_menu_capable(void) { return g_zoomCapable; }
// settingsnative: the disc's languages (port/port.h), as the Language pill lists them
int port_language_count(void) { return static_cast<int>(fake::cfg.languages.size()); }
const char* port_language_name(int i) { return i >= 0 && i < static_cast<int>(fake::cfg.languages.size()) ? fake::cfg.languages[static_cast<size_t>(i)].c_str() : ""; }
// flow (port/mp_ui_flow.h)
void mp_ui_net_info(MpNetInfo* out) { out->wifiUp = fake::wifi, out->service = fake::service, out->registered = fake::profile.registered; }
bool mp_ui_rejoin(void) {
    ++fake::rejoins;
    if (!fake::rejoinOk) return false;
    fake::status.phase = MP_UI_JOINING;
    return true;
}
bool mp_ui_slot_friend_code(int slot, char* out) {
    if (slot < 0 || slot >= 4 || !fake::codes[slot]) return false;
    snprintf(out, 10, "%s", fake::codes[slot]);
    return true;
}
// session: core2's call (port/mp_core_game.h), exactly: the code another console announced over the game link
bool mp_core_slot_friend_code(int slot, char* out, size_t cap) {
    if (fake::announcedOnly) return false;   // wire: the real call answers the service's verified code only; an announced one is in mp_core_slot_player
    if (slot < 0 || slot >= 4 || slot == fake::local || !fake::codes[slot] || cap < 10) return false;
    snprintf(out, cap, "%s", fake::codes[slot]);
    return true;
}
// wire: the player's record (verified or announced code), as MpCore::slotPlayer fills it
bool mp_core_slot_player(int slot, MpPlayerInfo* out) {
    if (out) memset(out, 0, sizeof *out);
    if (!out || !fake::active || slot < 0 || slot >= 4 || slot == fake::local || !fake::players[slot].present) return false;
    snprintf(out->name, sizeof out->name, "%s", fake::players[slot].name);
    out->look = static_cast<uint8_t>(fake::players[slot].look);
    if (fake::codes[slot]) { snprintf(out->code, sizeof out->code, "%s", fake::codes[slot]); out->codeVerified = !fake::announcedOnly; }
    return true;
}
// flow: core2's mp_gather_code_available (not in its fake): the test says whether a code can be given
bool mp_gather_code_available(void) { return fake::codeAvailable; }
// m-friends: service2's silent sign-in
int mp_service_ensure_account(void) {
    ++fake::ensureCalls;
    if (fake::ensureWork > 0) { --fake::ensureWork; return 0; }
    return fake::ensureResult;
}
bool mp_flow_store_get(MpFlowStore* out) { *out = fake::flow; return true; }
void mp_flow_store_set(const MpFlowStore* in) { fake::flow = *in; ++fake::flowSaves; }
bool mp_coop_meta_get(int slot, MpCoopMeta* out) { if (slot < 1 || slot > 6) return false; *out = fake::coop[slot - 1]; return out->used || out->count > 0; }
void mp_coop_meta_set(int slot, const MpCoopMeta* in) { if (slot >= 1 && slot <= 6) fake::coop[slot - 1] = *in, fake::coop[slot - 1].used = 1; }
bool mp_conntest_start(bool inRoom) {
    if (!fake::ct.startOk) return false;
    ++fake::ct.starts;
    fake::ct.lastInRoom = inRoom;
    memset(&fake::ct.view, 0, sizeof fake::ct.view);
    fake::ct.view.running = true;
    fake::ct.view.state[0] = MPCT_ST_RUNNING;
    return true;
}
void mp_conntest_pump(void) { ++fake::ct.pumps; }
void mp_conntest_view(MpCtView* out) { *out = fake::ct.view; }
void mp_conntest_cancel(void) { ++fake::ct.cancels; fake::ct.view.running = false; }
bool mp_conntest_weak_link(void) { return fake::ct.view.verdict.weakLink; }
bool mp_conntest_save_report(void) { ++fake::ct.reports; return fake::ct.reportOk; }
bool mp_conntest_relay_only(void) { return fake::ct.relay; }
bool mp_conntest_clock_wrong(void) { return fake::ct.clockWrong; }   // m-conntest: the port's flag (core-wanted.md)
bool mp_update_take_notice(void) { const bool p = fake::ct.updatePending; fake::ct.updatePending = false; return p; }
bool mp_active(void) { return fake::active; }
MpMode mp_mode(void) { return fake::active ? fake::mode : MP_MODE_NONE; }
bool mp_is_host(void) { return fake::host; }
int mp_local_slot(void) { return fake::active ? fake::local : -1; }
int mp_host_slot(void) { return fake::active ? fake::hostSlot : -1; }
uint16_t mp_host_epoch(void) { return fake::epoch; }
bool mp_slot_present(int s) { return fake::active && s >= 0 && s < 4 && fake::players[s].present; }
const char* mp_slot_name(int s) { return s >= 0 && s < 4 && fake::players[s].present ? fake::players[s].name : ""; }
MpLook mp_slot_look(int s) { return s >= 0 && s < 4 ? fake::players[s].look : MP_LOOK_MARIO; }
int mp_slot_ping_ms(int s) { return s >= 0 && s < 4 ? fake::players[s].ping : -1; }
bool mp_slot_quiet(int) { return false; }
const MpSettings* mp_settings(void) { return &fake::settings; }
uint16_t mp_scene_serial(void) { return 0; }
bool mp_send(uint8_t, const void*, uint16_t, bool) { return true; }
bool mp_send_to(int, uint8_t, const void*, uint16_t, bool) { return true; }
bool mp_poll(MpMsg*) { return false; }
uint32_t mp_poll_many(MpMsg*, uint32_t) { return 0; }
void mp_tick_game(uint32_t) {}
bool mp_save_suppressed(void) { return false; }
bool mp_save_autoconfirm(void) { return false; }
void mp_snapshot_offer(const void*, uint32_t) {}
uint32_t mp_snapshot_ready(void) { return 0; }
uint32_t mp_snapshot_get(void*, uint32_t) { return 0; }
void mp_snapshot_consumed(void) {}
void mp_ui_set_profile(const char* n, MpLook look) {
    snprintf(fake::profile.name, sizeof fake::profile.name, "%s", n);
    ++fake::wishes;
    fake::wish = look;
    // the host's arbitration: granted when nobody else has it
    if (fake::active && fake::grant) {
        bool free_ = true;
        for (int k = 0; k < 4; k++) if (k != fake::local && fake::players[k].present && fake::players[k].look == look) free_ = false;
        if (free_) fake::players[fake::local].look = look;
    }
}
bool mp_ui_host(const MpHostParams* p) {
    CHECK(p->settings.maxPlayers >= 2 && p->settings.maxPlayers <= 4);
    fake::hosted = *p;
    fake::lastHost = *p;
    fake::settings = p->settings;   // (flow: the room's settings are what the core has: Room Settings changes them live)
    fake::hostConn = p->conn;
    fake::mode = static_cast<MpMode>(p->settings.mode);
    fake::status.phase = MP_UI_CREATING;
    return true;
}
bool mp_ui_join_address(const char* t) {   // netx
    if (!t || !strchr(t, '.')) { fake::status.error = MP_ERR_ROOM_NOT_FOUND; return false; }
    fake::joinedAddr = t;
    fake::status.phase = MP_UI_JOINING;
    return true;
}
bool mp_ui_host_address(MpAddressInfo* out) {
    if (!fake::hostByAddr) return false;
    *out = fake::addrInfo;
    return true;
}
bool mp_ui_join_code(MpConn, const char* code) { CHECK(strlen(code) == 6); fake::status.phase = MP_UI_CONNECTING; return true; }
bool mp_ui_join_friend(const char*) { fake::status.phase = MP_UI_CONNECTING; return true; }
bool mp_ui_quick_match(MpMode) { fake::status.phase = MP_UI_SEARCHING; return true; }
bool mp_ui_join_local(MpConn, int) { CHECK(!"joins go by the room's id"); return false; }
bool mp_ui_join_local_room(MpConn, uint32_t id) {
    if (fake::refuseJoin != MP_ERR_NONE) { fake::status.error = fake::refuseJoin; return false; }
    for (int i = 0; i < fake::roomCount; i++)
        if (fake::rooms[i].id == id) { fake::joinedRoom = id; fake::status.phase = MP_UI_JOINING; return true; }
    return false;
}
bool mp_ui_start(void) { ++fake::starts; return !fake::startFails; }
void mp_ui_leave(void) { ++fake::leaves; fake::status.phase = MP_UI_IDLE; fake::active = false; }
void mp_ui_kick(int) { ++fake::kicks; }
void mp_ui_status(MpUiStatus* out) { *out = fake::status; }
int mp_ui_local_scan(MpConn) { return fake::roomCount; }
bool mp_ui_local_get(int i, MpLocalRoom* out) { if (i < 0 || i >= fake::roomCount) return false; *out = fake::rooms[i]; return true; }
void mp_ui_profile(MpProfile* out) { *out = fake::profile; }
const char* mp_core_saved_name(void) { return fake::savedName; }
uint8_t mp_core_saved_look(void) { return MP_LOOK_MARIO; }
uint8_t mp_core_saved_downed(void) { return fake::savedDowned; }
void mp_core_save_downed(uint8_t d) { fake::savedDowned = d; }
bool mp_core_rules_wanted(uint32_t) { return fake::rulesWanted; }   // touches
bool mp_core_rules_hidden() { return fake::rulesHidden; }
void mp_core_rules_done(uint32_t k, bool never) { fake::rulesDone = true; fake::rulesNever = never; fake::rulesKey = k; }
bool mp_core_update_settings(const MpSettings* s) {
    if (!fake::active || !fake::host) return false;
    fake::settings = *s;
    ++fake::settingsSent;
    return true;
}
bool mp_ui_register(const char*) { ++fake::registers; fake::profile.registered = fake::registerState != -1; return true; }   // (fix3: -1 = the service is still answering)
static void fakeDropIncoming(const char* code) {
    for (int i = 0; i < fake::incomingCount; i++)
        if (!strcmp(fake::incoming[i].code, code)) {
            for (int k = i; k + 1 < fake::incomingCount; k++) fake::incoming[k] = fake::incoming[k + 1];
            --fake::incomingCount;
            break;
        }
}
bool mp_ui_friends_request(void) { ++fake::friendRequests; fake::reloading = 1; return true; }
int mp_ui_friends_get(MpFriend* rows, int max) {
    if (fake::reloading > 0) { --fake::reloading; return -1; }
    if (fake::friendsState < 0) return fake::friendsState;
    if (fake::useMany) {   // m-friends: a list of any length
        const int c = static_cast<int>(fake::manyFriends.size()) < max ? static_cast<int>(fake::manyFriends.size()) : max;
        for (int i = 0; i < c; i++) rows[i] = fake::manyFriends[i];
        return c;
    }
    int n = fake::friendsState < max ? fake::friendsState : max;
    for (int i = 0; i < n; i++) rows[i] = fake::friends[i];
    return n;
}
bool mp_ui_friend_add(const char* c) { fake::ops.push_back(std::string("add:") + c); if (fake::opFails) return false; fakeDropIncoming(c); return true; }
bool mp_ui_friend_remove(const char* c) { fake::ops.push_back(std::string("remove:") + c); return true; }   // (m-friends: logged)
int mp_ui_incoming_get(MpFriend* rows, int max) {
    if (fake::reloading > 0 || fake::friendsState == -1) return -1;
    if (fake::incomingState < 0 || fake::friendsState < 0) return -2;
    if (fake::useMany) {
        const int c = static_cast<int>(fake::manyIncoming.size()) < max ? static_cast<int>(fake::manyIncoming.size()) : max;
        for (int i = 0; i < c; i++) rows[i] = fake::manyIncoming[i];
        return c;
    }
    const int n = fake::incomingCount < max ? fake::incomingCount : max;
    for (int i = 0; i < n; i++) rows[i] = fake::incoming[i];
    return n;
}
bool mp_ui_friend_dismiss(const char* c) { fake::ops.push_back(std::string("dismiss:") + c); if (fake::opFails) return false; fakeDropIncoming(c); return true; }
bool mp_ui_friend_block(const char* c) { fake::ops.push_back(std::string("block:") + c); if (fake::opFails) return false; fakeDropIncoming(c); return true; }
bool mp_ui_friend_unblock(const char* c) { fake::ops.push_back(std::string("unblock:") + c); return !fake::opFails; }
bool mp_ui_board_request(uint8_t, uint8_t, uint8_t) { return true; }
int mp_ui_board_get(MpBoardRow* rows, int max) {
    if (fake::boardState < 0) return fake::boardState;
    for (int i = 0; i < fake::boardState && i < max; i++) rows[i] = {static_cast<uint32_t>(i + 1), "Player", 5000u + i, 1, true, i == 0};
    return fake::boardState;
}
void mp_race_submit(uint8_t, uint8_t, uint32_t, const uint8_t*) {}
const char* mp_service_last_code(void) { return fake::svcCode; }   // fix3
int mp_service_register_state(void) { return fake::registerState; }
int mp_service_friend_op_state(void) { return fake::friendOpState; }
bool mp_service_account_delete(void) { ++fake::acctDeletes; return !fake::acctDeleteRefused; }
int mp_service_account_delete_state(void) { return fake::acctDeleteState; }
// beta diagnostics: the port's report / check, scripted (common.hpp: fake::dg*)
bool mp_diag_has_account(void) { return fake::dgAccount; }
bool mp_diag_service_enabled(void) { return fake::service; }
bool mp_diag_error_pending(void) { return fake::dgErrPending; }
void mp_diag_error_clear(void) { fake::dgErrPending = false; ++fake::dgErrClears; }
bool mp_diag_report_begin(int kind, int where, const char* note) { if (!fake::dgBeginOk) return false; ++fake::dgBegins; fake::dgLastKind = kind, fake::dgLastWhere = where; snprintf(fake::dgNote, sizeof fake::dgNote, "%s", note ? note : ""); return true; }
int mp_diag_report_poll(char* idOut, int cap) { if (fake::dgReportResult == 1) snprintf(idOut, cap, "%s", fake::dgId); return fake::dgReportResult; }
void mp_diag_report_cancel(void) { ++fake::dgCancels; fake::dgReportResult = 0; }
bool mp_diag_check_begin(void) { if (!fake::dgCheckBeginOk) return false; ++fake::dgCheckBegins; return true; }
int mp_diag_check_poll(MpDiagCheck* out) { if (fake::dgCheckResult == 1) *out = fake::dgCheck; return fake::dgCheckResult; }
void mp_diag_check_cancel(void) { ++fake::dgCheckCancels; fake::dgCheckResult = 0; }
bool mp_keyboard_open(const char*, const char* initial, int maxChars, bool) { fake::kbMax = maxChars; ++fake::kbOpens; snprintf(fake::kbInitial, sizeof fake::kbInitial, "%s", initial ? initial : ""); return true; }   // (m-friends: the limit recorded)
int mp_keyboard_poll(char* out, int cap) { snprintf(out, cap, "%s", fake::kbText); const int r = fake::kbResult; if (fake::kbOnce && r != 0) fake::kbResult = 0; return r; }   // (uxfix: kbOnce = one answer per keyboard)
bool mp_dialog_active(void) { return false; }
void mp_log(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fake::log.push_back(b);
    if (getenv("UI_TEST_LOG")) fprintf(stderr, "[mp] %s\n", b);
}
}


namespace MpSaveGame {
bool settingsReadOnly() { return fake::settingsRO; }   // flow (savefix)
bool takeLeftForMismatch() { const bool r = fake::mismatchLeft; fake::mismatchLeft = false; return r; }   // uxfix #2
bool enterSessionRoot() { return !fake::enterFails; }
bool leaveSessionRoot() {
    ++fake::rootLeaves;
    if (fake::leaveFails > 0) { --fake::leaveFails; return false; }
    fake::onMpRoot = false;
    return true;
}
bool isSessionRoot() { return fake::onMpRoot; }
bool heldAfterSession() { return false; }
bool importSinglePlayerFile(int, int) { return true; }
bool writeBackToSinglePlayer(int, int) { ++fake::writeBacks; return true; }
bool backupSinglePlayerFile() { ++fake::backups; return true; }
void beginSinglePlayerSession(int sp) { fake::spSession = sp; }
void abandonHostChoice() { ++fake::abandons; }   // gamefix
int compareImportProgress(int, int mp) { return mp == 2 ? -1 : 1; }
int slotState(bool coop, int f, int* stars) {   // m-files: the three-way answer (savefix's call; the fake follows the same numbers as slotStars)
    ++fake::slotReads;
    if (stars) *stars = -1;
    if (((coop ? fake::coopBad : fake::spBad) >> (f - 1)) & 1) return SLOT_UNREADABLE;
    const int v = coop ? (fake::coopFull ? 7 : (f == 2 && !fake::noCoop ? 12 : (fake::coopZero ? 0 : -1))) : ((fake::spMissing >> (f - 1)) & 1 ? -1 : 5);
    if (v < 0) return SLOT_NEW;
    if (stars) *stars = v;
    return SLOT_USED;
}
bool leaveDecisionPending() { return fake::leavePending; }
int slotStars(bool coop, int f) { ++fake::slotReads; return coop ? (fake::coopFull ? 7 : (f == 2 && !fake::noCoop ? 12 : (fake::coopZero ? 0 : -1))) : ((fake::spMissing >> (f - 1)) & 1 ? -1 : 5); }
bool offerSaveOnLeave(LeaveOffer* o) {
    *o = LeaveOffer();
    o->offer = o->coop = true;
    o->single = fake::spSession > 0;
    o->singleSlot = fake::spSession;
    o->stars = 8;
    for (int i = 0; i < 6; i++) o->coopUsed[i] = i == 1, o->coopStars[i] = i == 1 ? 12 : -1;   // co-op 2 has 12 stars
    return true;
}
bool saveOnLeave(int choice, int slot) { fake::leaveChoice = choice; fake::leaveSlot = slot; return true; }
bool recoveryPending(RecoveryInfo* out) {
    if (!fake::recovery) return false;
    *out = RecoveryInfo();
    out->stars = 3;
    out->spStars = -1;
    if (fake::recSingle) {   // gamefix: a session on single-player file 2
        out->single = true;
        out->sourceKind = 2;
        out->sourceSlot = 2;
        out->spStars = fake::recSpStars;
        out->spNewer = fake::recSpNewer;
    }
    return true;
}
bool recoveryKeep(int slot, int choice, bool confirmed) { fake::recoverConfirmed = confirmed; fake::recoverSlot = slot; fake::recoverChoice = choice; fake::recovery = false; return true; }
bool recoveryDiscard() { ++fake::recoveryDiscards; fake::recovery = false; return true; }
int sessionSlot() { return 1; }
    bool launchReady() { return true; }   // (savefix merge: the launch guard; the tests cover the ready path)
}  // namespace MpSaveGame
namespace MpUi { void sendStamp(int) {} }
namespace MpGameInternal {   // B6: the results card's waits count wall-clock ms; each call is one 60 fps frame here (the tests count frames)
int64_t nowMs() { static int64_t n = 0; return (++n) * 50 / 3; }
}
namespace MpChat {   // MP3 chat (MpChat.cpp is game code; its pure parts are tested in tests/mp/chat_test.cpp)
bool sendPhrase(int p) {
    if (fake::chatRefuse || p < 0 || p >= MP_CHAT_PHRASES) return false;
    fake::chatPhrases.push_back(p);
    return true;
}
bool sendText(const char* t) {
    if (fake::chatRefuse || !t) return false;
    fake::chatTexts.push_back(t);
    return true;
}
bool typedAllowed() { return fake::active && fake::chatTyped; }
void noteRoomPublic(bool p) { fake::roomPublic = p; }   // gamefix
bool muted(int s) { return s >= 0 && s < 4 && fake::chatMuted[s]; }
void setMuted(int s, bool on) { if (s >= 0 && s < 4) fake::chatMuted[s] = on; }
}  // namespace MpChat


// keepsafe as the menus see it (the same signatures as platform/include/port/mp_keepsafe.h; the contract: share/keep/API.md)
extern "C" {
static void KsPush(int kind, const char* name) {
    MpKsCopy c = fake::ksCopy(kind, name, fake::ks.now);
    fake::ks.copies.insert(fake::ks.copies.begin(), c);
}
int mp_keepsafe_notice_take(void) { const int n = fake::ks.notice; fake::ks.notice = 0; if (n) fake::ks.calls.push_back("notice"); return n; }
bool mp_keepsafe_may_migrate(void) { fake::ks.calls.push_back("may_migrate"); return !fake::ks.noMigrate; }
int mp_keepsafe_list_copies(struct MpKsCopy* out, int max) {
    int n = 0;
    for (const MpKsCopy& c : fake::ks.copies)
        if (n < max) out[n++] = c;
    return n;
}
int mp_keepsafe_make_copy(int flags, char* nameOut, size_t cap) {
    fake::ks.calls.push_back(std::string("make:") + std::to_string(flags));
    fake::ks.lastFlags = flags;
    if (fake::ks.makeRes != MP_KS_OK) return fake::ks.makeRes;
    char name[64];
    snprintf(name, sizeof name, "copy-%d.smgmp", ++fake::ks.madeCount);
    KsPush(MP_KS_COPY_MADE, name);
    snprintf(nameOut, cap, "%s", name);
    return MP_KS_OK;
}
int mp_keepsafe_inspect(const char* name, struct MpKsCopyInfo* info) {
    fake::ks.calls.push_back(std::string("inspect:") + name);
    if (fake::ks.inspectRes != MP_KS_OK) return fake::ks.inspectRes;
    *info = fake::ks.info;
    for (int i = 0; i < 6; i++)
        if (info->copyStars[i] < 0) info->slotBlank |= static_cast<uint8_t>(1 << i);   // (a Team File the copy does not hold is never offered)
    return MP_KS_OK;
}
int mp_keepsafe_bring_back_slot(const char* name, int slot, int confirmed, char* beforeOut, size_t cap) {
    fake::ks.calls.push_back(std::string("slot:") + name + ":" + std::to_string(slot) + ":" + std::to_string(confirmed));
    fake::ks.lastName = name, fake::ks.lastSlot = slot, fake::ks.lastConfirmed = confirmed;
    if (fake::ks.slotRes != MP_KS_OK) return fake::ks.slotRes;
    const bool inUse = (fake::ks.info.slotInUse >> (slot - 1)) & 1;
    if (inUse && !confirmed) return MP_KS_CONFIRM;
    if (inUse) {
        KsPush(MP_KS_COPY_BEFORE, "before-1.smgmp");
        snprintf(beforeOut, cap, "before-1.smgmp");
    }
    fake::ks.info.slotSame |= static_cast<uint8_t>(1 << (slot - 1));
    return MP_KS_OK;
}
int mp_keepsafe_use_account(const char* name, int confirmed) {
    fake::ks.calls.push_back(std::string("account:") + name + ":" + std::to_string(confirmed));
    fake::ks.lastConfirmed = confirmed;
    if (fake::ks.accountRes != MP_KS_OK) return fake::ks.accountRes;
    if (!confirmed) return MP_KS_CONFIRM;
    fake::ks.info.account = 0;
    return MP_KS_OK;
}
int mp_keepsafe_bring_back_settings(const char* name, int which, char* beforeOut, size_t cap) {
    fake::ks.calls.push_back(std::string("settings:") + name + ":" + std::to_string(which));
    fake::ks.lastWhich = which;
    if (fake::ks.settingsRes != MP_KS_OK) return fake::ks.settingsRes;
    KsPush(MP_KS_COPY_BEFORE, "before-2.smgmp");
    snprintf(beforeOut, cap, "before-2.smgmp");
    fake::ks.info.settings &= static_cast<uint8_t>(~which);
    return MP_KS_OK;
}
}
