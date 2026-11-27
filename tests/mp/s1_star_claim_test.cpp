// savesus S1 (was an expected-fail test; fixed by glitchfix): can a lying guest award a star to the host's own single-player file in an
// auto-confirmed "Play on my file" session? handleStar needs the game scene, so this test replays its checks in the same order with the
// pure pieces it uses (MpFlowLogic.hpp): readStar -> serial == scene serial -> starInMask or holder -> claimVerdict (the new rule: on a
// single-player file the star must have appeared in the host's own scene) -> StarLedger.claim. The claim message carries no proof.
#include "Game/MP/MpFlowLogic.hpp"
#include <stdio.h>
#include <stdlib.h>
using namespace MpFlowLogic;

static int sFails = 0;
#define CHECK(c) do { if (!(c)) { sFails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// handleStar's decision on the host for a claim from guest slot `from` (rate cap and the synthetic-or-holder test reduced to their inputs)
static bool hostAccepts(const uint8_t* wire, size_t len, uint16_t sceneSerial, uint32_t scenarioMask, bool holderHas, StarLedger& led, int from,
                        bool spFile = false, bool appeared = false) {
    Star s;
    if (!readStar(wire, len, &s)) return false;
    if (s.serial != sceneSerial) return false;
    const bool synthOk = starInMask(scenarioMask, s.starId);
    if (!(synthOk || holderHas)) return false;
    if (claimVerdict(spFile, false, holderHas, appeared, synthOk) != ClaimVerdict::Accept) return false;   // (Retry: the 3 s retry ends in Accept or a refusal; judged below)
    return led.claim(3, 2, s.starId, from, s.serial);
}

int main() {
    const uint32_t mask = 36;   // CosmosGarden scenario 3: stars 3 and 6
    StarLedger led;
    uint8_t b[16];
    MpWriter w(b, sizeof b);
    // 1. the claim is 12 bytes: serial, starId, grand, bySlot, turns, galaxy hash, scenario (a cross-check for the host). No proof of collection, no position, no object id.
    writeStar(w, Star{7, 3, 0, 1, 0, 0x1234, 3});
    CHECK(w.n == kStarBytes && kStarBytes == 12);
    // 2. a guest that never touched the star: accepted (the host will run the star-get sequence and record the star in ITS file)
    CHECK(hostAccepts(b, w.n, 7, mask, false, led, 1));
    // 3. the grand flag is the sender's too
    StarLedger led2;
    MpWriter w2(b, sizeof b);
    writeStar(w2, Star{7, 6, 1, 1, 0, 0x1234, 3});
    CHECK(hostAccepts(b, w2.n, 7, mask, false, led2, 1));
    // 4. what IS refused: another scene's serial, an id outside the scenario, a repeat
    StarLedger led3;
    MpWriter w3(b, sizeof b);
    writeStar(w3, Star{6, 3, 0, 1, 0, 0x1234, 3});
    CHECK(!hostAccepts(b, w3.n, 7, mask, false, led3, 1));
    MpWriter w4(b, sizeof b);
    writeStar(w4, Star{7, 4, 0, 1, 0, 0x1234, 3});
    CHECK(!hostAccepts(b, w4.n, 7, mask, false, led3, 1));
    // 5. THE RULE: in a session whose saves go to the host's single-player file, a guest's claim for a star the host's own scene has not
    //    revealed (only the scenario mask knows it) is not recorded; once the star has appeared in the host's scene it is.
    StarLedger led5;
    MpWriter w5(b, sizeof b);
    writeStar(w5, Star{7, 3, 0, 1, 0, 0x1234, 3});
    CHECK(!hostAccepts(b, w5.n, 7, mask, false, led5, 1, true, false));       // mask alone: refused (was accepted before the fix)
    CHECK(!hostAccepts(b, w5.n, 7, mask, true, led5, 1, true, false));       // holder has it, not appeared yet: waits (not accepted now)
    CHECK(hostAccepts(b, w5.n, 7, mask, true, led5, 1, true, true));          // appeared in the host's scene: accepted
    CHECK(!hostAccepts(b, w5.n, 7, mask, true, led5, 1, true, true));         // ...once (ledger)
    StarLedger led6;
    CHECK(hostAccepts(b, w5.n, 7, mask, false, led6, 1, false, false));       // a co-op file (no single-player file): rules as before
    // the lying guest of the old test: claims star 6 as a grand star with no object anywhere: refused on a single-player file
    StarLedger led7;
    MpWriter w6(b, sizeof b);
    writeStar(w6, Star{7, 6, 1, 1, 0, 0x1234, 3});
    // (fix2: the grand flag no longer helps a lone claim of a star the scenario has no holder for UNLESS the claimer is in our scene: see below)
    CHECK(!hostAccepts(b, w6.n, 7, mask, false, led7, 1, true, false));
    // ---- fix2 B: kinds only the claimer's console can spawn (hidden / boss star: the object is here, not appeared; a grand star with no
    // holder entry) count when the id belongs to the scenario, the claimer is in our scene serial and the claim passed the rate cap
    CHECK(claimVerdict(true, false, true, false, true, false, true) == ClaimVerdict::Accept);    // boss/hidden star, same scene
    CHECK(claimVerdict(true, false, true, false, true, false, false) == ClaimVerdict::Retry);    // claimer's scene unknown: wait, then refuse out loud
    CHECK(claimVerdict(true, false, false, false, true, true, true) == ClaimVerdict::Accept);    // grand star without holder entry
    CHECK(claimVerdict(true, false, false, false, true, false, true) == ClaimVerdict::Retry);    // bare mask, not grand, no holder: strict rule
    CHECK(claimVerdict(true, false, true, false, false, false, true) == ClaimVerdict::Retry);    // star id not in the scenario: strict rule
    CHECK(claimVerdict(true, false, false, false, false, true, true) == ClaimVerdict::Refuse);   // nothing knows it
    // ---- fix2 A: several claims waiting at once (before: one struct, the second overwrote the first)
    ClaimRetryTable t;
    CHECK(t.add(Star{7, 3, 0, 1, 0, 0x1234, 3}, 1, true, 3000));
    CHECK(t.add(Star{7, 6, 0, 1, 0, 0x1234, 3}, 1, true, 3100));
    CHECK(t.count() == 2);                                   // both kept
    CHECK(t.add(Star{7, 3, 0, 2, 0, 0x1234, 3}, 2, true, 3500));        // the same star again replaces its entry
    CHECK(t.count() == 2);
    CHECK(t.add(Star{7, 1, 0, 1, 0, 0x1234, 3}, 1, true, 3600));
    CHECK(t.add(Star{7, 2, 0, 1, 0, 0x1234, 3}, 1, true, 3700));
    CHECK(t.count() == kClaimRetryMax);
    CHECK(!t.add(Star{7, 4, 0, 1, 0, 0x1234, 3}, 1, true, 3800));       // full: reported, so the caller refuses it out loud
    t.slot[0].valid = false;
    CHECK(t.add(Star{7, 4, 0, 1, 0, 0x1234, 3}, 1, true, 3800));        // room again
    printf("s1_star_claim_test: %s\n", sFails ? "FAIL" : "ok");
    return sFails ? 1 : 0;
}
