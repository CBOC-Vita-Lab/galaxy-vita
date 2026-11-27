// Galaxy 3.0 beta: problem reports sent from the game with the player's consent (POST /v1/report), and the session summaries that ride on the
// room leave / close request (see summaries.js). A report is masked here BEFORE it is stored or logged: IPv4 / IPv6 addresses, e-mail addresses and
// 32+ hex runs are replaced, control characters other than \n and \t are removed. Reports live in the D1 database bound as REPORTS_DB (tables created on first use;
// 30 days, at most 2,000 rows and 64 MB, the oldest dropped first; the limits and the running totals stay in the Durable Object) and, masked, in Workers Logs (one `report` line and `report_log` chunks), so the owner can read them in the
// Cloudflare dashboard without the admin token. Never logged or shown: a token, a name, an address; the player id is shown to the admin as its last 4 digits.

import { ApiError, strict, b64url, randomBytes, te } from './util.js';
import { dailyCapError } from './limits.js';

export const REPORT_MAX_LOG_BYTES = 48000;
export const REPORT_NOTE_MAX = 200;
/** The largest request body of POST /v1/report: the worst case of JSON-escaping the 48,000-byte log (a control character is 1 byte raw and 6 as \u00XX: 288,000) plus the small fields. edge.js and app.js use this one number. */
export const REPORT_BODY_MAX = 294912;
export const LOG_CHUNK_BYTES = 16384;
export const KINDS = ['user', 'after_error'];
export const WHERES = ['mp_menu', 'settings', 'post_session'];
const FLAT_MAX_KEYS = 40;
const FLAT_STR_MAX = 64;
const KEY_RE = /^[A-Za-z0-9_.-]{1,32}$/;
const STAT_KEY = 'reports_stat'; // settings row "<rows>:<bytes>": the running total, so a report does not have to scan the table

// ---------------------------------------------------------------- masking
// eslint-disable-next-line no-control-regex
const CONTROL_RE = /[\u0000-\u0008\u000B-\u001F\u007F-\u009F]/g;
const BEARER_RE = /\bBearer\s+\S+/gi;
const EMAIL_RE = /[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9.-]{1,255}\.[A-Za-z]{2,24}/g; // bounded (64 / 255 / 24): linear on a 48 KB log
const MAC_RE = /(?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])|(?<![0-9A-Fa-f.])(?:[0-9A-Fa-f]{4}\.){2}[0-9A-Fa-f]{4}(?![0-9A-Fa-f])/g;
const LONG_RE = /[A-Za-z0-9_-]{40,}/g; // the session token is 43 base64url characters; any such run is token-shaped
const HEX_RE = /[0-9A-Fa-f]{32,}/g;
// profile/pid/account/uid/player followed by a run of 4+ digits (a player/profile id: 4-12 digits; longer fails closed), with spaces and/or = or : between; twin of the client scrubber (mp_report.cpp ScrubIdNumber). `pid4711` (glued) and `player 12` stay.
const ID_NUM_RE = /\b(profile|pid|account|uid|player)([ \t]*[=:][ \t]*|[ \t]+)\d{4,}/gi;
const IPV4_RE = /(?<!\d)(?:\d{1,3}\.){3}\d{1,3}(?!\d)/g; // glued to letters or underscores too (ip_1.2.3.4, x192.168.1.1, 1.2.3.4b)
const COLON_RE = /(?<![\w:.])[0-9A-Fa-f:]*:[0-9A-Fa-f:]*(?:\.\d{1,3}){0,3}/g; // checked by isIPv6; what follows (a letter too) does not matter

const octetsOk = (s) => s.split('.').every((o) => Number(o) <= 255);

