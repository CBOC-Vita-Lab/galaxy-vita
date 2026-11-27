// Server configuration: wrangler vars / secrets (env) with safe defaults, plus runtime overrides
// stored in the settings table (admin kill switches).

import { envInt } from './util.js';
import { DEFAULT_STAR_FLOORS } from './smg_floors.js';

export const PROTO = 1;
export const KILL_NAMES = ['all', 'register', 'rooms', 'ranked', 'ranking', 'turn'];

// One code base, two deployments: GAME=msc (default; Strikers Charged, unchanged) and GAME=smg (Super Mario Galaxy 3.0, worker
// smg-online). The game decides the route table (edge.js), the version gate defaults and the Galaxy-only features (SMG-PROTOCOL.txt).
export const GAMES = ['msc', 'smg'];
export const SMG_KILL_NAMES = ['all', 'register', 'rooms', 'turn', 'friends', 'quick', 'times', 'report'];
export const killNamesFor = (game) => (game === 'smg' ? SMG_KILL_NAMES : KILL_NAMES);
/** Per-game defaults; MIN_APP_VERSION / LATEST_APP_VERSION in env win when set. */
const GAME_DEFAULTS = {
  msc: { minApp: '', latestApp: '', maxBody: 8192 },
  smg: { minApp: '3.0.0', latestApp: '3.0.0', maxBody: 4096 },
};

/**
 * secreview32 (F1): the version gate as the Worker can apply it without the object (edge.js), from the same variables loadConfig reads:
 * { minProto, minApp }. Kept beside GAME_DEFAULTS so the two cannot drift apart.
 */
export function versionGate(env = {}) {
  const game = rawGame(env) === 'smg' ? 'smg' : 'msc';
  return { minProto: envInt(env, 'MIN_CLIENT_PROTO', 1, 1, 1000), minApp: String(env.MIN_APP_VERSION || GAME_DEFAULTS[game].minApp) };
}

/**
 * The game variable as given: SERVICE_GAME, or GAME when it is a string. (On Cloudflare the binding named GAME is the Durable Object
 * namespace, so wrangler.smg.toml spells the variable SERVICE_GAME; the fake server and the tests use GAME=smg.)
 */
export function rawGame(env) {
  if (!env) return undefined;
  if (env.SERVICE_GAME !== undefined && env.SERVICE_GAME !== '') return String(env.SERVICE_GAME);
  return typeof env.GAME === 'string' && env.GAME !== '' ? env.GAME : undefined;
}

/** The game: absent = msc. A value that is neither is a deployment mistake: refuse to start rather than guess. */
export function gameOf(env) {
  const g = rawGame(env) ?? 'msc';
  if (!GAMES.includes(g)) throw new Error(`GAME must be one of ${GAMES.join(', ')}`);
  return g;
}

/** SMG_STAR_FLOORS: JSON {"<galaxy>:<star>": frames, ...} (per-star minimum plausible time); bad entries are dropped. */
function parseFloors(raw) {
  const out = {};
  try {
    const o = JSON.parse(raw || '{}');
    if (o && typeof o === 'object' && !Array.isArray(o)) {
      for (const [k, v] of Object.entries(o)) {
        if (/^\d{1,2}:\d$/.test(k) && Number.isInteger(v) && v >= 0 && v <= 432000) out[k] = v;
      }
    }
  } catch { /* none */ }
  return out;
}

function hexToBytes(h) {
  if (typeof h !== 'string' || h.length % 2 || !/^[0-9a-fA-F]*$/.test(h) || !h.length) return null;
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
  return out;
}

/**
 * Static part of the configuration (from env). Cheap; build once per object start.
 * @param {Record<string, any>} env
 */
function parseServerList(raw) {
  try {
    const a = JSON.parse(raw || '[]');
    return Array.isArray(a)
      ? a.filter((e) => e && typeof e.url === 'string' && /^https:\/\/[A-Za-z0-9.-]+(:\d+)?\/?$/.test(e.url))
        .slice(0, 8).map((e, i) => ({ url: e.url.replace(/\/$/, ''), priority: Number.isInteger(e.priority) ? e.priority : i }))
      : [];
  } catch {
    return [];
  }
}

/** S9: the shortest ADMIN_TOKEN that enables the admin routes (a short one is guessable; the routes then answer 404 as if absent). */
export const ADMIN_TOKEN_MIN = 24;
export const adminTokenOk = (t) => typeof t === 'string' && t.length >= ADMIN_TOKEN_MIN;

