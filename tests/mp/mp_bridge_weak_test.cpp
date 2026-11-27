// Galaxy 3.0 multiplayer: weak defaults for every bridge function (tests only since the integration: the game links the real
// implementations; tests/mp/service_test.sh links these for the bridge calls mp_service.cpp makes).
// A real implementation (platform/src/mp/*.cpp) replaces each one. These keep the game in single-player behaviour.
#include "port/mp_bridge.h"
#include <stdarg.h>
#include <stdio.h>

#define MP_WEAK __attribute__((weak))

static MpSettings sDefaultSettings = {MP_MODE_NONE, 2, 1, 1, 1, 18, 1, 7, {0}};

extern "C" {
MP_WEAK bool mp_active(void) { return false; }
MP_WEAK MpMode mp_mode(void) { return MP_MODE_NONE; }
MP_WEAK bool mp_is_host(void) { return false; }
MP_WEAK int mp_local_slot(void) { return -1; }
MP_WEAK int mp_host_slot(void) { return -1; }
MP_WEAK uint16_t mp_host_epoch(void) { return 0; }
MP_WEAK bool mp_slot_present(int) { return false; }
MP_WEAK const char* mp_slot_name(int) { return ""; }
MP_WEAK MpLook mp_slot_look(int) { return MP_LOOK_MARIO; }
MP_WEAK int mp_slot_ping_ms(int) { return -1; }
MP_WEAK bool mp_slot_quiet(int) { return false; }
MP_WEAK const MpSettings* mp_settings(void) { return &sDefaultSettings; }
MP_WEAK uint16_t mp_scene_serial(void) { return 0; }
MP_WEAK bool mp_send(uint8_t, const void*, uint16_t, bool) { return false; }
MP_WEAK bool mp_send_to(int, uint8_t, const void*, uint16_t, bool) { return false; }
MP_WEAK bool mp_poll(MpMsg*) { return false; }
MP_WEAK uint32_t mp_poll_many(MpMsg*, uint32_t) { return 0; }
MP_WEAK void mp_tick_game(uint32_t) {}
MP_WEAK bool mp_save_suppressed(void) { return false; }
MP_WEAK bool mp_save_autoconfirm(void) { return false; }
MP_WEAK void mp_snapshot_offer(const void*, uint32_t) {}
MP_WEAK uint32_t mp_snapshot_ready(void) { return 0; }
MP_WEAK uint32_t mp_snapshot_get(void*, uint32_t) { return 0; }
MP_WEAK void mp_snapshot_consumed(void) {}
MP_WEAK void mp_ui_set_profile(const char*, MpLook) {}
MP_WEAK bool mp_ui_host(const MpHostParams*) { return false; }
MP_WEAK bool mp_ui_join_code(MpConn, const char*) { return false; }
MP_WEAK bool mp_ui_join_friend(const char*) { return false; }
MP_WEAK bool mp_ui_quick_match(MpMode) { return false; }
MP_WEAK bool mp_ui_join_local(MpConn, int) { return false; }
MP_WEAK bool mp_ui_join_local_room(MpConn, uint32_t) { return false; }
MP_WEAK bool mp_ui_start(void) { return false; }
MP_WEAK void mp_ui_leave(void) {}
MP_WEAK void mp_ui_kick(int) {}
MP_WEAK void mp_ui_status(MpUiStatus* out) { if (out) { *out = MpUiStatus(); out->phase = MP_UI_IDLE; out->error = MP_ERR_NONE; } }
MP_WEAK int mp_ui_local_scan(MpConn) { return 0; }
MP_WEAK bool mp_ui_local_get(int, MpLocalRoom*) { return false; }
MP_WEAK void mp_ui_profile(MpProfile* out) { if (out) *out = MpProfile(); }
MP_WEAK bool mp_ui_register(const char*) { return false; }
MP_WEAK bool mp_ui_friends_request(void) { return false; }
MP_WEAK int mp_ui_friends_get(MpFriend*, int) { return -2; }
MP_WEAK bool mp_ui_friend_add(const char*) { return false; }
MP_WEAK bool mp_ui_friend_remove(const char*) { return false; }
MP_WEAK int mp_ui_incoming_get(MpFriend*, int) { return -2; }
MP_WEAK bool mp_ui_friend_dismiss(const char*) { return false; }
MP_WEAK bool mp_ui_friend_block(const char*) { return false; }
MP_WEAK bool mp_ui_friend_unblock(const char*) { return false; }
MP_WEAK bool mp_ui_board_request(uint8_t, uint8_t, uint8_t) { return false; }
MP_WEAK int mp_ui_board_get(MpBoardRow*, int) { return -2; }
MP_WEAK void mp_race_submit(uint8_t, uint8_t, uint32_t, const uint8_t*) {}
MP_WEAK bool mp_keyboard_open(const char*, const char*, int, bool) { return false; }
MP_WEAK int mp_keyboard_poll(char*, int) { return -1; }
MP_WEAK bool mp_dialog_active(void) { return false; }
MP_WEAK void mp_log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); fputs("[mp] ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
}