/** A run of hex digits and colons that is an IPv6 address: a `::` somewhere, or eight groups, each of at most four digits (a time such as 12:34:56 is not one). */
function isIPv6(tok) {
  const colons = tok.split(':').length - 1;
  if (colons < 2) return false;
  let t = tok;
  let tailGroups = 0;
  const dot = t.indexOf('.');
  if (dot >= 0) { // an embedded IPv4 tail counts as two groups
    const lastColon = t.lastIndexOf(':');
    const quad = t.slice(lastColon + 1);
    if (!/^(?:\d{1,3}\.){3}\d{1,3}$/.test(quad) || !octetsOk(quad)) return false;
    t = t.slice(0, lastColon + 1) + '0:0';
    tailGroups = 0;
  }
  const dc = t.indexOf('::');
  if (dc >= 0 && t.indexOf('::', dc + 1) >= 0) return false; // two compressions
  const groups = t.split(':').filter((g, i, a) => !(g === '' && (i === 0 || i === a.length - 1 || dc >= 0)));
  if (!groups.every((g) => /^[0-9A-Fa-f]{1,4}$/.test(g))) return false;
  if (dc >= 0) return groups.length + tailGroups <= 7;
  return groups.length === 8;
}

/**
 * Masks a text before it is stored or logged: control characters out (except \n and \t); the request's OWN secrets (its bearer token) and `Bearer <anything>` replaced;
 * e-mail addresses, MAC addresses, token-shaped runs (40+ of [A-Za-z0-9_-]), long hex runs, 4+ digits after profile/pid/account/uid/player, IPv6 and IPv4 addresses replaced. Every rule is linear. Idempotent.
 * @param {string[]} [secrets] exact values to replace first (the caller's bearer token)
 */
export function maskText(text, secrets = []) {
  let s = String(text).replace(CONTROL_RE, '');
  for (const v of secrets) if (typeof v === 'string' && v.length >= 8) s = s.split(v).join('[REDACTED]');
  s = s.replace(BEARER_RE, 'Bearer [REDACTED]');
  s = s.replace(EMAIL_RE, '[email]');
  s = s.replace(MAC_RE, '[mac]');
  s = s.replace(LONG_RE, (m) => (/^[0-9A-Fa-f]+$/.test(m) ? '[hex]' : '[token]'));
  s = s.replace(HEX_RE, '[hex]');
  s = s.replace(ID_NUM_RE, '$1$2[id]');
  s = s.replace(COLON_RE, (m) => (isIPv6(m) ? '[ip]' : m));
  s = s.replace(IPV4_RE, (m) => (octetsOk(m) ? '[ip]' : m));
  return s;
}

/** A flat object of short strings and numbers; everything else (nested values, booleans, null, bad keys, a 41st key) is dropped. null when nothing is left. */
export function flatObject(v, secrets = []) {
  if (v === undefined || v === null) return null;
  if (typeof v !== 'object' || Array.isArray(v)) throw new ApiError(400, 'bad_request', 'expected an object or null');
  const out = {};
  let n = 0;
  for (const [k, val] of Object.entries(v)) {
    if (n >= FLAT_MAX_KEYS) break;
    if (!KEY_RE.test(k) || k === '__proto__') continue;
    // the KEY is masked too (a key such as "10.0.0.5" or a token would otherwise pass): a masked key keeps its place with the brackets turned into underscores
    let mk = maskText(k, secrets);
    if (mk !== k) mk = mk.replace(/[[\]]/g, '_').slice(0, 32);
    if (!KEY_RE.test(mk) || mk === '__proto__') continue;
    let val2;
    if (typeof val === 'string') val2 = maskText(val, secrets).slice(0, FLAT_STR_MAX);
    else if (typeof val === 'number' && Number.isFinite(val)) val2 = Math.max(-1e15, Math.min(1e15, val));
    else continue;
    if (!(mk in out)) n++;
    out[mk] = val2;
  }
  return n ? out : null;
}

const byteLen = (s) => te.encode(s).length;

/** Splits text into pieces of at most `max` UTF-8 bytes, never inside a character. */
export function chunkUtf8(text, max = LOG_CHUNK_BYTES) {
  const out = [];
  let cur = '';
  let bytes = 0;
  for (const ch of text) {
    const b = byteLen(ch);
    if (bytes + b > max) { out.push(cur); cur = ''; bytes = 0; }
    cur += ch;
    bytes += b;
  }
  if (cur || !out.length) out.push(cur);
  return out;
}

