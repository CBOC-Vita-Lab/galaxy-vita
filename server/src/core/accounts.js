// Sign-in (registration), profile, rename, delete.

import { ApiError, b64url, randomBytes, randomInt, sha256hex, isInt, strict } from './util.js';
import { CODE_ALPHABET, normalizeCode } from './rooms.js';
import { validateName } from './names.js';
import { dropPlayer } from './rooms4.js';
import { dayIndex, secondsToMidnight } from './limits.js';
import { queuePurge } from './reports.js';

const REGIONS = [1, 2, 4, 8]; // JP, US, EU, KR (DWC_RNK_REGION_*)

export function checkRegion(v, dflt = 2) {
  if (v === undefined || v === null) return dflt;
  if (!REGIONS.includes(v)) throw new ApiError(400, 'bad_region', 'region must be 1 (JP), 2 (US), 4 (EU) or 8 (KR)');
  return v;
}

/** Short public identity code, shown as XXXX-XXXX. Safe to share (it grants nothing), unlike the token. */
function newPublicCode(c) {
  for (let i = 0; i < 30; i++) {
    let s = '';
    for (let j = 0; j < 8; j++) s += CODE_ALPHABET[randomInt(CODE_ALPHABET.length)];
    if (!c.db.get(`SELECT 1 AS x FROM profiles WHERE code = ?`, s)) return s;
  }
  throw new ApiError(503, 'busy', 'could not allocate a code, try again');
}

export const fmtCode = (s) => `${s.slice(0, 4)}-${s.slice(4)}`;

function checkRelayOnly(v) {
  if (v === undefined) return undefined;
  if (typeof v !== 'boolean') throw new ApiError(400, 'bad_request', 'relayOnly must be true or false', { field: 'relayOnly' });
  return v ? 1 : 0;
}

export function publicProfile(p) {
  return {
    pid: p.pid,
    code: fmtCode(p.code),
    name: p.name,
    region: p.region,
    rating: p.rating,
    wins: p.wins,
    losses: p.losses,
    draws: p.draws,
    abandons: p.abandons,
    createdMs: p.created_ms,
  };
}

/**
 * Everything about a registration request that can be decided without writing: field allowlist, name rules, region,
 * relayOnly, and whether the name is already taken. Runs BEFORE any rate-limit bucket that counts successful
 * registrations is charged, so junk requests cannot use up the global daily budget.
 */
export function checkRegisterBody(c, body) {
  strict(body, ['name', 'region', 'relayOnly', 'pow']);
  const relayOnly = checkRelayOnly(body.relayOnly) || 0;
  const { name, key } = validateName(body.name, { extraBlock: c.cfg.nameBlockExtra });
  const region = checkRegion(body.region);
  if (c.cfg.register.uniqueNames && c.db.get(`SELECT 1 AS x FROM profiles WHERE name_key = ?`, key)) {
    throw new ApiError(409, 'name_taken', 'that name is already in use', { reason: 'taken' });
  }
  return { name, key, region, relayOnly };
}

// Galaxy (display names): profiles.name_key is UNIQUE in the schema (Strikers' rule), so a Galaxy row stores the folded name with its
// own unique friend code after it. Nothing looks a player up by name_key in GAME=smg.
const storedKey = (c, key, code) => (c.cfg.register.uniqueNames ? key : `${key}#${code}`);

const DAY_KEY = 'register_day';

/** The daily sign-up cap, tightened as the table fills (fill.js): half from 50 %, a quarter from 80 %. */
function dailyCap(c) {
  const lvl = c.fill ? c.fill.level(c.now) : 0;
  const max = c.cfg.register.maxPerDay;
  return lvl === 2 ? Math.max(1, Math.floor(max / 4)) : lvl === 1 ? Math.max(1, Math.floor(max / 2)) : max;
}

// A silent account: signed up and never used (no call more than 5 minutes after sign-up: last_seen_ms stays within that of created_ms), no
// friend row, no star time, not banned. SQL fragment shared by the eviction when full and the hourly sweep.
const SILENT = `pid <> 100000 AND banned = 0 AND last_seen_ms <= created_ms + 300000
  AND NOT EXISTS (SELECT 1 FROM friends f WHERE f.pid = profiles.pid OR f.friend_pid = profiles.pid)
  AND NOT EXISTS (SELECT 1 FROM star_times t WHERE t.pid = profiles.pid)`;