export function loadConfig(env) {
  const warnings = [];
  env = env || {};
  const list = (v) => String(v || '').split(',').map((s) => s.trim()).filter(Boolean);
  const policy = String(env.RANK_PUT_POLICY || 'credit');
  const game = gameOf(env);
  const gd = GAME_DEFAULTS[game];
  // The ceiling on the object's own request count (budget.js `touch`): every request that reaches the Durable Object, refused or not, and every alarm run.
  // Default 125 % of the request budget. It must NOT be below the request budget (the request budget could then never be reached): clamped up, with a warning.
  const dailyBudgetV = envInt(env, 'DAILY_REQUEST_BUDGET', game === 'smg' ? 45000 : 0, 0, 1_000_000_000);
  let objectBudget = envInt(env, 'DAILY_OBJECT_BUDGET', Math.round(dailyBudgetV * 1.25), 0, 1_000_000_000);
  if (dailyBudgetV > 0 && objectBudget > 0 && objectBudget < dailyBudgetV) {
    warnings.push(`DAILY_OBJECT_BUDGET (${objectBudget}) is below DAILY_REQUEST_BUDGET (${dailyBudgetV}): raised to ${dailyBudgetV}`);
    objectBudget = dailyBudgetV;
  }
  return {
    warnings,
    game,
    maxBody: gd.maxBody,
    minProto: envInt(env, 'MIN_CLIENT_PROTO', 1, 1, 1000),
    minApp: String(env.MIN_APP_VERSION || gd.minApp),
    latestApp: String(env.LATEST_APP_VERSION || gd.latestApp),
    requireApp: env.REQUIRE_APP_VERSION === '1',
    serverList: parseServerList(env.SERVER_LIST_JSON),
    killEnv: list(env.KILL_SWITCH),
    maintenanceMessage: String(env.MAINTENANCE_MESSAGE || '').slice(0, 200),
    nameBlockExtra: list(env.NAME_BLOCKLIST_EXTRA),
    allowTestClock: env.ALLOW_TEST_CLOCK === '1',
    rateScale: Number(env.RATE_LIMIT_SCALE) > 0 ? Number(env.RATE_LIMIT_SCALE) : 1,
    maxPlayers: envInt(env, 'MAX_PLAYERS', game === 'smg' ? 50000 : 200000, 1, 10_000_000), // README.md: free-tier arithmetic (Galaxy: SMG-PROTOCOL.txt "Sign-up")
    // accounts that never played (no match record, no score) and were not seen for this long are removed by the sweep; 0 = never
    // best rows kept per archived season (archive.js)
    archiveTop: envInt(env, 'ARCHIVE_TOP', 100, 10, 10000),
    pruneIdleMs: envInt(env, 'PRUNE_IDLE_DAYS', 180, 0, 3650) * 86400_000,
    // Galaxy: ANY account not seen for this long is removed with everything it owns (friends both ways, times, blocks); 0 = never.
    // A console that comes back registers again quietly (a new friend code). Strikers: 0 (only never-played accounts are pruned).
    expireIdleMs: envInt(env, 'ACCOUNT_EXPIRE_DAYS', game === 'smg' ? 365 : 0, 0, 3650) * 86400_000,
    renameCooldownMs: envInt(env, 'RENAME_COOLDOWN_HOURS', game === 'smg' ? 0 : 24, 0, 24 * 365) * 3600_000, // Galaxy: display names, no cooldown (rate limit only)
    stunServers: list(env.STUN_SERVERS || 'stun.cloudflare.com:3478'),
    rooms: {
      openTtlMs: envInt(env, 'ROOM_OPEN_TTL_SEC', 600, 10, 86400) * 1000,
      joinedTtlMs: envInt(env, 'ROOM_JOINED_TTL_SEC', 1200, 10, 86400) * 1000,
      startedTtlMs: envInt(env, 'ROOM_STARTED_TTL_SEC', 3 * 3600, 60, 86400) * 1000,
      keepClosedMs: envInt(env, 'ROOM_KEEP_CLOSED_SEC', 300, 0, 86400) * 1000,
      ackMs: envInt(env, 'JOIN_ACK_SEC', 5, 1, 120) * 1000,
      natLifeMs: envInt(env, 'NAT_LIFE_SEC', 30, 5, 600) * 1000,
      pairFailLimit: envInt(env, 'PAIR_FAIL_LIMIT', 2, 1, 100),
      pairFailKeepMs: envInt(env, 'PAIR_FAIL_KEEP_DAYS', 7, 1, 365) * 86400_000,
      // Strikers polls every 1.7 s (lost after 15 s, quiet after 5 s). Galaxy's started rooms poll every 8 s (the peers heartbeat each
      // other; the service is only for host migration and rejoin), so its link states are slower: lost after 20 s (two missed polls
      // still count as alive), quiet after 12 s.
      lostMs: envInt(env, 'LINK_LOST_SEC', game === 'smg' ? 20 : 15, 5, 300) * 1000,
      quietMs: envInt(env, 'LINK_PING_SEC', game === 'smg' ? 12 : 5, 1, 60) * 1000,
      // svcfix2: the poll rhythm the service dictates to a member (rooms4.pollPlan), seconds. A started room whose peers all heartbeat each other
      // directly costs the service about one request a minute (the host's held poll); a guest asks every 5 minutes.
      poll: {
        formingHoldSec: envInt(env, 'POLL_FORMING_HOLD_SEC', 25, 1, 60),
        busyHoldSec: envInt(env, 'POLL_BUSY_HOLD_SEC', 8, 1, 60),
        // quota32: a started room whose links never settle (relay / unlinked) backs its held poll off while nothing changes: busyHoldSec,
        // then busyMidHoldSec after busyMidAfterSec, then busyMaxHoldSec after busyMaxAfterSec. Any change snaps it back to busyHoldSec.
        busyMidHoldSec: envInt(env, 'POLL_BUSY_MID_HOLD_SEC', 15, 1, 60),
        busyMaxHoldSec: envInt(env, 'POLL_BUSY_MAX_HOLD_SEC', 30, 1, 60),
        busyMidAfterSec: envInt(env, 'POLL_BUSY_MID_AFTER_SEC', 30, 1, 3600),
        busyMaxAfterSec: envInt(env, 'POLL_BUSY_MAX_AFTER_SEC', 90, 1, 3600),
        settledHostHoldSec: envInt(env, 'POLL_SETTLED_HOST_HOLD_SEC', 55, 1, 60),
        settledGuestNextSec: envInt(env, 'POLL_SETTLED_GUEST_NEXT_SEC', 300, 5, 600),
      },
      // svcfix2 item 8: a stranger who joins a CODE room gets no address of the host until the host's console accepts him (or he is a mutual
      // friend); wrong codes are limited per address (a day's worth). REQUIRE_ACCEPT=0 switches the accept step off (old behaviour).
      requireAccept: envInt(env, 'REQUIRE_ACCEPT', 1, 0, 1) === 1,
      wrongCodesPerDay: envInt(env, 'WRONG_CODES_PER_DAY', 40, 5, 10000),
      punchDelayMs: envInt(env, 'PUNCH_DELAY_MS', 1000, 0, 10000),
      punch: {
        intervalMs: envInt(env, 'PUNCH_INTERVAL_MS', 700, 50, 5000),
        tries: envInt(env, 'PUNCH_TRIES', 7, 1, 50),
        lingerMs: envInt(env, 'PUNCH_LINGER_MS', 5000, 0, 60000),
      },
    },
    // code5: far-away invites (invites.js): a 5-character code that resolves to the host's sealed direct-code content while its gathering is open
    invites: {
      ttlMs: envInt(env, 'INVITE_TTL_SEC', 600, 30, 3600) * 1000,              // open this long; the host's keep (at half of it) renews
      maxLifeMs: envInt(env, 'INVITE_MAX_LIFE_SEC', 3600, 60, 86400) * 1000,   // never longer than this from the start, kept or not
      graceMs: envInt(env, 'INVITE_GRACE_SEC', 60, 0, 600) * 1000,             // a late keep does not strand a friend
      maxResolvers: envInt(env, 'INVITE_MAX_RESOLVERS', 3, 1, 3),              // different accounts that may take the code (the room holds 4)
      maxResolves: envInt(env, 'INVITE_MAX_RESOLVES', 12, 1, 1000),            // answers of the blob per invite, retries included
      maxLive: envInt(env, 'INVITE_MAX_LIVE', 2000, 10, 100000),               // live invites at once (rows on the free plan)
    },
    queue: {
      staleMs: envInt(env, 'QUEUE_STALE_SEC', 30, 5, 600) * 1000,
      baseWindow: envInt(env, 'QUEUE_BASE_WINDOW', 100, 0, 5000),
      step: envInt(env, 'QUEUE_WINDOW_STEP', 50, 1, 5000),
      stepMs: envInt(env, 'QUEUE_WINDOW_STEP_SEC', 5, 1, 600) * 1000,
      maxWindow: envInt(env, 'QUEUE_MAX_WINDOW', 1000, 0, 100000),
      regionStrictMs: envInt(env, 'QUEUE_REGION_STRICT_SEC', 20, 0, 600) * 1000,
      maxWaitMs: envInt(env, 'QUEUE_MAX_WAIT_SEC', 300, 10, 3600) * 1000,
    },
    matches: {
      resultWindowMs: envInt(env, 'RESULT_WINDOW_SEC', 120, 5, 3600) * 1000,
      maxMatchMs: envInt(env, 'MAX_MATCH_SEC', 90 * 60, 60, 86400) * 1000,
      startWaitMs: envInt(env, 'MATCH_START_WAIT_SEC', 300, 10, 3600) * 1000,
      keepMs: envInt(env, 'MATCH_KEEP_DAYS', 90, 1, 3650) * 86400_000,
      forfeitPoints: envInt(env, 'FORFEIT_POINTS', 10, 0, 100),
      restoreCredit: envInt(env, 'RESTORE_CREDIT', 5, 0, 100),
      eloK: envInt(env, 'ELO_K', 24, 1, 200),
      cooldownBaseSec: envInt(env, 'ABANDON_COOLDOWN_SEC', 60, 0, 3600),
      cooldownMaxSec: envInt(env, 'ABANDON_COOLDOWN_MAX_SEC', 900, 0, 86400),
      // Shortest plausible ranked match for a normal finish (result 0). The game's shortest timed match is 2 minutes
      // (sGameTimeOptions {120,180,240,300}); a goal-limited match (limit 3..10) can end sooner, so the default is
      // well under 120 s. 0 turns the check off.
      minMatchMs: envInt(env, 'MIN_MATCH_SEC', 60, 0, 3600) * 1000,
      // Rated (verified, ranked) matches per UTC day. 0 = no cap.
      ratedPerPairDay: envInt(env, 'RATED_PER_PAIR_DAY', 4, 0, 1000),
      ratedPerAccountDay: envInt(env, 'RATED_PER_ACCOUNT_DAY', 30, 0, 10000),
      // Win-trading flag: >= oneOppMinWins rated wins in the last oneOppDays and >= oneOppSharePct % of them against one opponent.
      oneOppMinWins: envInt(env, 'ONE_OPP_MIN_WINS', 8, 1, 10000),
      oneOppSharePct: envInt(env, 'ONE_OPP_SHARE_PCT', 70, 1, 100),
      oneOppDays: envInt(env, 'ONE_OPP_DAYS', 30, 1, 365),
      // Dodge rule: over the last dodgeWindow settled ranked matches, a player blamed for >= dodgeRatePct % disputes /
      // no-contests (at least dodgeMinSample matches) is flagged and takes dodgePenaltyPct % of an abandon's rating loss.
      dodgeWindow: envInt(env, 'DODGE_WINDOW', 20, 3, 200),
      dodgeMinSample: envInt(env, 'DODGE_MIN_SAMPLE', 6, 1, 200),
      dodgeRatePct: envInt(env, 'DODGE_RATE_PCT', 35, 1, 100),
      dodgePenaltyPct: envInt(env, 'DODGE_PENALTY_PCT', 75, 0, 100),
    },
    // Per-account daily caps (UTC day, stored); 0 = no cap. Every room and every match record costs rows on the free tier.
    caps: {
      roomsPerAccountDay: envInt(env, 'ROOMS_PER_ACCOUNT_DAY', 60, 0, 100000),
      matchesPerAccountDay: envInt(env, 'MATCHES_PER_ACCOUNT_DAY', 60, 0, 100000),
    },
    rank: {
      hmacKey: hexToBytes(env.RANK_HMAC_KEY_HEX),
      requireHmac: env.RANK_REQUIRE_HMAC !== '0',
      checkName: env.RANK_CHECK_NAME !== '0',
      putPolicy: ['credit', 'delta', 'open'].includes(policy) ? policy : 'credit',
      maxDelta: envInt(env, 'RANK_MAX_DELTA', 25, 0, 999999),
      maxScore: envInt(env, 'RANK_MAX_SCORE', 999999, 1, 2147483647),
      allowanceCap: envInt(env, 'RANK_ALLOWANCE_CAP', 200, 0, 999999),
      regionMode: env.RANK_REGION_MODE === 'mask' ? 'mask' : 'pool',
    },
    turn: {
      provider: String(env.TURN_PROVIDER || ''), // 'cloudflare' | 'stub' | '' (off)
      keyId: String(env.TURN_KEY_ID || ''),
      stubUrls: String(env.TURN_STUB_URLS || ''), // tests: comma separated turn: URLs handed out by the stub provider
      stubSecret: String(env.TURN_STUB_SECRET || ''), // tests: secret that signs the stub credentials (TURN REST scheme)
      apiToken: String(env.TURN_KEY_API_TOKEN || ''), // secret; never returned to clients
      ttlSec: envInt(env, 'TURN_TTL_SEC', 3600, 300, 86400),
      maxPerDay: envInt(env, 'TURN_MAX_CREDS_PER_DAY', 300, 0, 1_000_000),
      maxPerPlayerDay: envInt(env, 'TURN_MAX_CREDS_PER_PLAYER_DAY', 6, 0, 10_000),
      // svcfix3 (review S5), Galaxy only (0 = off; Strikers keeps its behaviour): a pool of aged accounts must not use up the day's credentials.
      // A room takes at most maxPerRoom credentials per clock HOUR (4 members, one refresh each; a credential lasts an hour, so renewals of a long
      // relayed room are counted in their own hour and the cap never closes a room that is still being played); one source address at most maxPerAddrDay a day (a household of consoles);
      // and credentials issued WITHOUT a visible reason (rooms4.turnNeeded: relay steer or a failed punch) share unneededSharePct % of the day's
      // maxPerDay, so the rest stays for players whose connection really needs the relay.
      maxPerRoom: envInt(env, 'TURN_MAX_CREDS_PER_ROOM', game === 'smg' ? 8 : 0, 0, 1000),
      maxPerAddrDay: envInt(env, 'TURN_MAX_CREDS_PER_ADDR_DAY', game === 'smg' ? 24 : 0, 0, 100_000),
      unneededSharePct: envInt(env, 'TURN_UNNEEDED_SHARE_PCT', game === 'smg' ? 50 : 100, 0, 100),
      // Accounts younger than newAccountHours share a sub-budget of the daily credentials, so a burst of fresh accounts
      // cannot use up the day for established players. 0 hours = no distinction.
      newAccountHours: envInt(env, 'TURN_NEW_ACCOUNT_HOURS', 24, 0, 24 * 365),
      newAccountDaily: envInt(env, 'TURN_NEW_ACCOUNT_DAILY', 60, 0, 1_000_000),
      // Usage reports: only against a credential minted within its TTL (+ grace for the report after the match),
      // clamped to what that TTL can carry at maxKbps, at most maxReportsPerPlayerDay per player per day.
      maxKbps: envInt(env, 'TURN_MAX_KBPS', 128, 1, 100_000),
      reportGraceMs: envInt(env, 'TURN_REPORT_GRACE_SEC', 300, 0, 3600) * 1000,
      maxReportsPerPlayerDay: envInt(env, 'TURN_MAX_REPORTS_PER_PLAYER_DAY', 12, 0, 10_000),
      monthlyBudgetMb: envInt(env, 'TURN_MONTHLY_BUDGET_MB', 100_000, 0, 100_000_000),
      maxReportMb: envInt(env, 'TURN_MAX_REPORT_MB', 200, 1, 10_000),
      // Hard usage stop (turn.js): Cloudflare's own egress numbers through GraphQL, read with a read-only token (secret TURN_USAGE_TOKEN) for the account
      // TURN_ACCOUNT_ID (a secret/variable set by the deploy script). No token = the estimate-based caps above only. Gigabytes are 10^9 bytes.
      usageToken: String(env.TURN_USAGE_TOKEN || ''), // secret
      accountId: String(env.TURN_ACCOUNT_ID || ''),
      hardMonthGb: envInt(env, 'TURN_HARD_MONTH_GB', 300, 0, 1_000_000),               // Galaxy's key, this month
      hardAccountMonthGb: envInt(env, 'TURN_HARD_ACCOUNT_MONTH_GB', 700, 0, 1_000_000), // the whole account (the free 1,000 GB a month is shared with Strikers)
      hardDayGb: envInt(env, 'TURN_HARD_DAY_GB', 25, 0, 1_000_000),                     // Galaxy's key, today (UTC)
      usageEverySec: envInt(env, 'TURN_USAGE_EVERY_SEC', 600, 1, 86400),               // at most one analytics read per this many seconds
      // POST /v1/turn/check (Galaxy: "Check Connection", a relay loopback test on one console, no room): 300 s passes tagged "check", counted inside maxPerDay
      checkPerPlayerDay: envInt(env, 'TURN_CHECK_PER_PLAYER_DAY', 2, 0, 1000),
      checkPerAddrDay: envInt(env, 'TURN_CHECK_PER_ADDR_DAY', 6, 0, 10_000),
      checkMaxPerDay: envInt(env, 'TURN_CHECK_MAX_PER_DAY', 60, 0, 1_000_000),
    },
    register: {
      // Per client address (IPv6 by /64): a burst of ipBurst registrations (a venue full of Vitas on one NAT), then ipPerHour
      // per hour. Requests that fail validation never touch these buckets or the global one; they are counted in their own
      // (badPerHour) bucket instead.
      ipBurst: Math.max(envInt(env, 'REGISTER_IP_BURST', 10, 1, 1000), envInt(env, 'REGISTER_IP_PER_HOUR', 3, 1, 1000)), // never below the sustained rate
      ipPerHour: envInt(env, 'REGISTER_IP_PER_HOUR', 3, 1, 1000),
      badPerHour: envInt(env, 'REGISTER_BAD_PER_HOUR', 30, 1, 10000),
      // Per network (IPv4 /24, IPv6 /48: one provider's customer block, one cloud tenant's range) per day. 0 = off. Galaxy only by default.
      netPerDay: envInt(env, 'REGISTER_NET_PER_DAY', game === 'smg' ? 60 : 0, 0, 1_000_000),
      // New accounts for the whole service per UTC day (Galaxy 2,000, Strikers 1,000). Galaxy keeps the count in storage (a restarted
      // object does not hand out a second day's worth); Strikers keeps its in-memory bucket.
      maxPerDay: envInt(env, 'REGISTER_MAX_PER_DAY', game === 'smg' ? 2000 : 1000, 1, 10_000_000),
      storedDaily: game === 'smg',
      // Proof of work (pow.js): leading zero bits of SHA-256 the client must find; 0 = off. Galaxy: 15 (a Vita: ESTIMATED 0.5 s on average, see README "Proof of work";
      // SMG-PROTOCOL.txt "Sign-up"), rising by one bit per registration from the same address beyond powFree in 24 h, up to powMaxBits.
      powBits: envInt(env, 'REGISTER_POW_BITS', game === 'smg' ? 15 : 0, 0, 28),
      powFree: envInt(env, 'REGISTER_POW_FREE', 3, 0, 1000),
      powMaxBits: envInt(env, 'REGISTER_POW_MAX_BITS', 17, 0, 28),
      // the most the clients accept (mp_pow.h kMaxBits): about 2 s on average and 6 s at the 95th percentile on a Vita (ESTIMATE, README "Proof of work")
      powClientMax: envInt(env, 'POW_CLIENT_MAX_BITS', 17, 1, 28),
      // Galaxy: names are display names (the friend code is the identity): two players may both be "Luigi". Strikers keeps unique names.
      uniqueNames: game !== 'smg',
      // Galaxy: when MAX_PLAYERS is reached, a new registration takes the place of the account that has been unseen longest, if it has
      // been unseen for at least this many days (0 = never: server_full).
      evictIdleMs: envInt(env, 'REGISTER_EVICT_IDLE_DAYS', game === 'smg' ? 60 : 0, 0, 3650) * 86400_000,
      // svc3 (review S6, sign-up farming): a SILENT account is one that registered and never made a call more than 5 minutes after (last_seen_ms
      // does not move before that), holds no friend and no star time. When the service is full a silent account at least evictUnusedMs old is
      // removed first (before the 60-day idle rule); the hourly sweep removes silent accounts older than pruneUnusedMs. A real console whose
      // account goes this way registers again quietly (np_dwc_login / mp_service: a refused token, one new sign-up). 0 = off. Galaxy only.
      evictUnusedMs: envInt(env, 'REGISTER_EVICT_UNUSED_HOURS', game === 'smg' ? 24 : 0, 0, 24 * 365) * 3600_000,
      // svcfix3 (review S4): when full and no silent account is a day old, any silent account this old (minutes) goes (a real console's first
      // sign-in call lands within seconds; 0 = only the 24 h rule), then an account with no friend and no star time unseen this many days (0 = off).
      evictSilentMinMs: envInt(env, 'REGISTER_EVICT_SILENT_MIN_MINUTES', game === 'smg' ? 30 : 0, 0, 1440) * 60_000,
      evictFriendlessMs: envInt(env, 'REGISTER_EVICT_FRIENDLESS_DAYS', game === 'smg' ? 14 : 0, 0, 3650) * 86400_000,
      pruneUnusedMs: envInt(env, 'PRUNE_UNUSED_DAYS', game === 'smg' ? 3 : 0, 0, 3650) * 86400_000,
    },
    // Galaxy (GAME=smg) only; see SMG-PROTOCOL.txt.
    // POST /v1/report (Galaxy beta: a problem report the player sent with consent) and the session summaries (rooms4 leave / close, "summary")
    reports: {
      perPlayerDay: envInt(env, 'REPORT_MAX_PER_PLAYER_DAY', 3, 0, 1000),
      perAddrDay: envInt(env, 'REPORT_MAX_PER_ADDR_DAY', 30, 0, 100_000),
      sharePct: envInt(env, 'REPORT_SHARE_PCT', 2, 0, 100),          // reports stored a day, as a share of DAILY_REQUEST_BUDGET (their own small slice)
      keepMs: envInt(env, 'REPORT_KEEP_DAYS', 30, 1, 365) * 86400_000,
      maxRows: envInt(env, 'REPORT_MAX_ROWS', 2000, 1, 100_000),
      maxBytes: envInt(env, 'REPORT_MAX_MB', 64, 1, 1024) * 1048576,
      summaryRing: envInt(env, 'SUMMARY_RING', 500, 1, 5000),
      // Summaries are D1 writes (about 3 rows written each: the ring insert and its prune, the day's aggregate): a day's total, a player's, and one per player and room keep
      // reports + summaries far under D1's free 100,000 rows written a day (3,000 x ~4 + 900 reports x ~3 = ~15,000).
      summaryMaxPerDay: envInt(env, 'SUMMARY_MAX_PER_DAY', 3000, 0, 1_000_000),
      summaryPerPlayerDay: envInt(env, 'SUMMARY_MAX_PER_PLAYER_DAY', 20, 0, 1000),
      // The leave never waits on D1 longer than this (and a hung write does not block the next one): the summary is dropped, the leave still answers ok.
      summaryTimeoutMs: envInt(env, 'SUMMARY_TIMEOUT_MS', 2500, 20, 10_000),
      d1TimeoutMs: envInt(env, 'REPORT_D1_TIMEOUT_MS', 5000, 20, 20_000),
    },
    smg: {
      maxMembers: 4,
      friendsMax: envInt(env, 'SMG_FRIENDS_MAX', 50, 1, 500),
      blocksMax: envInt(env, 'SMG_BLOCKS_MAX', 200, 1, 2000),
      onlineMs: envInt(env, 'SMG_ONLINE_SEC', 60, 10, 3600) * 1000,
      quickScan: 20,
      publicRoomsPerIp: envInt(env, 'SMG_PUBLIC_ROOMS_PER_IP', 4, 1, 100), // live public rooms one source address may host
      times: {
        floorFrames: envInt(env, 'SMG_TIME_FLOOR_FRAMES', 600, 0, 432000), // 10 s at 60 frames/s, for every star without its own floor
        floors: { ...DEFAULT_STAR_FLOORS, ...parseFloors(env.SMG_STAR_FLOORS) }, // smg_floors.js table; the variable wins per star
        capFrames: envInt(env, 'SMG_TIME_CAP_FRAMES', 2 * 3600 * 60, 600, 10 * 3600 * 60), // 2 h
        builds: list(env.SMG_TIME_BUILDS || '3.0.0'),
        perHour: envInt(env, 'SMG_TIMES_PER_HOUR', 30, 1, 10000),
        perDay: envInt(env, 'SMG_TIMES_PER_DAY', 120, 1, 100000), // per account per 24 h (stored)
        // Boards (service review, finding 13): rows nobody vouched for are seen only by their owner (and on the friends board) unless 'show'
        showUnverified: env.SMG_BOARD_UNVERIFIED === 'show',
        // a verified time needs a second member of the room who submitted the same star from a DIFFERENT address, both accounts at least this old
        verifyMinAgeMs: envInt(env, 'SMG_VERIFY_MIN_AGE_HOURS', 48, 0, 24 * 365) * 3600_000,
        verifyDistinctAddr: env.SMG_VERIFY_DISTINCT_ADDR !== '0',
        flagNewStar: env.SMG_FLAG_NEW_STAR !== '0', // flag the first visible time of a star (time_new_star)
        jumpPct: envInt(env, 'SMG_TIME_JUMP_PCT', 25, 1, 99), // improvement of one player's time within rollingMs (was: per step) that gets flagged
        rollingMs: envInt(env, 'SMG_TIME_ROLLING_HOURS', 24, 1, 48) * 3600_000,
        firstPct: envInt(env, 'SMG_TIME_FIRST_PCT', 15, 1, 99), // a first time more than this % below the star's best is flagged
        topPct: envInt(env, 'SMG_TIME_TOP_PCT', 8, 1, 99), // an unverified new #1 more than this % faster than the old #1 is flagged
        verifySlackFrames: envInt(env, 'SMG_VERIFY_SLACK_SEC', 3, 0, 600) * 60, // a verified time may exceed the room's age by this much
        verifyFrames: envInt(env, 'SMG_VERIFY_FRAMES', 2, 0, 600),
        verifyRaceFrames: envInt(env, 'SMG_VERIFY_RACE_SEC', 60, 1, 3600) * 60, // race: the two members' times may differ by this much (their finish times differ, but not by minutes)
        verifyWindowMs: envInt(env, 'SMG_VERIFY_WINDOW_SEC', 1800, 60, 86400) * 1000,
        keepSubmitsMs: 2 * 86400_000,
      },
    },
    // Requests this service answers per UTC day before it says "busy" to everything but the admin (service review: the free plan's 100,000
    // requests a day are the ACCOUNT's, shared with the owner's other service, which must never be starved). Checked first, counted in memory
    // and stored every 200 requests (a restart loses at most that many). 0 = no budget. Galaxy default 45,000; Strikers off.
    dailyBudget: envInt(env, 'DAILY_REQUEST_BUDGET', game === 'smg' ? 45000 : 0, 0, 1_000_000_000),
    // svcfix2: a second ceiling on the object's own request count (every request that reaches the object, refused or not; budget.js `touch`).
    // Default 125 % of the budget: refused answers are mostly served by the Worker from its verdict memory, so they are a small share.
    objectBudget,
    // svc3 (budget.js): the part of the budget kept for sessions already in progress. NEW sessions (sign-up, create, join, Quick Match,
    // friends, times, info, ...) are refused once the day's count reaches budget x (100 - reservePct) / 100; a console that is a member
    // of a live room (its room polls, endpoint / started / leave calls, relay credentials) is served up to the whole budget, so a running
    // game is not cut off by the crowd that arrived after it. 0 = no reserve (one limit for everybody).
    liveReservePct: envInt(env, 'LIVE_RESERVE_PCT', 20, 0, 90),
    // Sign-up (challenge + register: the only routes a NEW account alone uses) has its own share of the NEW-session limit, in requests
    // (a sign-up is 2 requests, more with a retried proof of work). Past it sign-up gets the normal 503 busy with retry-after, while rooms, joins and
    // Quick Match keep working; counted apart in the admin stats. 0 = no separate share. Galaxy default 20 %.
    signupSharePct: envInt(env, 'SIGNUP_SHARE_PCT', game === 'smg' ? 20 : 0, 0, 100),
    // sec34: keep the per-address / per-account daily counters in storage (table addr_day, budget.js). 0 = memory only (the old behaviour); a switch in case the row writes ever matter.
    persistCounters: envInt(env, 'PERSIST_ADDR_COUNTERS', 1, 0, 1) === 1,
    // svcfix2: what ONE live room / ONE address may take from the reserve (the part of the budget above the new-session limit) per UTC day.
    // A real room costs about 300 requests an hour (4 members at ~60 an hour, see README "Request budget"); 900 is a 3-hour game.
    reserve: {
      roomMax: envInt(env, 'RESERVE_ROOM_MAX', game === 'smg' ? 900 : 0, 0, 10_000_000),
      addrMax: envInt(env, 'RESERVE_ADDR_MAX', game === 'smg' ? 1800 : 0, 0, 10_000_000), // two rooms behind one NAT (a venue)
    },
    // svc3: the most the service asks the clients to stretch their room polls (pollScale in the room answers), in percent. The clients'
    // liveness timings (a member is "quiet" after 12 s, lost after 20 s) are why it cannot be more: 140 % is a 11 s hold + 5.6 s gap.
    paceMax: envInt(env, 'POLL_PACE_MAX_PCT', 140, 100, 140) / 100,
    // svc3: per-address and per-account ceilings per UTC day, held in the object's memory (the object is a singleton, so the count is exact
    // while it lives; an eviction resets it, which errs on the side of allowing a little more). Scaled by RATE_LIMIT_SCALE like the
    // other limits. 0 = off. Galaxy only by default. A request refused here is counted apart (budget.rejected) and does NOT use up the
    // day's budget: an abuser can no longer lock the honest players out. Numbers: honest use is about 354 requests per player-hour.
    abuse: {
      openPerDay: envInt(env, 'ADDR_DAILY_OPEN', game === 'smg' ? 1000 : 0, 0, 100_000_000),      // no token: info, servers, time, challenge, register, junk
      playerPerDay: envInt(env, 'ADDR_DAILY_PLAYER', game === 'smg' ? 3000 : 0, 0, 100_000_000),  // with a token, not a live room's traffic
      livePerDay: envInt(env, 'ADDR_DAILY_LIVE', game === 'smg' ? 10000 : 0, 0, 100_000_000),     // room traffic of live-room members (about 160 player-hours)
      pidPerDay: envInt(env, 'PID_DAILY_MAX', game === 'smg' ? 4000 : 0, 0, 100_000_000),         // one account, everything (about 65 player-hours)
      badPerDay: envInt(env, 'ADDR_BAD_DAY', game === 'smg' ? 2000 : 0, 0, 100_000_000),            // per address, requests that FAIL authentication (a lost token is rare: one re-registration)
    },
    adminToken: String(env.ADMIN_TOKEN || ''), // secret
    adminEnabled: adminTokenOk(env.ADMIN_TOKEN), // S9: a token shorter than ADMIN_TOKEN_MIN leaves the admin routes off
  };
}

