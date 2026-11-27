// The service's own request accounting (svc3). Pure logic over a tiny storage handle, unit tested in node.
//
// What a Worker CAN and CANNOT do on the free plan (see README.md "Quota defence"): a request that reaches the Worker has already been
// counted by Cloudflare against the account's 100,000 a day, whatever we answer. Nothing here can give those back. What this module does:
//   1. keeps the day's count of requests we ADMITTED apart from the ones we REFUSED for abuse, so a flood from one address no longer
//      spends the budget that honest players need (the old count included every request, even the ones turned away);
//   2. per-address and per-account daily ceilings (exact, in the singleton object's memory) with a verdict the Worker remembers, so
//      the next requests of that address are answered by the Worker without waking the object (no Durable Object request);
//   3. a reserve: new sessions stop at (100 - reservePct) % of the budget, members of a live room go on to the whole budget;
//   4. `pace`: 1.0 .. paceMax, rising from half of the new-session budget, sent to the clients with every room answer so their
//      polls stretch before the cliff instead of after it.
// The day is the UTC day, like Cloudflare's own counter. A rolling window was considered and rejected: the platform's quota resets
// at 00:00 UTC, so a rolling 24 h count would keep refusing players for hours after the real quota is fresh again.

import { ApiError } from './util.js';

export const CLASS_OPEN = 'open';
export const CLASS_PLAYER = 'player';
export const CLASS_LIVE = 'live';
/** Room routes (and the relay credentials) a console in a running session depends on: what the reserve is for. Shared with the Worker (edge.js). */
export const LIVE_PATTERNS = new Set(['/v1/rooms/:code', '/v1/rooms/:code/endpoint', '/v1/rooms/:code/punch', '/v1/rooms/:code/started', '/v1/rooms/:code/leave', '/v1/turn', '/v1/turn/usage']);
/** Routes that are live traffic only for some callers (a dropped player rejoining a started game; a time posted from inside a game): the object decides, the Worker lets them through. */
export const MAYBE_LIVE = new Set(['/v1/rooms/join', '/v1/times', '/v1/invites/resolve']);
/** cf-connecting-ip the Worker puts on its own look at the switches (worker.js refreshKill): never counted against an address. */
export const INTERNAL_ADDRESS = 'worker-internal';
const MAX_KEYS = 20000;
/**
 * sec34: the per-key daily counters are also kept in storage (table addr_day, one row per day + class + key, WITHOUT ROWID so a write is
 * one row), so that an eviction of the Durable Object does not hand every address a fresh allowance. Cost model (fix34):
 *  - READ: a key is read by one point read the first time it is counted after a wake (cached for the life of the object); nothing is bulk-loaded.
 *  - WRITE: at every multiple of FLUSH_STEP (about 1 row per 20 counted requests of a key), when the count reaches its cap (so a cap is never lost),
 *    and on the first count after a wake for the classes that are unauthenticated or abusive (FIRST_WRITE: open, bad, player). The classes that
 *    honest polling traffic touches (live, pid, resRoom, resIp) are step-only: an object evicted between polls writes nothing for them.
 *  A step-only count can lose up to FLUSH_STEP - 1 requests of a key when the object is evicted (an undercount, never an overcount).
 *  The key is whatever the caller passes (app.js passes a keyed hash, never an address). The stored count only ever grows (MAX on upsert).
 */
export const FLUSH_STEP = 20;
const FIRST_WRITE = new Set(['open', 'bad', 'player']);
const KEY = 'budget_day';
/** A registration writes about 4-5 rows (profile, token, name index, counters); the worst case of the sign-up share is taken at 5 each. */
export const ROWS_PER_REGISTER = 5;
/** The free plan's SQLite rows written per day, for the whole account. */
export const ROWS_DAILY_CAP = 100000;
/** The longest wait the service ever asks of a player for a budget refusal (seconds). */
export const RETRY_CAP_SEC = 900;

function parse(value, day) {
  // day:n:ra:rb:ob[:su:rs[:rw]] (the later fields, the sign-up share's counts and the rows-written estimate, are absent in values stored by older builds)
  const [d, n, ra, rb, ob, su, rs, rw] = String(value || '').split(':').map(Number);
  if (d !== day) return { n: 0, ra: 0, rb: 0, ob: 0, su: 0, rs: 0, rw: 0 };
  return { n: n || 0, ra: ra || 0, rb: rb || 0, ob: ob || 0, su: su || 0, rs: rs || 0, rw: rw || 0 };
}

