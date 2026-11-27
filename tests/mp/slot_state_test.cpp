// flow (review 2 #3): with a REAL GameData.bin (the harness fixture /data/saves0, or $SMG_TEST_GAMEDATA), a never-created user
// file is "New" (state 0, never offered as a file), a created one with its stars is a file (state 1), and a slot whose block does
// not parse is refused (-1). Also: the corrupt-file check of the whole image (mp_save_validate) still sees a damaged file.
//   g++ -std=c++20 -fsanitize=address,undefined -g -Iplatform/include tests/mp/slot_state_test.cpp platform/src/mp/mp_savefile.cpp -pthread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "../../platform/src/mp/mp_savefile.h"

static int sChecks = 0, sFails = 0;
#define CHECK(c) do { ++sChecks; if (!(c)) { ++sFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    const char* path = getenv("SMG_TEST_GAMEDATA") ? getenv("SMG_TEST_GAMEDATA") : "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    FILE* f = fopen(path, "rb");
    if (!f) { printf("slot_state_test: SKIP (no %s)\n", path); return 0; }
    std::vector<uint8_t> img(1 << 20);
    const size_t len = fread(img.data(), 1, img.size(), f);
    fclose(f);
    CHECK(len > 0 && mp_save_validate(img.data(), (uint32_t)len, true, true) == 0);
    int created = 0, fresh = 0, firstCreated = 0, firstFresh = 0;
    for (int s = 1; s <= 6; s++) {
        const int st = mp_save_slot_state(img.data(), s);
        const int stars = mp_save_slot_stars(img.data(), s, nullptr, nullptr);
        printf("  slot %d: state %d stars %d\n", s, st, stars);
        CHECK(st == 0 || st == 1);
        if (st == 1) { ++created; if (!firstCreated) firstCreated = s; }
        if (st == 0) { ++fresh; if (!firstFresh) firstFresh = s; CHECK(stars == 0); }   // (the old reading: 0 stars, as if a file)
    }
    CHECK(created >= 1);
    CHECK(mp_save_slot_state(img.data(), 0) == -1 && mp_save_slot_state(img.data(), 7) == -1);
    // the fixture has the three cases: created with stars (slot 2: 50), created with 0 stars (slot 1: a file that is a file),
    // never created (3..6: "New", although the old count said 0 stars, as for slot 1)
    CHECK(mp_save_slot_state(img.data(), 2) == 1 && mp_save_slot_stars(img.data(), 2, nullptr, nullptr) == 50);
    CHECK(mp_save_slot_state(img.data(), 1) == 1 && mp_save_slot_stars(img.data(), 1, nullptr, nullptr) == 0);
    CHECK(mp_save_slot_state(img.data(), 3) == 0 && mp_save_slot_stars(img.data(), 3, nullptr, nullptr) == 0);
    uint32_t mo, ms, co, cs;
    char m[16], c[16];
    snprintf(m, sizeof m, "mario%d", firstCreated), snprintf(c, sizeof c, "config%d", firstCreated);
    CHECK(mp_save_find(img.data(), m, &mo, &ms) && mp_save_find(img.data(), c, &co, &cs));
    // the created slot with its config block zeroed: never created
    std::vector<uint8_t> empty = img;
    memset(empty.data() + co, 0, cs);
    CHECK(mp_save_slot_state(empty.data(), firstCreated) == 0);
    // a damaged block header: refused, and the whole-file check fails too (the game's corrupt-file path)
    std::vector<uint8_t> bad = img;
    bad[mo] = 7;
    CHECK(mp_save_slot_state(bad.data(), firstCreated) == -1);
    CHECK(mp_save_validate(bad.data(), (uint32_t)len, true, true) != 0);
    printf("slot_state_test: %d slots created, %d new; %d checks, %d failures\n", created, fresh, sChecks, sFails);
    return sFails ? 1 : 0;
}
