// core2: the gathering fake does what its script says (it is what the menu tests build against).
#include "mp_gather_fake.h"
#include <stdio.h>
#include <string.h>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
int main() {
    MpGatherStatus s;
    mp_gather_fake_reset();
    mp_gather_fake_set_radio(false, true);
    CHECK(mp_gather_start(MP_MODE_TOGETHER, MP_CONN_NONE));
    mp_gather_status(&s);
    CHECK(s.state == MP_GATHER_LISTENING && s.conn == MP_CONN_NEARBY && !s.canStart);
    mp_gather_fake_add_head(100, "Me", 0);
    mp_gather_fake_open_room();
    mp_gather_status(&s);
    CHECK(s.state == MP_GATHER_GATHERING && s.canStart && s.heads[0].flags == (MP_HEAD_SELF | MP_HEAD_LEADER));
    mp_gather_fake_other_room(true);
    mp_gather_status(&s);
    CHECK(!s.canStart);
    mp_gather_fake_merge_begin();
    mp_gather_fake_add_head(200, "Ann", 1);
    mp_gather_fake_set_arriving(200, true);
    mp_gather_status(&s);
    CHECK(s.state == MP_GATHER_MERGING && s.headCount == 2 && (s.heads[1].flags & MP_HEAD_ARRIVING) && !s.canStart);
    mp_gather_fake_other_room(false);
    mp_gather_fake_merge_end(200);
    mp_gather_status(&s);
    CHECK(s.state == MP_GATHER_GATHERING && !s.canStart && (s.heads[1].flags & MP_HEAD_LEADER) && !(s.heads[1].flags & MP_HEAD_ARRIVING));
    CHECK(!mp_gather_begin());
    mp_gather_fake_set_leader(100);
    CHECK(mp_gather_begin() && mp_gather_fake_begins() == 1);
    mp_gather_stop();
    mp_gather_fake_add_playing(77, "Bob", MP_MODE_RACE, 3);
    MpLocalRoom r[4];
    CHECK(mp_gather_playing_rooms(r, 4) == 1 && r[0].id == 77 && mp_gather_join_playing(77) && mp_gather_fake_joined_room() == 77);
    CHECK(mp_ui_join_direct_code("not a code") != MP_CODE_OK);
    char code[MP_DIRECT_CODE_MAX];
    mp_gather_fake_reset();
    CHECK(mp_gather_start(MP_MODE_TOGETHER, MP_CONN_WIFI));
    mp_gather_fake_add_head(1, "Me", 0);
    mp_gather_fake_open_room();
    CHECK(mp_gather_get_code(code, sizeof code));
    mp_gather_status(&s);
    CHECK(s.conn == MP_CONN_DIRECT && !strcmp(s.code, code) && mp_ui_join_direct_code(code) == MP_CODE_OK);
    printf("mp_gather_fake_test: %d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