const FRIENDLESS = `pid <> 100000 AND banned = 0
  AND NOT EXISTS (SELECT 1 FROM friends f WHERE f.pid = profiles.pid OR f.friend_pid = profiles.pid)
  AND NOT EXISTS (SELECT 1 FROM star_times t WHERE t.pid = profiles.pid)`;

/** The account a full service removes for a new sign-up, or 0 (see register). */
function pickVictim(c, rg) {
  const one = (where, ...args) => c.db.get(`SELECT pid FROM profiles WHERE ${where} ORDER BY last_seen_ms ASC LIMIT 1`, ...args)?.pid || 0;
  let pid = 0;
  if (rg.evictUnusedMs) pid = one(`${SILENT} AND last_seen_ms < ?`, c.now - rg.evictUnusedMs);
  if (!pid && rg.evictUnusedMs && rg.evictSilentMinMs) pid = one(`${SILENT} AND created_ms < ?`, c.now - rg.evictSilentMinMs);
  if (!pid && rg.evictFriendlessMs) pid = one(`${FRIENDLESS} AND last_seen_ms < ?`, c.now - rg.evictFriendlessMs);
  if (!pid && rg.evictIdleMs) pid = one(`pid <> 100000 AND banned = 0 AND last_seen_ms < ?`, c.now - rg.evictIdleMs);
  return pid;
}

/** Removes silent accounts unseen since before `olderThanMs` (at most `limit`, oldest first); returns how many. */
export function pruneUnused(c, olderThanMs, limit = 200) {
  const rows = c.db.all(`SELECT pid FROM profiles WHERE ${SILENT} AND last_seen_ms < ? ORDER BY last_seen_ms ASC LIMIT ?`, c.now - olderThanMs, limit);
  for (const { pid } of rows) purgeAccount(c, pid);
  return rows.length;
}

/** Galaxy: registrations so far today (UTC), from storage. */
function registeredToday(c) {
  const r = c.db.get(`SELECT value FROM settings WHERE key = ?`, DAY_KEY);
  if (!r) return 0;
  const [day, n] = String(r.value).split(':').map(Number);
  return day === dayIndex(c.now) ? n || 0 : 0;
}

function countRegistration(c) {
  const v = `${dayIndex(c.now)}:${registeredToday(c) + 1}`;
  c.db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, DAY_KEY, v);
}

/** Everything an account owns, then the account (account deletion, expiry, eviction). */
export function purgeAccount(c, pid) {
  if (c.fill) c.fill.invalidate();
  for (const t of ['scores', 'season_scores', 'allowance', 'queue', 'daily', 'flags', 'turn_mints', 'star_times', 'time_submits']) {
    c.db.run(`DELETE FROM ${t} WHERE pid = ?`, pid);
  }
  dropPlayer(c, pid);
  // svcfix3 (account deletion from the game): everything else that names the account. What stays is only what the abuse limits need: the
  // per-day counters keyed by the (never reused) pid, the day's sign-up count and the per-address buckets, none of which holds a name or a
  // code; and the closed room rows (host_pid, closed_reason 'account_deleted'), which the sweep removes within minutes.
  c.db.run(`DELETE FROM room_left WHERE pid = ?`, pid);
  c.db.run(`DELETE FROM invites WHERE pid = ?`, pid);
  c.db.run(`DELETE FROM results WHERE pid = ?`, pid);
  c.db.run(`DELETE FROM pair_stats WHERE lo_pid = ? OR hi_pid = ?`, pid, pid);
  c.db.run(`DELETE FROM pair_daily WHERE lo_pid = ? OR hi_pid = ?`, pid, pid);
  if (c.online) c.online.delete(pid);
  c.db.run(`DELETE FROM friends WHERE pid = ? OR friend_pid = ?`, pid, pid);
  c.db.run(`DELETE FROM friend_blocks WHERE pid = ? OR blocked_pid = ?`, pid, pid);
  c.db.run(`DELETE FROM board_bans WHERE pid = ?`, pid);
  queuePurge(c, pid); // the problem reports the player sent (Galaxy beta, in D1): deleted from D1 right after (reports.drainPurges)
  c.db.run(
    `UPDATE rooms SET state = 'closed', closed_reason = 'account_deleted', version = version + 1
     WHERE (host_pid = ? OR join_pid = ?) AND state NOT IN ('closed','expired')`,
    pid, pid,
  );
  c.db.run(`DELETE FROM profiles WHERE pid = ?`, pid);
}

