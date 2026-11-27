// Admin endpoints (secret bearer token): stats, export/import for backups, kill switches, bans.

import { ApiError, isInt, strict } from './util.js';
import { EXPORT_TABLES, EXPORT_HIDDEN_SETTINGS } from './schema.js';
import { turnStatus } from './turn.js';
import { fmtCode } from './accounts.js';
import { dropPlayer } from './rooms4.js';
import { reportStats } from './reports.js';
import { summaryAggregates } from './summaries.js';

export async function stats(c) {
  const n = (q, ...b) => c.db.get(q, ...b).n;
  return {
    nowMs: c.now,
    profiles: n(`SELECT COUNT(*) AS n FROM profiles`),
    banned: n(`SELECT COUNT(*) AS n FROM profiles WHERE banned = 1`),
    roomsLive: n(`SELECT COUNT(*) AS n FROM rooms WHERE state IN ('open','joined','ready','started')`),
    queue: n(`SELECT COUNT(*) AS n FROM queue WHERE status = 'queued'`),
    matches: Object.fromEntries(
      c.db.all(`SELECT status, COUNT(*) AS n FROM matches GROUP BY status`).map((r) => [r.status, r.n]),
    ),
    scores: n(`SELECT COUNT(*) AS n FROM scores`),
    flags: Object.fromEntries(c.db.all(`SELECT kind, COUNT(*) AS n FROM flags GROUP BY kind`).map((r) => [r.kind, r.n])),
    turn: turnStatus(c),
    kill: [...c.killSwitches()],
    // svc3: today's request accounting (budget.js): requests admitted / refused for abuse / refused because the budget is spent, and the pace
    budget: c.budgetStatus ? c.budgetStatus() : undefined,
    // Galaxy beta: stored problem reports (count, bytes, today's count against the day's share) and the session summaries' daily aggregates by transport
    ...(c.cfg.game === 'smg' ? { reports: reportStats(c), summaries: await summaryAggregates(c) } : {}),
  };
}

/** GET /admin/export: no table -> list; ?table=..&after=<rowid>&limit=<n> -> a page of rows. */
export function exportTable(c, params) {
  const table = params.get('table');
  if (!table) {
    return {
      tables: EXPORT_TABLES.map((t) => ({ table: t, rows: t === 'settings' ? c.db.get(`SELECT COUNT(*) AS n FROM settings WHERE key NOT IN (${EXPORT_HIDDEN_SETTINGS.map(() => '?').join(',')})`, ...EXPORT_HIDDEN_SETTINGS).n : c.db.get(`SELECT COUNT(*) AS n FROM ${t}`).n })),
      schema: c.db.all(`SELECT id, name FROM _migrations ORDER BY id`),
      note: 'page each table with ?table=NAME&after=0&limit=500; profile rows include token hashes, keep backups private',
    };
  }
  if (!EXPORT_TABLES.includes(table)) throw new ApiError(400, 'bad_table', 'unknown table');
  const after = Number(params.get('after') || 0);
  const limit = Math.min(2000, Math.max(1, Number(params.get('limit') || 500)));
  if (!Number.isFinite(after) || after < 0) throw new ApiError(400, 'bad_request', 'bad cursor');
  const hide = table === 'settings' ? ` AND key NOT IN (${EXPORT_HIDDEN_SETTINGS.map(() => '?').join(',')})` : '';
  const rows = c.db.all(`SELECT rowid AS _rowid, * FROM ${table} WHERE rowid > ?${hide} ORDER BY rowid LIMIT ?`, after, ...(hide ? EXPORT_HIDDEN_SETTINGS : []), limit);
  const next = rows.length === limit ? rows[rows.length - 1]._rowid : null;
  return { table, rows: rows.map(({ _rowid, ...r }) => r), next };
}

/** POST /admin/import {table, rows}: INSERT OR REPLACE by primary key, columns checked against the schema. */
export function importRows(c, body) {
  strict(body, ['table', 'rows']);
  if (!EXPORT_TABLES.includes(body.table) || !Array.isArray(body.rows)) {
    throw new ApiError(400, 'bad_request', 'expected {table, rows[]}');
  }
  if (body.rows.length > 2000) throw new ApiError(413, 'too_large', 'at most 2000 rows per call');
  const cols = c.db.all(`PRAGMA table_info(${body.table})`).map((r) => r.name);
  let n = 0;
  c.tx(() => {
    for (const row of body.rows) {
      const keys = Object.keys(row);
      if (!keys.length || !keys.every((k) => cols.includes(k))) throw new ApiError(400, 'bad_request', 'unknown column in row');
      const vals = keys.map((k) => {
        const v = row[k];
        if (!(v === null || typeof v === 'string' || typeof v === 'number')) throw new ApiError(400, 'bad_request', 'bad value');
        return v;
      });
      c.db.run(
        `INSERT OR REPLACE INTO ${body.table} (${keys.map((k) => `"${k}"`).join(',')}) VALUES (${keys.map(() => '?').join(',')})`,
        ...vals,
      );
      n++;
    }
  });
  return { ok: true, imported: n };
}