/**
 * @param {{get: (q: string, ...b: any[]) => any, run: (q: string, ...b: any[]) => void}} db
 * @param {{dailyBudget: number, liveReservePct: number, paceMax?: number, abuse: {openPerDay: number, playerPerDay: number, livePerDay: number, pidPerDay: number, badPerDay?: number}}} cfg
 * @param {number} rateScale RATE_LIMIT_SCALE (the ceilings are scaled like every other limit; tests set it high)
 * @param {{dayIndex: (now: number) => number, secondsToMidnight: (now: number) => number}} cal
 */
export function createBudget(db, cfg, rateScale, cal) {
  // n admitted, ra refused for abuse, rb refused over budget, ob = EVERY request that reached the object (the platform bills those whatever we answer)
  // su = sign-up requests admitted (a share of n), rs = sign-up requests refused for their share (counted apart from rb)
  // rw = SQLite rows written today (an ESTIMATE, see addRows); rwEst = how many of them are statement-count estimates (not stored)
  const st = { day: -1, n: 0, ra: 0, rb: 0, ob: 0, su: 0, rs: 0, rw: 0, rwEst: 0, stored: 0 };
  /** Sign-up's own share of the new-session limit, in requests (0 = no separate share). */
  const signupLimit = () => (cfg.signupSharePct > 0 ? Math.floor((newLimit() * Math.min(100, cfg.signupSharePct)) / 100) : 0);
  /** @type {Record<'open'|'player'|'live'|'pid'|'bad'|'resRoom'|'resIp', Map<string, number>>} */
  const emptyMaps = () => ({ open: new Map(), player: new Map(), live: new Map(), pid: new Map(), bad: new Map(), resRoom: new Map(), resIp: new Map() });
  let counts = emptyMaps();
  /** count last written to addr_day, `${cls}\0${key}` -> n */
  let flushed = new Map();
  /** keys counted since this object started (their first count is written at once) */
  let touched = new Set();
  const persistent = !!(db.all && cfg.persistCounters !== false);
  const newLimit = () => Math.max(0, Math.floor((cfg.dailyBudget * (100 - cfg.liveReservePct)) / 100));
  const scaled = (v) => (v > 0 ? Math.max(1, Math.ceil(v * rateScale)) : 0);

  function save(now) {
    st.stored = st.ob;
    db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, KEY, `${cal.dayIndex(now)}:${st.n}:${st.ra}:${st.rb}:${st.ob}:${st.su}:${st.rs}:${st.rw}`);
  }
  /** Loads today's stored counts when the day changed (or on the first call after a restart). */
  function roll(now) {
    const day = cal.dayIndex(now);
    if (st.day === day) return;
    const row = db.get(`SELECT value FROM settings WHERE key = ?`, KEY);
    const s = parse(row && row.value, day);
    st.day = day; st.n = s.n; st.ra = s.ra; st.rb = s.rb; st.ob = s.ob; st.su = s.su; st.rs = s.rs; st.rw = s.rw; st.rwEst = 0; st.stored = s.ob;
    counts = emptyMaps();
    flushed = new Map();
    touched = new Set();
    if (persistent && (!row || String(row.value).split(':')[0] !== String(day))) dropOldDays(day); // a new day (first wake of it): earlier days' rows go
  }
  /** Day change: removal of the rows of earlier days (one statement, nothing is read). Nothing else is loaded: a key is read when it is first counted (see `count`). */
  function dropOldDays(day) {
    try { db.run(`DELETE FROM addr_day WHERE day < ?`, day); } catch (e) { /* no table: memory only */ }
  }
  /** One key's count today: from memory, else ONE point read of its row (cached for the life of the object, a miss is cached as 0). */
  function count(cls, key) {
    const map = counts[cls];
    let v = map.get(key);
    if (v !== undefined) return v;
    v = 0;
    if (persistent) {
      try {
        const r = db.get(`SELECT n FROM addr_day WHERE day = ? AND cls = ? AND k = ?`, st.day, cls, key);
        if (r) { v = r.n; flushed.set(`${cls}\0${key}`, v); }
      } catch (e) { /* no table: memory only */ }
    }
    remember(map, cls, key, v);
    return v;
  }
  function remember(map, cls, key, v) {
    if (!map.has(key) && map.size >= MAX_KEYS) {
      for (const [k, c] of map) if (c <= 2) { map.delete(k); flushed.delete(`${cls}\0${k}`); } // one-off visitors go first: the big counts (the offenders) stay
      if (map.size >= MAX_KEYS) { let q = MAX_KEYS >> 2; for (const k of map.keys()) { map.delete(k); flushed.delete(`${cls}\0${k}`); if (--q <= 0) break; } }
    }
    map.set(key, v);
  }
  function flushCounter(cls, key, v) {
    try {
      db.run(`INSERT INTO addr_day (day, cls, k, n) VALUES (?, ?, ?, ?) ON CONFLICT(day, cls, k) DO UPDATE SET n = MAX(n, excluded.n)`, st.day, cls, key, v);
      flushed.set(`${cls}\0${key}`, v);
    } catch (e) { /* no table: memory only */ }
  }
  /** Stored every 200 counted events (a restart loses at most that many). */
  function dirty(now) {
    if (st.ob - st.stored >= 200) save(now);
  }
  /** Counts one request of `key`. Written (see FLUSH_STEP): at every multiple of FLUSH_STEP, when the count reaches `cap`, and on the first count after a wake for the classes in FIRST_WRITE. */
  function bump(cls, key, cap = 0) {
    const v = count(cls, key) + 1;
    counts[cls].set(key, v);
    if (persistent) {
      const id = `${cls}\0${key}`;
      const first = FIRST_WRITE.has(cls) && !touched.has(id);
      if (first) touched.add(id);
      if (first || v % FLUSH_STEP === 0 || (cap > 0 && v === cap)) flushCounter(cls, key, v);
    }
    return v;
  }
  const midnight = (now) => cal.secondsToMidnight(now);

  const api = {
    /** Counted state for the admin page and the tests. */
    status(now) {
      roll(now);
      return {
        day: st.day, used: st.n, limit: cfg.dailyBudget, newSessionLimit: newLimit(), reservePct: cfg.liveReservePct,
        rejectedAddress: st.ra, rejectedBudget: st.rb, objectRequests: st.ob, objectLimit: cfg.objectBudget || 0,
        signup: { used: st.su, limit: signupLimit(), refused: st.rs, sharePct: cfg.signupSharePct || 0, rowsPerRegister: ROWS_PER_REGISTER, worstCaseRows: signupLimit() * ROWS_PER_REGISTER },
        rowsWritten: {
          today: st.rw, accountCap: ROWS_DAILY_CAP,
          source: st.rwEst === 0 ? 'platform (rowsWritten of each statement)' : 'estimate (one row per write statement where the platform number is not available: a LOWER bound, index rows are not counted)',
          note: `SQLite rows written: the free plan allows ${ROWS_DAILY_CAP} a day for the WHOLE account (shared with the Strikers service); this is Galaxy's own estimate, the dashboard has the real number. Worst case of the sign-up share = its request limit x ${ROWS_PER_REGISTER} rows (every request taken as a registration).`,
        }, pace: api.pace(),
        topAddresses: ['open', 'player', 'live'].flatMap((cls) => [...counts[cls]].map(([, v]) => ({ cls, count: v }))).sort((a, b) => b.count - a.count).slice(0, 5),
      };
    },
    used: () => st.n,
    /** Every request that reaches the object, admitted or refused, is one Durable Object request on the platform's bill: counted here first of all. */
    touch(now) {
      roll(now);
      st.ob++;
      dirty(now);
    },
    objects: () => st.ob,
    /** SQLite rows written by one statement: the platform's own `rowsWritten` when the cursor has it (it includes index rows), else `estimated` = true and n = 1. */
    addRows(now, n, estimated = false) {
      roll(now);
      st.rw += n;
      if (estimated) st.rwEst += n;
    },

    /**
     * Per-address daily ceiling of one class. Returns an ApiError (429, with `x-msc-block: <class>:<seconds>` for the Worker) or null.
     * A refused request is counted in `rejectedAddress` only.
     */
    gateAddress(cls, ipk, now) {
      const cap = scaled(cls === CLASS_OPEN ? cfg.abuse.openPerDay : cls === CLASS_LIVE ? cfg.abuse.livePerDay : cfg.abuse.playerPerDay);
      if (!cap || ipk === INTERNAL_ADDRESS) return null;
      roll(now);
      if (count(cls, ipk) >= cap) {
        st.ra++; dirty(now);
        const wait = midnight(now);
        return new ApiError(429, 'rate_limited', 'too many requests from this address today', { retryAfterSec: wait, reason: 'daily_cap', scope: 'address' },
          { 'retry-after': String(wait), 'x-msc-block': `${cls}:${wait}` });
      }
      bump(cls, ipk, cap);
      return null;
    },

    /**
     * A request from `ipk` failed authentication (wrong, missing or unknown token). Over `abuse.badPerDay` a day the address gets 429 daily_cap with
     * `x-msc-block: bad:<seconds>` until midnight: the Worker then refuses that address's requests unless their token has been seen to succeed
     * (a NAT's honest consoles keep working; worker.js). Strikers d330d612 (failed-authentication ceiling). Returns an ApiError or null.
     */
    gateBadAuth(ipk, now) {
      const cap = scaled(cfg.abuse.badPerDay);
      if (!cap || ipk === INTERNAL_ADDRESS) return null;
      roll(now);
      if (bump('bad', ipk, cap + 1) > cap) {
        st.ra++; dirty(now);
        const wait = midnight(now);
        return new ApiError(429, 'rate_limited', 'too many failed sign-ins from this address today', { retryAfterSec: wait, reason: 'daily_cap', scope: 'address' },
          { 'retry-after': String(wait), 'x-msc-block': `bad:${wait}` });
      }
      return null;
    },

    /** Per-account daily ceiling (everything the account asks). */
    gatePid(pid, now) {
      const cap = scaled(cfg.abuse.pidPerDay);
      if (!cap) return null;
      roll(now);
      const key = String(pid);
      if (count('pid', key) >= cap) {
        st.ra++; dirty(now);
        const wait = midnight(now);
        return new ApiError(429, 'rate_limited', 'daily limit reached, try again tomorrow (UTC)', { retryAfterSec: wait, reason: 'daily_cap', scope: 'account' }, { 'retry-after': String(wait), 'x-msc-block': `pid:${wait}` });
      }
      bump('pid', key, cap);
      return null;
    },

    /**
     * The day's budget. `live`: the request belongs to a member of a live room (served up to the whole budget); otherwise only up to the
     * new-session limit. `who` = { leave, room, ipk }: a leave / close request FREES resources and is never refused by the budget (it is
     * still counted); inside the reserve (new sessions already refused) a live room may use only what a real room needs, and one address
     * only what a few real rooms need (cfg.reserve): a pile of fake "started" rooms cannot drain the part of the budget that honest games
     * in progress depend on. Admitted requests are counted; refused ones are counted apart. Returns an ApiError (503 over_budget) or null.
     * Every wait is capped at RETRY_CAP_SEC: a player can act on "try again in a quarter of an hour", not on "17 hours".
     */
    admit(now, live, who = {}) {
      if (!cfg.dailyBudget) return null;
      roll(now);
      if (who.leave) { st.n++; dirty(now); return null; }
      // Sign-up (who.signup) has its own share of the new-session limit: past it a sign-up gets the normal busy answer while rooms, joins and
      // Quick Match go on. No x-msc-budget header: the Worker's verdict memory must not turn this into "all new sessions are over".
      if (who.signup && !live && cfg.signupSharePct > 0 && st.su >= signupLimit()) {
        st.rs++; dirty(now);
        const w = Math.min(RETRY_CAP_SEC, Math.max(60, midnight(now)));
        return new ApiError(503, 'over_budget', 'online play is busy, please try again later', { retryAfterSec: w, reason: 'signup_share' }, { 'retry-after': String(w) });
      }
      const limit = live ? cfg.dailyBudget : newLimit();
      // the object's own request count (admitted + refused answers) is a second ceiling: refused answers are cheap but not free
      const objectsSpent = cfg.objectBudget > 0 && st.ob > cfg.objectBudget;
      const wait = Math.min(RETRY_CAP_SEC, Math.max(60, midnight(now)));
      if (st.n >= limit || objectsSpent) {
        st.rb++; dirty(now);
        return new ApiError(503, 'over_budget', 'online play is busy, please try again later', { retryAfterSec: wait, reason: 'busy' },
          { 'retry-after': String(wait), 'x-msc-budget': live || objectsSpent || cfg.liveReservePct === 0 ? 'over' : 'new-over' });
      }
      if (live && cfg.liveReservePct > 0 && st.n >= newLimit() && who.room) {
        const rcap = scaled(cfg.reserve.roomMax), icap = scaled(cfg.reserve.addrMax);
        const inRoom = count('resRoom', who.room), inAddr = who.ipk ? count('resIp', who.ipk) : 0;
        const why = rcap && inRoom >= rcap ? 'room_share' : icap && inAddr >= icap ? 'address_share' : null;
        if (why) {
          st.rb++; dirty(now);
          return new ApiError(503, 'over_budget', 'online play is busy, please try again later', { retryAfterSec: wait, reason: why },
            { 'retry-after': String(wait), 'x-msc-block': `reserve:${wait}` });
        }
        bump('resRoom', who.room, rcap);
        if (who.ipk) bump('resIp', who.ipk, icap);
      }
      st.n++;
      if (who.signup) st.su++;
      dirty(now);
      return null;
    },

    /** Poll pacing for the clients: 1 up to half of the new-session budget, then rising to paceMax (a hint; liveness limits cap it). */
    pace() {
      const max = cfg.paceMax || 1;
      const nl = newLimit() || cfg.dailyBudget;
      if (!cfg.dailyBudget || max <= 1 || !nl) return 1;
      const used = st.n / nl;
      if (used <= 0.5) return 1;
      return Math.round((1 + (max - 1) * Math.min(1, (used - 0.5) / 0.5)) * 100) / 100;
    },
  };
  return api;
}