export async function register(c, body) {
  const { name, key, region, relayOnly } = checkRegisterBody(c, body);
  const token = b64url(randomBytes(32));
  const hash = await sha256hex(token);
  const rg = c.cfg.register;
  const row = c.tx(() => {
    if (rg.storedDaily && registeredToday(c) >= dailyCap(c)) {
      const wait = secondsToMidnight(c.now);
      throw new ApiError(429, 'rate_limited', 'no more new accounts today', { retryAfterSec: wait, reason: 'register_daily' }, { 'retry-after': String(wait) });
    }
    if ((c.fill ? c.fill.count(c.now, true) : c.db.get(`SELECT COUNT(*) AS n FROM profiles WHERE pid <> 100000`).n) >= c.cfg.maxPlayers) {
      // svcfix3 (review S4): a new player must always find room while the table is held by never-used or idle accounts. In order:
      //  1. a silent account (see SILENT) at least evictUnusedMs old, oldest first (svc3);
      //  2. any silent account at least evictSilentMinMs old: junk made in the last day must not close sign-up for a day;
      //  3. an account with no friend and no star time unseen for evictFriendlessMs (14 days);
      //  4. as before, the account unseen longest when it has been unseen for evictIdleMs (60 days). Never a banned one (a ban must stay).
      // An account with a friend, a star time or a call after its first 5 minutes is only ever reached by step 3 or 4, after weeks.
      const victim = pickVictim(c, rg);
      if (!victim) throw new ApiError(503, 'server_full', 'registration is closed for now', { retryAfterSec: 3600 });
      purgeAccount(c, victim);
    }
    if (rg.uniqueNames && c.db.get(`SELECT 1 AS x FROM profiles WHERE name_key = ?`, key)) {
      throw new ApiError(409, 'name_taken', 'that name is already in use', { reason: 'taken' });
    }
    const code = newPublicCode(c);
    const r = c.db.get(
      `INSERT INTO profiles (code, name, name_key, token_hash, created_ms, last_seen_ms, name_changed_ms, region, relay_only)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING pid, code`,
      code, name, storedKey(c, key, code), hash, c.now, c.now, c.now, region, relayOnly,
    );
    if (rg.storedDaily) countRegistration(c);
    if (c.fill) c.fill.added();
    return r;
  });
  return { pid: row.pid, code: fmtCode(row.code), token, name, region, relayOnly: !!relayOnly, createdMs: c.now };
}

export function me(c, p) {
  return {
    ...publicProfile(p),
    relayOnly: !!p.relay_only,
    nameChangeAllowedMs: Math.max(c.now, p.name_changed_ms + c.cfg.renameCooldownMs),
    cooldownUntilMs: p.cooldown_until_ms > c.now ? p.cooldown_until_ms : 0,
  };
}

export function rename(c, p, body) {
  strict(body, ['name', 'region', 'relayOnly']);
  const relayOnly = checkRelayOnly(body.relayOnly);
  const { name, key } = validateName(body.name === undefined ? p.name : body.name, { extraBlock: c.cfg.nameBlockExtra });
  const region = body.region === undefined ? p.region : checkRegion(body.region);
  return c.tx(() => {
    if (relayOnly !== undefined) c.db.run(`UPDATE profiles SET relay_only = ? WHERE pid = ?`, relayOnly, p.pid);
    if (name !== p.name || region !== p.region) {
      if (name !== p.name && p.name_changed_ms + c.cfg.renameCooldownMs > c.now) {
        throw new ApiError(429, 'rename_too_soon', 'name was changed recently', {
          retryAfterSec: Math.ceil((p.name_changed_ms + c.cfg.renameCooldownMs - c.now) / 1000),
        });
      }
      if (c.cfg.register.uniqueNames) {
        const other = c.db.get(`SELECT pid FROM profiles WHERE name_key = ? AND pid <> ?`, key, p.pid);
        if (other) throw new ApiError(409, 'name_taken', 'that name is already in use', { reason: 'taken' });
      }
      c.db.run(
        `UPDATE profiles SET name = ?, name_key = ?, region = ?, name_changed_ms = ? WHERE pid = ?`,
        name, storedKey(c, key, p.code), region, name !== p.name ? c.now : p.name_changed_ms, p.pid,
      );
    }
    return me(c, c.db.get(`SELECT * FROM profiles WHERE pid = ?`, p.pid));
  });
}

