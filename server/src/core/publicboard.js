// Public, read-only leaderboard (GET /v1/public/leaderboard), the data behind the /leaderboard web page.
// It reads the same `scores` rows, window and order as POST /v1/rank/get (score desc, older update first, lower pid first;
// ties share the competition rank 1,2,2,4) and shows only a display name, the win / loss record, the points and the date of the player's
// last verified ranked match (1.3). Nothing else leaves the object: no pid, code, token, region or address. Banned accounts are left out.
// Boards: season (category 0, this week), daily (category 1, this UTC day), alltime (verified points of every ranked match, profiles),
// archive (no season: the list of archived seasons; season=<start ms>: that season's final standings, archive.js).
// The answer is memoised in the object for MEMO_MS, so a busy page costs one query a minute per board (see README).

import { ApiError, b64decode } from './util.js';
import { windowStart, season, dayStart } from './calendar.js';
import { blobByteOrder } from './ranking.js';

export const MEMO_MS = 60_000;
export const DEFAULT_LIMIT = 50;
export const MAX_LIMIT = 100;
const BOARDS = { season: 0, daily: 1, alltime: -1, archive: -2 }; // season / daily: the game's categories (see calendar.js)
const DAY = 86400000;

/** Parse ?board= and ?limit= strictly. Returns {board, limit} or throws bad_param. */
export function parseQuery(params) {
  const board = params.get('board') ?? 'season';
  if (!Object.hasOwn(BOARDS, board)) throw new ApiError(400, 'bad_param', 'board must be season or daily', { field: 'board' });
  const raw = params.get('limit');
  let limit = DEFAULT_LIMIT;
  if (raw !== null) {
    if (!/^\d{1,4}$/.test(raw) || Number(raw) < 1 || Number(raw) > MAX_LIMIT) {
      throw new ApiError(400, 'bad_param', `limit must be 1..${MAX_LIMIT}`, { field: 'limit' });
    }
    limit = Number(raw);
  }
  let seasonStart = null;
  const rawSeason = params.get('season');
  if (rawSeason !== null) {
    if (board !== 'archive' || !/^\d{1,15}$/.test(rawSeason) || season(Number(rawSeason)).startMs !== Number(rawSeason)) {
      throw new ApiError(400, 'bad_param', 'season is the start (ms) of a past season, with board=archive only', { field: 'season' });
    }
    seasonStart = Number(rawSeason);
  }
  return { board, limit, season: seasonStart };
}

/** The memo key of a parsed query (the Worker's memo uses it too). */
export const queryKey = (q) => `${q.board}:${q.limit}:${q.season ?? ''}`;

// Last verified ranked match, to the hour (enough for the page's local date; not when exactly someone played). null = never.
const HOUR = 3600000;
const lastPlayed = (ms) => (ms > 0 ? Math.floor(ms / HOUR) * HOUR : null);

/** Wins and losses from the stored 0x80-byte submission (either byte order); 0 / 0 when the blob has no readable record. */
export function recordOf(b64) {
  const blob = b64decode(b64);
  const le = blob && blob.length === 0x80 ? blobByteOrder(blob) : null;
  if (le === null) return { wins: 0, losses: 0 };
  const dv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
  return { wins: dv.getInt32(4, le), losses: dv.getInt32(8, le) };
}

function ranked(rows, points) {
  let rank = 0;
  return rows.map((r, i) => {
    if (i === 0 || points(r) !== points(rows[i - 1])) rank = i + 1;
    return rank;
  });
}

function scoreRows(rows) {
  const ranks = ranked(rows, (r) => r.score);
  return rows.map((r, i) => {
    const rec = recordOf(r.data);
    return { rank: ranks[i], name: r.name, wins: rec.wins, losses: rec.losses, points: r.score, lastPlayedMs: lastPlayed(r.last_played_ms) };
  });
}

function build(c, q) {
  const { board, limit } = q;
  const s = season(c.now);
  const body = { board, season: { index: s.index, startMs: s.startMs, endMs: s.endMs } };
  if (board === 'daily') body.day = { startMs: dayStart(c.now), endMs: dayStart(c.now) + DAY };
  body.updatedMs = c.now; // (key order kept from 1.2: board, season, day, updatedMs, rows)
  if (board === 'season' || board === 'daily') {
    const category = BOARDS[board];
    body.rows = scoreRows(c.db.all(
      `SELECT p.name AS name, p.last_played_ms AS last_played_ms, s.score AS score, s.data_b64 AS data
         FROM scores s JOIN profiles p ON p.pid = s.pid
        WHERE s.category = ? AND s.updated_ms >= ? AND p.banned = 0
        ORDER BY s.score DESC, s.updated_ms ASC, s.pid ASC LIMIT ?`,
      category, windowStart(category, c.now), limit,
    ));
  } else if (board === 'alltime') {
    // every verified ranked match's points (matches.js settle), the rated record and the last verified ranked match; never reset
    const rows = c.db.all(
      `SELECT name, points_total, wins, losses, last_played_ms FROM profiles
        WHERE points_total > 0 AND banned = 0 ORDER BY points_total DESC, last_played_ms ASC, pid ASC LIMIT ?`,
      limit,
    );
    const ranks = ranked(rows, (r) => r.points_total);
    body.rows = rows.map((r, i) => ({ rank: ranks[i], name: r.name, wins: r.wins, losses: r.losses, points: r.points_total, lastPlayedMs: lastPlayed(r.last_played_ms) }));
  } else if (q.season === null) {
    // the archived seasons, newest first
    body.seasons = c.db.all(
      `SELECT season_start_ms AS startMs, COUNT(*) AS players FROM season_scores GROUP BY season_start_ms ORDER BY season_start_ms DESC LIMIT 104`,
    ).map((r) => ({ startMs: r.startMs, endMs: season(r.startMs).endMs, index: season(r.startMs).index, players: r.players }));
    body.rows = [];
  } else {
    const a = season(q.season);
    body.archived = { startMs: a.startMs, endMs: a.endMs, index: a.index };
    body.rows = scoreRows(c.db.all(
      `SELECT p.name AS name, p.last_played_ms AS last_played_ms, a.score AS score, a.data_b64 AS data
         FROM season_scores a JOIN profiles p ON p.pid = a.pid
        WHERE a.season_start_ms = ? AND p.banned = 0
        ORDER BY a.score DESC, a.updated_ms ASC, a.pid ASC LIMIT ?`,
      q.season, limit,
    ));
  }
  return body;
}

/** The memo lives in the object (one per createApp); `clear` empties it after a ban or account deletion. */
export function createBoard() {
  const memo = new Map();
  return {
    get(c, query) {
      const key = queryKey(query);
      const hit = memo.get(key);
      if (hit && c.now >= hit.at && c.now - hit.at < MEMO_MS) return hit.body;
      const body = build(c, query);
      if (memo.size >= 4 * MAX_LIMIT) memo.clear(); // bounded
      memo.set(key, { at: c.now, body });
      return body;
    },
    clear() { memo.clear(); },
  };
}
