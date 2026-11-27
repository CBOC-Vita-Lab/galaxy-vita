## Session security (core2, protocol 3): what is built

This file describes the session layer as it is in the code (platform/src/mp: mp_core.cpp, mp_seal.h, mp_gather.cpp, mp_direct_code.cpp;
net/src/vlink.cpp), what it protects against, what it costs, what it does not protect against, and what has NOT been tested. It answers
an outside review of the network code (findings 1 to 11; 12 is the game side, 13 the server; the review report and the earlier design notes
are kept outside the source tree). Where the earlier design said something else, this file is right and the difference is listed at the end.

**What a player can reach today (mp-next).** The session layer below runs under every kind of game. The sealed datagrams, the cookie join,
rejoin tokens, the removed-console notice and the room-key tag on beacons are in use whenever a game is hosted and joined from the menus
(Same Wi-Fi, Nearby, a typed address, the online service). The redesigned menus also use the gathering driver (`mp_gather.cpp`, the one
gathering screen every game now starts from) and the far-away code of a Play by Address game (`mp_gather_get_code`, **Invite from Far
Away**). The far-away code is being changed (a shorter code); the parts of this file that depend on the code's secret describe the core as
it is now. Nothing here has run on a console or a real network.

### 1. What a player gets (plain words; PRIVACY.md and MULTIPLAYER.md may quote these)
- Only a member of a game can speak as that member. Nobody else on the Wi-Fi, the internet or the same room can make a console say,
  move, collect, kick or leave something it did not.
- Everything the consoles send each other during a game is encrypted and signed: other people on the same Wi-Fi cannot read chat,
  positions, the save copy or who is in the room, and cannot change any of it unnoticed.
- In a Play by Address game opened with a code (no screen opens one yet; see above), a stranger who knows the host's address but not
  its code cannot join, cannot learn anything about the room and gets at most one small packet back (never a larger one: the host cannot
  be used to flood someone else). In a game opened with a typed address, anyone with the address can ask to join; they get the same
  small answer, and a join still needs a valid cookie.
- **Your friend code** is shown to the people in your game so they can add you as a friend. It lets them send you a friend request
  and nothing else: until you add them back they cannot see when you are online, join your game or see your times on the friends
  board (server/src/core/friends.js and rooms4.js: presence, rooms and the friends board are for mutual friends only). In a game through the online service the service vouches for the code; in a Same Wi-Fi / Nearby / Play by Address game
  your console announces it itself (only after the game's encryption is set up, never in the room's public announcements, never in
  logs). The core can tell the menus whether a code was verified by the service (`mp_ui_slot_friend_code` in `platform/src/mp/mp_ui_flow.cpp`
  now returns the core's own answer: the service's verified code online, the announced one in a Same Wi-Fi / Nearby room).
- What it does NOT do: see section 6.

### 2. Keys
- Each console has a 128-bit console key in mp.cfg (made once, never sent). Per game session (session id s) it derives
  - its X25519 key pair: sk = BLAKE2b(key = console key, "SMG3 sk" | s), when the session is known before joining (hosting, joining a
    room seen in the scan, a rejoin); a random key pair otherwise (a first join by code or address: nobody holds an older key of it);
  - its rejoin token: BLAKE2b(key = console key, "SMG3 rejoin" | s). The token travels only masked under a pair key; the roster carries
    only its hash (the commitment).
  The same console therefore has the same public key and token for the whole session: after a crash, a restart or a rejoin it is
  recognised, and a member that missed its rejoin (a stale roster) still derives the right pair key (needed for host changes).
- Pair key K(i, j) = BLAKE2b-256(key = PSK, "SMG3 pair" | X25519(sk_i, pk_j) | lower pk | higher pk | session id). PSK = BLAKE2b(code
  secret) in a Play by Address room joined or hosted with a code, else 32 zero bytes. Low-order public keys are refused.
