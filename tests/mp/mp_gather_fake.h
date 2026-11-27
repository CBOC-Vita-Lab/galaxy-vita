// core2: a scriptable fake of port/mp_gather.h for menu host tests (ui_menu_test and the like). Link tests/mp/mp_gather_fake.cpp
// (and platform/src/mp/mp_direct_code.cpp) INSTEAD of the real core. The fake keeps one gathering in memory; the test script moves it.
#pragma once
#include "port/mp_gather.h"

extern "C" {
void mp_gather_fake_reset(void);
// the console's facts
void mp_gather_fake_set_radio(bool wifiConnected, bool nearbyPossible);
void mp_gather_fake_set_hidden(int fullRooms, int otherVersionRooms);   // lobby: rooms heard but never offered
// heads: add (or update) a console with a stable id; the first added after reset is SELF
void mp_gather_fake_add_head(uint32_t id, const char* name, uint8_t look);
void mp_gather_fake_remove_head(uint32_t id);
void mp_gather_fake_set_leader(uint32_t id);
void mp_gather_fake_set_arriving(uint32_t id, bool arriving);
// after mp_gather_start: LISTENING; this moves it on (GATHERING)
void mp_gather_fake_open_room(void);
// a merge: MERGING (no Start prompt; heads stay), then done: the target room's heads (incoming ids) join; the leader may change
void mp_gather_fake_merge_begin(void);
void mp_gather_fake_merge_end(uint32_t newLeaderId);
// another gathering room heard / no longer heard (canStart is false while one is heard)
void mp_gather_fake_other_room(bool heard);
// a started room nearby that this console may join
void mp_gather_fake_add_playing(uint32_t roomId, const char* host, uint8_t mode, uint8_t players);
void mp_gather_fake_heard(uint32_t datagrams);
// what the menu did (for asserts)
int mp_gather_fake_begins(void);          // successful mp_gather_begin calls
uint32_t mp_gather_fake_joined_room(void);  // last mp_gather_join_playing room id (0 none)
// code5: what the service does for the host's "Invite from Far Away": 0 = up (a 5-character code, "KCBC4"), 1 = unreachable or over budget (the long
// code, codeLong set), 2 = this build has no service (the long code, silently; the default, so older tests keep their meaning)
void mp_gather_fake_set_service(int mode);
// code5: the lookup of a typed 5-character code ends: 0 = nothing yet, else FAILED with that MpInviteError
void mp_gather_fake_invite_fail(int inviteError);
const char* mp_gather_fake_joined_code(void);   // last code accepted by mp_ui_join_direct_code ("" none)
// mpwifi44: the reasons of mp_gather_last_error. fail_start: the next mp_gather_start answers false with this MpUiError (0 = starts work
// again); fail: the gathering goes FAILED with it. A fresh fake (reset) has no reason: the menu then shows what it showed before.
void mp_gather_fake_fail_start(int error, const char* reason);
void mp_gather_fake_fail(int error, const char* reason);
}
