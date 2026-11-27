// Round f (package l): report totals, lost updates, summaries after the room closed, caller-side timeouts, the alarm's sweeps, the launch-week TURN setting.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { makeSmg, smgSignUp, roomOf } from '../smg-helpers.js';

const ADMIN = 'test-admin-token-0123456789';
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
let lines; let spy;
beforeEach(() => { lines = []; spy = vi.spyOn(console, 'log').mockImplementation((x) => { lines.push(String(x)); }); });
afterEach(() => spy.mockRestore());
const body = (o = {}) => ({ v: 1, kind: 'user', where: 'settings', log: 'line', ...o });
const stats = async (s) => (await s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data.reports;

describe('report totals and reservations', () => {
  it('a delete that returns no rows but meta.changes > 0 is still counted: the totals are marked inexact and recounted', async () => {
    const s = makeSmg({ REPORT_MAX_ROWS: '3', REPORT_MAX_PER_PLAYER_DAY: '99', REPORT_MAX_PER_ADDR_DAY: '99' });
    const a = await smgSignUp(s, 'Rep');
    const real = s.d1.batch;
    s.d1.batch = async (list) => (await real(list)).map((r) => (r.results.length ? { ...r, results: [], meta: { ...r.meta, changes: r.results.length } } : r)); // a D1 that hides the RETURNING rows
    for (let i = 0; i < 7; i++) { expect((await s.call('POST', '/v1/report', body({ log: `entry ${i} ` + 'ab '.repeat(i + 1) }), a.token)).status).toBe(201); s.advance(1000); }
    const rows = s.d1.rows('SELECT COUNT(*) AS n, SUM(bytes) AS b FROM reports')[0];
    expect(rows.n).toBe(3);
    const st = await stats(s);
    expect(st.stored).toBe(3);
    expect(st.bytes).toBe(rows.b);
  });
  it('two interleaved reports both count (the totals are re-read inside the final write, no lost update)', async () => {
    const s = makeSmg({ REPORT_MAX_PER_PLAYER_DAY: '9' });
    const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb'); const c = await smgSignUp(s, 'Ccc');
    expect((await s.call('POST', '/v1/report', body(), a.token)).status).toBe(201); // warms the tables
    const real = s.d1.batch; let arrived = 0; let open; const both = new Promise((r) => { open = r; });
    s.d1.batch = async (list) => { if (++arrived >= 2) open(); await Promise.race([both, wait(500)]); return real(list); }; // both reports have read the totals before either writes
    const rs = await Promise.all([b, c, a].map((p) => s.call('POST', '/v1/report', body({ log: 'x '.repeat(50) }), p.token)));
    for (const r of rs) expect(r.status).toBe(201);
    const row = s.d1.rows('SELECT COUNT(*) AS n, SUM(bytes) AS b FROM reports')[0];
    s.d1.batch = real;
    expect(row.n).toBe(4);
    const st = await stats(s);
    expect(st.stored).toBe(4);
    expect(st.bytes).toBe(row.b);
  });
  it('a batch that timed out KEEPS the reservation (a late success cannot be followed by a second stored copy past the limits); other failures give it back', async () => {
    const s = makeSmg({ REPORT_D1_TIMEOUT_MS: '50', REPORT_MAX_PER_PLAYER_DAY: '1', REPORT_MAX_PER_ADDR_DAY: '99' });
    const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb');
    expect((await s.call('POST', '/v1/report', body(), b.token)).status).toBe(201); // warms the tables
    const real = s.d1.batch;
    s.d1.batch = () => new Promise(() => {}); // the write is handed to D1 and never answers in time
    const r = await s.call('POST', '/v1/report', body(), a.token);
    expect(r.status).toBe(503); expect(r.data.error.code).toBe('report_storage');
    s.d1.batch = real;
    expect((await s.call('POST', '/v1/report', body(), a.token)).status).toBe(429); // still counted: its one report of the day
    // a failure BEFORE the write (D1 down at once) gives the limit back
    const d = await smgSignUp(s, 'Ddd');
    s.d1.failing = true;
    expect((await s.call('POST', '/v1/report', body(), d.token)).status).toBe(503);
    s.d1.failing = false;
    expect((await s.call('POST', '/v1/report', body(), d.token)).status).toBe(201);
  });
  it('a purge: row is not forgotten by the 40-day counters sweep, an old ordinary counter is', async () => {
    const s = makeSmg();
    const old = s.clock.now - 50 * 86400_000;
    s.store.sql.exec(`INSERT INTO counters (key, value, updated_ms) VALUES ('purge:4242', 1, ?)`, old);
    s.store.sql.exec(`INSERT INTO counters (key, value, updated_ms) VALUES ('zz:old', 1, ?)`, old);
    s.app.sweep();
    const keys = s.store.sql.exec(`SELECT key FROM counters WHERE key IN ('purge:4242', 'zz:old')`).toArray().map((r) => r.key);
    expect(keys).toEqual(['purge:4242']);
  });
  it('the alarm\'s hourly sweeps are bounded: a hung D1 does not hold turnTick', async () => {
    const s = makeSmg({ REPORT_D1_TIMEOUT_MS: '60' });
    await smgSignUp(s, 'Aaa');
    s.advance(3600_000 + 1);
    s.d1.hang = true;
    const t0 = Date.now();
    await s.app.turnTick();
    expect(Date.now() - t0).toBeLessThan(1500);
    s.d1.hang = false;
  });
});

describe('summaries after the room closed, kicked, and the caller-side time limit', () => {
  const SUM = { transport: 'udp', ended: 'leave', dur_s: 10 };
  let n = 0;
  /** a fresh host and guest in their own room (an account is in one room at a time) */
  async function pair(s) {
    n++;
    const h = await smgSignUp(s, `Hst${n}`); const g = await smgSignUp(s, `Gst${n}`);
    const code = await roomOf(s, [h, g]);
    const gpid = s.store.sql.exec(`SELECT pid FROM room_members WHERE code = ? ORDER BY slot DESC LIMIT 1`, code).toArray()[0].pid;
    return { h, g, code, gpid };
  }
  const ended = (s) => s.d1.rows('SELECT data FROM summaries ORDER BY id').map((r) => JSON.parse(r.data).ended);
  const leave = (s, p, code, summary) => s.call('POST', `/v1/rooms/${code}/leave`, { summary }, p.token);
  it('host_lost sent after the room closed is kept (once per room)', async () => {
    const s = makeSmg();
    const { g, code } = await pair(s);
    s.store.sql.exec(`UPDATE rooms SET state = 'closed' WHERE code = ?`, code);
    const r = await leave(s, g, code, { ...SUM, ended: 'host_lost' });
    expect(r.status).toBe(200); expect(r.data.state).toBe('closed');
    expect(ended(s)).toEqual(['host_lost']);
    await leave(s, g, code, { ...SUM, ended: 'host_lost' });
    expect(ended(s)).toEqual(['host_lost']); // the once-per-room key meters it
  });
  it('kicked: the member row is gone (403 not_member) but the summary of a former member is kept, once; a stranger or a moved account records nothing', async () => {
    const s = makeSmg();
    const { g, code, gpid } = await pair(s);
    s.store.sql.exec(`DELETE FROM room_members WHERE code = ? AND pid = ?`, code, gpid);
    s.store.sql.exec(`INSERT INTO room_left (code, pid, slot, left_ms, reason, accepted) VALUES (?, ?, 1, ?, 'lost', 1)`, code, gpid, s.clock.now);
    const r = await leave(s, g, code, { ...SUM, ended: 'kicked' });
    expect(r.status).toBe(403);
    expect(ended(s)).toEqual(['kicked']);
    expect((await leave(s, g, code, { ...SUM, ended: 'kicked' })).status).toBe(403);
    expect(ended(s)).toEqual(['kicked']);
    // a stranger (never in the room): nothing, and no D1 call
    const x = await smgSignUp(s, 'Strg'); const calls = s.d1.calls;
    expect((await leave(s, x, code, SUM)).status).toBe(403);
    expect(s.d1.calls).toBe(calls);
    // an account that moved to another room: that summary belongs to the other room
    const p2 = await pair(s);
    s.store.sql.exec(`DELETE FROM room_members WHERE code = ? AND pid = ?`, p2.code, p2.gpid);
    s.store.sql.exec(`INSERT INTO room_left (code, pid, slot, left_ms, reason, accepted) VALUES (?, ?, 1, ?, 'moved', 1)`, p2.code, p2.gpid, s.clock.now);
    expect((await leave(s, p2.g, p2.code, SUM)).status).toBe(409);
    expect(ended(s)).toEqual(['kicked']);
  });
  it('a stalled D1: every queued leave waits at most SUMMARY_TIMEOUT_MS from when it enqueued, not k times that', async () => {
    const s = makeSmg({ SUMMARY_TIMEOUT_MS: '200' });
    const ps = [await pair(s), await pair(s), await pair(s)];
    s.d1.hang = true;
    const t0 = Date.now();
    const rs = await Promise.all(ps.map((p) => leave(s, p.g, p.code, SUM)));
    for (const r of rs) expect(r.status).toBe(200);
    expect(Date.now() - t0).toBeLessThan(480); // started from the job start in the chain it would be about 600
    s.d1.hang = false;
    await wait(900);
  });
  it('more than 3 stores waiting behind a slow D1: the rest are dropped at once', async () => {
    const s = makeSmg({ SUMMARY_TIMEOUT_MS: '200' });
    const ps = []; for (let i = 0; i < 8; i++) ps.push(await pair(s));
    s.d1.hang = true;
    const rs = await Promise.all(ps.map((p) => leave(s, p.g, p.code, SUM)));
    for (const r of rs) expect(r.status).toBe(200);
    s.d1.hang = false;
    await wait(1500);
    expect(s.d1.rows('SELECT COUNT(*) AS n FROM summaries')[0].n).toBeLessThanOrEqual(4); // the running one and 3 waiting
  });
  it('a store that timed out and lands late cannot overwrite a later one: the day\'s aggregate holds both', async () => {
    const s = makeSmg({ SUMMARY_TIMEOUT_MS: '100' });
    const p1 = await pair(s); const p2 = await pair(s);
    await leave(s, p1.g, p1.code, SUM); // creates the tables (and the row for day, udp)
    await wait(50);
    let release; const gate = new Promise((r) => { release = r; });
    let armed = true;
    const realPrepare = s.d1.prepare;
    s.d1.prepare = (q) => {
      const st = realPrepare(q);
      if (armed && /FROM summary_days WHERE day/.test(q)) { armed = false; return { ...st, bind: (...a) => { const b2 = st.bind(...a); return { ...b2, first: async (col) => { const v = await b2.first(col); await gate; return v; } }; } }; }
      return st;
    };
    const p3 = await pair(s);
    expect((await leave(s, p2.g, p2.code, { ...SUM, dur_s: 20 })).status).toBe(200); // stuck at its read, given up on after 100 ms
    await wait(150);
    expect((await leave(s, p3.g, p3.code, { ...SUM, dur_s: 30 })).status).toBe(200); // stored normally
    expect(s.d1.rows('SELECT n FROM summary_days')[0].n).toBe(2);
    release(); await wait(250); // the late one resumes with its stale read: its guarded write changes nothing, it merges again
    const row = s.d1.rows('SELECT n, sums FROM summary_days')[0];
    expect(row.n).toBe(3);
    expect(JSON.parse(row.sums).dur_s).toBe(60);
  });
});

describe('launch week: TURN_NEW_ACCOUNT_DAILY', () => {
  it('wrangler.smg.toml sets 200 and the config reads it (the default stays 60)', () => {
    const toml = readFileSync(new URL('../../wrangler.smg.toml', import.meta.url), 'utf8');
    const m = toml.match(/^TURN_NEW_ACCOUNT_DAILY = "(\d+)"/m);
    expect(m && m[1]).toBe('200');
    expect(makeSmg({ TURN_NEW_ACCOUNT_DAILY: m[1] }).app.cfg.turn.newAccountDaily).toBe(200);
    expect(makeSmg().app.cfg.turn.newAccountDaily).toBe(60);
  });
});