- Every pair of members has a key: the host's with a joiner from the join, the others from the public keys in the (sealed) roster, so
  a new host already shares a key with everyone and a host claim cannot be forged.
- Room key G (32 random bytes): made by the first host, sent inside the sealed WELCOME, kept across host changes. It tags beacons
  (section 5).

### 3. Sealed datagrams
```
 'G' 'S' | proto u8 | sender slot u8 (0xFF while joining) | counter u48 LE | ciphertext | tag[16]          (26 bytes more per datagram)
 XChaCha20-Poly1305 (Monocypher, compiled into mp_core.cpp's unit with internal linkage), key K(sender, receiver),
 nonce = sender slot | "SMG3" | counter u64 | zeros (the two directions of a pair differ by the sender slot), additional data = header
```
- Everything after the join is sealed, including the reliable channel's acknowledgements, heartbeats, bundles, BYE, the quit-time
  goodbye and the save snapshot. Plain datagrams are accepted only to start a join (HELLO1 / HELLO2 to a host, COOKIE / REJECT to a
  console that is connecting); everything else plain is dropped.
- Replay: per pair and direction, a 64-entry window behind the highest counter seen. A membership's counters start at
  (membership order byte << 40), above every earlier membership of the same console (same key: section 2), so a recorded datagram of
  an earlier stay cannot be replayed into a later one. The host tells a rejoining console its floor in the COOKIE.
- Anything that does not open is dropped silently and counts against nobody: no "bad message" count, no rate cap, no log beyond a
  throttled counter. A member is only ever removed for what it sealed itself.
- Identity is the key, not the address: a datagram that opens with a member's key from a new address moves that member there (NAT
  rebinding, Wi-Fi reconnect). A plain one never moves anything.

### 4. Joining, rejoining, leaving, being removed
- HELLO1 (joiner -> host, plain, unreliable): fingerprint, protocol, public key, 8-byte nonce, padding to 70 bytes. The host keeps no
  state and answers COOKIE: nonce, host public key, cookie, session id, host slot, "needs a code" flag, counter floor. The COOKIE is
  never larger than HELLO1. cookie = BLAKE2b(key = the host's cookie secret, address | public key | nonce | 10-s time bucket); the
  current and the previous bucket are accepted.
- HELLO2 (joiner -> host, reliable, plain): the old HELLO fields + public key, nonce, cookie, proof = BLAKE2b(K, "hello" | cookie |
  nonce), the rejoin token masked with BLAKE2b(K, "tmask" | ...), and the token's hash. Only a HELLO2 with a valid cookie and proof
  takes a slot; WELCOME and everything after it is sealed. A wrong proof gets no answer at all (a code guesser learns nothing). A host
  looks at no more than 4 wrong proofs a second from one source address, and no more than 16 a second in all from the sources that
  already failed one (a "suspect" for 30 s); a console the room knows (a member, a recent leaver) is held only to its own allowance.
  Guessing a 36-bit secret (68.7 billion values) at that ceiling: assuming an attacker with many addresses that is held to the 16 a
  second, about 68 years on average (136 years to try every value); from one address at 4 a second, about 272 years on average (545
  years for every value). The attacker also needs a valid cookie for each try, and the host's key-exchange budget (20 a second, 6 of them
  kept for sources that never failed) is slightly tighter than 16, so these are lower bounds on the time. A code only lives while its
  room is open and is made fresh for each game.
- Join floods: a cookie (HELLO1 answer) costs the host no memory and no key exchange, and has its own
  allowance: up to 1,000 a second for unknown sources and, apart from it, 100 a second for consoles the room knows, so a forged flood
  from many addresses cannot make an honest player wait. A key exchange (the costly step of a HELLO2) is metered at 20 a second for
  strangers; a source that failed a proof is a suspect for 30 s and may use only what is left after 6 a second are kept for sources that
  never failed. Questions that need no key exchange are answered first, from what the HELLO already shows: a kicked console, a full or
  closed room, a repeat of a HELLO2 that already started a channel (dropped). A console the room knows is not held to the strangers'
  budgets, and a returning player's place is never "full". A joiner that never acknowledged anything gives way to a real player when the
  room is full, and the slot of a player who lost its link is kept for that player alone (see "A held place", below).
