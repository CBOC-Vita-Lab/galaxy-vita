// Galaxy 3.0 beta: the session summary on the leave / close request (clamp, ring, daily aggregates, admin views, log line).
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { makeSmg, smgSignUp, roomOf } from '../smg-helpers.js';
import { clampSummary } from '../../src/core/summaries.js';

const ADMIN = 'test-admin-token-0123456789';
const DAY = 86400_000;
let lines; let spy;
beforeEach(() => { lines = []; spy = vi.spyOn(console, 'log').mockImplementation((x) => { lines.push(String(x)); }); });
afterEach(() => spy.mockRestore());

const SUM = { transport: 'udp', ended: 'leave', relay_used: false, dur_s: 600, members: 2, rx_rate: 59.5, gap_mean_ms: 20, gap_max_ms: 300, buf_mean_ms: 40, buf_max_ms: 90, extrap: 3, hold: 1, snap: 0, late: 2, stall100: 0, tx_fail: 0 };

async function setup(env = {}) {
  const s = makeSmg(env);
  const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb');
  const leave = async (summary, p = b, extra = {}) => {
    const code = await roomOf(s, [a, p]);
    return s.call('POST', `/v1/rooms/${code}/leave`, summary === undefined ? undefined : { summary, ...extra }, p.token);
  };
  const admin = (path) => s.raw('GET', path, undefined, ADMIN);
  return { s, a, b, leave, admin };
}

describe('clampSummary', () => {
  it('keeps known numbers clamped to their ranges, rounds to 2 decimals, and drops everything unknown', () => {
    const c = clampSummary({ ...SUM, dur_s: 1e9, members: -3, rx_rate: 12.3456, name: 'Aaa', ip: '1.2.3.4', gap_max_ms: 'x', extra: { a: 1 } }, '3.0.1');
    expect(c).toEqual({ transport: 'udp', dur_s: 86400, members: 0, rx_rate: 12.35, gap_mean_ms: 20, buf_mean_ms: 40, buf_max_ms: 90, extrap: 3, hold: 1, snap: 0, late: 2, stall100: 0, tx_fail: 0, ended: 'leave', relay_used: false, app: '3.0.1' });
  });
  it('needs transport udp or relay; ended must be leave, host_lost, kicked or error:<code>', () => {
    expect(clampSummary({ dur_s: 5 })).toBeNull();
    expect(clampSummary({ transport: 'tcp' })).toBeNull();
    expect(clampSummary('x')).toBeNull();
    expect(clampSummary([1])).toBeNull();
    expect(clampSummary({ transport: 'relay', ended: 'whatever' }).ended).toBeUndefined();
    expect(clampSummary({ transport: 'relay', ended: 'error:rx_timeout' }).ended).toBe('error:rx_timeout');
    expect(clampSummary({ transport: 'relay', ended: 'error:Bad Code' }).ended).toBeUndefined();
  });
});

