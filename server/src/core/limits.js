// Per-account daily counters (UTC day). One row per player in `daily`, overwritten when the day changes, so the table
// never grows past the number of players and each event costs one row written. These counters live in storage, not
// in the in-memory rate limiter, because that one resets whenever the object is evicted.

import { ApiError } from './util.js';
import { dayStart } from './calendar.js';

const FIELDS = ['rooms', 'matches', 'rated', 'turn_reports'];
const DAY_MS = 86400000;

export const dayIndex = (now) => Math.floor(dayStart(now) / DAY_MS);
export const secondsToMidnight = (now) => Math.max(1, Math.ceil((dayStart(now) + DAY_MS - now) / 1000));

/** Today's counters for a player (zeros when the stored row is from an earlier day). */
export function getDaily(c, pid) {
  const r = c.db.get(`SELECT * FROM daily WHERE pid = ?`, pid);
  if (!r || r.day !== dayIndex(c.now)) return { rooms: 0, matches: 0, rated: 0, turn_reports: 0 };
  return r;
}

export function bumpDaily(c, pid, field, by = 1) {
  if (!FIELDS.includes(field)) throw new Error(`bad daily field ${field}`);
  const day = dayIndex(c.now);
  const reset = FIELDS.filter((f) => f !== field).map((f) => `${f} = CASE WHEN day = excluded.day THEN ${f} ELSE 0 END`).join(', ');
  c.db.run(
    `INSERT INTO daily (pid, day, ${field}) VALUES (?, ?, ?)
     ON CONFLICT(pid) DO UPDATE SET ${field} = CASE WHEN day = excluded.day THEN ${field} + excluded.${field} ELSE excluded.${field} END,
       ${reset}, day = excluded.day`,
    pid, day, by,
  );
}

/**
 * Throws 429 `rate_limited` (a code every client already handles) with reason `daily_cap` when the player has used
 * `cap` of `field` today. `cap` 0 means no cap.
 */
export function requireDaily(c, pid, field, cap, scope) {
  if (!cap) return;
  if (getDaily(c, pid)[field] >= cap) throw dailyCapError(c, scope);
}

export function dailyCapError(c, scope) {
  const wait = secondsToMidnight(c.now);
  return new ApiError(429, 'rate_limited', 'daily limit reached, try again tomorrow (UTC)', { retryAfterSec: wait, reason: 'daily_cap', scope }, { 'retry-after': String(wait) });
}

export function atDailyCap(c, pid, field, cap) {
  return !!cap && getDaily(c, pid)[field] >= cap;
}

/** Ranked results settled as rated today between a pair of players. */
export function pairRatedToday(c, a, b) {
  const [lo, hi] = a < b ? [a, b] : [b, a];
  const r = c.db.get(`SELECT day, rated FROM pair_daily WHERE lo_pid = ? AND hi_pid = ?`, lo, hi);
  return r && r.day === dayIndex(c.now) ? r.rated : 0;
}

export function bumpPairRated(c, a, b) {
  const [lo, hi] = a < b ? [a, b] : [b, a];
  const day = dayIndex(c.now);
  c.db.run(
    `INSERT INTO pair_daily (lo_pid, hi_pid, day, rated) VALUES (?, ?, ?, 1)
     ON CONFLICT(lo_pid, hi_pid) DO UPDATE SET rated = CASE WHEN day = excluded.day THEN rated + 1 ELSE 1 END, day = excluded.day`,
    lo, hi, day,
  );
}