/** Look a player up by their public code (friend lists, "play with"). Public fields only. */
export function byCode(c, rawCode) {
  const code = normalizeCode(String(rawCode).replace(/-/g, ''), 8);
  const p = c.db.get(`SELECT pid, code, name, region FROM profiles WHERE code = ?`, code);
  if (!p) throw new ApiError(404, 'player_not_found', 'no such player');
  return { pid: p.pid, code: fmtCode(p.code), name: p.name, region: p.region };
}

/** Delete the account (DELETE /v1/me, the player's own delete from the game): purgeAccount removes every row that names it; see there for what is kept. */
export function deleteAccount(c, p) {
  c.tx(() => purgeAccount(c, p.pid));
  c.clearBoard();
  return { ok: true };
}

/**
 * Housekeeping (alarm): accounts that never played and were not seen for PRUNE_IDLE_DAYS (180) go. "Never played" is strict: no match ever
 * started (matches_played, which outlives the 90-day match records), no win, loss, draw, abandon, verified point or ranked date, no score row
 * and no archived season row. Anyone who played is never removed, however long they are away; nothing else is touched. Without it every registration stayed
 * forever: MAX_PLAYERS fills up at the REGISTER_MAX_PER_DAY pace (Strikers: 200,000 at 1,000 a day; Galaxy: 5,000 at 100 a day), after which nobody can sign up (`server_full`),
 * and each such account holds its name. Only accounts with no match record, no score and no ban are touched (a ban must stay), at most
 * `limit` per sweep. A console that still holds the token of a removed account is refused once and registers again with its stored
 * name (np_dwc_login.cpp), so a real player who comes back loses nothing that existed.
 */
export function pruneIdle(c, limit = 200) {
  let expired = 0;
  if (c.cfg.expireIdleMs) {
    // Galaxy: any account unseen for ACCOUNT_EXPIRE_DAYS (played or not; never a banned one), with all it owns
    const old = c.db.all(`SELECT pid FROM profiles WHERE pid <> 100000 AND banned = 0 AND last_seen_ms < ? LIMIT ?`, c.now - c.cfg.expireIdleMs, limit);
    for (const { pid } of old) purgeAccount(c, pid);
    expired = old.length;
    if (expired) c.clearBoard();
  }
  if (c.cfg.register.pruneUnusedMs) expired += pruneUnused(c, c.cfg.register.pruneUnusedMs, limit); // svc3: silent accounts (sign-up farming)
  if (!c.cfg.pruneIdleMs) return expired;
  const rows = c.db.all(
    `SELECT pid FROM profiles p WHERE banned = 0 AND last_seen_ms < ? AND matches_played = 0
       AND wins = 0 AND losses = 0 AND draws = 0 AND abandons = 0 AND points_total = 0 AND last_played_ms = 0
       AND NOT EXISTS (SELECT 1 FROM season_scores a WHERE a.pid = p.pid)
       AND NOT EXISTS (SELECT 1 FROM matches m WHERE m.p0 = p.pid OR m.p1 = p.pid)
       AND NOT EXISTS (SELECT 1 FROM scores s WHERE s.pid = p.pid)
       AND NOT EXISTS (SELECT 1 FROM star_times t WHERE t.pid = p.pid)
       AND NOT EXISTS (SELECT 1 FROM friends f WHERE f.pid = p.pid OR f.friend_pid = p.pid)
     LIMIT ?`,
    c.now - c.cfg.pruneIdleMs, limit,
  );
  for (const { pid } of rows) {
    for (const t of ['allowance', 'queue', 'daily', 'flags', 'turn_mints', 'room_members']) c.db.run(`DELETE FROM ${t} WHERE pid = ?`, pid);
    c.db.run(`DELETE FROM friend_blocks WHERE pid = ? OR blocked_pid = ?`, pid, pid);
    c.db.run(`DELETE FROM profiles WHERE pid = ?`, pid);
  }
  return expired + rows.length;
}

export function checkPid(v) {
  return isInt(v, 1, 2147483647);
}