export function setSwitches(c, body) {
  strict(body, ['kill', 'message']);
  const kill = body.kill === undefined ? undefined : body.kill;
  if (kill !== undefined && (!Array.isArray(kill) || !kill.every((k) => c.killNames.includes(k)))) {
    throw new ApiError(400, 'bad_request', `kill must be a list of: ${c.killNames.join(', ')}`);
  }
  const message = body.message === undefined ? undefined : String(body.message).slice(0, 200);
  c.tx(() => {
    const put = (k, v) => c.db.run(
      `INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, k, v,
    );
    if (kill !== undefined) put('kill', JSON.stringify(kill));
    if (message !== undefined) put('message', message);
  });
  c.invalidateSettings();
  return { ok: true, kill: [...c.killSwitches()] };
}

export function ban(c, body) {
  strict(body, ['pid', 'banned', 'reason']);
  if (!isInt(body.pid, 1, 2147483647)) throw new ApiError(400, 'bad_request', 'pid required');
  const banned = body.banned === false ? 0 : 1;
  const reason = String(body.reason || '').slice(0, 100);
  c.tx(() => {
    c.db.run(`UPDATE profiles SET banned = ?, ban_reason = ? WHERE pid = ?`, banned, reason, body.pid);
    if (banned) {
      c.db.run(`DELETE FROM queue WHERE pid = ?`, body.pid);
      c.db.run(`DELETE FROM scores WHERE pid = ?`, body.pid);
      dropPlayer(c, body.pid); // Galaxy: out of every room, the host role moves on (boards hide banned players)
    }
  });
  c.clearBoard();
  return { ok: true };
}

/** GET /admin/flags: accounts the service flagged (never banned automatically), newest first. */
export function listFlags(c, params) {
  const kind = params.get('kind');
  const rows = c.db.all(
    `SELECT f.pid, f.kind, f.since_ms, f.updated_ms, f.hits, f.detail, p.code, p.name, p.rating, p.wins, p.losses, p.draws, p.abandons, p.banned
       FROM flags f LEFT JOIN profiles p ON p.pid = f.pid ${kind ? 'WHERE f.kind = ?' : ''} ORDER BY f.updated_ms DESC LIMIT 500`,
    ...(kind ? [kind] : []),
  );
  return {
    flags: rows.map((r) => {
      let detail = null;
      try { detail = JSON.parse(r.detail); } catch { /* keep null */ }
      return {
        pid: r.pid, code: r.code ? fmtCode(r.code) : null, name: r.name, kind: r.kind, sinceMs: r.since_ms, updatedMs: r.updated_ms, hits: r.hits,
        detail, rating: r.rating, wins: r.wins, losses: r.losses, draws: r.draws, abandons: r.abandons, banned: !!r.banned,
      };
    }),
  };
}

/** POST /admin/flags/clear {pid, kind?}: an admin reviewed the flag. */
export function clearFlags(c, body) {
  strict(body, ['pid', 'kind']);
  if (!isInt(body.pid, 1, 2147483647)) throw new ApiError(400, 'bad_request', 'pid required');
  if (body.kind !== undefined && typeof body.kind !== 'string') throw new ApiError(400, 'bad_request', 'kind must be a string');
  if (body.kind === undefined) c.db.run(`DELETE FROM flags WHERE pid = ?`, body.pid);
  else c.db.run(`DELETE FROM flags WHERE pid = ? AND kind = ?`, body.pid, body.kind);
  // Galaxy: a reviewed time (time_jump, time_first, time_top) shows on the boards again
  if (body.kind === undefined || body.kind.startsWith('time_')) c.db.run(`UPDATE star_times SET flagged = 0 WHERE pid = ?`, body.pid);
  c.clearBoard();
  return { ok: true };
}

// ------------------------------------------------------------------ Galaxy star times (service review, finding 13)

function starFilter(body) {
  const where = ['pid = ?'];
  const binds = [body.pid];
  for (const [k, lo, hi] of [['galaxy', 0, 63], ['star', 1, 8], ['category', 1, 4]]) {
    if (body[k] === undefined) continue;
    if (!isInt(body[k], lo, hi)) throw new ApiError(400, 'bad_request', `${k} out of range`, { field: k });
    where.push(`${k} = ?`);
    binds.push(body[k]);
  }
  return { where: where.join(' AND '), binds };
}

/** GET /admin/times?galaxy=&star=[&category=]: every row of one star, hidden ones included (pid, flags, verification), for review. */
export function listTimes(c, params) {
  const g = Number(params.get('galaxy'));
  const s = Number(params.get('star'));
  const cat = params.get('category');
  if (!isInt(g, 0, 63) || !isInt(s, 1, 8) || (cat !== null && !isInt(Number(cat), 1, 4))) throw new ApiError(400, 'bad_request', 'galaxy, star (category) required');
  const rows = c.db.all(
    `SELECT t.pid, p.code, p.name, t.category, t.frames, t.verified, t.flagged, t.room, t.build, t.updated_ms, p.created_ms, p.banned
       FROM star_times t LEFT JOIN profiles p ON p.pid = t.pid
      WHERE t.galaxy = ? AND t.star = ? ${cat !== null ? 'AND t.category = ?' : ''} ORDER BY t.category, t.frames LIMIT 500`,
    g, s, ...(cat !== null ? [Number(cat)] : []),
  );
  return { rows: rows.map((r) => ({ ...r, code: r.code ? fmtCode(r.code) : null, banned: !!r.banned })) };
}

/** POST /admin/times/remove {pid, galaxy?, star?, category?}: the account's times (all, one galaxy, one star, one category) go. */
export function removeTimes(c, body) {
  strict(body, ['pid', 'galaxy', 'star', 'category']);
  if (!isInt(body.pid, 1, 2147483647)) throw new ApiError(400, 'bad_request', 'pid required');
  const f = starFilter(body);
  let removed = 0;
  c.tx(() => {
    removed = c.db.get(`SELECT COUNT(*) AS n FROM star_times WHERE ${f.where}`, ...f.binds).n;
    c.db.run(`DELETE FROM star_times WHERE ${f.where}`, ...f.binds);
    c.db.run(`DELETE FROM time_submits WHERE ${f.where}`, ...f.binds);
  });
  c.clearBoard();
  return { ok: true, removed };
}

/** POST /admin/times/approve {pid, galaxy, star, category}: an admin vouches for one row (verified = 2, flag cleared): it is shown. */
export function approveTime(c, body) {
  strict(body, ['pid', 'galaxy', 'star', 'category']);
  if (!isInt(body.pid, 1, 2147483647) || body.galaxy === undefined || body.star === undefined || body.category === undefined) {
    throw new ApiError(400, 'bad_request', 'pid, galaxy, star and category required');
  }
  const f = starFilter(body);
  const n = c.db.get(`SELECT COUNT(*) AS n FROM star_times WHERE ${f.where}`, ...f.binds).n;
  if (!n) throw new ApiError(404, 'not_found', 'no such time');
  c.db.run(`UPDATE star_times SET verified = 2, flagged = 0 WHERE ${f.where}`, ...f.binds);
  c.clearBoard();
  return { ok: true };
}

/** POST /admin/times/ban {pid, banned?, reason?}: keep an account off the boards (its rows go, its new times are refused); it still plays. */
export function boardBan(c, body) {
  strict(body, ['pid', 'banned', 'reason']);
  if (!isInt(body.pid, 1, 2147483647)) throw new ApiError(400, 'bad_request', 'pid required');
  const on = body.banned !== false;
  c.tx(() => {
    if (on) {
      c.db.run(
        `INSERT INTO board_bans (pid, since_ms, reason) VALUES (?, ?, ?) ON CONFLICT(pid) DO UPDATE SET reason = excluded.reason`,
        body.pid, c.now, String(body.reason || '').slice(0, 100),
      );
      c.db.run(`DELETE FROM star_times WHERE pid = ?`, body.pid);
      c.db.run(`DELETE FROM time_submits WHERE pid = ?`, body.pid);
    } else c.db.run(`DELETE FROM board_bans WHERE pid = ?`, body.pid);
  });
  c.clearBoard();
  return { ok: true, banned: on };
}
