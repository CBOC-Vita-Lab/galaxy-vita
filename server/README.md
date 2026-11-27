# Strikers Charged Vita: online service

One Cloudflare Worker plus one Durable Object (SQLite). Sign-in, invite rooms / signaling, ranked queue, match records and
result checks, DWC-style leaderboard, TURN credential minting, admin backup. The wire format is in `PROTOCOL.md`.
No account ids, no secrets and nothing personal are in this folder; `wrangler.toml` carries a placeholder name.

```
src/worker.js        edge entry: answers unknown paths, wrong methods, oversized bodies and malformed Authorization / version headers itself (core/edge.js), forwards the rest to the single object
src/do.js            Durable Object (SQLite, alarm housekeeping)
src/core/publicboard.js, leaderboard_page.js   the public web leaderboard: `GET /v1/public/leaderboard` (JSON, no private fields, 60 s memo) and `GET /leaderboard` (one self-contained page)
src/core/*.js        the whole service as Request -> Response over an abstract SQL handle (pure JS, no platform imports)
src/sqlite-node.js   node:sqlite adapter used by tests and the fake
fake/server.mjs      local fake: same code, HTTP + HTTPS with a throw-away root certificate, clock / fault controls
scripts/             extract-hmac-key.mjs, backup.mjs, restore.mjs, estimate-writes.mjs (free-tier budget per match)
test/core            unit and integration tests on node:sqlite with an injected clock
test/worker          the same service inside workerd (miniflare) through the real Worker and Durable Object
```

## Develop and test (no account, no network service)

```sh
# Node >= 22.5 is required (node:sqlite for the tests and the fake)
cd server && npm install
npm test                              # both suites; `npm run test:core` is the fast one (about 5 s)
npm run fake                          # http://127.0.0.1:8787 and https://localhost:8788; the web leaderboard is http://127.0.0.1:8787/leaderboard
```
The fake writes its root to `fake/certs/root.pem` / `root.der` on first start (needs `openssl`); trust only that root in the client's host test.
`cp .dev.vars.example .dev.vars` and `npm run dev` runs the real Worker under `wrangler dev` (workerd) if you want the production runtime locally.

## Configuration

Plain variables (`[vars]` in `wrangler.toml`, or the environment of the fake). All optional.