- A refusal (REJECT) to a connecting console must echo its nonce; a 1-byte REJECT VERSION still goes to an older build that has no
  cookie step (stateless, rate-limited, smaller than the request).
- A cookie that is not followed by a WELCOME within 2 s restarts the join (the console that answered was not the room: an old room at
  that address that is going away, a restarted host).
- Slot takeover (a rejoin, or the same console restarting) only with the token whose hash the roster holds; a name or console id
  proves nothing. A HELLO2 from a member's own address replaces that member's session only after its cookie and proof check out.
- Kicks: the kicked console is refused by the commitment of its rejoin token, by its public key, or (not online) by its address. The clear-text
  console id of a HELLO bans nobody (a hostile member could join under a victim's id and get the victim banned). The ban list (at most 8
  entries: token commitment, key and, off the internet, address; no names) rides as a trailer of the roster and the WELCOME to every
  member, so a ban survives a change of host. It is a ring: a ninth removal forgets the oldest. It ends with the session.
- A held place: the slot of a player whose link dropped is reserved for that player alone, by the commitment of its rejoin token: 70 s
  after the host removed it for silence (which happens after 15 s of silence), and 15 s after any other departure or after its host vanished. A newcomer is
  refused with the ordinary "full" answer even when that is the only free slot (the core cannot tell it why); the returning console gets its
  own slot back whatever the room's count, and a HELLO that matches a held slot is never refused as full before the key exchange.
- Removed while still running (the host took it for gone, kicked it, or it was an old host): for 30 s the host answers anything that
  console seals with its old key with a sealed "you were removed"; the console then rejoins at once instead of waiting for silence.
- Ghost joiners (outside review, mp-int 9844032): a joiner that never acknowledged anything keeps its 5-s clock across new HELLOs,
  and a full room lets the oldest such joiner (older than 0.5 s) go when a real player asks.

### 5. Discovery, gathering, VLink
- Beacons (52 bytes) are public: anyone on the Wi-Fi can read them and forge them. They carry an 8-byte tag made with the room key over
  the first 44 bytes. A split session (two rooms of one session after a partition) merges only into a room whose beacon carries its
  own room key's tag (and still needs 3 consecutive beacons from a member's address). Lists of OTHER rooms (scan, gathering) cannot
  check tags and do not.
- Scanned rooms keep their first address while it beacons (moved only after 2.5 s of silence); one address lists at most 2 rooms.
- Gathering (one door) runs before any key exists: see 6. Every game's gathering screen starts it.
- VLink (Same Wi-Fi, Nearby, Play by Address): a virtual address is pinned to the real address it was first heard at; frames with it
  from elsewhere arrive under an alias address (127.x.y.z) that the session authenticates (sealed: the member moves) or drops. One
  real address holds at most 2 virtual addresses; the table evicts the entry heard from least recently; frames to closed ports teach
  nothing. Per-socket queue (128): a source holding an eighth of a full queue loses its own oldest datagram; anyone else's arrival
  pushes out the oldest.
- Before keys (stage 2, still in force): only STATE / POINTER travel unreliable; unreliable sequence at most 1024 ahead of the last
  accepted (else dropped, unless that origin was silent 2 s); per-origin receive budget 160 messages (a full queue removes that
  player, never ends the session); the rate cap (200/s, doubled in a 3-s grace after a host change) counts everything but bare acks;
  rosters carry members' addresses only on Same Wi-Fi / Nearby (host migration needs them).
- Play by Address from the gathering screen (mp_gather_get_code): the leader's room becomes a Play by Address room with a fresh
  secret (36 bits in the current core); its members are told the leader's LAN address and the secret in a sealed REDIRECT_CODE and follow;
  the code the leader reads out is what lets a friend reach the room. (The code is being replaced by a short one: this paragraph is about
  the core's secret, not the code's shape.)

