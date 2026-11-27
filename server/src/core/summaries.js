// Galaxy 3.0 beta: the session summary a console may attach to its room leave / close request (POST /v1/rooms/:code/leave, DELETE /v1/rooms/:code),
// in the optional field "summary": how the link behaved, in numbers. No new request. NOTHING that names the player is stored with it: no player id, no
// name, no address. Numbers are clamped to a sane range and unknown keys are dropped; a summary that cannot be understood is ignored (leaving a room never fails
// because of it). The service keeps a ring of the last 500 summaries and, per UTC day and transport, the count, the sum and the maximum of every number and the
// count of every "ended" value. They live in the D1 database bound as REPORTS_DB (tables summaries and summary_days). One structured line {evt:"summary", ...} goes to Workers Logs for each.

import { ApiError, VERSION_RE } from './util.js';
import { ensureTables, withTimeout } from './reports.js';

const changesOf = (res) => (res && res.meta && typeof res.meta.changes === 'number' ? res.meta.changes : 0);
const MAX_QUEUED = 3; // more than this many stores waiting behind the running one: the summary is dropped (leaving a room never waits for a backlog)

/** field -> [min, max] */
export const NUMBER_FIELDS = {
  dur_s: [0, 86400],
  members: [0, 4],
  rx_rate: [0, 10000],
  gap_mean_ms: [0, 60000],
  gap_max_ms: [0, 600000],
  buf_mean_ms: [0, 60000],
  buf_max_ms: [0, 600000],
  extrap: [0, 1_000_000],
  hold: [0, 1_000_000],
  snap: [0, 1_000_000],
  late: [0, 1_000_000],
  stall100: [0, 1_000_000],
  tx_fail: [0, 1_000_000],
};
export const TRANSPORTS = ['udp', 'relay'];
const ENDED_RE = /^(?:leave|host_lost|kicked|error:[a-z_]{1,24})$/; // letters and underscores only: no digit or dot pattern (an address) can ride in it
const MAX_ENDED_KEYS = 24;
const KEEP_DAYS = 60;

/** The summary as it will be stored, or null when it is not an object with a transport. Never throws. */
export function clampSummary(raw, headerApp) {
  if (typeof raw !== 'object' || raw === null || Array.isArray(raw)) return null;
  if (!TRANSPORTS.includes(raw.transport)) return null;
  const out = { transport: raw.transport };
  for (const [k, [lo, hi]] of Object.entries(NUMBER_FIELDS)) {
    const v = raw[k];
    if (typeof v === 'number' && Number.isFinite(v)) out[k] = Math.round(Math.min(hi, Math.max(lo, v)) * 100) / 100;
  }
  if (typeof raw.ended === 'string' && ENDED_RE.test(raw.ended)) out.ended = raw.ended;
  if (typeof raw.relay_used === 'boolean') out.relay_used = raw.relay_used;
  const app = typeof raw.app === 'string' && VERSION_RE.test(raw.app) ? raw.app : VERSION_RE.test(String(headerApp || '')) ? String(headerApp) : '';
  if (app) out.app = app;
  return out;
}

const dayStr = (now) => new Date(now).toISOString().slice(0, 10);

// The writes of one object are queued one after another (the daily aggregate is guarded against interleaving anyway, see store). Each queued write is bounded in time from the moment it
// starts, so a stalled D1 cannot hold the queue; the caller bounds its own wait from the moment it enqueues (recordSummary), and at most MAX_QUEUED + 1 are outstanding.
let chain = Promise.resolve();
let pending = 0;

async function store(c, s) {
  const db = c.reportsDb;
  await ensureTables(db, c.cfg.reports.summaryTimeoutMs);
  const keep = c.cfg.reports.summaryRing;
  await db.batch([
    db.prepare(`INSERT INTO summaries (created, transport, ended, app, data) VALUES (?, ?, ?, ?, ?)`).bind(c.now, s.transport, s.ended || '', s.app || '', JSON.stringify(s)),
    db.prepare(`DELETE FROM summaries WHERE id <= (SELECT MAX(id) FROM summaries) - ?`).bind(keep),
  ]);
  // per day and transport: n, sum and max of every number (relay_used as 0 / 1), and the count of every ended value.
  // Read, merge, write back guarded by the n that was read (UPDATE ... WHERE n = <read>): a store that was given up on (its caller timed out) and lands late cannot overwrite a newer one,
  // its guarded write then changes nothing and it merges again on top of the newer row. Bounded retries; nothing is counted twice because only one attempt can succeed.
  const day = dayStr(c.now);
  const nums = { ...Object.fromEntries(Object.keys(NUMBER_FIELDS).filter((k) => k in s).map((k) => [k, s[k]])), ...('relay_used' in s ? { relay_used: s.relay_used ? 1 : 0 } : {}) };
  for (let attempt = 0; attempt < 4; attempt++) {
    const cur = await db.prepare(`SELECT n, sums, maxs, ended FROM summary_days WHERE day = ? AND transport = ?`).bind(day, s.transport).first();
    const sums = cur ? JSON.parse(cur.sums) : {};
    const maxs = cur ? JSON.parse(cur.maxs) : {};
    const ended = cur ? JSON.parse(cur.ended) : {};
    for (const [k, v] of Object.entries(nums)) {
      sums[k] = Math.round(((sums[k] || 0) + v) * 100) / 100;
      maxs[k] = Math.max(maxs[k] === undefined ? v : maxs[k], v);
    }
    if (s.ended) {
      const key = s.ended in ended || Object.keys(ended).length < MAX_ENDED_KEYS ? s.ended : 'error:other';
      ended[key] = (ended[key] || 0) + 1;
    }
    const res = cur
      ? await db.prepare(`UPDATE summary_days SET n = ?, sums = ?, maxs = ?, ended = ? WHERE day = ? AND transport = ? AND n = ?`)
        .bind(cur.n + 1, JSON.stringify(sums), JSON.stringify(maxs), JSON.stringify(ended), day, s.transport, cur.n).run()
      : await db.prepare(`INSERT INTO summary_days (day, transport, n, sums, maxs, ended) VALUES (?, ?, 1, ?, ?, ?) ON CONFLICT(day, transport) DO NOTHING`)
        .bind(day, s.transport, JSON.stringify(sums), JSON.stringify(maxs), JSON.stringify(ended)).run();
    if (changesOf(res) > 0) return;
  }
}

