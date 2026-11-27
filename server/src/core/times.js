// Galaxy (GAME=smg) star times: the fastest time per player, galaxy, star and category (category = players in the run, 1..4).
//
// Sanity (every submission): frames between the star's floor (SMG_STAR_FLOORS, else SMG_TIME_FLOOR_FRAMES = 600 = 10 s) and the cap
// (2 h), splits strictly increasing and not past the time, build in the allowlist (SMG_TIME_BUILDS), at most SMG_TIMES_PER_HOUR (30)
// submissions per player per hour (stored, so an evicted object does not reset it). The galaxy must exist (0..41, the Observatory list).
// A time that looks too good is KEPT but FLAGGED (flags table, admin review queue; a flagged time is hidden from other players and from
// the public board until an admin clears the flag, the player still sees his own). Flag kinds:
//   time_jump  the player's total improvement of this star within SMG_TIME_ROLLING_HOURS (24 h) is more than SMG_TIME_JUMP_PCT (25 %)
//              (a rolling window: improving in many small steps does not get around it)
//   time_first the player's first time on this star is more than SMG_TIME_FIRST_PCT (15 %) faster than the board's best
//   time_top   an unverified time that becomes #1 and is more than SMG_TIME_TOP_PCT (8 %) faster than the old #1
//
// Verified (service review, finding 13: membership alone was enough, and one person with two accounts is two members): a race or co-op
// time is verified only when ANOTHER member of the same started room submitted the same star from that room within SMG_VERIFY_WINDOW_SEC
// (co-op: within +-2 frames, SMG_VERIFY_FRAMES; race: within SMG_VERIFY_RACE_SEC, 60 s, review S6), both times fit in the time the room had been running when each was sent
// (+ SMG_VERIFY_SLACK_SEC), the two members call from different addresses (room_members.src_ip; SMG_VERIFY_DISTINCT_ADDR=0 turns that
// off), and both accounts are at least SMG_VERIFY_MIN_AGE_HOURS (48) old and not banned. The first of the two is upgraded when the second
// arrives. Solo times are never verified. Race and co-op times (players > 1) must come with a room that has started.
// WHAT STAYS FORGEABLE: every time is measured by the client. Two consoles (or one console and a script) on two networks, with two
// accounts registered two days earlier, can still agree on any time above the floor. The checks raise the price (two addresses, aged
// accounts, a room that really ran that long, a second submission) and the board hides what nobody vouched for; they cannot prove a run.
// Boards: unverified rows are seen only by their owner (and by mutual friends on the friends board) unless SMG_BOARD_UNVERIFIED=show;
// an admin can approve one (verified = 2), remove times, or keep an account off the boards (board_bans: its times are refused).
// Flags: also time_new_star, the first visible time of a star (a forger's favourite: an empty board has nothing to compare against).

import { ApiError, strict, isInt } from './util.js';
import { normalizeCode } from './rooms.js';
import { members } from './rooms4.js';
import { GALAXIES } from './smg_floors.js';

export const MAX_GALAXY = GALAXIES.length - 1; // 41: only 42 galaxies exist

export const MODES = ['top', 'near', 'friends'];
const DAY = 86400_000;
const HEX32 = /^[0-9a-fA-F]{32}$/;
const BUILD_RE = /^[A-Za-z0-9._+-]{1,32}$/;
const HOUR = 3600_000;
/** S3: the most rows one count over a board reads (a board larger than this reports `capped`, and a rank past it is a floor). */
export const SCAN_CAP = 1000;
/** S3: a `times/get` answer is reused this long (per galaxy, star, category, mode, limit and viewer class). */
export const GET_MEMO_MS = 20_000;

function checkStar(body) {
  if (!isInt(body.galaxy, 0, MAX_GALAXY)) throw new ApiError(400, 'bad_param', `galaxy must be 0..${MAX_GALAXY}`, { field: 'galaxy' });
  if (!isInt(body.star, 1, 8)) throw new ApiError(400, 'bad_param', 'star must be 1..8', { field: 'star' });
}