| name | default | meaning |
|---|---|---|
| `KILL_SWITCH` | empty | comma list of `all register rooms ranked ranking turn`; the admin API can also change it at runtime. `ranking` and `all` also switch off the public leaderboard data (the page stays up and says so) |
| `MAINTENANCE_MESSAGE` | empty | shown to players while something is switched off |
| `MIN_CLIENT_PROTO` | 1 | older wire protocol gets `426 upgrade_required` |
| `MIN_APP_VERSION` `LATEST_APP_VERSION` `REQUIRE_APP_VERSION` | empty / empty / off in code, **`1` in `wrangler.toml`** | game versions: below the minimum gets `426 update_required`; the latest is echoed in every error; `1` makes `X-Msc-App` mandatory (production sets it; the tests set `0` explicitly). A malformed `X-Msc-Proto` is always `400` |
| `SERVER_LIST_JSON` | `[]` | `[{"url":"https://...","priority":0}]` served by `/v1/servers` (https only) |
| `JOIN_ACK_SEC` `LINK_PING_SEC` `LINK_LOST_SEC` `NAT_LIFE_SEC` | 5 / 5 / 15 / 30 (Galaxy: ping 12, lost 20) | session liveness timings (also published in `/v1/info`). Galaxy's started rooms poll every 8 s, so its defaults are slower |
| `PAIR_FAIL_LIMIT` `PAIR_FAIL_KEEP_DAYS` | 2 / 7 | failed punches per pair before steering to the relay |
| `RANK_PUT_POLICY` | `credit` | `credit` = a score only rises by what verified matches earned; `delta` = at most +`RANK_MAX_DELTA` (25) per put; `open` = anything in range |
| `RANK_REGION_MODE` | `pool` | `pool` = one board for all regions; `mask` = honour the client's region mask |
| `RANK_REQUIRE_HMAC` / `RANK_CHECK_NAME` | on | set `0` to relax the digest / blob-name checks (debugging only) |
| `ROOM_OPEN_TTL_SEC` `ROOM_JOINED_TTL_SEC` `ROOM_STARTED_TTL_SEC` | 600 / 1200 / 10800 | room lifetimes |
| `PUNCH_INTERVAL_MS` `PUNCH_TRIES` `PUNCH_LINGER_MS` `PUNCH_DELAY_MS` | 700 / 7 / 5000 / 1000 | hints sent to clients (GameSpy natneg values) |
| `QUEUE_BASE_WINDOW` `QUEUE_WINDOW_STEP` `QUEUE_WINDOW_STEP_SEC` `QUEUE_MAX_WINDOW` `QUEUE_REGION_STRICT_SEC` | 100 / 50 / 5 / 1000 / 20 | skill window |
| `RESULT_WINDOW_SEC` | 120 | how long the second player has to report |
| `FORFEIT_POINTS` `RESTORE_CREDIT` `ELO_K` `ABANDON_COOLDOWN_SEC` `ABANDON_COOLDOWN_MAX_SEC` | 10 / 5 / 24 / 60 / 900 | disconnect penalty rule |
| `MIN_MATCH_SEC` | 60 | a normal finish (result 0) sooner than this after both sides started is refused (429 `too_early`). The game's shortest timed match is 120 s (`sGameTimeOptions` 120/180/240/300, default 180); only a 3-goal limit can end sooner, so the default is half the shortest timed match. 0 = off |
| `RATED_PER_PAIR_DAY` `RATED_PER_ACCOUNT_DAY` | 4 / 30 | rated (verified or forfeit-win) ranked matches per UTC day; 0 = no cap |
| `ONE_OPP_MIN_WINS` `ONE_OPP_SHARE_PCT` `ONE_OPP_DAYS` | 8 / 70 / 30 | win-trading flag: that many rated wins with that share against one opponent in that many days (flag only, no ban) |
| `DODGE_WINDOW` `DODGE_MIN_SAMPLE` `DODGE_RATE_PCT` `DODGE_PENALTY_PCT` | 20 / 6 / 35 / 75 | dodge rule (PROTOCOL.md section 6): blamed disputes / no-contests over the window, and the share of an abandon's rating loss charged |
| `TURN_PROVIDER` | `cloudflare` | `cloudflare`, `stub` (fake credentials) or empty (off) |
| `TURN_TTL_SEC` `TURN_MAX_CREDS_PER_DAY` `TURN_MAX_CREDS_PER_PLAYER_DAY` `TURN_MONTHLY_BUDGET_MB` | 3600 / 300 / 6 (Galaxy: 12) / 100000 | TURN budget and breaker |
| `TURN_NEW_ACCOUNT_HOURS` `TURN_NEW_ACCOUNT_DAILY` | 24 / 60 (Galaxy: 200 in launch week, in wrangler.smg.toml: every account is new; lower it to 60 after the first week) | accounts younger than this share at most that many of the daily credentials |
| `TURN_MAX_KBPS` `TURN_REPORT_GRACE_SEC` `TURN_MAX_REPORTS_PER_PLAYER_DAY` `TURN_MAX_REPORT_MB` | 128 / 300 / 12 / 200 | usage reports: clamp per credential (TTL x kbit/s), grace after expiry, reports per player per day, hard per-report ceiling |
| `MAX_PLAYERS` `RENAME_COOLDOWN_HOURS` `NAME_BLOCKLIST_EXTRA` `STUN_SERVERS` | 200000 (Galaxy: 50000) / 24 / empty / stun.cloudflare.com:3478 | `MAX_PLAYERS`: see "Free-tier capacity" below |
| `PRUNE_IDLE_DAYS` | 180 | the alarm removes accounts that **never played**: no match ever started (unranked included), no win, loss, draw, abandon, verified point, score row or archived season, no ban, and not seen for this many days (at most 200 a sweep); 0 = never. Anyone who played is never removed, however long they are away |
| `ARCHIVE_TOP` | 100 | best rows kept per past season in the archive (`board=archive`) |
| `REGISTER_IP_BURST` `REGISTER_IP_PER_HOUR` `REGISTER_MAX_PER_DAY` `REGISTER_BAD_PER_HOUR` | 10 / 3 / 1000 (Galaxy: 2000, wrangler.smg.toml: 10000 for launch week) / 30 | registration limits: per address (IPv6 per /64) a burst, then a slow rate; the whole service per day; bad requests per address per hour (they never touch the other two) |
| `REGISTER_POW_BITS` | 0 (off; Galaxy: 15) | proof of work on register: leading zero bits of SHA-256 (PROTOCOL.md section 3); 16 is about 65k hashes. Turn on only after the client can solve it |
| `DAILY_REQUEST_BUDGET` `LIVE_RESERVE_PCT` `POLL_PACE_MAX_PCT` | 0 (Galaxy: 45000) / 20 / 140 | svc3: requests admitted per UTC day; the share kept for members of a live room; the most the clients are asked to stretch their polls (percent). See "Quota defence" |
| `ADDR_DAILY_OPEN` `ADDR_DAILY_PLAYER` `ADDR_DAILY_LIVE` `PID_DAILY_MAX` | 0 (Galaxy: 1000 / 3000 / 10000 / 4000) | svc3: requests per address (by class) and per account per UTC day; 0 = off; scaled by `RATE_LIMIT_SCALE` |
| `ADDR_BAD_DAY` | 0 (Galaxy: 2000) | requests per address and UTC day that fail authentication (wrong, missing or unknown token); over it: 429 `daily_cap` with `X-Msc-Block: bad:<seconds>`, and the Worker then refuses that address's requests unless their token was seen to succeed on a route that checks it, or is an unknown token (one per 20 s per address, burst 3, reach the object); 0 = off; see "Failed authentication and shared addresses"; scaled by `RATE_LIMIT_SCALE` |
| `EDGE_IP_BURST` `EDGE_IP_PER_SEC` `EDGE_IP_AGG_BURST` `EDGE_IP_AGG_PER_SEC` | 120 / 1, 300 / 5 | the Worker's best-effort buckets: a request with a player token has its own address+token bucket (120 / 1) and the address's aggregate (300 / 5), so consoles behind one NAT are not throttled together; no token: the per-address bucket (120 / 1); 0 burst = off |
| `REGISTER_EVICT_UNUSED_HOURS` `PRUNE_UNUSED_DAYS` | 0 (Galaxy: 24 / 3) | svc3: silent accounts (never used) are removed first when the service is full, and by the sweep after this many days; 0 = off |
| `REGISTER_EVICT_SILENT_MIN_MINUTES` `REGISTER_EVICT_FRIENDLESS_DAYS` | 0 (Galaxy: 30 / 14) | svcfix3 (review S4), when the table is full and no silent account is a day old: the oldest silent account at least this many minutes old goes; then an account with no friend and no star time unseen this many days; the 60-day rule (`REGISTER_EVICT_IDLE_DAYS`) is last. An account with a friend, a star time or a call after its first 5 minutes is never taken by the first two steps; 0 = off |
| `TURN_MAX_CREDS_PER_ROOM` `TURN_MAX_CREDS_PER_ADDR_DAY` `TURN_UNNEEDED_SHARE_PCT` | 0 / 0 / 100 (Galaxy: 8 / 24 / 50) | svcfix3 (review S5), Galaxy rooms only: relay credentials per room per clock HOUR (a credential lasts an hour, so a long relayed room renews inside its own hour's count); per source address per UTC day (the address counters are stored and swept after 2 days); and the share of the day's `TURN_MAX_CREDS_PER_DAY` that credentials WITHOUT a visible reason may use (a visible reason: the member's pair is steered to the relay, for a relay-only account, an unverified endpoint, a symmetric NAT or repeated failures, or has a recorded failed punch). A pool of aged accounts therefore cannot spend the whole day: answers `429 turn_room_limit`, `429 turn_player_limit` (per address) or `503 turn_budget` |
| `ROOMS_PER_ACCOUNT_DAY` `MATCHES_PER_ACCOUNT_DAY` | 60 / 60 | rooms created and match records per account per UTC day (stored); 0 = no cap |

Secrets (never in files; `wrangler secret put`): `RANK_HMAC_KEY_HEX` (the ranking digest key, 64 hex), `ADMIN_TOKEN`, `TURN_KEY_ID`, `TURN_KEY_API_TOKEN`.
The ranking key is the game's `sRankingHmacKey`; `node scripts/extract-hmac-key.mjs` prints it from the decomp source. It is not logged anywhere by the service
and never appears in a response. (It also ships inside every copy of the game, so the 0x70 HMAC is an integrity check and **not authentication**: it only keeps honest clients honest and anybody can forge a valid digest. The real protections are the
credit rule, both-sides result agreement, the minimum match length, daily caps, flags and rate limits.)

## Free-tier capacity (1.3; the arithmetic behind `MAX_PLAYERS` and `REGISTER_MAX_PER_DAY`)

Cloudflare's documented Workers Free limits for Durable Objects (developers.cloudflare.com/durable-objects/platform/pricing and .../limits, read
6 Nov 2026): 100,000 requests a day, 100,000 SQLite rows written a day, 5,000,000 rows read a day, 5 GB stored per account, and 1 GB per object on
the Free plan (one place on the limits page says 10 GB; take the smaller). Every index entry a write touches counts as one more row written.
* **Storage per player** (measured with this schema in node:sqlite, 20,000 synthetic players): a profile with its unique indexes about 265 bytes;
  an active player's three score rows and five credit rows about 885 bytes more: **about 1.15 KB per active player**. 200,000 players is about
  230 MB; with 90 days of match records at the write-bound rate below (about 1,000 ranked matches a day, a few hundred bytes each) and the season
  archive (100 rows a week) it stays under 1 GB with more than 3x headroom. So **`MAX_PLAYERS` = 200,000** (was 20,000).
* **Writes.** `node scripts/estimate-writes.mjs`: a ranked match costs about 40 requests and 98 rows written (1.3 adds about 5: the played counters,
  points / last played on a verified match and its index row), an invite match 15 / 33 (+2), a registration 4 rows (+1 for the all-time index).
  **`REGISTER_MAX_PER_DAY` = 1,000** costs at most 5,000 rows a day, 5 % of the write budget, which leaves room for about 950 ranked matches a day;
  at that rate the never-played pruning (180 days) keeps at most about 180,000 dormant sign-ups, under `MAX_PLAYERS`.
* **Reads are the real ceiling, not players.** A NEAR / ORDER ranking request counts the rows above the player on the current board (`rankOf`),
  so it reads about as many rows as there are players on that board; at 5,000,000 rows read a day, a board of N active players allows roughly
  5,000,000 / (3 N) such requests a day (N = 2,000: about 800). Registered players who do not play cost no reads. Watch "rows read" in the
  dashboard first; a paid plan or a cached rank is the fix if a weekly board ever holds thousands of players.

## Deploying (what the owner has to do; nothing here has been run)

1. **Cloudflare account.** Free plan is enough (Durable Objects with SQLite and Workers are on the free plan). Use a neutral account name.
   In the dashboard open *Workers & Pages* once so the account gets its `workers.dev` subdomain (choose a neutral one).
2. **Tooling.** `cd server && npm ci` (Node >= 22.5).
3. **Log in.** `npx wrangler login` (browser) or, headless, create an API token with the *Edit Cloudflare Workers* template and
   `export CLOUDFLARE_API_TOKEN=... CLOUDFLARE_ACCOUNT_ID=...`. Do not commit the ids.
4. **Name.** Edit `name = "msc-online-example"` in `wrangler.toml` to the final neutral worker name. The URL will be `https://<name>.<subdomain>.workers.dev`.
5. **TURN key.** Dashboard -> *Realtime* -> *TURN* -> create a key. Keep the *Key ID* and *API token*. (Skip to run without relay: set `TURN_PROVIDER = ""`.)
6. **First deploy.** `npx wrangler deploy`. This creates the Worker and applies the Durable Object migration `v1` (`new_sqlite_classes`).
7. **Secrets.**
   ```sh
   node scripts/extract-hmac-key.mjs | npx wrangler secret put RANK_HMAC_KEY_HEX
   openssl rand -base64 32 | tr '+/' '-_' | tr -d '=\n' | tee /dev/stderr | npx wrangler secret put ADMIN_TOKEN   # note the printed value in a password manager
   npx wrangler secret put TURN_KEY_ID
   npx wrangler secret put TURN_KEY_API_TOKEN
   ```
   Secrets are live immediately (a secret change makes a new deployment of the same code).
8. **Smoke test.**
   ```sh
   curl -s https://<url>/v1/info
   curl -s https://<url>/v1/time
   ADMIN_TOKEN=... curl -s -H "Authorization: Bearer $ADMIN_TOKEN" https://<url>/v1/admin/stats
   ```
9. **TLS roots for the Vita.** `openssl s_client -connect <name>.<subdomain>.workers.dev:443 -showcerts </dev/null` and note which root signs the chain (Cloudflare's
   `workers.dev` certificates are issued by Google Trust Services or Let's Encrypt and the issuer can change over time). Bundle ISRG Root X1/X2 and GTS Root R1-R4 (plus the
   cross-signing GlobalSign root if the chain shows it) in the client's own root store, and check the tool's TLS test against the live host before shipping.
10. **Give the client worker the URL** (build-time constant, e.g. `MSC_ONLINE_HOST`).
11. **Backups.** `ADMIN_TOKEN=... node scripts/backup.mjs https://<url> > backup-$(date +%F).json` (weekly is plenty). **A backup is sensitive**: it holds every account's token hash, names and friend codes, friend lists, star times and the room rows (the Galaxy export leaves out `room_members`, the live IP:ports, but the rest is private data). Keep it off shared drives, encrypt it at rest, delete old copies.
    Restore: `ADMIN_TOKEN=... node scripts/restore.mjs https://<url> < backup.json`.
11b. **Galaxy 3.0 beta diagnostics** (smg only). `POST /v1/report` (a consented problem report: masked, 3 a player and 30 an address a day, 48,000-byte log, 294,912-byte body: the worst case of JSON-escaping the log), a `summary` object on the leave / close request (ring of 500 plus per-UTC-day aggregates in the admin stats), `POST /v1/turn/check` (a 300 s relay self-test credential: 2 a player, 6 an address, 60 a day inside `TURN_MAX_CREDS_PER_DAY`, and the same new-account gate as a normal pass; `TURN_CHECK_PER_PLAYER_DAY` / `TURN_CHECK_PER_ADDR_DAY` / `TURN_CHECK_MAX_PER_DAY`). Reports and summaries live in D1 (binding `REPORTS_DB`, database `galaxy-beta-reports`, tables created on first use; 30 days, `REPORT_MAX_ROWS` 2000, `REPORT_MAX_MB` 64; limits `REPORT_MAX_PER_PLAYER_DAY` `REPORT_MAX_PER_ADDR_DAY` `REPORT_SHARE_PCT` `SUMMARY_RING`, `SUMMARY_MAX_PER_DAY` 3000, `SUMMARY_MAX_PER_PLAYER_DAY` 20; a summary is recorded only when the leave really removed the member, once per player and room, and the leave never waits on D1 more than `SUMMARY_TIMEOUT_MS` 2500). Admin: `GET /v1/admin/reports[?since=]`, `GET /v1/admin/reports/<id>`, `GET /v1/admin/summaries`. Kill switch `report`. Account delete purges the player's reports.
12. **Emergency stop** without a redeploy: `curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" -d '{"kill":["ranked","turn"],"message":"Back soon"}' https://<url>/v1/admin/switches`
    (`["all"]` stops everything but `/info` and `/time`; `[]` clears). Roll back code with `npx wrangler rollback`. Never edit a shipped migration in `src/core/schema.js`; append a new one.
13. **Cost guards.** Workers Logs are on for Galaxy only (`[observability] enabled = true`, `head_sampling_rate = 1`, `[observability.logs] invocation_logs = false`, `[observability.traces] enabled = false`: no per-request invocation logs and no traces; the lines the code writes are the beta's `report`, `report_log` and `summary` lines, and the code logs no URL, header or address itself; check in the dashboard after the first deploy what metadata the platform adds to each line). Strikers' `wrangler.toml` keeps platform logging off. The free-plan quotas and what a match costs are in "Free-tier budget" below. The Worker answers junk itself (unknown paths,
    wrong methods, oversized bodies, malformed `Authorization` / version headers) so it never spends a Durable Object request; per-account daily caps bound rooms and match records.
    TURN is guarded by `TURN_MAX_CREDS_*` and `TURN_MONTHLY_BUDGET_MB`; also set a billing alert / usage notification in the dashboard.

**TURN exhaustion, worked through.** Three things can be abused: the daily credential count, the monthly byte counter and the player's patience.
*Credential count:* the old 10 per player and 300 per day meant 30 accounts (about 10 hours of registrations from one address at 3/h, minutes for a few addresses) used up the whole day and the relay was
gone for everyone. Now a player can mint 6 a day (a credential lasts an hour and one is enough for a room), fresh accounts (< 24 h) share a sub-budget of 60, so 10 fresh accounts can
at most use their own slice and established players keep the other 240; exhausting those needs 40 accounts that each waited a day, and the admin sees them. The kill switch (`turn`) and a ban
remain the emergency tools. *Byte counter:* usage reports used to be accepted from anybody, up to 200 MB per report, 30 reports per 10 minutes, so one account could trip the 100 GB monthly breaker in under
three hours. Now a report needs a live credential of the reporter, is clamped to TTL x 128 kbit/s (57.6 MB), a credential carries that total only once, and 12 reports a day: the worst case per account and day is
6 x 57.6 MB, i.e. 100 accounts could add 35 GB a day at most, and the number can never exceed what the credentials the server minted could physically carry. The real relay usage is in the Cloudflare dashboard; set a usage alert there.

## Public web leaderboard

`https://<url>/leaderboard` shows the game's SEASON LEADERS / DAILY LEADERS tables, ALL-TIME LEADERS and PAST SEASONS to anybody with a browser; it reads `GET /v1/public/leaderboard?board=season|daily|alltime|archive&limit=50`
(PROTOCOL.md section 8). Display name, win / loss record and points only: no player code, id, token, region or address. Banned accounts are left out. No new variable,
secret or migration: it reads the existing `scores` and `profiles` tables. `GET /leaderboard` is answered by the Worker without waking the object; the data is memoised for 60 s in the
object and in the Worker, so a page left open in many browsers costs about one object request a minute per isolate (each open page refreshes once a minute while visible), and a
cache miss is one indexed query (`LIMIT` 50) plus a join of those rows to `profiles`. Rate limit: 30 data requests a minute per address. After a ban or an account deletion the board
changes within a minute (the object's memo is cleared at once; the Worker's copy expires). Fan-project footer only; nothing identifies the owner.

## Free-tier budget

Cloudflare's numbers as read on 30 Oct 2026 (they change: re-check before relying on them):

| Free plan | Limit | Source |
|---|---|---|
| Workers requests | 100,000 per day, resets 00:00 UTC (over it: error 1027) | <https://developers.cloudflare.com/workers/platform/limits/> |
| Workers CPU time | 10 ms per HTTP request; 50 subrequests per request | same page |
| Durable Objects (SQLite) requests | 100,000 per day; HTTP requests, RPC sessions, alarm invocations and WebSocket messages (20:1) all count | <https://developers.cloudflare.com/durable-objects/platform/pricing/> |
| Durable Objects duration | 13,000 GB-s per day (one object active all day at 128 MB is about 11,000) | same page |
| SQLite rows read | 5 million per day | same page |
| SQLite rows written | **100,000 per day**; deletes count, `setAlarm()` is one row | same page |
| SQLite storage | 5 GB per account | same page; per-object limits and the 1,000 requests/second soft limit: <https://developers.cloudflare.com/durable-objects/platform/limits/> |
| How rows are counted | an INSERT / UPDATE / DELETE counts the row, plus one more row for each index the write touches; failed statements are not documented | <https://developers.cloudflare.com/d1/platform/pricing/> (the Durable Objects page says it matches D1) |

Every client request that reaches the service uses one Worker request **and** one Durable Object request, so the two daily quotas of 100,000 are spent together; a request the Worker rejects
at the edge uses only the Worker's. When a quota is used up the platform fails further operations of that kind until 00:00 UTC.

**Write budget per match** (`node scripts/estimate-writes.mjs` replays a typical match through the real code and applies the row rules above; an upper bound, because upserts are counted as inserts):

| | requests | rows written |
|---|---|---|
| ranked match (queue 10 s, 3 min play, polls at the documented rates, results, 2 x put, 2 x get per player) | ~34 + ~6 alarm | ~92 + ~6 `setAlarm` = **~98** |
| invite match (create, join, endpoints, punch, started, leave) | ~15 | ~33 |
| registration | 1 | 4 |

So the binding limit is **rows written: about 1,000 ranked matches a day** (about 2,500 by requests), or roughly 3,000 invite matches. The caps keep one account from using a
disproportionate share: 60 rooms and 60 match records a day, 30 rated matches, 12 TURN reports. Rows read (5 million) is not a constraint at this scale; the one query that grows with the crowd is
the matchmaker's scan of up to 200 queue rows per poll, so a very long queue is the first thing to watch in the dashboard.

Maintenance notes: the object runs a 60 s housekeeping alarm only while rooms, queue entries or unsettled matches exist (Galaxy: only while a room is live; the idle-account scan inside the sweep runs at most hourly, on an index of `profiles.last_seen_ms`). Account deletion (`DELETE /v1/me`) and bans are in the API.
Match records are kept 90 days. Peer addresses live only in `rooms` rows and disappear a few minutes after a room ends.

## Galaxy service (smg-online): request budget, hardening and operations

Everything above is the Strikers-flavoured text; this section is what changed for the Galaxy deployment after the security review (Nov 2026).
Numbers are for the free plan (100,000 Worker requests and 100,000 Durable Object requests a day, both spent together by every request that reaches the object).

**Request budget of a room (svcfix2: the service dictates the rhythm).** The service answers every room poll (`GET /v1/rooms/<code>?v=<version>&wait=<s>&p=1[&l=<mask>]`) with a plan, `poll: {holdSec, nextSec}`: how long the next poll may be held and how long to wait after an answer. The client obeys within bounds (hold at most 60 s, wait 0.3 s to 10 min) and keeps its old rhythm when the field is absent (an older service). `l` is the console's report of the slots it exchanges direct heartbeats with (`SetDirectLinks`, from the peer links of `np_online.cpp`); a report is evidence that the member it names is alive (`rooms.seenAt`, valid 150 s), so a started room whose peers all heartbeat each other needs no polling to stay alive.
* **forming** (open, joined, ready): held polls of `POLL_FORMING_HOLD_SEC` (25 s; was 8), the next one 0.3 s after the answer: about 144 an hour while people gather (a lobby lasts minutes).
* **started, every peer linked directly and nobody quiet**: the HOST keeps one held poll of `POLL_SETTLED_HOST_HOLD_SEC` (55 s) in flight (about 65 an hour: the keep-alive that proves the host alive, stamped every 4 s while held); a GUEST does not hold and asks again after `POLL_SETTLED_GUEST_NEXT_SEC` (300 s, 12 an hour).
* **started, anything else** (a link down, a member quiet, a member not linked yet, no report): held polls of `POLL_BUSY_HOLD_SEC` (8 s) and an immediate re-ask: the old rhythm, about 430 an hour.
* **a loss is reported at once**: when a console's link to a peer goes (or a new one comes) it drops its held poll and asks with the new `l` within a second (at most once a second). Guests that lose the host's heartbeat withdraw their evidence, and once the host has also not called for `LINK_LOST_SEC` (20 s) the lowest guest becomes host: measured 20 to 25 s after the host stopped, as before.
`GET /v1/info` is asked once per session (the answer is believed for 2 hours on the client; a switched-off service is refused by the create / join itself).

Measured by `test/core/svcfix2-cadence.test.js` (virtual time, one hour of a started room, the real application):

| | requests per player-hour |
|---|---|
| before: every console held polls of 8 s + a gap | about **435** (4-player room: 1,740 an hour) |
| after, 4-player room, all linked: host 65, each guest 13 | **26** (the room: 104 an hour) |
| after, 2 players | **39** (host 65, guest 13) |
| forming | 144 per member; a lobby of 3 minutes costs each member about 7 |
| a whole 1-hour session: forming + sign-in / create / join / endpoint / started / relay credentials (about 12) + the started hour | **about 45 (4 players) to 60 (a pair)**; 60 is the figure the budget uses (was 354) |

So 45,000 requests a day (`DAILY_REQUEST_BUDGET`) are about **750 player-hours** a day (was 169), and 600 of them can START a session before the 80 % line (36,000 requests; was 135). The 100,000 of the whole account would be about 1,670. `LINK_LOST_SEC` stays **20** and `LINK_PING_SEC` 12: they are measured on the member's calls OR on the direct-link evidence above, so the longer gaps of a settled room do not make anyone look quiet. `GUEST_GONE_MS` (60 s) is unchanged.

**Launch-day table** (`node scripts/launch-day.mjs 1500`): an ASSUMED demand curve (share of the day's player-hours begun in each UTC hour; the Americas' evening is 23:00 to 04:00 UTC) with 1,500 player-hours of demand on the day, served at 354 (before) or 60 (after) requests per player-hour; new sessions stop at 48,000:

| start hour UTC | wanted (player-hours) | before: served | before: requests used | after: served | after: requests used |
|---|---|---|---|---|---|
| 00:00 | 103 | 103 | 36,621 | 103 | 6,207 |
| 01:00 | 103 | 32, then refused | 48,000 | 103 | 12,414 |
| 02:00 | 91 | 0 | 48,000 | 91 | 17,845 |
| 04:00 | 52 | 0 | 48,000 | 52 | 25,603 |
| 08:00 | 13 | 0 | 48,000 | 13 | 31,034 |
| 12:00 | 26 | 0 | 48,000 | 26 | 35,690 |
| 16:00 | 65 | 0 | 48,000 | 65 | 47,328 |
| 17:00 | 78 | 0 | 48,000 | 11, then refused | 48,000 |
| 18:00 to 23:00 | 701 | 0 | 48,000 | 0 | 48,000 |

At this demand the old service served 136 player-hours and was closed to new players from 01:00 UTC (8 pm Eastern) until the reset; the new one serves 800 and closes at 17:00 UTC (noon Eastern), still before the evening. At 3,000 player-hours of demand it closes at 04:00 UTC. Two honest readings: the reserve (12,000 requests) still serves the games in progress when the line is crossed, and the real answer to a launch-day crowd is the paid plan (README "Quota defence"), not a smaller number of requests.

**The alarm.** The object's 60 s housekeeping alarm (each firing is one Durable Object request, and a pending alarm keeps the object awake and billing duration) re-arms itself only while a room is live
(`open`, `joined`, `ready`, `started`). When no room is live the alarm stops; the next request of any kind arms it again, and stale rows are cleaned by the first sweep after that.

**Failed authentication and shared addresses.** An address that sends more than `ADDR_BAD_DAY` (2,000) requests a UTC day that fail authentication gets 429 `daily_cap` with `X-Msc-Block: bad:<seconds>` for the rest of the day, and the Worker answers for the object. The Worker then lets through only (a) tokens it has seen succeed on a route that checks the token (player or live class; never an open route, which ignores Authorization, so an invented token sent to `/v1/info` proves nothing), and (b) **unknown tokens at a continuous rate of one per 20 seconds per address (burst 3)**, so a console that registered while its address is under attack can prove itself and is then remembered as good (any answer that shows the token authenticated counts: success, 404, 409, 410). A token the object refused (401) is remembered as bad for an hour and is refused at the Worker without using that allowance; invented tokens are therefore bounded to 3 object requests a minute per address (about 4,300 a day, none of them counted against the budget). What is guaranteed under an ACTIVE same-address attacker: the attacker cannot take more than that rate, and cannot hold every slot by timing (there is no minute boundary), so a console that retries every few seconds gets slots at a fair share, typically within a minute or two; an attacker who adapts to the refill instants and always arrives first can still delay one console, so this is mitigation, not a guarantee. Not solved: a household, a venue or a carrier-grade NAT (many consoles, one address) shares the address's count; an attacker on the same address can push it over the ceiling, after which consoles of that address that this isolate has not yet seen succeed get in at the allowance rate (and a console never seen by an isolate that restarted proves itself again the same way). The memory is per isolate and best effort like the edge bucket.

**Edge token bucket (best effort).** `src/worker.js` runs a limiter (a request with a player token: its own address+token bucket, **120 burst and 1 request a second**, plus the address's aggregate, **300 burst and 5 a second**, so a venue of consoles behind one NAT is not throttled together; a request with no token: the plain per-address bucket, 120 and 1; was one per-address bucket of 200 and 3; IPv6 by /64; `EDGE_IP_BURST` / `EDGE_IP_PER_SEC` / `EDGE_IP_AGG_BURST` / `EDGE_IP_AGG_PER_SEC`, `EDGE_IP_BURST=0` turns all of it off) before it wakes the
object. **It is best effort**: the table is in the isolate's memory, a Worker may run in many isolates and restarts often, so a flood spread over isolates gets through in part (the
review's reproducer, `bucket.mjs`, measured 254,118 requests a day passing from ONE address at the old numbers). It is the first, cheap layer; the hard limit is the object's daily ceiling per address (next section).
The Workers **Rate Limiting binding** (`[[ratelimits]]`, `env.X.limit({ key })`, per Cloudflare location, period 10 or 60 s) was considered and NOT added: a call to it happens inside the Worker, i.e. after the request has been counted, so it
protects the Durable Object but not the Worker quota, which is the scarce one, and its syntax and plan terms have changed between wrangler versions (check the current docs before adding it).

## Quota defence: what a Worker can and cannot do on the free plan (svc3, Nov 2026)

**The facts this rests on** (from Cloudflare's documentation as remembered, not re-read from the live site in this session; check `share/cloudflare-costs.md` and the pages it names before relying on a number):
* Workers Free: 100,000 requests a day per ACCOUNT (shared with the Strikers service), reset at 00:00 UTC; over it the platform answers Error 1027 itself. **A request that reaches the Worker has already been counted.**
  Whatever the code answers (a 429, a 503, a cached page), it cannot give that request back. So the only real defence is making abusive requests NOT REACH the Worker, and the code can only make them cheaper once they do.
* Static assets (files in an assets directory) are served without running the Worker, and requests for them are free and unlimited on the Free plan. (UNSURE: whether a custom `_headers` rule changes that; whether
  `run_worker_first` defaults to off for all paths on every wrangler version.) Only a path that has a file is covered, so an attacker simply asks for another path: static assets protect the pages that are expected to be popular, not the API.
* Whether a Durable Object request also counts against the 100,000 Worker requests (it has its own 100,000 a day on the Free plan) is not confirmed (`share/cloudflare-costs.md`). The code assumes the worst: each request that reaches the object is both.
* workers.dev is a Cloudflare-owned zone: the owner cannot put a WAF / rate-limiting rule in front of a `*.workers.dev` name. A custom domain on Cloudflare DNS (`galaxy.psvita.dev`; the Free plan includes
  5 WAF custom rules and ONE rate-limiting rule with a 10 s period and a 10 s mitigation, re-read 14 Nov 2026) is the only way to stop a flood before the Worker is invoked.

**What the code does** (all of it tested in `test/core/svc3-quota.test.js`, `svc3-farming.test.js`, `smg-budget.test.js`):

| layer | where | what it costs the attacker / us |
|---|---|---|
| edge bucket 120 burst + 1/s per address | Worker memory, per isolate | an attacker still passes about 86,000 requests a day per isolate-address at the sustained rate; honest consoles ask far less (a lobby about 1 a second, a started room 0.08) |
| **daily ceiling per address and class** | Durable Object memory, mirrored to storage (sec34: table `addr_day`, one row per UTC day + class + keyed hash of the address; the IPv6 key is the /48, IPv4 the address; written at an address's first counted request after a restart and then once per 20 requests, so an eviction loses at most 19 per key; `PERSIST_ADDR_COUNTERS=0` turns the storage off) | `ADDR_DAILY_OPEN` 1,000 (no token: info, servers, time, challenge, register, junk paths), `ADDR_DAILY_PLAYER` 3,000 (token, not a live room's traffic; wrong tokens count here), `ADDR_DAILY_LIVE` 10,000 (a live room member's room and relay calls, about 160 player-hours behind one NAT), `PID_DAILY_MAX` 4,000 per account (about 65 player-hours). Over it: 429 `rate_limited` with `reason: daily_cap`, until 00:00 UTC |
| **the Worker remembers the verdict** | Worker memory, bounded (4,000 entries) | the object's 429 carries `x-msc-block: <class>:<seconds>`; this isolate then answers that address's requests of that class without waking the object (checked in the tests: a flood of 6,000 requests reaches the object about 1,000 times). It costs the Worker request the platform already counted, and no Durable Object request |
| **refused requests are not budget** | `core/budget.js` | the day's `DAILY_REQUEST_BUDGET` (45,000) counts requests the object ADMITTED. Requests refused for abuse are counted apart (`rejectedAddress`) as are requests refused because the budget is spent (`rejectedBudget`); both are in `GET /v1/admin/stats` under `budget`. Before: every request counted, so one address locked every honest player out |
| **reserve for sessions in progress** | `LIVE_RESERVE_PCT` 20 | new sessions (sign-up, create, join, Quick Match, friends, times, info) stop at 80 % of the budget (36,000); a console that is a member of a live room is served to the whole 45,000: its room polls, endpoint / punch / started / leave and relay calls. The refusal is `503 over_budget` with `x-msc-budget: new-over` (the Worker then answers the new-session paths itself and still forwards the live ones); at 100 % it is `x-msc-budget: over` for everybody. A mid-game DROP-IN or REJOIN is a new session (join) and is refused in the `new-over` state: the one limit of the design |
| **pace** | `POLL_PACE_MAX_PCT` 140 | room answers carry `pollScale` (absent at 1): 1 up to half of the new-session budget, rising to 1.4. The client stretches its held poll from 8 to at most 11 s and its gaps by the same factor (never past the 12 s "quiet" limit: that is why 1.4 is the most), about 28 % fewer requests per console at the top |
| client | `np_signal_http.cpp` | honours `503 over_budget`: no retry storm (the room-deciding calls used to try 3 times), remembers it for at most 10 minutes and answers the next Host() / Join() itself; a running room's poll waits what the service said, capped at 60 s |
| sign-up farming | `accounts.js`, `fill.js` | see below |

**The day resets at 00:00 UTC, and that is kept.** Decision: no rolling window. The platform's own quota resets at 00:00 UTC (about 19:00 / 20:00 US Eastern, 16:00 / 17:00 Pacific), i.e. at the START of the Americas' evening, so a day budget spent
during an evening is fresh again for the next evening; a rolling 24-hour count would keep refusing players for hours after the real quota is fresh again, which is the opposite of the goal. What does hurt is the cliff: the whole budget is
gone in the middle of a busy evening and every game in progress loses its signaling. The reserve (nobody mid-game is cut off by the crowd that arrived later), the pace (the clients slow down before the cliff, not after) and the refusal of new sessions at 80 % are the answer; the lock-out
message tells the player the service is busy, and the clients stop asking for up to 10 minutes.

**Duration (13,000 GB-s a day), review S4.** `node scripts/estimate-duration.mjs`. Duration is billed per OBJECT in wall-clock time at 128 MB, not per request: 30 held polls at once cost the same as one. Held polls are therefore not the cause: a player in a started room has a request
in flight about 2/3 of the time and the gap is 4 s, but with plain 8 s polls the object would be just as awake (an idle object lingers for at least about 10 s; UNSURE of the exact figure, the script shows 10 / 70 / 140 s). The ceiling is by construction one object awake ALL day:
86,400 s x 0.125 GB = 10,800 GB-s = 83 % of the quota, so Galaxy alone can NOT exhaust it. A day with 1,000 player-hours (the whole request budget at 60 requests per player-hour; it was 170 at 354) in a 6-hour evening is about 2,800-3,800 GB-s (21-29 %), spread over 14 hours 6,500-8,400 (50-65 %). What can exhaust it is Galaxy
and Strikers together (their objects add up on one account): if both stay awake all day that is 21,600. Nothing in the code can shorten Galaxy's awake time while someone plays; watch "duration" in the dashboard and, if it climbs, lower `DAILY_REQUEST_BUDGET`.

**Sign-up farming (S6).** Per-address and per-network caps and the proof of work only price a sign-up. What is added: a SILENT account (signed up, never a call more than 5 minutes later, no friend, no star time, not banned) is (1) removed first when the service is full
(`REGISTER_EVICT_UNUSED_HOURS` 24: before this a full table stayed full for 60 days) and (2) removed by the hourly sweep after `PRUNE_UNUSED_DAYS` (3). A console whose silent account went registers again quietly (it keeps its stored name). **Sign-up never starves behind idle accounts (svcfix3, review S4):** when the table is full the order is (1) a silent account a day old, (2) the oldest silent account at least `REGISTER_EVICT_SILENT_MIN_MINUTES` (30) old, so junk made in the last day cannot close sign-up for a day, (3) an account with no friend and no star time unseen `REGISTER_EVICT_FRIENDLESS_DAYS` (14) days, (4) the account unseen longest after `REGISTER_EVICT_IDLE_DAYS` (60). A banned account, one with a friend or a star time, or one used after its first 5 minutes is never taken by steps 1 to 3. Not done: the proof of work does not yet scale with the day's global count, and a shared CGNAT /24 can still spend its own 60 a day. The table's fill (`core/fill.js`, from memory,
re-read every 10 minutes: a `COUNT(*)` per sign-up scanned the whole table and counted every row as a row read) tightens the door: from 50 % of `MAX_PLAYERS` the daily sign-up cap halves and the proof of work is one bit harder, from 80 % a quarter and two bits (never above `POW_CLIENT_MAX_BITS`, 17, the client's limit).
10,000 farmed sign-ups used to fill 50,000 accounts for 60 days; now a filled table makes room for real players from the second day.

**Proof of work: what it buys, and what it does not (svcfix2).** The proof of work (`core/pow.js`, client `platform/src/mp/mp_pow.h`) finds a nonce so that SHA-256(challenge + ":" + nonce) starts with `bits` zero bits (15 bits = about 32,000 tries on average). It prices a script that registers accounts by hand, and nothing more: against a GPU it buys almost nothing (a single consumer GPU does billions of SHA-256 tries a second, so even 17 bits is a few microseconds), and a farm does not need to wait. The difficulty is bound to the state when the solution ARRIVES: a stockpile of challenges fetched in advance at the base price is refused with a new, harder challenge (`428 pow_required`) once the address has signed up more than `REGISTER_POW_FREE` times in 24 h. What really limits sign-up farming is the per-/64 (IPv6) and per-/48 or IPv4 /24 daily caps (`REGISTER_NET_PER_DAY`, the per-address bucket `REGISTER_IP_*`), the daily total, the silent-account eviction and the fill levels above.
On a Vita the solver runs 6 ms of every 16 ms menu frame (`kSolveBudgetMs` / `kSolveWindowMs` in `mp_service.cpp`), i.e. at most 37 % of the core. ESTIMATES, not measured on a console (this build machine does 1.0 million tries a second on one core; a 444 MHz Cortex-A9 is guessed at 150 to 200 thousand, about 60 to 75 thousand in effect): 15 bits about 0.5 s on average, 17 bits about 2 s (95th percentile about 6 s), 18 bits about 4 s (95th about 12 s), 20 bits about 15 s. A player must never wait more than about 10 s, so the client refuses more than 17 bits (`mp_pow::kMaxBits`) and shows the kind "online play is busy" line instead of grinding, and the service never asks for more (`POW_CLIENT_MAX_BITS`, 17; `REGISTER_POW_MAX_BITS` 17). Please measure it on a console before relying on the numbers.

**What the DEPLOYER must do in Cloudflare** (the code cannot; since 14 Nov 2026 a deploy script and a zone script do it, one Codex request, `docs/ONLINE-DOMAIN-SETUP.md`; facts re-read from Cloudflare's documentation on 14 Nov 2026, each with its page):
1. **Custom domain.** The Worker's public address is `https://galaxy.psvita.dev`, attached ONCE by the owner in the dashboard (Workers & Pages > smg-online > Settings > Domains & Routes > Add > Custom Domain). `wrangler.smg.toml` has NO `routes` and no custom domain on purpose, and `scripts/deploy-smg-online.sh` refuses a config that has one: wrangler's deploy would post the domain changeset with `replace_state=true` and, without a terminal, override an existing DNS record or another Worker's binding without asking. Removing the domain is a dashboard step. Before the deploy script sends the admin token to an address it checks `GET /v1/prove` there (the service proves it holds the admin secret for that host name). A Custom Domain is a whole HOSTNAME: no wildcard, and paths and query strings are ignored, so every path of `galaxy.psvita.dev` reaches the Worker. The zone `psvita.dev` must be active; the hostname must have no existing CNAME record; Cloudflare creates the DNS record and the certificate itself (https://developers.cloudflare.com/workers/configuration/routing/custom-domains/). `workers_dev = true` stays for now and is turned OFF at release, once the hardware test has passed on the new address (no WAF rule or rate limit can sit in front of `workers.dev`). The clients default to the custom domain and never fall back to `workers.dev` (an account belongs to one server address).
2. **Rate limiting: the Free plan has ONE rule per zone, and the zone `psvita.dev` is SHARED by the games** (Galaxy at `galaxy.psvita.dev`, Strikers at its own host on its own Worker). So the rule is ONE shared rule (ref `psvita-shared-ratelimit`) in the path form only: it matches the paths `/v1/*` and `/times` of the whole zone, counted by `cf.colo.id` + `ip.src`, **100 requests per 10 s** (ten times a busy honest lobby), counting period 10 s, Block for 10 s. The expression is `(starts_with(http.request.uri.path, "/v1/") or http.request.uri.path eq "/times")` (https://developers.cloudflare.com/waf/rate-limiting-rules/, parameters page: `cf.colo.id` is mandatory; the Free plan documents only Path and Verified Bot as fields, so there is no host form any more). Galaxy owns this rule's script; **Strikers must not add a second rate-limiting rule** (the Free plan has one; `scripts/setup-psvita-zone.sh` stops and changes nothing when the zone's rule is not its own). If another game's paths differ, the owner runs the script again with `--ratelimit-extra-paths "/prefix/*,/exact"`: the one rule grows, it is never a second rule. `--ratelimit-path-only` is a no-op kept for one release. The script never PATCHes the shared rule over another game's paths without `--ratelimit-drop-paths` (it stops and prints both expressions), and `--remove` leaves the shared rule alone unless `--remove-shared-ratelimit` is given. Rules Galaxy owns on the zone: the rate-limiting rule above (shared in effect) and the WAF block rule `galaxy-psvita-block-unserved`, which is always scoped to `http.host eq "galaxy.psvita.dev"` and can never block another game's host.
3. **Bot Fight Mode must stay OFF on psvita.dev** while the game API lives there. It cannot be skipped with WAF custom rules (Skip, Bypass and Allow have no effect on it), it protects the whole domain without endpoint exceptions, "may challenge API or mobile app traffic" and issues computational challenges that a Vita cannot answer (https://developers.cloudflare.com/bots/get-started/free/). The zone script never changes it (only the opt-in `--bot-fight-off` does), `--verify-only` fails if it is on, and a normal run with it ON stops BEFORE any write (nothing is written; it reports and waits for the owner). Every line the script prints that came from the API (rule expressions, descriptions, error summaries) is scrubbed of e-mails, long hex ids and the token value itself. `deploy-smg-online.sh` also checks that `/v1/info` announces app min and latest exactly as `wrangler.smg.toml` declares them, answers the game's User-Agent, and that the admin stats answer on the custom domain and on workers.dev. It is zone-wide, so it matters for both games on the zone. **Browser Integrity Check** is zone-wide too: `setup-psvita-zone.sh --verify-only` reports whether it is on, as text only, and never changes it (it can refuse requests without a normal User-Agent).
4. **WAF custom rule: block every path the Worker does not serve**, so junk traffic never invokes the Worker (a request that reaches the Worker is already counted against the 100,000 a day). Free plan: 5 custom rules (https://developers.cloudflare.com/waf/custom-rules/). The Worker serves exactly: `/v1/*` (the routes of `src/core/app.js` `smgRoutes()`: info, servers, time, register, me, players, rooms, invites, quick, friends, times, turn, public/times, admin/*; an unknown `/v1/` path is a 404 from the Worker itself) and `/times` (the star-time page; with `--static-assets` the same path is served from the assets, no other file is published: `times.html` answers `/times`, `_headers` is not served). Rule (action Block): `(http.host eq "galaxy.psvita.dev" and not starts_with(http.request.uri.path, "/v1/") and http.request.uri.path ne "/times" and not starts_with(http.request.uri.path, "/.well-known/acme-challenge/") and not starts_with(http.request.uri.path, "/.well-known/pki-validation/"))` (the two `.well-known` paths are the certificate authorities' domain-validation requests, left alone so the certificate can renew: UNSURE whether Cloudflare needs that; harmless). UNSURE: whether the Free plan accepts `starts_with` in a custom rule (regular expressions are not available on Free; functions are normally allowed). The root `/` of the hostname is therefore a 403 from Cloudflare. This rule is Galaxy's alone and is always scoped to the host `galaxy.psvita.dev` (the expression below starts with that condition), so it never touches the Strikers host.
5. **Notifications.** There is NO Workers or Durable Objects usage alert on the Free plan: "Usage Based Billing" notifications are for Professional plans and up and for pay-as-you-go accounts only, as are budget alerts (https://developers.cloudflare.com/notifications/notification-available/, https://developers.cloudflare.com/billing/manage/budget-alerts/). The earlier text that said "usage alerts for Workers requests and Durable Objects (email)" was wrong for Free. The only related alert on every plan is the HTTP DDoS Attack Alert (above 100 requests a second mitigated): it needs an e-mail destination, so it is NOT set up by the scripts. The day's 100,000 is the cap; over it the platform answers 1027. Watch the numbers instead: Workers & Pages > smg-online > Metrics, `GET /v1/admin/stats`.
6. Optional static assets for the `/times` page: `node scripts/build-assets.mjs` then `npx wrangler deploy --config wrangler.smg.toml --assets public` (`deploy-smg-online.sh --account-id <32 hex> --admin-token-file <file> --static-assets` does both; `--account-id` is required, see DEPLOY-SMG.txt). UNTESTED here (no wrangler run was possible); the Worker still serves `/times` when the file is missing.
7. Watch **Workers & Pages > smg-online > Metrics**: requests (the account's 100,000), and Durable Objects > duration and rows read. `GET /v1/admin/stats` shows `budget` (used / rejectedAddress / rejectedBudget / pace / the busiest addresses' counts).
8. Agree the split with the Strikers service: the account's 100,000 is shared; Galaxy takes at most `DAILY_REQUEST_BUDGET` (45,000) admitted requests plus whatever abuse the layers above fail to stop. The agreed split is Galaxy 45,000 and Strikers 45,000: the two sums must stay at 90,000 or less, which leaves 10,000 for requests the abuse layers refuse (they still count against the 100,000). Lower it when Strikers is busy.

**Request bodies.** A `POST` / `PUT` without a numeric `Content-Length` (a chunked upload) is answered **411** at the edge; the announced length is capped (4 KiB); and the object reads the body stream with the same hard cap
(`readBodyCapped`), so a lying `Content-Length` cannot get a big body past it (413). A request with no body at all is fine.

**Star times.** Galaxy must be 0..41. A time that looks too good is kept but **flagged** (admin queue `GET /v1/admin/flags`, kinds `time_jump`, `time_first`, `time_top`) and hidden from other players and the public page until
`POST /v1/admin/flags/clear` (the player still sees it). `time_jump`: more than `SMG_TIME_JUMP_PCT` (25 %) better than the player's time at the start of the last `SMG_TIME_ROLLING_HOURS` (24 h) (small steps add up);
`time_first`: a first time more than `SMG_TIME_FIRST_PCT` (15 %) below the board's best; `time_top`: an unverified time that becomes #1 by more than `SMG_TIME_TOP_PCT` (8 %). Race and co-op times (more than one player) need a room
that has **started** (`room_required` / `room_not_started`); a verified time must not be longer than the room's age (+ `SMG_VERIFY_SLACK_SEC`, 3 s). Valid star numbers per galaxy are not checked (there is no authoritative table; a star
the galaxy does not have simply has no real competitors and is caught by the floors and flags). The race rule "2 members is enough" is unchanged, but since svcfix3 the two race times must also be within `SMG_VERIFY_RACE_SEC` (60 s) of each other.

**Endpoint claims.** `POST /v1/rooms/<code>/endpoint` is checked against the address the request came from (`cf-connecting-ip`): a `public` address or a public (`srflx` / `observed`) candidate that differs from it (or any claim from an IPv6-only caller)
puts that member on the relay (`steer.reason = endpoint_unverified`, no addresses exchanged for its pairs). The LAN address (`local`, `host` candidates) is passed on only to a member that calls from the same public address.
In a **public** room a guest receives the host's address only after the host has called since the guest joined. At most `SMG_PUBLIC_ROOMS_PER_IP` (4) live public rooms per address (`too_many_rooms`). The public list shows rooms with the
most players first, then the newest. Relay addresses a member posts are not verified.

**Caps.** `REGISTER_MAX_PER_DAY` 10000 (launch week; 2000 afterwards) and `MAX_PLAYERS` 50000 (`wrangler.smg.toml`; the code's defaults, when a variable is absent, are 2000 and 50000; a test, `test/core/svc3-readme.test.js`, keeps these numbers equal to the file). Stored incoming friend requests are capped at 100 per player (the oldest are dropped; `GET /v1/friends` returns `incomingTotal`).
**TURN is best effort**: when the daily credential count, the player's share or the monthly budget is used up, `POST /v1/turn` answers 503 / 429 (`turn_budget`, `turn_player_limit`) and the client says so in a sentence and carries on with direct connections;
only a pair that cannot connect directly is left without a path. Watch `credsToday` in `GET /v1/admin/stats` (the Free plan has no usage alert for it, see "What the DEPLOYER must do").

**Backups are sensitive** (see Deploying step 11). The export does not include `room_members`.

**Pointing the game at another deployment (a community's own server).** The service address is, in this order: the environment / `ux0:data/smg/env.txt` entry `SMG_ONLINE_URL=https://...` (older, for testing), the **`server`** key of
`ux0:data/smg/mp/mp.cfg`, then the build's `NP_ONLINE_URL`. In `mp.cfg`: `server = https://my-galaxy.example.org` (HTTPS only; host and optional port, no path; at most 120 characters; anything else is ignored). The server must be a deployment of this
code with `SERVICE_GAME=smg` and the same `MIN_APP_VERSION` / `SMG_TIME_BUILDS` as the game build. The TLS certificate must chain to one of the roots compiled into the game (`net/src/np_https_roots.h`: GTS Root R1 / R4, ISRG Root X1 / X2, SSL.com roots;
that covers `workers.dev` and Cloudflare custom domains); there is no switch to turn verification off, so a self-signed server will not work. **Tokens are per server**: the build's own account stays in `online.dat`, every other address gets its own
`online_<hash>.dat`, so switching servers never sends one server's token to another; the first online action on a new server offers to register there, and switching back finds the old account again.

## Room codes and addresses (svcfix2, independent review)

A room code is six characters of a 31-letter alphabet (887 million codes). It is a secret only as long as guessing is expensive, and it
used to open too much: a stranger who joined by code and posted his own endpoint was handed the host's PUBLIC address. Now:

* **The host's console accepts.** A stranger who joins a `code` room (neither the host's mutual friend, nor a member the host accepted
  before) is `accepted: false`. Nobody sees an address of him and he sees none of the host's, until the host's console calls
  `POST /v1/rooms/:code/accept {slot}` (host only). Public rooms (the host chose them) and friends are accepted at once. The game's
  `HttpSignal` makes that call for each joiner (`Signal::SetAcceptPolicy` is the hook for a prompt; **until a lobby prompt exists the default
  policy accepts**, so what the step buys today is: a guessed code reveals nothing to a stranger who walks away, a dead or abandoned room,
  or a script that only joins and reads). `REQUIRE_ACCEPT=0` switches the step off.
* **Wrong codes cost the address, not the account** (accounts are cheap): ten in 10 minutes, `WRONG_CODES_PER_DAY` (40) a day, per /64
  address key, whichever account asked. At 40 guesses a day against, say, 500 live code rooms the chance of hitting one is about
  40 x 500 / 887,000,000 = 0.002 % per address per day (it was about 0.3 % with the old per-account allowance).
* A stranger who leaves and comes back is still waiting; a player the host accepted who drops and rejoins is not asked again.
* Still true: members of a room see each other's addresses (that is what a game is). A room's code is shown to its members on screen.

## Far-away invites (code5): a 5-character code

The long direct code (address, port, secret) is too long to say aloud. A host's console can instead register a 5-character invite
(same 31-letter alphabet, 28.6 million codes). The console picks the five characters, seals its address, port and secret under a key stretched from
them (XChaCha20-Poly1305) and gives the service only the 48-character blob; the service stores it opaquely and keeps it out of the admin export.

* `POST /v1/invites {code, blob}` (host; one live invite per account; 409 `invite_taken` if the code is live, which is charged as a wrong guess),
  `POST /v1/invites/resolve {code}` -> `{blob}` (at most 3 different accounts and 12 resolves per invite), `POST /v1/invites/:code/keep`,
  `POST /v1/invites/:code/close` (also `DELETE`; `np_https` has no DELETE, so POST is the one the game uses).
* Lifetime: `INVITE_TTL_SEC` (600) renewed by keep; `INVITE_MAX_LIFE_SEC` (3600); `INVITE_GRACE_SEC` (60) after a missed keep. Closing kills it at
  once (410 `invite_closed`); dead rows stay `ROOM_KEEP_CLOSED_SEC` so a friend hears "expired", not "wrong". `INVITE_MAX_LIVE` (2000) caps the table.
* Wrong codes share the room-code allowance (10 per 10 min, `WRONG_CODES_PER_DAY` a day per /64 address). At 40 guesses a day against N live
  invites the chance per address-day is 40 x N / 28,629,151 (N = 30: about 1 in 24,000). Lower `WRONG_CODES_PER_DAY` to tighten it.
* Budget: about 3.8 requests per invite (1,000 invites a day is about 6% of `DAILY_REQUEST_BUDGET`). Register is refused in the reserve zone;
  resolve of an open invite is live traffic; close is never refused. Kill switch: the `rooms` one. Migration 11 creates the `invites` table.
* The seal hides the address from logs and exports only: a 25-bit key can be brute-forced by anyone holding a database copy.

## What a verified time proves, and what it cannot (svcfix2, independent review)

A time is **verified** when ANOTHER member of the same started room submits the same star from that room within `SMG_VERIFY_WINDOW_SEC`
(30 minutes), with the frame count within `SMG_VERIFY_FRAMES` (2) of it (co-op) or within `SMG_VERIFY_RACE_SEC` (60 s) of it (race: svcfix3, review S6; before, a race confirmed with no agreement on the time), both accounts at least `SMG_VERIFY_MIN_AGE_HOURS` (48) old, calling
from different addresses (`SMG_VERIFY_DISTINCT_ADDR`), and the time not longer than the room's age (+ `SMG_VERIFY_SLACK_SEC`). Unverified
rows are seen only by their owner (and mutual friends) unless `SMG_BOARD_UNVERIFIED=show`, so **a first time is already hidden from the
public board until a second independent session confirms it**; nothing more was needed for the "hide first times" request.

What that proves: *two accounts, two addresses, both older than two days, sat in a started room together for at least that long and
reported the same time.* It does **not** prove that a game was played, or that the two are strangers. Two colluding accounts on two
addresses, aged past 48 hours, can start a room, wait out the clock and both submit any time above the floor (`smg_floors.js`) that
stays under the 8 % top flag (the reviewer did exactly this with 112 seconds of waiting, and got a verified rank 1). The service sees
requests, never the game, so nothing it can do closes this. What limits it: the floors (a record 30 % faster than the speedrun record is
refused), the flags that queue a suspicious first or top time for an admin (`time_first`, `time_top`, `time_new_star`, `time_jump`), the
48 hour wait, the per-address account limit, and the admin tools (`board_bans`, removing a row). **Known limit:** a patient pair of forged
accounts can post a verified time a few percent below the real record, or any time on a star nobody has played; a forged time that is
also plausible cannot be told from an honest one. Deployers who care should watch the admin flag queue and keep `SMG_FLAG_NEW_STAR` on.