const cnt = (c, key) => c.db.get(`SELECT value FROM counters WHERE key = ?`, key)?.value || 0;
const bumpCnt = (c, key, by = 1) => c.db.run(`INSERT INTO counters (key, value, updated_ms) VALUES (?, ?, ?) ON CONFLICT(key) DO UPDATE SET value = value + excluded.value, updated_ms = excluded.updated_ms`, key, by, c.now);

/**
 * Stores one summary (ring + daily aggregate) and logs it. Never throws: a summary that cannot be understood, is over a limit or cannot be stored in time is dropped
 * (leaving a room must not fail or wait because of it). The limits are the object's counters, taken BEFORE D1 is called: SUMMARY_MAX_PER_DAY in all, SUMMARY_MAX_PER_PLAYER_DAY
 * a player, and ONE per player and room (`who` = {pid, code}). A summary over a limit is not stored and not logged. Returns the clamped summary, or null.
 */
export async function recordSummary(c, raw, headerApp, who) {
  const s = clampSummary(raw, headerApp);
  if (!s) return null;
  const k = c.cfg.reports;
  const day = dayStr(c.now);
  if (who) {
    const ok = c.tx(() => {
      const roomKey = `summary:room:${who.code}:p${who.pid}`;
      if (cnt(c, roomKey) >= 1) return false;
      if (!k.summaryMaxPerDay || cnt(c, `summary:${day}`) >= k.summaryMaxPerDay) return false;
      if (!k.summaryPerPlayerDay || cnt(c, `summary:${day}:p${who.pid}`) >= k.summaryPerPlayerDay) return false;
      bumpCnt(c, roomKey); bumpCnt(c, `summary:${day}`); bumpCnt(c, `summary:${day}:p${who.pid}`);
      return true;
    });
    if (!ok) return null;
  }
  console.log(JSON.stringify({ evt: 'summary', ...s })); // Workers Logs: the clamped numbers only (no player id, name or address exists here)
  if (!c.reportsDb) return s;
  if (pending > MAX_QUEUED) return s; // a backlog behind a slow D1: dropped (already logged and metered)
  pending++;
  const job = chain.then(() => withTimeout(store(c, s), k.summaryTimeoutMs));
  const settled = job.catch(() => {}).finally(() => { pending--; });
  chain = settled;
  try { await withTimeout(job, k.summaryTimeoutMs); } catch { /* dropped (an error or the time limit, counted from the enqueue): the leave answers anyway */ }
  return s;
}

/** The leave / close request body: nothing, or {summary}. Returns the raw summary (or undefined). */
export function summaryOf(body) {
  if (body === undefined) return undefined;
  if (typeof body !== 'object' || body === null || Array.isArray(body)) throw new ApiError(400, 'bad_request', 'JSON object expected');
  for (const k of Object.keys(body)) if (k !== 'summary') throw new ApiError(400, 'unknown_field', 'unexpected field', { field: k.slice(0, 32) });
  return body.summary;
}

export async function sweepSummaries(c) {
  if (!c.reportsDb) return;
  await ensureTables(c.reportsDb);
  await c.reportsDb.prepare(`DELETE FROM summary_days WHERE day < ?`).bind(dayStr(c.now - KEEP_DAYS * 86400_000)).run();
}

/** GET /v1/admin/summaries: the ring, newest first. */
export async function listSummaries(c) {
  if (!c.reportsDb) throw new ApiError(503, 'report_unavailable', 'summaries are not available on this service');
  await ensureTables(c.reportsDb);
  const { results } = await c.reportsDb.prepare(`SELECT id, created, data FROM summaries ORDER BY id DESC LIMIT ?`).bind(c.cfg.reports.summaryRing).all();
  return { summaries: results.map((r) => ({ id: r.id, created: r.created, ...JSON.parse(r.data) })) };
}

/** For the admin stats: the aggregates of the last `days` UTC days, by day and transport. */
export async function summaryAggregates(c, days = 14) {
  if (!c.reportsDb) return { available: false };
  try {
    await ensureTables(c.reportsDb);
    const from = dayStr(c.now - (days - 1) * 86400_000);
    const { results } = await c.reportsDb.prepare(`SELECT day, transport, n, sums, maxs, ended FROM summary_days WHERE day >= ? ORDER BY day DESC, transport`).bind(from).all();
    const out = {};
    for (const r of results) (out[r.day] ||= {})[r.transport] = { count: r.n, sum: JSON.parse(r.sums), max: JSON.parse(r.maxs), ended: JSON.parse(r.ended) };
    const ring = (await c.reportsDb.prepare(`SELECT COUNT(*) AS n FROM summaries`).first()).n;
    return { available: true, ring, days: out };
  } catch {
    return { available: false };
  }
}