/** POST /v1/times */
export function submitTime(c, p, body) {
  strict(body, ['galaxy', 'star', 'frames', 'players', 'room', 'build', 'splits', 'digest']);
  const t = c.cfg.smg.times;
  checkStar(body);
  if (!isInt(body.players, 1, 4)) throw new ApiError(400, 'bad_param', 'players must be 1..4', { field: 'players' });
  if (!Number.isInteger(body.frames)) throw new ApiError(400, 'bad_time', 'frames must be a whole number', { field: 'frames' });
  if (typeof body.build !== 'string' || !BUILD_RE.test(body.build)) throw new ApiError(400, 'bad_param', 'build is required', { field: 'build' });
  if (!t.builds.includes(body.build)) throw new ApiError(400, 'bad_build', 'times from this build are not accepted', { field: 'build' });
  const floor = t.floors[`${body.galaxy}:${body.star}`] ?? t.floorFrames;
  if (body.frames < floor) throw new ApiError(400, 'bad_time', 'that time is faster than possible', { field: 'frames', reason: 'too_fast' });
  if (body.frames > t.capFrames) throw new ApiError(400, 'bad_time', 'that time is longer than the limit', { field: 'frames', reason: 'too_slow' });
  let splits = null;
  if (body.splits !== undefined) {
    const s = body.splits;
    if (!Array.isArray(s) || s.length > 16 || !s.every((x, i) => isInt(x, 1, body.frames) && (i === 0 || x > s[i - 1]))) {
      throw new ApiError(400, 'bad_splits', 'splits: at most 16 increasing frame counts, none past the time', { field: 'splits' });
    }
    splits = JSON.stringify(s);
  }
  if (body.digest !== undefined && (typeof body.digest !== 'string' || !HEX32.test(body.digest))) {
    throw new ApiError(400, 'bad_param', 'digest must be 32 hex characters', { field: 'digest' });
  }
  const roomCode = body.room === undefined ? null : normalizeCode(body.room);
  const category = body.players;
  const { galaxy, star, frames } = body;
  if (category > 1 && !roomCode) throw new ApiError(400, 'room_required', 'race and co-op times come from a room', { field: 'room' });

  return c.tx(() => {
    if (c.db.get(`SELECT 1 AS x FROM board_bans WHERE pid = ?`, p.pid)) {
      throw new ApiError(403, 'board_banned', 'times from this account are not accepted');
    }
    const n = c.db.get(`SELECT COUNT(*) AS n FROM time_submits WHERE pid = ? AND submitted_ms > ?`, p.pid, c.now - HOUR).n;
    if (n >= t.perHour) {
      throw new ApiError(429, 'rate_limited', 'too many times submitted this hour', { retryAfterSec: 600, reason: 'hourly_cap' }, { 'retry-after': '600' });
    }
    const nd = c.db.get(`SELECT COUNT(*) AS n FROM time_submits WHERE pid = ? AND submitted_ms > ?`, p.pid, c.now - DAY).n;
    if (nd >= t.perDay) {
      throw new ApiError(429, 'rate_limited', 'too many times submitted today', { retryAfterSec: 3600, reason: 'daily_cap' }, { 'retry-after': '3600' });
    }
    // verification through the room: a second member who vouches (see the top of this file)
    let verified = false;
    const confirm = []; // other players' submissions this one confirms (and that confirm it)
    if (roomCode) {
      const r = c.db.get(`SELECT * FROM rooms WHERE code = ?`, roomCode);
      const ms = r && r.game === 'smg' ? members(c, roomCode) : [];
      const me = ms.find((m) => m.pid === p.pid);
      if (!me) throw new ApiError(403, 'not_member', 'you are not in that room');
      // S6: the board category is the players in the run: a room that never had N members cannot post an N-player time (`peak_members`
      // is the most it ever held; rooms from before migration 9 have 0, so their live count stands in)
      if (category > Math.max(r.peak_members || 0, ms.length)) {
        throw new ApiError(400, 'bad_param', 'players is more than the room ever had', { field: 'players', reason: 'players_exceed_room' });
      }
      if (category > 1 && r.state !== 'started') throw new ApiError(409, 'room_not_started', 'that room has not started', { field: 'room' });
      // a verified time cannot be longer than the room had been running when it was sent
      const fitsAt = (f, atMs) => r.started_ms > 0 && (f * 1000) / 60 <= atMs - r.started_ms + t.verifySlackFrames * (1000 / 60);
      const aged = (pid) => {
        const pr = c.db.get(`SELECT created_ms, banned FROM profiles WHERE pid = ?`, pid);
        return !!pr && !pr.banned && c.now - pr.created_ms >= t.verifyMinAgeMs;
      };
      if (r.state === 'started' && category > 1 && fitsAt(frames, c.now) && aged(p.pid)) {
        const others = c.db.all(
          `SELECT id, pid, frames, category, submitted_ms FROM time_submits
            WHERE room = ? AND galaxy = ? AND star = ? AND pid <> ? AND category > 1 AND submitted_ms > ?`,
          roomCode, galaxy, star, p.pid, c.now - t.verifyWindowMs,
        );
        for (const o of others) {
          const om = ms.find((m) => m.pid === o.pid); // still a member (its address is known only while it is)
          if (!om || !fitsAt(o.frames, o.submitted_ms) || !aged(o.pid)) continue;
          if (t.verifyDistinctAddr && (!me.src_ip || !om.src_ip || me.src_ip === om.src_ip)) continue;
          // co-op: the same run, within a few frames; race: each member's own finish, so the times differ, but must still agree within
          // SMG_VERIFY_RACE_SEC (review S6: a race used to confirm with no agreement on the time at all)
          if (Math.abs(o.frames - frames) <= (r.mode === 'race' ? t.verifyRaceFrames : t.verifyFrames)) confirm.push(o);
        }
        verified = confirm.length > 0;
      }
    }
    c.db.run(
      `INSERT INTO time_submits (pid, galaxy, star, category, frames, room, verified, submitted_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
      p.pid, galaxy, star, category, frames, roomCode, verified ? 1 : 0, c.now,
    );
    for (const o of confirm) {
      c.db.run(`UPDATE time_submits SET verified = 1 WHERE id = ?`, o.id);
      c.db.run(
        `UPDATE star_times SET verified = 1 WHERE pid = ? AND galaxy = ? AND star = ? AND category = ? AND frames = ? AND room = ?`,
        o.pid, galaxy, star, o.category, o.frames, roomCode,
      );
    }
    // the best row
    const best = c.db.get(`SELECT * FROM star_times WHERE pid = ? AND galaxy = ? AND star = ? AND category = ?`, p.pid, galaxy, star, category);
    const better = !best || frames < best.frames || (frames === best.frames && verified && !best.verified);
    let flagged = false;
    if (better) {
      const why = suspicion(c, t, p.pid, { galaxy, star, category, frames }, best, verified);
      flagged = why.length > 0;
      for (const w of why) {
        c.db.run(
          `INSERT INTO flags (pid, kind, since_ms, updated_ms, hits, detail) VALUES (?, ?, ?, ?, 1, ?)
           ON CONFLICT(pid, kind) DO UPDATE SET updated_ms = excluded.updated_ms, hits = hits + 1, detail = excluded.detail`,
          p.pid, w.kind, c.now, c.now, JSON.stringify({ galaxy, star, category, after: frames, ...w.detail }),
        );
      }
      c.db.run(
        `INSERT INTO star_times (pid, galaxy, star, category, frames, players, verified, flagged, room, build, splits, digest, updated_ms)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT(pid, galaxy, star, category) DO UPDATE SET frames = excluded.frames, players = excluded.players,
           verified = excluded.verified, flagged = MAX(flagged, excluded.flagged), room = excluded.room, build = excluded.build,
           splits = excluded.splits, digest = excluded.digest, updated_ms = excluded.updated_ms`,
        p.pid, galaxy, star, category, frames, category, verified ? 1 : 0, flagged ? 1 : 0, roomCode, body.build, splits,
        body.digest ? body.digest.toLowerCase() : null, c.now,
      );
    }
    const mine = c.db.get(`SELECT frames, verified, flagged FROM star_times WHERE pid = ? AND galaxy = ? AND star = ? AND category = ?`, p.pid, galaxy, star, category);
    const rank = 1 + c.db.get(
      `SELECT COUNT(*) AS n FROM (SELECT 1 FROM star_times s JOIN profiles pr ON pr.pid = s.pid
        WHERE s.galaxy = ? AND s.star = ? AND s.category = ? AND s.frames < ? AND s.flagged = 0 AND pr.banned = 0
          ${t.showUnverified ? '' : 'AND s.verified > 0'} LIMIT ${SCAN_CAP})`,
      galaxy, star, category, mine.frames,
    ).n;
    if (c.timesMemo) c.timesMemo.dropStar(galaxy, star);
    return {
      ok: true, best: better, verified, flagged: !!mine.flagged, confirmed: confirm.length,
      personalBest: { frames: mine.frames, verified: !!mine.verified }, rank, category,
    };
  });
}

/**
 * Why a new best looks too good: a list of { kind, detail } (empty = fine). `best` is the player's current row of this star and
 * category (or null). Pure reads; the caller stores the flags.
 */
function suspicion(c, t, pid, { galaxy, star, category, frames }, best, verified) {
  const out = [];
  // 1. rolling improvement: against what the player's time was at the start of the window (or, with no older time, his first in it)
  const windowStart = c.now - t.rollingMs;
  let baseline = null;
  const older = c.db.get(
    `SELECT MIN(frames) AS f FROM time_submits WHERE pid = ? AND galaxy = ? AND star = ? AND category = ? AND submitted_ms <= ?`,
    pid, galaxy, star, category, windowStart,
  ).f;
  if (older !== null) baseline = older;
  else if (best && best.updated_ms <= windowStart) baseline = best.frames;
  else {
    const first = c.db.get(
      `SELECT frames FROM time_submits WHERE pid = ? AND galaxy = ? AND star = ? AND category = ? AND submitted_ms > ? ORDER BY submitted_ms ASC, id ASC LIMIT 1`,
      pid, galaxy, star, category, windowStart,
    );
    if (first) baseline = first.frames;
  }
  // (the submission being judged is already in time_submits: it is its own baseline only when it is the first, which is no improvement)
  if (baseline !== null && frames < baseline * (1 - t.jumpPct / 100)) {
    out.push({ kind: 'time_jump', detail: { before: baseline, windowHours: t.rollingMs / 3600_000 } });
  }
  // 2. the board's best (other players, not flagged, not banned)
  const top = c.db.get(
    `SELECT MIN(s.frames) AS f FROM star_times s JOIN profiles pr ON pr.pid = s.pid
      WHERE s.galaxy = ? AND s.star = ? AND s.category = ? AND s.flagged = 0 AND pr.banned = 0 AND s.pid <> ?`,
    galaxy, star, category, pid,
  ).f;
  if (top !== null) {
    if (!best && frames < top * (1 - t.firstPct / 100)) out.push({ kind: 'time_first', detail: { boardBest: top } });
    const alreadyAhead = !!best && best.frames <= top;
    if (!verified && !alreadyAhead && frames < top * (1 - t.topPct / 100)) out.push({ kind: 'time_top', detail: { boardBest: top } });
  }
  // 3. finding 13: a new best on a star whose board shows nobody else (nothing to compare against). "Shows": rows other players can
  // see, so a hidden (unverified or flagged) time of a second account cannot make the board look taken. The admin reviews it once.
  const visible = c.db.get(
    `SELECT COUNT(*) AS n FROM star_times s JOIN profiles pr ON pr.pid = s.pid
      WHERE s.galaxy = ? AND s.star = ? AND s.category = ? AND s.flagged = 0 AND pr.banned = 0 AND s.pid <> ? ${t.showUnverified ? '' : 'AND s.verified > 0'}`,
    galaxy, star, category, pid,
  ).n;
  if (!visible && t.flagNewStar) out.push({ kind: 'time_new_star', detail: {} });
  return out;
}

/**
 * The board of one star as an SQL source: one row per player (in `category`, or each player's best over every category when null),
 * banned accounts out, flagged rows out except the viewer's own. Ordered by frames, then who got there first, then pid.
 */
function boardSql(category, extraWhere = '', showUnverified = false) {
  const seen = showUnverified ? '' : 'AND (verified > 0 OR pid = ?3)'; // finding 13: nobody vouched for it: its owner only
  const inner = category === null
    ? `SELECT pid, MIN(frames) AS frames, players, verified, updated_ms FROM star_times
        WHERE galaxy = ?1 AND star = ?2 AND (flagged = 0 OR pid = ?3) ${seen} AND ?4 = 0 GROUP BY pid`
    : `SELECT pid, frames, players, verified, updated_ms FROM star_times
        WHERE galaxy = ?1 AND star = ?2 AND category = ?4 AND (flagged = 0 OR pid = ?3) ${seen}`;
  return `WITH b AS (SELECT t.* FROM (${inner}) t JOIN profiles pr ON pr.pid = t.pid WHERE pr.banned = 0 ${extraWhere})`;
}

function pageRows(c, sqlHead, binds, offset, limit, viewer) {
  const rows = c.db.all(
    `${sqlHead} SELECT b.pid, b.frames, b.players, b.verified, b.updated_ms, pr.name FROM b JOIN profiles pr ON pr.pid = b.pid
      ORDER BY b.frames ASC, b.updated_ms ASC, b.pid ASC LIMIT ?5 OFFSET ?6`,
    ...binds, limit, offset,
  );
  let rank = 0;
  return rows.map((r, i) => {
    if (i === 0) rank = 1 + c.db.get(`${sqlHead} SELECT COUNT(*) AS n FROM (SELECT 1 FROM b WHERE frames < ?5 LIMIT ${SCAN_CAP})`, ...binds, r.frames).n;
    else if (r.frames !== rows[i - 1].frames) rank = offset + i + 1;
    return { rank, name: r.name, frames: r.frames, players: r.players, verified: !!r.verified, you: r.pid === viewer };
  });
}

/** POST /v1/times/get {"galaxy","star","category"?,"mode":"top|near|friends","limit"?} */
export function getTimes(c, p, body) {
  const memo = c.timesMemo;
  if (!memo) return getTimesNow(c, p, body);
  strict(body, ['galaxy', 'star', 'category', 'mode', 'limit']);
  checkStar(body);
  // viewer class: the answer depends on the viewer only through his own (possibly hidden) rows, so a viewer with no row on this star
  // shares one answer with every other such viewer; the friends board is always per viewer
  const mine = body.mode === 'friends' || c.db.get(`SELECT 1 AS x FROM star_times WHERE pid = ? AND galaxy = ? AND star = ? LIMIT 1`, p.pid, body.galaxy, body.star);
  const key = `${body.galaxy}:${body.star}:${body.category ?? '-'}:${body.mode ?? 'top'}:${body.limit ?? 20}:${mine ? `p${p.pid}` : 'x'}`;
  return memo.get(c, key, () => getTimesNow(c, p, body));
}

/** The memo of getTimes (one per application). A submission for a star drops that star's entries; admin actions clear it all. */
export function createTimesMemo() {
  const memo = new Map();
  return {
    get(c, key, build) {
      const hit = memo.get(key);
      if (hit && c.now >= hit.at && c.now - hit.at < GET_MEMO_MS) return hit.body;
      const body = build();
      if (memo.size >= 2000) memo.clear();
      memo.set(key, { at: c.now, body });
      return body;
    },
    dropStar(galaxy, star) { for (const k of [...memo.keys()]) if (k.startsWith(`${galaxy}:${star}:`)) memo.delete(k); },
    clear() { memo.clear(); },
    size: () => memo.size,
  };
}

function getTimesNow(c, p, body) {
  strict(body, ['galaxy', 'star', 'category', 'mode', 'limit']);
  checkStar(body);
  if (body.category !== undefined && !isInt(body.category, 1, 4)) throw new ApiError(400, 'bad_category', 'category must be 1..4', { field: 'category' });
  const mode = body.mode === undefined ? 'top' : body.mode;
  if (!MODES.includes(mode)) throw new ApiError(400, 'bad_mode', 'mode must be top, near or friends', { field: 'mode' });
  if (body.limit !== undefined && !isInt(body.limit, 1, 50)) throw new ApiError(400, 'bad_param', 'limit must be 1..50', { field: 'limit' });
  const limit = body.limit ?? 20;
  const category = body.category ?? null;
  const friendsOnly = mode === 'friends'
    ? `AND (t.pid = ?3 OR t.pid IN (SELECT f1.friend_pid FROM friends f1 JOIN friends f2 ON f2.pid = f1.friend_pid AND f2.friend_pid = f1.pid WHERE f1.pid = ?3))`
    : '';
  // the friends board shows mutual friends' unverified times too (people you chose to trust); top / near / the public page do not
  const head = boardSql(category, friendsOnly, mode === 'friends' || c.cfg.smg.times.showUnverified);
  const binds = [body.galaxy, body.star, p.pid, category ?? 0];
  const total = c.db.get(`${head} SELECT COUNT(*) AS n FROM (SELECT 1 FROM b LIMIT ${SCAN_CAP})`, ...binds).n;
  const me = c.db.get(`${head} SELECT frames, players, verified, updated_ms FROM b WHERE pid = ?3`, ...binds);
  let you = null;
  let offset = 0;
  if (me) {
    const before = c.db.get(
      `${head} SELECT COUNT(*) AS n FROM (SELECT 1 FROM b WHERE frames < ?5 OR (frames = ?5 AND (updated_ms < ?6 OR (updated_ms = ?6 AND pid < ?3))) LIMIT ${SCAN_CAP})`,
      ...binds, me.frames, me.updated_ms,
    ).n;
    const rank = 1 + c.db.get(`${head} SELECT COUNT(*) AS n FROM (SELECT 1 FROM b WHERE frames < ?5 LIMIT ${SCAN_CAP})`, ...binds, me.frames).n;
    you = { rank, frames: me.frames, players: me.players, verified: !!me.verified };
    if (mode === 'near') offset = Math.max(0, Math.min(before - Math.floor(limit / 2), total - limit));
  }
  if (mode === 'near' && !me) offset = 0;
  const rows = pageRows(c, head, binds, offset, limit, p.pid);
  return { galaxy: body.galaxy, star: body.star, category, mode, total, offset, rows, you, ...(total >= SCAN_CAP ? { capped: true } : {}) };
}

// ------------------------------------------------------------------ public, read-only (the /times page)

export const PUBLIC_MEMO_MS = 60_000;

/** ?galaxy=&star=&category=&limit= (all strict). Without galaxy: the list of stars that have times. */
export function parsePublicQuery(params) {
  const num = (name, min, max) => {
    const raw = params.get(name);
    if (raw === null) return null;
    if (!/^\d{1,3}$/.test(raw) || Number(raw) < min || Number(raw) > max) throw new ApiError(400, 'bad_param', `${name} must be ${min}..${max}`, { field: name });
    return Number(raw);
  };
  const galaxy = num('galaxy', 0, MAX_GALAXY);
  const star = num('star', 1, 8);
  if ((galaxy === null) !== (star === null)) throw new ApiError(400, 'bad_param', 'galaxy and star go together', { field: 'star' });
  return { galaxy, star, category: num('category', 1, 4), limit: num('limit', 1, 100) ?? 50 };
}

export const publicKey = (q) => `${q.galaxy}:${q.star}:${q.category}:${q.limit}`;

function buildPublic(c, q) {
  const HOURMS = 3600_000;
  const show = c.cfg.smg.times.showUnverified;
  if (q.galaxy === null) {
    const stars = c.db.all(
      `SELECT s.galaxy, s.star, COUNT(DISTINCT s.pid) AS players, MIN(s.frames) AS best FROM star_times s JOIN profiles pr ON pr.pid = s.pid
        WHERE s.flagged = 0 AND pr.banned = 0 ${show ? '' : 'AND s.verified > 0'} GROUP BY s.galaxy, s.star ORDER BY s.galaxy, s.star LIMIT 600`,
    );
    return { updatedMs: c.now, stars };
  }
  const rows = c.db.all(
    `${boardSql(q.category, '', show)} SELECT b.frames, b.players, b.verified, b.updated_ms, pr.name FROM b JOIN profiles pr ON pr.pid = b.pid
      ORDER BY b.frames ASC, b.updated_ms ASC, b.pid ASC LIMIT ?5`,
    q.galaxy, q.star, -1, q.category ?? 0, q.limit,
  );
  let rank = 0;
  return {
    galaxy: q.galaxy, star: q.star, category: q.category, updatedMs: c.now,
    rows: rows.map((r, i) => {
      if (i === 0 || r.frames !== rows[i - 1].frames) rank = i + 1;
      return { rank, name: r.name, frames: r.frames, players: r.players, verified: !!r.verified, dateMs: Math.floor(r.updated_ms / HOURMS) * HOURMS };
    }),
  };
}

export function createPublicTimes() {
  const memo = new Map();
  return {
    get(c, q) {
      const key = publicKey(q);
      const hit = memo.get(key);
      if (hit && c.now >= hit.at && c.now - hit.at < PUBLIC_MEMO_MS) return hit.body;
      const body = buildPublic(c, q);
      if (memo.size >= 400) memo.clear();
      memo.set(key, { at: c.now, body });
      return body;
    },
    clear() { memo.clear(); },
  };
}
