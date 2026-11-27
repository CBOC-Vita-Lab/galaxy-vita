# Online service protocol, version 1

What the Vita client speaks to the Strikers Charged online service (a Cloudflare Worker + one Durable Object, or the
local fake in `fake/`). Both run the same code, so this document is valid for both. Everything is JSON over HTTPS.
Match traffic itself never goes through the service: it is Vita to Vita UDP (see "Client recipes").

## 1. Conventions

| Item | Rule |
|---|---|
| Base | `https://<host>/v1/...` (the fake also offers plain `http://127.0.0.1:8787`). No cookies, no redirects. |
| Methods | `GET`, `POST`, `PUT`, `DELETE`. Bodies are JSON (`Content-Type: application/json`), UTF-8, at most **8192 bytes**. `GET` has no body. |
| Auth | `Authorization: Bearer <token>`; token = 43 chars of `[A-Za-z0-9_-]` returned once by `/v1/register`. Store it in the save file. Endpoints marked "open" need no token. |
| Version | Client sends `X-Msc-Proto: 1` (wire protocol; a whole number, defaults to 1 when absent; **anything else, such as `abc`, `1.5` or an empty value, is `400 bad_app_version`**) and **`X-Msc-App: <game version>`** such as `1.2.0` (dotted numbers, compared numerically) on **every request: the production service requires it** (`REQUIRE_APP_VERSION=1`; a request without it is `400 bad_app_version`, except `/v1/info`, `/v1/time`, `/v1/servers`), plus optionally `X-Msc-Build: <8..64 hex>` (the build / rule fingerprint hash). Every answer carries `X-Msc-Proto: 1`. A too-old protocol is `426 upgrade_required`; a game older than the server's minimum is `426 update_required` with `minVersion`; **every error body carries `latestVersion` when the server knows it**, so any failure can show "update to X". Show "different version" in the UI for both. Answers may gain new fields at any time (ignore unknown fields); **requests are strict**, see below. |
| Strict requests | Every request body is checked against an allowlist of fields; an unknown field is `400 unknown_field` (with `field`). Endpoints without parameters accept no body or `{}`. Types and ranges are validated field by field. |
| Time | Milliseconds since the Unix epoch, UTC, JSON numbers (fit in a double). `GET /v1/time` is the server clock. |
| Binary | Base64, standard alphabet **with** `=` padding. Never URL-safe except the bearer token. |
| Integers | JSON integers. Profile ids (`pid`) are positive and > 100000, always fit a signed 32-bit int. |
| Success | HTTP 200 and a JSON object. |
| Failure | Non-2xx and `{"error":{"code":"name_taken","message":"...", ...extra}}`. Branch on `code`, never on `message`. `retryAfterSec` is present on 429/503 where waiting helps; `Retry-After` is set on 429. |
| Caching | Everything is `Cache-Control: no-store`. |
| Privacy | The service sees the connecting address of each request for rate limiting only (kept in memory, never stored). Peer addresses are stored for the lifetime of a room and deleted a few minutes after it ends. |

### Error codes

| HTTP | code | meaning |
|---|---|---|
| 400 | `bad_json`, `bad_request`, `unknown_field` (+`field`), `bad_app_version` | body is not JSON / not the expected object / a field outside the allowlist / malformed or missing `X-Msc-App`, malformed `X-Msc-Proto` (`field:"x-msc-proto"`) or `X-Msc-Build` |
| 400 / 428 | `bad_pow` (+`reason`: `format` `expired` `mac` `hash` `used`), `pow_required` (+`bits`, `challenge`) | only when the service has the optional registration proof of work switched on (section 3) |
| 400 | `bad_name` (+`reason`) | `too_short` `too_long` `charset` `edges` `spaces` `too_few_alnum` `reserved` `filtered` `type` |
| 400 | `bad_region`, `bad_category`, `bad_score`, `bad_param` (+`field`), `bad_mode` | field validation |
| 400 | `bad_code`, `bad_endpoint`, `bad_nonce`, `bad_match`, `bad_result`, `bad_checksum`, `relay_only_account` | room / match field validation (`relay_only_account`: a relay-only profile sent a direct address) |
| 400 | `bad_blob`, `bad_digest`, `bad_blob_name` | ranking userdata rejected (size, HMAC-MD5, or the name inside it) |
| 401 | `unauthorized` | missing, malformed (not `Bearer` + 43 characters of `A-Za-z0-9_-`) or unknown token. Malformed headers are answered at the edge and cost the service nothing; unknown tokens also count toward a per-address brake |
| 403 | `banned`, `not_member`, `turn_not_allowed` | |
| 404 | `not_found`, `room_not_found`, `match_not_found`, `player_not_found` | |
| 404/405/413/426 | `not_found` (unknown path), `method_not_allowed` (`Allow` header lists the right one), `too_large`, `upgrade_required`, `update_required` | unknown paths, wrong methods and oversized bodies are answered at the edge |
| 409 | `name_taken`, `own_room`, `room_full`, `in_ranked_match`, `room_not_ready`, `match_not_started`, `match_closed`, `already_submitted`, `mode_mismatch` | state conflicts |
| 410 | `room_expired`, `room_closed`, `ticket_expired` | |
| 429 | `rate_limited` (may carry `reason`: `too_early`, `daily_cap`), `rename_too_soon`, `queue_cooldown`, `turn_player_limit` | wait `retryAfterSec` |
| 500/502/503 | `internal`, `turn_upstream`, `service_disabled` (kill switch / maintenance, `message` may be shown to the player), `server_full`, `turn_budget`, `turn_unavailable`, `server_misconfigured`, `busy` | |

Network failures, timeouts and any 5xx are retried by the client with backoff (1, 2, 4, 8 s, then every 15 s) up to the
caller's patience; 4xx are not retried except 429 after `retryAfterSec`. The DWC ranking client turns every failure into
`DWC_RNK_IN_ERROR` (the original hid failures as success; we do not).

## 2. Service info and clock (open)

