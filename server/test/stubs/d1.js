// A stand-in for a Cloudflare D1 binding over node:sqlite, for the node suites (the worker suites use miniflare's real local D1).
// The part of the API the service uses: prepare(sql).bind(...).run() / all() / first(), and batch([...]) (one transaction).
// `fake.failing = true` makes every call reject (a D1 outage); `fake.hang = true` makes every call never settle (a stalled D1); `fake.calls` counts statements.
// A statement with RETURNING hands its rows back in `results` (as real D1 does).
import { DatabaseSync } from 'node:sqlite';

export function makeD1() {
  const db = new DatabaseSync(':memory:');
  const fake = { failing: false, calls: 0, raw: db, sqls: [] };
  const isRead = (q) => /^\s*(select|with|pragma)\b/i.test(q) || /\breturning\b/i.test(q);
  fake.hang = false; // true: every call never settles (a stalled D1)
  const exec = (q, args) => {
    fake.calls++; fake.sqls.push(q);
    if (fake.failing) throw new Error('D1_ERROR: the database is not available');
    const st = db.prepare(q);
    if (isRead(q)) return { results: st.all(...args).map((r) => ({ ...r })), success: true, meta: { changes: 0 } };
    const r = st.run(...args);
    return { results: [], success: true, meta: { changes: Number(r.changes), last_row_id: Number(r.lastInsertRowid) } };
  };
  const stmt = (q, args = []) => ({
    q, args,
    bind: (...a) => stmt(q, a),
    run: async () => { if (fake.hang) return new Promise(() => {}); return exec(q, args); },
    all: async () => { if (fake.hang) return new Promise(() => {}); return exec(q, args); },
    first: async (col) => { if (fake.hang) return new Promise(() => {}); const r = exec(q, args).results[0]; return r === undefined ? null : col ? r[col] : r; },
  });
  fake.prepare = (q) => stmt(q);
  fake.batch = async (list) => {
    if (fake.hang) return new Promise(() => {});
    if (fake.failing) { fake.calls++; throw new Error('D1_ERROR: the database is not available'); }
    db.exec('BEGIN');
    try { const out = list.map((s) => exec(s.q, s.args)); db.exec('COMMIT'); return out; } catch (e) { db.exec('ROLLBACK'); throw e; }
  };
  fake.rows = (q, ...a) => db.prepare(q).all(...a).map((r) => ({ ...r }));
  return fake;
}