describe('leave with a summary', () => {
  it('the leave works as before (with or without a summary, also on DELETE /v1/rooms/{code})', async () => {
    const t = await setup();
    expect((await t.leave(undefined)).data.ok).toBe(true);
    const r = await t.leave(SUM);
    expect(r.status).toBe(200);
    expect(r.data.ok).toBe(true);
    const code = await roomOf(t.s, [t.a, t.b]);
    const d = await t.s.call('DELETE', `/v1/rooms/${code}`, undefined, t.b.token);
    expect(d.status).toBe(200);
  });
  it('an unusable summary is ignored (the leave still succeeds, nothing stored); an unknown field next to it is 400', async () => {
    const t = await setup();
    for (const bad of ['text', 5, [1], { transport: 'tcp' }, null]) expect((await t.leave(bad)).status).toBe(200);
    expect(t.s.d1.calls).toBe(0); // nothing reached the database
    const r = await t.leave(SUM, t.b, { name: 'x' });
    expect(r.status).toBe(400);
  });
  it('stores the clamped summary in the ring, no player id, name or address anywhere in the row', async () => {
    const t = await setup();
    await t.leave({ ...SUM, name: 'Bbb', ip: '203.0.113.5' });
    const rows = t.s.d1.rows(`SELECT * FROM summaries`);
    expect(rows).toHaveLength(1);
    expect(rows[0]).toMatchObject({ transport: 'udp', ended: 'leave', app: '3.0.0' });
    const all = JSON.stringify(rows);
    for (const x of [String(t.b.pid), 'Bbb', '203.0.113.5', t.b.token, t.b.code]) expect(all).not.toContain(x);
  });
  it('the ring keeps at most SUMMARY_RING rows (the newest)', async () => {
    const t = await setup({ SUMMARY_RING: '5' });
    for (let i = 0; i < 8; i++) await t.leave({ ...SUM, dur_s: 10 + i });
    const rows = t.s.d1.rows(`SELECT data FROM summaries ORDER BY id`).map((r) => JSON.parse(r.data).dur_s);
    expect(rows).toEqual([13, 14, 15, 16, 17]);
    const l = (await t.admin('/v1/admin/summaries')).data.summaries;
    expect(l.map((x) => x.dur_s)).toEqual([17, 16, 15, 14, 13]); // newest first
  });
  it('per UTC day and transport aggregates (count, sums, maxima, ended counts) show in admin stats', async () => {
    const t = await setup();
    await t.leave({ ...SUM, dur_s: 100, gap_max_ms: 50, relay_used: false });
    await t.leave({ ...SUM, dur_s: 200, gap_max_ms: 90, ended: 'host_lost', relay_used: true });
    await t.leave({ transport: 'relay', dur_s: 7, ended: 'leave' });
    t.s.advance(DAY);
    await t.leave({ ...SUM, dur_s: 1 });
    const st = (await t.admin('/v1/admin/stats')).data.summaries;
    expect(st.available).toBe(true);
    expect(st.ring).toBe(4);
    const days = Object.keys(st.days).sort();
    expect(days).toHaveLength(2);
    const d0 = st.days[days[0]];
    expect(d0.udp.count).toBe(2);
    expect(d0.udp.sum.dur_s).toBe(300);
    expect(d0.udp.max.gap_max_ms).toBe(90);
    expect(d0.udp.sum.relay_used).toBe(1);
    expect(d0.udp.ended).toEqual({ leave: 1, host_lost: 1 });
    expect(d0.relay.count).toBe(1);
    expect(st.days[days[1]].udp.count).toBe(1);
  });
  it('many distinct error codes do not grow the aggregate without bound', async () => {
    const t = await setup({ SUMMARY_MAX_PER_PLAYER_DAY: '100' });
    for (let i = 0; i < 30; i++) await t.leave({ transport: 'udp', ended: `error:${String.fromCharCode(97 + (i % 26))}${String.fromCharCode(97 + Math.floor(i / 26))}` });
    const st = (await t.admin('/v1/admin/stats')).data.summaries;
    const ended = Object.values(st.days)[0].udp.ended;
    expect(Object.keys(ended).length).toBeLessThanOrEqual(25);
    expect(ended['error:other']).toBe(6);
  });
  it('a D1 outage never fails the leave', async () => {
    const t = await setup();
    t.s.d1.failing = true;
    expect((await t.leave(SUM)).status).toBe(200);
  });
  it('without the D1 binding the summary is only logged; admin summaries say 503', async () => {
    const t = await setup({ REPORTS_DB: null });
    expect((await t.leave(SUM)).status).toBe(200);
    expect(lines.some((l) => l.includes('"evt":"summary"'))).toBe(true);
    expect((await t.admin('/v1/admin/summaries')).status).toBe(503);
    expect((await t.admin('/v1/admin/stats')).data.summaries).toEqual({ available: false });
  });
  it('logs one {evt:"summary"} line of the clamped fields only', async () => {
    const t = await setup();
    await t.leave({ ...SUM, name: 'Bbb' });
    const l = lines.map((x) => { try { return JSON.parse(x); } catch { return null; } }).filter((x) => x && x.evt === 'summary');
    expect(l).toHaveLength(1);
    expect(l[0]).toMatchObject({ evt: 'summary', transport: 'udp', dur_s: 600, ended: 'leave' });
    expect(JSON.stringify(l[0])).not.toMatch(/Bbb|pid|token/);
  });
  it('admin summaries needs the admin token', async () => {
    const t = await setup();
    expect((await t.s.raw('GET', '/v1/admin/summaries')).status).toBe(401);
    expect((await t.s.call('GET', '/v1/admin/summaries', undefined, t.a.token)).status).toBe(401);
  });
});