### `GET /v1/info`
```json
{"proto":1,"minProto":1,"serverMs":1793361600000,"kill":[],"message":null,
 "features":{"ranked":true,"turn":true,"ranking":true},
 "limits":{"nameMin":2,"nameMax":10,"dataMax":768,"blobSize":128},
 "stun":["stun.cloudflare.com:3478"],"punch":{"intervalMs":700,"tries":7,"lingerMs":5000},
 "roomCodeAlphabet":"ABCDEFGHJKMNPQRSTUVWXYZ23456789","resultWindowSec":120,
 "app":{"min":"1.2.0","latest":"1.3.0"},
 "link":{"pingAfterSec":5,"lostAfterSec":15,"queueDropSec":30,"joinAckSec":5,"natLifeSec":30},
 "servers":[{"url":"https://example.workers.dev","priority":0}]}
```
Call it first on "Online": it doubles as the reachability and ping test (`kill`/`message` explain a switched-off service). `link` gives the
liveness timings every session follows: **ping after 5 s of silence, link lost after 15 s, silent queue tickets dropped after 30 s, a ranked pairing
must be confirmed within 5 s, NAT mappings live ~30 s** (see sections 4 and 5).

### `GET /v1/servers` (open)
`{"version":1,"ttlSec":86400,"servers":[{"url":"https://...","priority":0}],"stun":["..."],"app":{"min":null,"latest":"1.3.0"},"serverMs":...}`
The list of service URLs (https only, lowest `priority` first). The client ships one built-in URL, caches the last list it received for `ttlSec`, and
falls back through it when the built-in host stops answering, so the service can move without a Vita update. A list can also be hosted anywhere
static (same JSON); `servers.example.json` shows the shape. A list is only trusted over a verified TLS connection.

### `GET /v1/time`
```json
{"utcMs":1793361600000,"iso":"2026-10-30T12:00:00.000Z","year":2026,"month":10,"month0":9,"mday":30,
 "hour":12,"min":0,"sec":0,"wday":5,"yday":302,"dayStartMs":1793318400000,
 "season":{"index":43,"startMs":1793232000000,"endMs":1793836800000}}
```
For `DWC_GetDateTime`: `DWCDate{year,month=month0,mday,wday,yday}`, `DWCTime{hour,min,sec}` (month is 0-based in DWC; the game
adds 1 itself). `wday` 0 = Sunday, `yday` 0-based. Use the HTTP `Date` header (or this) to sanity check the Vita clock
before TLS certificate dates are judged. The 52 weekly "seasons" are the game's own calendar (`NetworkSeasonCalendar.cpp`).

## 3. Sign-in and profile

### `POST /v1/register` (no token; a burst of 10 per address, then 3 per hour)
Request `{"name":"Sally","region":2,"relayOnly":false}` (+ `"pow"` when proof of work is on, below). `region` optional: 1 JP, 2 US, 4 EU, 8 KR (default 2). `relayOnly` optional (privacy mode, section 4).
Response `{"pid":100123,"code":"K7QM-4DXT","token":"<43 chars>","name":"Sally","region":2,"relayOnly":false,"createdMs":...}`.
Identity is three things: the secret **token** (proves who you are, never shown to others), the **public code** `XXXX-XXXX` (safe to show and share; grants nothing)
and the typed **display name**. `pid` is the numeric id used in rankings.
The token is shown **once**; the server keeps only its SHA-256. A lost token cannot be recovered (register a new name).

Registration limits (all `429 rate_limited` + `Retry-After`): per client address a **burst of 10** (a venue full of Vitas behind one NAT), then **3 per hour**; IPv6 clients count as one address per /64;
500 registrations a day for the whole service. The body is validated **first**: a request that fails validation (bad JSON, unknown field, bad name, taken name) is never charged to those
limits, only to a separate bucket of 30 bad requests an hour per address, so garbage cannot use up the day's registrations.

**Optional proof of work (off by default; `REGISTER_POW_BITS` > 0 switches it on).** `GET /v1/register/challenge` returns `{"bits":0}` when it is off, otherwise
`{"bits":16,"algo":"sha256","challenge":"<expMs>.<16 hex>.<16 hex>","expiresMs":...}`. The challenge is bound to the caller's address (an IPv6 /64), valid for 2 minutes and usable once. The client
finds a string `nonce` of 1..20 characters of `0-9A-Za-z` such that `SHA-256(UTF-8(challenge + ":" + nonce))` starts with at least `bits` zero bits (about `2^bits` hashes: 16 bits is ~65k hashes), then sends
`POST /v1/register {"name":...,"pow":{"challenge":"...","nonce":"..."}}`. A register without `pow` while it is on is `428 pow_required` and the error already carries `bits` and a fresh `challenge`, so a client
that does not want the extra call can just try, read the 428 and solve it. Bad proofs are `400 bad_pow` with `reason` `format`, `expired`, `mac` (other address or forged), `hash` (not enough zero bits) or `used`.
While it is off the `pow` member is accepted and ignored, so a client can always send what it has.