// ---------------------------------------------------------------- storage (D1, binding REPORTS_DB)
// Tables are created on first use (ensureTables). The limits (3 a player, 30 an address, the day's share) and the running totals live in the Durable Object, so a
// report that is refused never touches D1. D1 holds only the text.
const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS reports (id TEXT PRIMARY KEY, pid INTEGER NOT NULL, created INTEGER NOT NULL, kind TEXT NOT NULL, where_ TEXT NOT NULL, app TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', check_json TEXT NOT NULL DEFAULT '', stats_json TEXT NOT NULL DEFAULT '', bytes INTEGER NOT NULL DEFAULT 0, log TEXT NOT NULL DEFAULT '')`, // bytes BEFORE log: reading the size never reads the big text
  `CREATE INDEX IF NOT EXISTS reports_created ON reports(created)`,
  `CREATE INDEX IF NOT EXISTS reports_pid ON reports(pid)`,
  `CREATE TABLE IF NOT EXISTS summaries (id INTEGER PRIMARY KEY AUTOINCREMENT, created INTEGER NOT NULL, transport TEXT NOT NULL, ended TEXT NOT NULL DEFAULT '', app TEXT NOT NULL DEFAULT '', data TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS summary_days (day TEXT NOT NULL, transport TEXT NOT NULL, n INTEGER NOT NULL DEFAULT 0, sums TEXT NOT NULL DEFAULT '{}', maxs TEXT NOT NULL DEFAULT '{}', ended TEXT NOT NULL DEFAULT '{}', PRIMARY KEY (day, transport))`,
];
const ready = new WeakMap();
/** Creates the tables once per database handle (a failed attempt is forgotten, so the next request tries again). */
export async function ensureTables(db, ms = 5000) {
  let p = ready.get(db);
  if (!p) {
    // bounded: a stalled attempt is forgotten when its time is up, so a later request tries again instead of waiting for it forever
    p = withTimeout(db.batch(SCHEMA.map((q) => db.prepare(q))), ms).catch((e) => { ready.delete(db); throw e; });
    ready.set(db, p);
  }
  await p;
}

function readStat(c) {
  const r = c.db.get(`SELECT value FROM settings WHERE key = ?`, STAT_KEY);
  const [n, b] = String(r ? r.value : '').split(':').map(Number);
  return { n: n || 0, bytes: b || 0 };
}
function writeStat(c, st) {
  c.db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, STAT_KEY, `${Math.max(0, st.n)}:${Math.max(0, st.bytes)}`);
}
async function recount(c) {
  const r = await c.reportsDb.prepare(`SELECT COUNT(*) AS n, COALESCE(SUM(bytes), 0) AS b FROM reports`).first();
  c.tx(() => writeStat(c, { n: r.n, bytes: r.b }));
}
/** Rejects when the promise has not settled in `ms` (D1 can stall): the caller answers without it. The stalled work is left to finish or fail on its own. */
export function withTimeout(promise, ms) {
  let t;
  return Promise.race([promise, new Promise((_, rej) => { t = setTimeout(() => rej(new Error('d1_timeout')), ms); })]).finally(() => clearTimeout(t));
}
const changesOf = (res) => (res && res.meta && typeof res.meta.changes === 'number' ? res.meta.changes : 0);
/** Adds one DELETE ... RETURNING bytes result to acc {n, bytes, exact}. The returned rows are trusted only when they account for meta.changes: a result with fewer rows than changes (an empty list included) counts the changes and marks the totals inexact (a recount follows). */
function tally(acc, r) {
  const rows = r && Array.isArray(r.results) ? r.results : null;
  const ch = changesOf(r);
  if (rows && rows.length >= ch) { acc.n += rows.length; for (const x of rows) acc.bytes += Number(x.bytes) || 0; return; }
  if (ch > 0) { acc.exact = false; acc.n += ch; }
}

const dayKey = (c) => new Date(c.now).toISOString().slice(0, 10);
const counter = (c, key) => c.db.get(`SELECT value FROM counters WHERE key = ?`, key)?.value || 0;
function bump(c, key, by = 1) {
  c.db.run(
    `INSERT INTO counters (key, value, updated_ms) VALUES (?, ?, ?) ON CONFLICT(key) DO UPDATE SET value = value + excluded.value, updated_ms = excluded.updated_ms`,
    key, by, c.now,
  );
}