### 6. What remains possible (threat model)
Someone on the same Wi-Fi or in Nearby range, or on the path of a Play by Address game, can still:
- jam the radio, drop, delay or reorder datagrams: members then time out exactly as on bad Wi-Fi (15 s) and rejoin;
- see that a game exists, who hosts it (the beacon's name), its mode, player count, started or not, and the sizes and timing of the
  encrypted traffic;
- list fake rooms in the scan, and in the gathering phase (before any key) make a gathering room merge into its own room, or keep
  honest gathering rooms apart for a while with forged CLOSING beacons (each honest room can still start on its own; tested);
- **man-in-the-middle a Same Wi-Fi / Nearby join**: there is no earlier trust between two consoles on a LAN, so a console that answers
  a joiner before the real host (or sits between them) can make the joiner join it instead. It does not get into the real room (it has
  no member's key), but the joiner may end up in a fake one. Play by Address with a code (the secret is the PSK) and the online service
  prevent this; plain Play by Address with a typed address does not;
- refuse a join it can see (it can forge a REJECT that echoes a nonce it sniffed);
- as a former member who was kicked: stay out only while it keeps the identity it joined with (its key and token; and, off the internet,
  its address). With a new console key it is a new player, and from a new address it can come back to an open Same Wi-Fi / Nearby room
  (the host can remove it again). A removal is for ending a disruption among friends, not a permanent ban;
- as a member: anything a member may do. The host checks rates and sizes; plausibility of world events is the game side's world guard,
  which can now report a sender (mp_core_report_bad).
They cannot: speak as a member, kick, move, freeze or desynchronise one, read the game's traffic, take over a slot, replay old traffic,
or join a Play by Address room without its code.

### 7. Costs
Wire: 26 bytes per datagram (one tag per datagram, not per packet: bundles share it). At 30 poses a second per link that is about
0.8 KB/s per link each way; a 1100-byte datagram (the largest the core sends) grows by 2 %. Beacons grow from 44 to 52 bytes (2 a second).
Measured on the development machine (Xeon 2.1 GHz, x86-64, -O2, tests/mp/seal_bench.cpp, the same Monocypher code):

| operation | time |
|---|---|
| pair key (X25519 + BLAKE2b) | 320 us |
| seal / open, 80-byte datagram | 3.5 / 3.5 us |
| seal / open, 300-byte datagram | 6.7 / 7.1 us |
| seal / open, 1200-byte test buffer (the largest real datagram is 1100 bytes, so this is a little above the worst case) | 16.7 / 21.8 us |

**Vita: estimated, NOT measured.** The Vita's Cortex-A9 runs at 444 MHz (a fifth of the clock) and does 32-bit arithmetic only;
portable ChaCha20 / Poly1305 / X25519 need roughly 1.5 to 3 times the cycles they need on x86-64. That gives about 3 to 6 ms for a pair
key (once per member at a join or a host change, on the network thread, never on the game thread) and about 15 to 30 us for a
300-byte datagram, 80 to 150 us for the largest one (the test used 1200 bytes; the real ceiling is 1100). A 4-player host seals and opens about 200 datagrams a second (mostly poses of
100 to 300 bytes): 4 to 8 ms of one core per second, under 1 %. To measure on hardware: put `SMG_MP_CRYPTO_LOG=1` in
ux0:data/smg/env.txt; each key making (host start, join) then logs one line "crypto cost: pair key N us, 1200-byte datagram seal N us,
open N us". If the numbers came out far worse, the fallback is a MAC-only seal (SipHash, 8-byte tag), which keeps every protection
here except confidentiality.

### 8. Tested, and NOT tested
Tested on the host (loopback hub, virtual time, ASan / UBSan): tests/mp/core_tests.cpp (incl. core2_sec_tests.h: cookie join, code
secret, proof ceiling, every byte of a sealed datagram flipped, replay, sealed rebinding, the removed-console notice, ghost restarts,
self-announced friend codes and their hostile forms, the bad-message report; rev_sec_tests.h: all 7 of the outside review's tests),
core_fuzz (1 million mutated datagrams, member traffic delivered as authenticated), payload_fuzz, seal_test, gather_soak (2 to 4
consoles, 15 % loss, forged beacons; 270 runs), gather_code_test (the code switch with 3 consoles, a friend by code, a wrong secret,
Start), soak_exact (exactly-once across rejoins and host changes, 5 to 25 % loss, backoff shifts and clock creep), the service tests
against the local fake service (tests/mp/service_e2e_test.sh).
**Not tested:**
- **The menus.** The tests above drive the core directly, not through the screens; the menus' own tests and two-console runs on a computer
  exercise the gathering screen, but not on a console.
- **Play by Address with a code on real networks**: the UDP socket, the router's port mapping, the public address (STUN), the members
  finding the leader by its LAN address (mp_direct_local_ip) and NAT rebinding were compiled for desktop and Vita but only exercised
  over the loopback hub.
- **Anything on Vita hardware**: the crypto costs above are estimates; Nearby (ad hoc) with sealed traffic has not run on consoles.
- Same Wi-Fi with the sealed protocol on real Wi-Fi between consoles.

### 9. Known limits
- **255 rejoins**: a membership's counters start at its order byte << 40; that byte wraps after 255 rejoins of one console in one
  session. A console's datagrams to members that kept the old replay window are then dropped until it rejoins once more (it does so by
  itself: they time it out).
- A beacon tag is 8 bytes (64 bits): forging one means guessing it; a wrong guess only makes the host ignore a beacon.
- The room key is not changed after a kick (the earlier design said it would be): a kicked console that kept the key can still make
  tagged beacons of that session, which can at most make a split session's host merge into its room, where it has no slot.
- The core's 36-bit code secret is short so that people can read codes aloud; the per-source and all-suspects ceilings in section 4 are what
  make guessing it useless in practice (about 68 years on average at the best the ceilings allow an attacker).
- Self-announced friend codes are not verified (by design: see 1).

### 10. Differences from the earlier design (design notes, kept outside the source tree)
- Key pairs and tokens are per console and session (derived from the console key), not new at every join: needed so that consoles
  with a stale roster and restarted consoles keep working; replay across stays is prevented by the counter bases instead.
- The nonce has the sender slot only (the receiver is implied by the pair key).
- Cookie bucket 10 s (current and previous accepted), not 20 s.
- No Link::Rebind call: VLink delivers moved frames under an alias address and the session moves the member when the datagram opens.
- Per-origin receive budget 160, not 64 (a host change's catch-up is 100 to 200 messages).
- The room key is not rotated after a kick (see 9). A removed player can come back as a new console in an open Same Wi-Fi or Nearby game: the player documents say so.
- REDIRECT / REDIRECT_CODE are core messages 0x0A / 0x0C; the self-announced friend code is 0x0D.

### 11. Findings of the outside review -> what fixes them
1 VLink identity / eviction: VLink pinning, aliases, per-address limit, least-recent eviction (5). 2 forged REJECT: REJECT only to a
connecting console, with the nonce (4). 3 junk kicks: "bad" counts only sealed traffic (3). 4 forged unreliable events: only poses
unreliable (5) + sealing. 5 pose freeze: sequence window (5) + sealing. 6 forged ACK: acks only from sealed datagrams (3). 7 queue
flood: per-origin budget, rate cap (5). 8 console-id takeover / kick evasion: tokens and key bans (4; limit in 6). 9 strangers by
address / reflection: code secret + cookie (4). 10 roster addresses: not sent by address / online, and sealed (5). 11 discovery /
merge: first-seen address (5) + room-key tags (5). 12 game side: mp_core_report_bad gives the world guard a way to act.