Name rules: 2..10 characters after trimming (the game's `mName` holds 10 + terminator), `A-Z a-z 0-9 space . _ -`, must start
and end with a letter or digit, no double spaces, at least two letters/digits, not reserved, passes a small profanity
filter. Unique, ignoring case, spacing/punctuation and look-alikes (`0/O`, `1/l/I`, `3/E`, `4/A`, `5/S`, `7/T`, ...):
`Sally`, `sally`, `Sa11y` and `S.a.l.l.y` are the same name (`409 name_taken`). Validate on the Vita first (same rules) and show the
`reason` from the server otherwise. Show the Vita user name as a *suggestion* only.

### `GET /v1/me`
`{"pid","code","name","region","relayOnly","rating","wins","losses","draws","abandons","createdMs","nameChangeAllowedMs","cooldownUntilMs"}`

### `PUT /v1/me`
`{"name":"New","region":4,"relayOnly":true}` (every field optional). Names can change once per 24 h (`429 rename_too_soon`). Returns the profile.

### `DELETE /v1/me`
Deletes the profile, its leaderboard rows and queue entry (privacy). The name becomes free. Token stops working.

### `GET /v1/players/{code}`
Find a player by public code (`K7QM-4DXT`, dash/case optional): `{"pid","code","name","region"}`. `404 player_not_found`. 30 per minute.

### `GET /v1/me/matches?limit=10`
`{"matches":[<match view>, ...]}` newest first.

## 4. Invite rooms and signaling (unranked)

A room links exactly two players. The service exchanges their addresses, hands both a session key and cookie, and assigns
unique virtual IPs (vips). Rooms are created by the host and joined by a short code.

### `POST /v1/rooms` -> room view (host)
Closes your other open rooms. 10 per 10 minutes.

### `POST /v1/rooms/join` `{"code":"K7QM4D"}` -> room view (guest)
Codes are 6 characters from `ABCDEFGHJKMNPQRSTUVWXYZ23456789` (no `I L O 0 1`); input is case-insensitive and ignores spaces
and dashes. Idempotent for the same guest. 20 tries per 10 minutes. Errors: `bad_code`, `room_not_found`, `own_room`,
`room_full`, `room_expired`, `room_closed`, `in_ranked_match`.

### `GET /v1/rooms/{code}` -> room view, `GET /v1/rooms/{code}?v=<version>`
Poll every **1.5-2 s** while waiting. With `?v=` equal to the last seen `version` the answer is the tiny
`{"code","state","version","changed":false}`; otherwise the full view with `"changed":true`. Members only (`403 not_member`).

### `POST /v1/rooms/{code}/endpoint`
```json
{"public":"203.0.113.7:41000","local":"192.168.1.20:41000","nonce":"AAECAwQFBgc=","nat":"cone",
 "candidates":[{"type":"host","addr":"192.168.1.20:41000"},{"type":"srflx","addr":"203.0.113.7:41000"},{"type":"observed","addr":"203.0.113.7:50111"}],
 "relay":"198.51.100.8:50001"}
```
* `public` = the address STUN saw for the **game's own UDP socket**, `local` = the socket's LAN address (both IPv4 `a.b.c.d:port`, no leading zeros). Required, except for relay-only accounts.
* `nonce` = 8..32 random bytes (base64) the sender puts in its punch packets so the receiver can recognise the right peer. Required.
* `nat` optional: the NAT class from two STUN servers (same mapping = `cone`/`open`/`restricted`, different = `symmetric`, else `unknown`). `symmetric` steers the room to the relay at once.
* `candidates` optional, up to 6 `{type: host|srflx|observed|relay, addr}`: everything the client learned about itself (host, server-reflexive from STUN, and the source address a first inbound packet showed).
* `relay` optional: the address of the player's TURN allocation, so the partner can send through the relay (needed for relay-only play and for the fallback).
May be posted before the guest arrives, and again after a reconnect or when told `stale`. The partner's endpoint appears in `peer.endpoint` only after **both** sides posted.

### `POST /v1/rooms/{code}/punch`
`{"result":"ok","path":"direct","ms":850}` — `result` `ok`/`fail`, `path` `direct`/`lan`/`relay`, `ms` optional time to connect. Each member reports once per attempt.
The server keeps a failure count per pair of players (a success counts back; "fail" from either side wins over "ok"; a `relay` path counts as a failed punch; counts expire after 7 days).
When failures minus successes reach 2 for a pair, their next room has `session.steer.relay = true` and they skip punching. Returns the room view.

### `POST /v1/rooms/{code}/started`
Both players call it once the link is confirmed both ways and the match is about to begin. When both did, `state` becomes
`started` and the match record starts (`matchId`). `409 room_not_ready` before endpoints were exchanged.

### `POST /v1/rooms/{code}/leave` (or `DELETE /v1/rooms/{code}`)
Host: closes the room. Guest before the start: leaves, the room reopens with a fresh key. After the start, or in ranked rooms:
the room closes for both. `{"ok":true}`.

### Room view
```json
{"code":"K7QM4D","kind":"unranked","state":"ready","version":7,"role":"host","slot":0,
 "createdMs":..,"expiresMs":..,"closedReason":null,"matchId":"aB3dE5gH7jK9",
 "you":{"pid":100123,"name":"Sally","vip":"10.70.3.1"},
 "peer":{"pid":100456,"name":"Bert","slot":1,"vip":"10.70.3.2","link":"ok",
         "endpoint":{"public":"203.0.113.9:41000","local":"192.168.0.5:41000","candidates":[],"relay":null,"nonce":"...","nat":"cone","ageMs":2100,"stale":false}},
 "session":{"key":"<32 bytes base64>","cookie":2918331747,"punchAtMs":1793361601000,
            "punch":{"intervalMs":700,"tries":7,"lingerMs":5000},"stun":["stun.cloudflare.com:3478"],
            "natLifeSec":30,"steer":{"relay":false,"reason":null}},
 "started":{"you":false,"peer":false},"changed":true}
```
* `state`: `open` (host alone) -> `joined` (two players, `session` present) -> `ready` (both endpoints posted) -> `started`;
  terminal: `closed` (see `closedReason`: `replaced`, `host_left`, `guest_left`, `match_over`, `account_deleted`) and `expired`.
* `peer` is `null` until a guest joined; `peer.endpoint` is `null` until both posted. `session` is `null` before the second player joins and after the room ended.
* `peer.link`: `ok` / `quiet` (no call from the partner for 5 s) / `lost` (15 s). **Any call a member makes counts as presence**; `GET /v1/rooms/{code}?v=` every 1.5-2 s is the ping. Show "waiting for partner" on `quiet`, end the attempt on `lost`.
* `peer.endpoint` = `{public, local, candidates, relay, nonce, nat, ageMs, stale}`. **`stale: true`** means the partner's address is older than the 30 s NAT-mapping life and no punch has been reported: re-run STUN and re-post yours and ask the partner (they see the same) to do the same. Keep punching sockets alive with a packet at least every 20 s.
* `session.steer` = `{relay, reason}`: when `relay` is true (`reason` `relay_only`, `nat`, `pair_failures`) skip hole punching and request TURN credentials.
* **Privacy / relay-only.** With `relayOnly` set on a profile (`PUT /v1/me`), that player never sends or receives a raw IP: `public`, `local` and `candidates` are omitted from `peer.endpoint` on both sides (only `relay` and `nonce` remain), and endpoint posts from that account must contain only `relay` + `nonce` (`400 relay_only_account` otherwise). Everyone else's addresses are only ever shown to the one partner, only after both posted, and are deleted a few minutes after the room ends.
* `slot` 0 = host, 1 = guest. In ranked rooms the player who waited longer is slot 0. Use the slot for deterministic sides/channels.
* `vip` = the virtual IPv4 for this player inside the game's VLink layer (`10.64-127.x.1` host, `.2` guest; unique among live rooms). Use these instead of the Wi-Fi address.
* `punchAtMs` = server time at which both sides should start punching (both know each other's endpoints from then on); compare with `/v1/time`.
* TTLs: `open` 10 min, `joined`/`ready` 20 min, `started` 3 h, then `expired`. Closed/expired rooms are readable for ~5 minutes, then `404`.
* `session.key`: 32 random bytes per room, shared by exactly the two players. Use it directly with an AEAD (ChaCha20-Poly1305 or AES-256-GCM); nonce = `sender slot (1 byte) || 3 zero bytes || 64-bit counter (BE)`, never reused, counter travels in the packet.
  `session.cookie` (u32) goes into every punch/ping packet (the GameSpy natneg cookie) so strangers' packets can be dropped.
* `session.natLifeSec` (30) and `session.stun` repeat the values from `/info`.

## 5. Ranked matchmaking

### `POST /v1/ranked/queue` -> queue view; `GET /v1/ranked/queue` poll; `DELETE /v1/ranked/queue` leave; `POST /v1/ranked/queue/resume`
Poll every **2 s**. The join answer carries a one-time **ticket** `{"id","token"}`; keep it (RAM is enough, or the save for a long wait).
```json
{"status":"queued","waitedMs":4200,"window":100,"link":"ok","ticketId":"...","ticket":{"id":"...","token":"..."},"expiresMs":...,"roomCode":null}
{"status":"matched","waitedMs":6100,"window":150,"roomCode":"R9TQ2M","room":<room view>}
{"status":"none"}                     // not queued (left, dropped, timed out; "reason":"ack_timeout" if you missed the 5 s confirmation)
```
* **Liveness.** `link` is `ok` / `quiet` (no poll for 5 s) / `lost` (15 s). A `lost` ticket is skipped by the matchmaker (no pairing with ghosts) and dropped after 30 s without a poll; waiting more than 5 minutes ends with `status:"timeout"`.
* **Resume.** After a dropped connection (Wi-Fi blip, app restart, suspend of under 30 s): `POST /v1/ranked/queue/resume {"ticketId","ticketToken"}` returns the current queue view and keeps the place and waiting time. `410 ticket_expired` = join again.
* **Confirmation.** Receiving `matched` (or any later call on the room) is your confirmation. If one side has not confirmed within 5 s the pairing is voided: the confirming player is put back in the queue with the same waiting time (`status:"queued","requeued":true` or just `queued`), the other is dropped (`none`/`ack_timeout`).

Matching: a hidden rating (starts 1000, Elo K=24, floor 100) decides. Two players pair when their rating difference is within
**both** windows; the window starts at 100 and grows by 50 every 5 s of waiting, up to 1000. Different regions only pair after
20 s of waiting. After `matched` continue exactly like an invite room with `room.code` (endpoint, punch, started). Ranked rooms
cannot be joined by code. Errors: `queue_cooldown` (you abandoned a ranked match, `retryAfterSec`), `in_ranked_match`.
Leaving a ranked room before it starts voids the match without penalty.

## 6. Match records and result checks

Each room that reaches `started` has a match record (`room.matchId`; ranked rooms have it from the moment of pairing).
When the match ends each player submits what their simulation produced. **A ranked result counts only when both players
submit the same scores and checksum.**

### `POST /v1/matches/{matchId}/result`
```json
{"result":0,"score0":3,"score1":1,"checksum":"0f1e2d3c4b5a6978","attempt":1,"build":"a1b2c3d4e5f60718","mode":"ranked"}
```
* `result` = the game's own code from `CalculateAndReportGameResult`: **0** normal finish, **2** desync/sync error, **3**/**4** connection lost / queue overflow.
* `score0`/`score1` = final goals of slot 0 and slot 1 (0..99). `checksum` = 8..64 hex chars, the **end-of-match state hash** (STR-151 digest), the same value on both Vitas for the same match. **A result is committed only when both scores and both hashes agree.**
* `attempt` (1..100000, default 1), `build` (hex build/rule fingerprint hash) and `mode` (`ranked`/`unranked`, must match the match, else `409 mode_mismatch`) are optional metadata for the client's retry queue.
* **Not too early.** A normal finish (`result` 0) in a ranked match is refused with `429 rate_limited` and `"reason":"too_early"` (+ `retryAfterSec` = time left, `Retry-After` set) while less than `MIN_MATCH_SEC` (default **60 s**) have passed since both players called `started`. Nothing is stored; retry after the wait (the normal retry queue does this by itself). The game's shortest timed match is 2 minutes (`sGameTimeOptions` = 120/180/240/300 s, default 180 s); only a goal-limited match (3..10 goals) can end sooner, and 60 s is below any realistic 3-goal game. Desync / connection-lost reports (2, 3, 4) and unranked matches are never held back.
* **Idempotent.** Send the report from a persistent retry queue (it must survive suspend and crashes) and keep resending, with a growing `attempt`, until you get a 200. Resending the identical report always returns 200 with the current view and `"duplicate":true` (also after the match settled), and changes nothing; drop the queue entry then. A *different* report for the same match and player is `409 already_submitted`. `409 match_not_started` if the room never started, `409 match_closed` if the window passed before your first report arrived (you were the leaver).

### `GET /v1/matches/{matchId}`  (poll every 2 s until `final`)
```json
{"matchId":"aB3dE5gH7jK9","kind":"ranked","status":"verified","final":true,"slot":0,"leaverSlot":null,
 "submitted":{"you":true,"peer":true},"attempts":1,"buildsMatch":true,"resultDeadlineMs":null,"scores":{"slot0":3,"slot1":1},
 "credit":{"points":13,"won":true},"reason":null,"penalty":null}
```
`reason` (new, ignore if unknown) says why a settled match was not rated: `daily_cap_pair` / `daily_cap_account` (see below). `penalty` (new) is `{"rating":-9,"restoreWithheld":true}` when this player was charged under the dodge rule, else `null`.
| status | meaning | rankings |
|---|---|---|
| `pending` / `started` | not finished | - |
| `awaiting_peer` | one report in; `resultDeadlineMs` = first report + 120 s | - |
| `verified` | both `result:0`, identical scores and checksum | points credited, rating, W/L |
| `disputed` | both `result:0` but scores or end-of-match hashes differ (`buildsMatch:false` explains a likely cause) | nobody gains; the 5-point start penalty is restored |
| `no_contest` | a desync (2), both lost the connection (3/4), or no report for 90 min | same as disputed |
| `abandoned` | only one player reported within the window | the silent player is the **leaver**: forfeit loss, `abandons+1`, no restore of the 5 points, queue cooldown 60 s x repeats (max 15 min, repeats counted within 24 h); the other gets a flat forfeit win (10 points + restore) |
| `void` | the match never started (room left/expired, 5 min) | nothing |

**Rated-match caps.** Per UTC day a pair of players can have `RATED_PER_PAIR_DAY` (default 4) and one account `RATED_PER_ACCOUNT_DAY` (default 30) rated matches (verified results, and forfeit wins, count). The matchmaker never pairs a pair or a player that is at a cap, and `POST /v1/ranked/queue` answers a player at their own cap with `429 rate_limited` (`"reason":"daily_cap"`, `"scope":"ranked"`, `retryAfterSec` = seconds to 00:00 UTC). A match that still ends over a cap (two matches settled in parallel) settles as `no_contest` with `reason` `daily_cap_pair` / `daily_cap_account`: nobody gains or loses and the 5 points come back; a forfeit over a cap still charges the leaver but the stayer gets no forfeit win. Caps use the server's UTC day.

**Win-trading flag.** A winner with at least `ONE_OPP_MIN_WINS` (8) rated wins in the last 30 days of which `ONE_OPP_SHARE_PCT` (70 %) or more are against the same opponent gets a stored flag (`single_opponent`, with the opponent and the counts). It is a flag for the admin (section 9), never an automatic ban; the player sees nothing.

### The dodge rule (disputes and no-contests)

A loser could avoid a loss by reporting a desync / lost connection or a different score, because a dispute or no-contest changes nothing. The rule that closes this:

1. **Blame.** When a ranked match settles `disputed` or `no_contest`, the server records who caused it. `disputed`: both players (nobody when both sent a build hash and the hashes differ: different builds explain it). `no_contest`: the player(s) who reported 2, 3 or 4 (a player who reported a normal finish is a victim and is never blamed); if nobody reported at all, both. A `no_contest` from a daily cap blames nobody.
2. **Rate.** Over a player's last `DODGE_WINDOW` (20) settled ranked matches (verified, disputed, no-contest and abandoned; this one included), with at least `DODGE_MIN_SAMPLE` (6) of them, a player blamed in `DODGE_RATE_PCT` (35 %) or more is *above the threshold* and gets a stored `dodger` flag (admin only).
3. **Penalty.** A blamed player above the threshold gets no restore of the 5 start points and loses `DODGE_PENALTY_PCT` (75 %) of what losing to this opponent would cost in hidden rating (`ELO_K` x expected score, at least 1). That is smaller than the abandon penalty (100 % of it, no restore, queue cooldown, abandon counter, the opponent gets the forfeit win). The match view shows it as `penalty`. Everybody else (below the threshold, or not blamed) gets the usual restore and loses nothing. Honest matches push old disputes out of the window, so the penalty stops by itself.
4. **Invariant: silence is never better than an honest report.** For the same loss, in hidden rating: abandon (silence) >= honest loss > flagged dodge > 0; in points: an honest loss earns `1 + goals + 5`, silence and a flagged dodge earn 0; only silence also gets the queue cooldown. A test pins this (`results-abuse.test.js`).

`credit.points` is what the game's formula gives that player (win = 10 + min(goals, 10), loss/tie = 1 + min(goals, 10); desync = 0).
Unranked matches follow the same states but change no rating, statistics or credit.

## 7. TURN relay credentials

### `POST /v1/turn` `{"room":"K7QM4D"}`
Only for members of a room in `joined`/`ready`/`started`. The Cloudflare TURN key never leaves the service; the answer holds
short-lived credentials (default 1 h) and only UDP-capable URLs (the Vita has no TCP):
```json
{"ttlSec":3600,"expiresMs":...,"servers":[
  {"urls":["stun:stun.cloudflare.com:3478"]},
  {"urls":["turn:turn.cloudflare.com:3478?transport=udp","turn:turn.cloudflare.com:53?transport=udp"],"username":"...","credential":"..."}]}
```
Use the credential only for this room, and reuse it for the whole hour instead of minting again. Budget: **6 per player per day** (`TURN_MAX_CREDS_PER_PLAYER_DAY`),
300 per day overall, of which accounts younger than 24 h share at most 60 (`TURN_NEW_ACCOUNT_DAILY`), and a monthly relayed-megabyte
budget: errors `429 turn_player_limit`, `503 turn_budget` (also "busy for new accounts"), `503 turn_unavailable` (relay not configured), `503 service_disabled`. In all of
those fall back to direct play or tell the player the relay is unavailable.

### `POST /v1/turn/usage` `{"bytes":123456789}`
Optional: after a relayed match report roughly how many bytes were relayed (counts toward the monthly budget). The number is untrusted, so:
* it is accepted only from a player who **minted credentials within their TTL** (`ttlSec`, plus a 5 minute grace so the report after the match still works); otherwise `403 turn_not_allowed`;
* each report is **clamped** to what one credential can carry: `ttlSec x 128 kbit/s` (57.6 MB for the default hour), and a credential cannot be reported on beyond that in total;
* at most **12 reports per player per UTC day** (`429 rate_limited`, `"reason":"daily_cap"`, `"scope":"turn_reports"`).
The answer is `{"ok":true,"countedBytes":..., "clamped":false}` (`clamped` true when the figure was cut down). Report once, right after the match; a client that sends nothing is fine.

### Client behaviour with the relay (native/platform/sdk/net/online/np_turn.cpp, np_online.cpp)

* **When.** The console asks for credentials when the relay is wanted: the player chose "Relay only" (`MSC_NET_RELAY_ONLY=1`; the client also makes the profile say so with
  `GET /v1/me` + `PUT /v1/me {"relayOnly":...}` before it creates or joins a room, so what the service hides and what the client posts always agree), the room view says
  `session.steer.relay` (honored by default, `MSC_NET_HONOR_STEER=0` ignores it), the partner already published a `relay` address (both sides need an allocation), both NATs are symmetric, or a
  punch used its time. A host alone in an `open` room cannot ask (`turn_not_allowed`): it asks the moment the guest joins.
* **Allocation.** The URLs are resolved on a worker thread (UDP only; the first address that answers is used, the next one after an 8.5 s silence), Allocate with long-term credentials (first request
  unauthenticated -> 401 with REALM + NONCE -> again with USERNAME, REALM, NONCE, MESSAGE-INTEGRITY = HMAC-SHA1 with key MD5(user:realm:credential)), `LIFETIME` 300 s requested and refreshed at half
  of what is granted. Success responses are not integrity-checked (transaction id and source address only, as for STUN).
* **Publishing.** The XOR-RELAYED-ADDRESS is posted as `relay` in `POST /v1/rooms/{code}/endpoint` (nonce unchanged, so the partner does not restart). A relay-only console posts **only**
  `relay` + `nonce` (no STUN is run, no `public`, `local`, `nat` or `candidates` is ever sent). The endpoint is posted again whenever the relayed address changes.
* **Paths.** For the partner's relayed address: CreatePermission and ChannelBind (channel 0x4000.., a new number for a new address), refreshed every 3 minutes (permissions live 5, bindings 10).
  The relay is a candidate of that partner's session: the same encrypted probes and data, framed as ChannelData (padded to 4 bytes). It ranks last, so a direct path that confirms later still takes over.
* **Credentials.** One credential per room and hour: it is reused for every allocation, refresh and rebuilt link of the room, and the console asks again only a minute before `ttlSec` ends
  (tracked on its own clock). It then releases the allocation (Refresh with lifetime 0) while the old credentials still work, allocates with the new ones and posts the new relayed address; the
  partner binds it. If the credentials died early (refusals after a working allocation) the match keeps running on the old allocation until its lifetime ends and the new credentials take over
  then (a few seconds of silence); an allocation orphaned by dead credentials is waited out (the new Allocate gets 437; retried every 3 s for up to 5 minutes). A sign-in the server refuses
  on a new Allocate asks for fresh credentials at most twice.
* **Leaving.** Refresh with lifetime 0 (twice, no wait), then one `POST /v1/turn/usage {"bytes":N}` with the bytes this console **sent** into the relay (each side reports only its own sends, so the
  service's monthly counter is not double counted). The answer is ignored (`403 turn_not_allowed` after the grace, `429` over the daily report cap).
* **Failures** end the attempt with a sentence only when nothing else can work (relay-only, steered pair, or the direct attempt already failed); otherwise they are a warning: `turn_budget`
  "the relay allowance of the online service is used up", `turn_player_limit` "all of today's relay connections", `turn_unavailable`, `service_disabled` (its message), a silent relay server
  "did not answer", a refused sign-in, 486/508 "no room for another player", `429 rate_limited` with `reason` `daily_cap` "Daily limit reached".

### What Cloudflare's real TURN must confirm (the client was written against the RFCs and the documentation from memory)

1. `generate-ice-servers` answers `{"iceServers":[{"urls":[stun...]},{"urls":["turn:turn.cloudflare.com:3478?transport=udp", ...,"turns:..."],"username":"...","credential":"..."}]}`
   (array; the older `generate` call returned one object). `server/src/core/turn.js` accepts both shapes, keeps `stun:`/`turn:` URLs without `transport=tcp`, and the client uses only `turn:` UDP URLs
   with a username. If the UDP URLs carry port 53 as well as 3478, both are tried in order; if only TCP/TLS URLs come back the client reports "not available on this online service".
2. The host name in the URL resolves to IPv4 addresses (the client has no IPv6) and the allocation is **IPv4 UDP**.
3. Long-term credentials: the first Allocate without MESSAGE-INTEGRITY is answered 401 with REALM and NONCE; `MESSAGE-INTEGRITY` (SHA-1) is accepted; stale nonces are answered 438 or 401 with a new
   NONCE (both are handled).
4. **Relay-to-relay forwarding between two allocations** (each side sends from its relayed address to the other's, both on Cloudflare's network) works with CreatePermission for the other relayed
   address' IP. This is the one design assumption that a real test must prove: the whole relay path depends on it.
5. ChannelBind on 0x4000-0x7FFE, ChannelData accepted with 4-byte padding over UDP; Data indications arrive when no channel is bound yet (handled).
6. Credential lifetime: whether a Refresh / CreatePermission with credentials past their expiry is refused (the reactive path) or the allocation simply lives on; the allocation lifetime Cloudflare
   grants (the client reads LIFETIME) and whether a username can be reused for a second allocation after a release (rotation).
7. UDP 3478 reachable from the player networks the Vita will use (a second port, 53, is tried when the first stays silent).

## 8. Rankings (DWC contract)

These two calls carry `DWC_RnkPutScoreAsync` / `DWC_RnkGetScoreAsync`. The server decides which rows come back and which
time window counts; the client's own `since` is accepted (u32) and ignored.

### `POST /v1/rank/put` (12 per minute)
```json
{"category":0,"region":2,"score":14,"data":"<base64 of the 0x80-byte NetworkRankingSubmission>"}
```
* `category` 0..100 (game: 0 season, 1 start-of-day, 2 friends, 3/4 EU alternates). `region` exactly one of 1 JP, 2 US, 4 EU, 8 KR (the DWC enum). `score` 0..999999.
* `data` <= 768 bytes. The game sends exactly **128 bytes** and the server enforces that: bytes `0x70..0x80` must equal
  HMAC-MD5 (key = `sRankingHmacKey`, `NetworkStats.cpp`) over bytes `0..0x6F` (`bad_digest` otherwise), and the UTF-16 name at
  offset `0x0E` (11 units, either byte order) must equal the registered name (`bad_blob_name`). The server recomputes the digest itself and also range-checks the fields (day 1..31, month 1..12, year 2000..2100, wins/losses 0..9999, region 0..15; `bad_blob`).
  **The 0x70 HMAC is an integrity check, not authentication.** Its key (`sRankingHmacKey`) ships inside every copy of the game and `scripts/extract-hmac-key.mjs` prints it, so anybody can compute a valid digest. It catches corrupted or hand-edited blobs and keeps honest clients honest; it proves nothing about *who* sent a blob or whether the numbers are true. Identity is the bearer token; truth comes from the credit rule (scores rise only by what two-sided verified matches earned), both-sides result agreement, the minimum match length, the daily caps and the flags above.
  Only the score, region, update time and this 128-byte blob are stored per player and category; nothing else about the player is kept for the leaderboard. The blob is stored and returned byte for byte; the server never interprets the other fields.
* Response `{"ok":true,"stored":14,"clamped":false}`. The server stores `stored`, which may be lower than `score`: a score can only
  rise by what verified matches earned ("credit"). The game takes 5 points off when a ranked match starts and gives them back at the end; both directions
  work: decreases are always accepted, increases are limited by credit, and every settled match grants the 5-point restore except to the leaver.
  So: **submit the result (section 6), wait until the match is `final`, then put the new totals.** `clamped:true` means the local total is ahead of the server's; re-read
  your own row (NEAR) and continue from the server's value. Credit is per category, so put every category you changed.
* A row only counts in its window: categories 1 and 4 = the current UTC day, everything else = the current weekly season (section 2). A put in a new window starts from 0.

### `POST /v1/rank/get`
```json
{"mode":2,"category":0,"region":2,"sort":1,"since":1234,"limit":5,"friends":[100456,100789]}
```
| mode | name | `limit` | param size in DWC | answer |
|---|---|---|---|---|
| 0 | ORDER | - | 8 | `order`, `score` of the caller (no rows) |
| 1 | TOPLIST | 1..10 | 12 | the best `limit` rows |
| 2 | NEAR | 2..10 | 12 | `limit` rows centred on the caller, **always including the caller's own row** |
| 3 | FRIENDS | 2..65 | 268 | the caller plus up to 64 listed `friends` (profile ids; zeros/unknown ignored) |

`sort` 0 ascending / 1 descending (the game uses 1). `category` 0..100, `region` is a mask (1..255, default 255). Mode may also be given by name
(`"toplist"`). Anything else is `bad_mode` / `bad_param`. By default all regions are pooled (`RANK_REGION_MODE=pool`); with `mask` the request mask filters rows.
```json
{"mode":2,"category":0,"total":42,"serverMs":..,"count":3,"rows":[
  {"order":6,"pid":100456,"score":120,"region":2,"lastupdate":1793361000,"size":128,"data":"<base64>"},
  {"order":7,"pid":100123,"score":119,"region":2,"lastupdate":1793361100,"size":128,"data":"<base64>"}]}
```
Build `DWCRnkData` from a row as: `order` (u32, competition rank: 1,2,2,4, ties share), `pid` (s32), `score` (s32), `region` (u32), `lastupdate`
(u32, Unix seconds), `size` (u32, always `size` bytes of `base64decode(data)`), `userdata` = those bytes untouched (the game requires `size == 0x80`, then checks the HMAC itself).
The original wire's big-endian layout is not used: only the C structure the game reads matters.
* The caller with no current row gets a synthesized row (`score` 0, 128 zero bytes, `order` >= 1) in NEAR and FRIENDS, so the game's `metadata[-1]` crash cannot happen. The game treats the zero blob as "no metadata" (invalid digest) and shows its own name there.
* A NEAR answer has `min(limit, total or total+1)` rows; a lone player gets exactly their own row.
* Row order is best first (ties: older update first, then lower pid).

### `GET /v1/public/leaderboard` (open, read-only, for the web page)

`GET /v1/public/leaderboard?board=season|daily|alltime|archive&limit=50[&season=<start ms>]` needs **no token and no `X-Msc-App`** (a browser sends neither); `board` defaults to `season`
(category 0, this weekly season), `daily` is category 1 (this UTC day); `limit` is 1..100 (default 50); anything else is `400 bad_param`. It reads the same rows,
window and order as `rank/get` (best first; ties: older update first, then lower pid) and ranks them the same way (competition ranks 1,2,2,4).
```json
{"board":"season","season":{"index":43,"startMs":1793232000000,"endMs":1793836800000},"updatedMs":1793361000000,
 "rows":[{"rank":1,"name":"Sam","wins":30,"losses":3,"points":980,"lastPlayedMs":1793358000000},{"rank":2,"name":"Alex","wins":29,"losses":8,"points":940,"lastPlayedMs":null}]}
```
`board=daily` also carries `"day":{"startMs":..,"endMs":..}` (the current UTC day); `season` is always present (the page counts down to `endMs`).
`name` is the registered display name (letters, digits, space, dot, dash, underscore); `wins` / `losses` are read from the stored 0x80 submission (either byte
order) and are 0 when it holds no readable record; `points` is the stored score; `lastPlayedMs` (1.3) is the time of the player's last **verified ranked**
match, rounded down to the hour (`null` = none; it does not move with any other request). **Nothing else is exposed**: no profile id, player code, token,
region or address. Banned accounts and accounts without a profile are left out (a ban or an account deletion also deletes the score rows).
* **`board=alltime`** (1.3): every verified ranked match's points as the game counts them (10 for a win or 1, plus goals up to 10), the rated record
  (`wins` / `losses`: verified and forfeit results) and `lastPlayedMs`, from the profile; never reset. Read through an index (`profiles_points`).
* **`board=archive`** (1.3): without `season`, `"seasons":[{"startMs","endMs","index","players"}]` newest first (up to 104) and empty `rows`; with
  `season=<startMs of a past season>` (anything else is `400 bad_param`), `"archived":{"startMs","endMs","index"}` and that season's final standings
  (category 0, best `ARCHIVE_TOP` rows, default 100), same row format. The final standing of a season is copied when the season changes (alarm) and by
  a put that overwrites an old-season row before that; deleting the account removes its archived rows too.
* **What a season change does.** The season board (categories 0, 2, 3) only counts rows updated in the current weekly season (UTC); at the change every
  player starts again from 0 in the game and on the service, the old row stays in storage until the player's next put overwrites it, and its final standing
  is kept in the archive. Daily boards (1, 4) reset at 00:00 UTC and are not archived. The all-time board and the profile record never reset.
  The game's own Online Central has no all-time tab and its row format is fixed (0x80 bytes per row), so all-time is on the web page only.
* **Cost control.** The object keeps the answer for 60 s per (board, limit, season) and a ban or deletion clears that memo; the Worker keeps a copy in the isolate's memory for 60 s
  per (board, limit), so a busy page costs the object about one request a minute (errors are never kept). The reply says `Cache-Control: public, max-age=60`.
  `updatedMs` is the server time of the snapshot, which may therefore be up to 2 minutes old.
* **Limits and switches.** 30 per minute per address (`429 rate_limited`, `Retry-After`; hits served by the Worker's copy are not counted). The `ranking` and `all` kill
  switches answer `503 service_disabled` (with the maintenance message), after the Worker's 60 s copy has expired.

### `GET /leaderboard` (the web page)

One self-contained HTML page (inline CSS and script, no external file of any kind) that shows the two tabs of the game's ranking screen, SEASON LEADERS and
DAILY LEADERS, plus ALL-TIME LEADERS and PAST SEASONS (a season picker) (red arrow buttons, the dots or the left / right keys; `#daily`, `#alltime`, `#archive`
open those tabs). Each row shows rank, name, record, last played (a date in the reader's own locale and time zone, `toLocaleDateString`) and points, the season time remaining and an empty state. It is answered by the Worker itself
(the object is never woken for it), is the same for everybody and carries `Content-Security-Policy: default-src 'none'; script-src 'sha256-..'; style-src 'sha256-..'; connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`
(the two inline blocks by hash), `Cache-Control: public, max-age=300`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`.
Names are only ever written with `textContent`. Not affected by kill switches (the page explains when the data is off).

## 9. Admin (secret bearer token, disabled when `ADMIN_TOKEN` is not set)

`Authorization: Bearer <ADMIN_TOKEN>`; 30 per minute per address, 5 wrong tokens per 10 minutes per address.
`GET /v1/admin/stats`; `GET /v1/admin/export` (table list + schema) and `?table=profiles&after=<cursor>&limit=500` (pages; `next` is the next cursor or `null`);
`POST /v1/admin/import {"table","rows":[...]}` (replace by key, <= 2000 rows); `POST /v1/admin/switches {"kill":["ranked"],"message":"..."}`
(names: `all register rooms ranked ranking turn`; takes effect at once, survives restarts; `all` leaves `/info`, `/time` and admin working);
`POST /v1/admin/ban {"pid":100123,"banned":true,"reason":"..."}`;
`GET /v1/admin/flags[?kind=single_opponent|dodger]` lists flagged accounts (name, code, rating, W/L, abandons, hit count, evidence; flags are never turned into bans automatically; the table is also in the export); `POST /v1/admin/flags/clear {"pid":100123,"kind":"dodger"}` (kind optional) clears them after review. `scripts/backup.mjs` / `restore.mjs` wrap export/import.

## 10. Rate limits (per player unless noted; 429 + `Retry-After`)

register burst 10 then 3/h per address (IPv6 per /64; +500/day total; 30 bad requests/h) | rooms create 10/10 min | join 20/10 min | room poll 150/min | endpoint, started, leave 20/min |
queue join 10/10 min, poll 120/min | result 20/10 min, match poll 60/min | turn 10/10 min | rank put 12/min, get 40/min | public leaderboard 30/min per address (page 60/min) | wrong tokens 20/10 min per address.
Per account per UTC day (stored, so they survive restarts; `429 rate_limited` with `"reason":"daily_cap"`, `"scope":"rooms"|"matches"|"host_matches"|"ranked"|"turn_reports"` and `retryAfterSec` = seconds to 00:00 UTC; **do not retry before that**, show "daily limit reached"):
60 rooms created, 60 match records (every room join and every ranked pairing counts for both players), 30 rated ranked matches (4 per pair), 12 TURN usage reports.
Also: punch report 20/min, queue resume 20/10 min, player lookup 30/min, `/servers` and `/info` 60-120/min per address.
A client polling at the recommended 1.5-2 s intervals stays far below them.

## 11. Client recipes

**Every request** carries `X-Msc-App` (+ `X-Msc-Build`). On `426 update_required` stop and show "different version, update to `latestVersion`".
**First run.** `GET /v1/info` (fail closed on TLS errors, show `message` if `kill` is set; remember `servers` for fallback) -> name prompt (Vita keyboard, user name pre-filled
as a suggestion, same rules as section 3) -> `POST /v1/register` -> store `pid` (save offset 0x1C, > 0) and `token`. Later runs skip registration
and verify the token with `GET /v1/me` (`401` = account gone: register again).

**Invite (unranked).** Host `POST /v1/rooms`, show the code. Guest types it and calls `/rooms/join`. Both: STUN from the game's own UDP socket
(port 41000) (two STUN servers: classify the NAT) -> `POST .../endpoint` (with `nat`, `candidates`) -> poll the room until `peer.endpoint` is present (obey `steer` and `stale`) -> wait until `punchAtMs` (server time) -> punch
(`intervalMs` x `tries`, packets carry cookie + own nonce, expect the partner's nonce; switch to the source address of the first inbound ping,
confirm both ways, linger `lingerMs`) -> `POST .../punch` with the outcome -> `POST .../started` -> play on the VLink Wi-Fi/UDP backend with the room's vips, encrypted with `session.key`.
If punching fails: `POST /v1/turn`, relay through TURN, report usage afterwards.

**Ranked.** Section 5 to get a room, then as above. During the match nothing is sent to the service. At the end both submit the result,
poll `GET /v1/matches/{id}` until `final`, then `rank/put` the new totals for categories 0 and 1 (and 2 on EU builds), then `rank/get` for the tables.
If the connection drops: submit `result:4` and wait for the window; the other side is judged by the rules in section 6.

**Failure policy.** Never disable certificate verification. If a call fails after retries, show the error and stay in the menu; the local LAN multiplayer path is unaffected.