/** The day's own share of the request budget for reports (REPORT_SHARE_PCT of DAILY_REQUEST_BUDGET; at least 1 while the share is on). */
export function reportShareLimit(cfg) {
  if (!(cfg.reports.sharePct > 0)) return 0;
  return cfg.dailyBudget > 0 ? Math.max(1, Math.floor((cfg.dailyBudget * cfg.reports.sharePct) / 100)) : 1000;
}

/**
 * POST /v1/report. Body {v:1, kind, where, note?, check?, stats?, log}. Answers 201 {id}. 400 bad shape, 413 too big, 429 over the limits, 503 over budget / storage.
 * Validated and masked first (a bad request is never counted); the limits are reserved before D1 is called and given back when D1 refuses.
 */
export async function submitReport(c, p, body, ipk, app, secrets = []) {
  strict(body, ['v', 'kind', 'where', 'note', 'check', 'stats', 'log']);
  if (body.v !== 1) throw new ApiError(400, 'bad_request', 'v must be 1');
  if (!KINDS.includes(body.kind)) throw new ApiError(400, 'bad_request', 'kind must be user or after_error');
  if (!WHERES.includes(body.where)) throw new ApiError(400, 'bad_request', 'where must be mp_menu, settings or post_session');
  let note = '';
  if (body.note !== undefined && body.note !== null) {
    if (typeof body.note !== 'string') throw new ApiError(400, 'bad_request', 'note must be a string');
    if ([...body.note].length > REPORT_NOTE_MAX) throw new ApiError(400, 'bad_request', `note is at most ${REPORT_NOTE_MAX} characters`);
    note = maskText(body.note, secrets);
  }
  if (typeof body.log !== 'string') throw new ApiError(400, 'bad_request', 'log must be a string');
  if (byteLen(body.log) > REPORT_MAX_LOG_BYTES) throw new ApiError(413, 'too_large', `log is at most ${REPORT_MAX_LOG_BYTES} bytes`);
  const check = flatObject(body.check, secrets);
  const stats = flatObject(body.stats, secrets);
  const log = maskText(body.log, secrets);
  const checkJson = check ? JSON.stringify(check) : '';
  const statsJson = stats ? JSON.stringify(stats) : '';
  const db = c.reportsDb;
  if (!db) throw new ApiError(503, 'report_unavailable', 'problem reports are not available on this service');
  const k = c.cfg.reports;
  const size = byteLen(log) + byteLen(note) + checkJson.length + statsJson.length + 120;
  const appStr = String(app || '').slice(0, 20);
  const day = dayKey(c);

  const st = c.tx(() => {
    if (k.perPlayerDay && counter(c, `report:${day}:p${p.pid}`) >= k.perPlayerDay) throw dailyCapError(c, 'report');
    if (k.perAddrDay && ipk && counter(c, `report:${day}:a${ipk}`) >= k.perAddrDay) throw dailyCapError(c, 'report_address');
    const share = reportShareLimit(c.cfg);
    if (!share || counter(c, `report:${day}`) >= share) throw new ApiError(503, 'report_budget', 'problem reports are paused for today', { retryAfterSec: 3600 }, { 'retry-after': '3600' });
    bump(c, `report:${day}`);
    bump(c, `report:${day}:p${p.pid}`);
    if (ipk) bump(c, `report:${day}:a${ipk}`);
    return readStat(c);
  });
  const giveBack = () => c.tx(() => { bump(c, `report:${day}`, -1); bump(c, `report:${day}:p${p.pid}`, -1); if (ipk) bump(c, `report:${day}:a${ipk}`, -1); });

  let id = '';
  let sent = false; // true once the batch has been handed to D1: from then on a timeout does not prove the write failed
  try {
    await withTimeout(ensureTables(db, k.d1TimeoutMs), k.d1TimeoutMs);
    // Every delete says what it removed (RETURNING bytes): the running totals stay exact without a recount per report (a recount reads every row; the hourly sweep still does one).
    const stmts = [db.prepare(`DELETE FROM reports WHERE created < ? RETURNING bytes`).bind(c.now - k.keepMs)]; // expired
    const overRows = Math.max(0, st.n + 1 - k.maxRows);
    if (overRows) stmts.push(db.prepare(`DELETE FROM reports WHERE id IN (SELECT id FROM reports ORDER BY created ASC, rowid ASC LIMIT ?) RETURNING bytes`).bind(overRows));
    if (st.bytes + size > k.maxBytes) {
      stmts.push(db.prepare(`DELETE FROM reports WHERE id IN (SELECT id FROM (SELECT id, SUM(bytes) OVER (ORDER BY created DESC, rowid DESC) AS run FROM reports) WHERE run > ?) RETURNING bytes`).bind(Math.max(0, k.maxBytes - size)));
    }
    const nDel = stmts.length;
    id = b64url(randomBytes(6));
    stmts.push(db.prepare(`INSERT INTO reports (id, pid, created, kind, where_, app, note, check_json, stats_json, bytes, log) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
      .bind(id, p.pid, c.now, body.kind, body.where, appStr, note, checkJson, statsJson, size, log));
    sent = true;
    const res = await withTimeout(db.batch(stmts), k.d1TimeoutMs);
    const acc = { n: 0, bytes: 0, exact: true };
    for (let i = 0; i < nDel; i++) tally(acc, res[i]);
    // re-read the totals now: another report may have written them while this one waited for D1 (st above is only the pre-write snapshot)
    c.tx(() => { const cur = readStat(c); writeStat(c, { n: cur.n + 1 - acc.n, bytes: cur.bytes + size - acc.bytes }); });
    if (!acc.exact) { try { await withTimeout(recount(c), k.d1TimeoutMs); } catch { /* the hourly sweep recounts */ } }
  } catch (e) {
    // a batch that timed out may still land: keep the reservation (the day's counts stay up) so a retry cannot store a second copy past the limits
    if (!(sent && e && e.message === 'd1_timeout')) giveBack();
    if (e instanceof ApiError) throw e;
    throw new ApiError(503, 'report_storage', 'report storage is not available right now', { retryAfterSec: 600 }, { 'retry-after': '600' });
  }

  // Workers Logs: one structured line, then the masked log in chunks of at most 16 KB (seq counts from 1). Nothing here names a player, a token or an address.
  console.log(JSON.stringify({ evt: 'report', id, kind: body.kind, where: body.where, app: appStr, note, check, stats, log_bytes: byteLen(log) }));
  const chunks = chunkUtf8(log);
  chunks.forEach((text, i) => console.log(JSON.stringify({ evt: 'report_log', id, seq: i + 1, of: chunks.length, text })));
  return { id };
}

/** Hourly (the alarm): the 30-day expiry, a recount so the running totals cannot drift, and the old per-address counters. */
export async function sweepReports(c) {
  c.tx(() => c.db.run(`DELETE FROM counters WHERE key LIKE 'report:%' AND updated_ms < ?`, c.now - 2 * 86400_000));
  const db = c.reportsDb;
  if (!db) return;
  await ensureTables(db);
  await db.prepare(`DELETE FROM reports WHERE created < ?`).bind(c.now - c.cfg.reports.keepMs).run();
  await recount(c);
}

// Account deletion / expiry: the pids whose reports must go are remembered in the object (so a restart or a D1 failure loses nothing) and deleted from D1 as soon as it can.
// One counters row per pid ("purge:<pid>"), not one list: nothing is ever cut off (a list capped at N would silently forget the oldest ids during a long D1 outage), a row costs
// a few bytes in the object's own SQLite, and each pass takes at most PURGE_BATCH of them, so a backlog drains over several ticks instead of one huge D1 call.
// The counters sweeps (app.js) skip purge: rows: a pid waiting for its purge is never forgotten, however long D1 stays down.
const PURGE_BATCH = 200;
/** Called from purgeAccount (inside its transaction). Galaxy only. */
export function queuePurge(c, pid) {
  if (c.cfg.game !== 'smg' || !c.reportsDb) return;
  c.db.run(`INSERT INTO counters (key, value, updated_ms) VALUES (?, 1, ?) ON CONFLICT(key) DO UPDATE SET updated_ms = excluded.updated_ms`, `purge:${pid}`, c.now);
}
/** Deletes the queued players' reports from D1. Never throws; true when the queue is empty afterwards. */
export async function drainPurges(c) {
  const db = c.reportsDb;
  if (!db) return true;
  const pids = c.db.all(`SELECT key FROM counters WHERE key LIKE 'purge:%' ORDER BY updated_ms ASC LIMIT ?`, PURGE_BATCH).map((r) => Number(String(r.key).slice(6)));
  if (!pids.length) return true;
  try {
    await withTimeout(ensureTables(db, c.cfg.reports.d1TimeoutMs), c.cfg.reports.d1TimeoutMs);
    const acc = { n: 0, bytes: 0, exact: true };
    for (let i = 0; i < pids.length; i += 50) {
      const part = pids.slice(i, i + 50);
      const res = await withTimeout(db.batch(part.map((pid) => db.prepare(`DELETE FROM reports WHERE pid = ? RETURNING bytes`).bind(pid))), c.cfg.reports.d1TimeoutMs);
      for (const r of res) tally(acc, r);
      c.tx(() => { for (const pid of part) c.db.run(`DELETE FROM counters WHERE key = ?`, `purge:${pid}`); });
    }
    if (acc.n > 0) {
      if (acc.exact) c.tx(() => { const st = readStat(c); writeStat(c, { n: st.n - acc.n, bytes: st.bytes - acc.bytes }); });
      else await withTimeout(recount(c), c.cfg.reports.d1TimeoutMs);
    }
    return c.db.get(`SELECT COUNT(*) AS n FROM counters WHERE key LIKE 'purge:%'`).n === 0;
  } catch {
    return false;
  }
}

// ---------------------------------------------------------------- admin
const last4 = (pid) => String(pid).slice(-4);

/** GET /v1/admin/reports?since=<ms>: newest first, at most 100: id, created, kind, where, app, note, stored size in bytes. */
export async function listReports(c, query) {
  const since = query.get('since') === null ? 0 : Number(query.get('since'));
  if (!Number.isFinite(since) || since < 0) throw new ApiError(400, 'bad_request', 'since must be a time in ms');
  if (!c.reportsDb) throw new ApiError(503, 'report_unavailable', 'problem reports are not available on this service');
  await ensureTables(c.reportsDb);
  const { results } = await c.reportsDb.prepare(
    `SELECT id, created, kind, where_, app, note, bytes FROM reports WHERE created >= ? ORDER BY created DESC, rowid DESC LIMIT 100`,
  ).bind(since).all();
  return { reports: results.map((r) => ({ id: r.id, created: r.created, kind: r.kind, where: r.where_, app: r.app, note: r.note, bytes: r.bytes })) };
}

/** GET /v1/admin/reports/<id>: the whole row, the player id as its last 4 digits only. */
export async function getReport(c, id) {
  if (!c.reportsDb) throw new ApiError(503, 'report_unavailable', 'problem reports are not available on this service');
  await ensureTables(c.reportsDb);
  const r = await c.reportsDb.prepare(`SELECT * FROM reports WHERE id = ?`).bind(id).first();
  if (!r) throw new ApiError(404, 'not_found', 'no such report');
  const parse = (t) => { try { return t ? JSON.parse(t) : null; } catch { return null; } };
  return { id: r.id, pid: last4(r.pid), created: r.created, kind: r.kind, where: r.where_, app: r.app, note: r.note, check: parse(r.check_json), stats: parse(r.stats_json), log: r.log };
}

/** For the admin stats (the running totals are the object's; no D1 call). */
export function reportStats(c) {
  const st = readStat(c);
  return { stored: st.n, bytes: st.bytes, today: counter(c, `report:${dayKey(c)}`), shareToday: reportShareLimit(c.cfg), maxRows: c.cfg.reports.maxRows, maxBytes: c.cfg.reports.maxBytes, d1: !!c.reportsDb };
}
