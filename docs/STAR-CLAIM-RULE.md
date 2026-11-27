# Star claims on the host's single-player file (rule, fix2, Nov 12; 2b starfix, Nov 13)

Owner's two rules, in order: (1) never damage a single-player save; (2) never lose a player's progress silently.

## The decision
A guest's STAR claim is credited to a single-player file in these cases (MpFlowLogic.hpp claimVerdict, called from MpFlow.cpp handleStar):
1. Strict (unchanged): the star's object exists in the host's scene AND has appeared there (the host's own simulation proved it).
2. Widened (new), only for kinds the host's console can never produce by itself:
   - a star whose object is in the scene but not appeared: hidden stars and boss stars (each console runs its own boss and its own hidden spots);
   - a grand star with no holder entry.
   It counts when ALL hold: the star id belongs to the scenario the host is in (PowerStarId mask), the claim's scene serial is the host's, the claimer's last
   pose says it is in that same serial, and the claim passed the per-sender rate cap (K_STAR, already applied before the verdict).
2b. Late claim from the scene the host has just left (APPROVED 13 Nov 23:01, owner delegated to the coordinator; implemented by DESIGN-star revision 4, approved 13 Nov 23:40; in force with the starfix code).
   A STAR whose scene serial EQUALS the serial of the scene the host last left (serial equality, not "one less": serials wrap 65535 -> 1), that arrives at most 5 seconds after
   the host executed its own MOVE (the host's clock), from a present player, within the per-sender cap, and whose galaxy/scenario fields equal the scene the host left (a cross-check
   only), is judged by the SAME rules as cases 1 and 2 but against the scene the host LEFT (the star's holder entry, appeared flag and scenario mask as the host recorded them at the MOVE,
   before the scene was torn down), never against the scene the host is now in, and never against anything else the claimer says. The claimer must have been in that old serial: the host
   froze, at the MOVE, which slots' last pose carried it (no pose is read later). It is credited to the host's REMEMBERED key (galaxy, scenario of the left scene), not to the STAR's own
   fields. A claim the strict rule would still have waited for (case 3's retry) is refused at once on this path: the left scene is frozen. A late GRAND star is refused with the toast
   (it cannot be saved late). Any other claim with a different serial is refused with the toast.
   The host sends its verdict to everyone (STARV) for EVERY claim, live or late, accepted or refused, and for its own stars. A STARV counts only from the current host and the host epoch it
   was sent in. The other consoles credit ONLY on a credited STARV (they no longer retry or judge by a clock of their own), with a check against the STATIC scenario mask only; the claimer
   sets the flag again on it. A claimer whose STAR got no verdict re-sends it to a new host after a host change, and tells the player after 60 s without one.
3. Everything else (a bare scenario-mask claim for a normal star with no holder entry, or an id that is not in the scenario) is retried for 3 s (up to 4 claims
   waiting at once; a full table refuses at once), then refused.

## A refusal is never silent
Whenever a claim is finally refused (no such star, waited 3 s, table full) the console logs it ("flow refused STAR ...") and shows the toast
MP_T_STAR_NOT_SAVED ("<name>'s Power Star couldn't be saved this time. Sorry!"). The leader also sends it to everyone with the existing MP_MSG_TOAST
(host-only, already length- and id-checked by the receiver), so both players see it. No new packet type.

## What a cheating guest can still do
Credit the host's single-player file with any star of the SCENARIO BEING PLAYED (set grand=1 on a normal star, or claim a hidden/boss star never
collected), at the rate cap, while its pose says it is in the host's scene. It cannot award a star of another galaxy or scenario, a star id outside
the scenario's mask, a star from another scene serial, or more than the cap allows; the ledger credits each star once. The worst case is therefore a
star the host could have earned in the very scene it is standing in. The owner can overrule by deleting the widened line in claimVerdict (case 2):
honest boss/hidden/grand stars then end in the refusal toast instead of the file.

With 2b (approved 13 Nov 23:01) a cheating guest can also credit, in the 5 seconds after the host moves on, a star of the scene the host just LEFT that the host could have
earned there: the same set as before, shifted by one scene and 5 seconds. It still cannot credit any other galaxy, scenario, serial, or a star id outside that scenario's mask, and the ledger
still credits each star once.
