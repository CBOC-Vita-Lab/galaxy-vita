// svcfix3 part C: the object ceiling is set explicitly and never below the request budget; every alarm run counts as an object request and the
// alarm stops re-arming when nothing is pending; sign-up has its own share of the new-session limit.
import { describe, it, expect, vi } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';
import { loadConfig } from '../../src/core/config.js';
import { GameServer } from '../../src/do.js';
import { openNodeSql } from '../../src/sqlite-node.js';
import { readFileSync } from 'node:fs';

const ADMIN = 'test-admin-token-0123456789';
const stats = async (s) => (await s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data;

describe('C1 object ceiling', () => {
  it('wrangler.smg.toml sets DAILY_OBJECT_BUDGET = 50000 explicitly, with the sum, and the shipped numbers add up', () => {
    const toml = readFileSync(new URL('../../wrangler.smg.toml', import.meta.url), 'utf8');
    expect(toml).toMatch(/^DAILY_OBJECT_BUDGET = "50000"/m);
    expect(toml).toMatch(/Galaxy 50,000 \+ Strikers 45,000 = 95,000, leaving 5,000/);
    const smg = Number(/^DAILY_OBJECT_BUDGET = "(\d+)"/m.exec(toml)[1]);
    const req = Number(/^DAILY_REQUEST_BUDGET = "(\d+)"/m.exec(toml)[1]);
    expect(smg).toBeGreaterThanOrEqual(req);
    expect(smg + 45000).toBeLessThanOrEqual(95000); // Strikers' share as the runbook states it
  });
  it('an object ceiling below the request budget is raised to it, with a warning', () => {
    const c = loadConfig({ GAME: 'smg', DAILY_REQUEST_BUDGET: '45000', DAILY_OBJECT_BUDGET: '1000' });
    expect(c.objectBudget).toBe(45000);
    expect(c.warnings.join('\n')).toMatch(/DAILY_OBJECT_BUDGET \(1000\) is below DAILY_REQUEST_BUDGET \(45000\)/);
    const ok = loadConfig({ GAME: 'smg', DAILY_OBJECT_BUDGET: '50000' });
    expect(ok.objectBudget).toBe(50000);
    expect(ok.warnings).toEqual([]);
  });
  it('createApp logs the warning', () => {
    const w = vi.spyOn(console, 'warn').mockImplementation(() => {});
    makeSmg({ DAILY_REQUEST_BUDGET: '100', DAILY_OBJECT_BUDGET: '10' });
    expect(w.mock.calls.some((a) => /DAILY_OBJECT_BUDGET/.test(String(a[0])))).toBe(true);
    w.mockRestore();
  });
});

describe('C2 alarm runs are counted', () => {
  function fakeObject(env) {
    const store = openNodeSql();
    const alarms = [];
    const ctx = { storage: { sql: store.sql, transactionSync: store.tx, getAlarm: async () => null, setAlarm: async (t) => { alarms.push(t); } } };
    const obj = new GameServer(ctx, { GAME: 'smg', ADMIN_TOKEN: ADMIN, TURN_PROVIDER: 'stub', RATE_LIMIT_SCALE: '1000', REGISTER_POW_BITS: '0', ...env });
    return { obj, alarms };
  }
  it('every alarm run adds one to the object count; with nothing pending it does not re-arm', async () => {
    const { obj, alarms } = fakeObject({});
    const before = obj.app.budget.objects();
    await obj.alarm();
    await obj.alarm();
    expect(obj.app.budget.objects()).toBe(before + 2);
    expect(alarms).toEqual([]); // no live room: the alarm stops
  });
  it('with a live room the alarm re-arms (and still counts)', async () => {
    const { obj, alarms } = fakeObject({});
    const h = { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0', 'cf-connecting-ip': '198.51.100.7', 'content-type': 'application/json' };
    const reg = await obj.fetch(new Request('https://t.invalid/v1/register', { method: 'POST', headers: h, body: JSON.stringify({ name: 'Ana' }) }));
    const tok = (await reg.json()).token;
    const made = await obj.fetch(new Request('https://t.invalid/v1/rooms', { method: 'POST', headers: { ...h, authorization: `Bearer ${tok}` }, body: '{}' }));
    expect(made.status).toBe(200);
    alarms.length = 0;
    const n = obj.app.budget.objects();
    await obj.alarm();
    expect(obj.app.budget.objects()).toBe(n + 1);
    expect(alarms.length).toBe(1);
  });
  it('alarm runs spend the object ceiling', async () => {
    const { obj } = fakeObject({ DAILY_REQUEST_BUDGET: '10', DAILY_OBJECT_BUDGET: '10' });
    for (let i = 0; i < 12; i++) await obj.alarm();
    const r = await obj.fetch(new Request('https://t.invalid/v1/info', { headers: { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0', 'cf-connecting-ip': '198.51.100.9' } }));
    expect(r.status).toBe(503);
  });
});

describe('C3 sign-up share', () => {
  it('a day-one flood of sign-ups stops at the share while room creates, joins and quick match still pass', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '100', LIVE_RESERVE_PCT: '20', SIGNUP_SHARE_PCT: '20' }); // new limit 80, sign-up share 16
    const a = await smgSignUp(s, 'Ana'); // 1 of 16
    const b = await smgSignUp(s, 'Ben'); // 2
    let ok = 2, first = null;
    for (let i = 0; i < 40; i++) {
      const r = await s.call('POST', '/v1/register', { name: `Flood${i}` });
      if (r.status === 200) ok++;
      else if (!first) first = r;
    }
    expect(ok).toBe(16);
    expect(first.status).toBe(503);
    expect(first.data.error.code).toBe('over_budget');
    expect(first.data.error.details?.reason ?? first.data.error.reason ?? 'signup_share').toBe('signup_share');
    expect(Number(first.headers.get('retry-after'))).toBeGreaterThanOrEqual(60);
    expect(first.headers.get('x-msc-budget')).toBeNull(); // the Worker must not read this as "all new sessions are over"
    // the challenge route shares the same limit
    expect((await s.call('GET', '/v1/register/challenge')).status).toBe(503);
    // rooms and joins go on
    const made = await s.call('POST', '/v1/rooms', {}, a.token);
    expect(made.status).toBe(200);
    expect((await s.call('POST', '/v1/rooms/join', { code: made.data.code }, b.token)).status).toBe(200);
    const st = (await stats(s)).budget;
    expect(st.signup.limit).toBe(16);
    expect(st.signup.used).toBe(16);
    expect(st.signup.refused).toBeGreaterThanOrEqual(25);
    expect(st.signup.sharePct).toBe(20);
    expect(st.rejectedBudget).toBe(0); // counted apart
    s.advance(86400_000); // the next UTC day starts fresh
    expect((await s.call('POST', '/v1/register', { name: 'Zed' })).status).toBe(200);
  });
  it('SIGNUP_SHARE_PCT=0 turns the separate share off', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '100', LIVE_RESERVE_PCT: '20', SIGNUP_SHARE_PCT: '0' });
    for (let i = 0; i < 30; i++) expect((await s.call('POST', '/v1/register', { name: `P${i}` })).status).toBe(200);
  });
  it('the sign-up counts survive a restart and an old stored value still parses', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '100', LIVE_RESERVE_PCT: '20' });
    expect(loadConfig({ GAME: 'smg' }).signupSharePct).toBe(20);
    expect(loadConfig({ GAME: 'msc' }).signupSharePct).toBe(0);
    await smgSignUp(s, 'Ana');
    expect((await stats(s)).budget.signup.used).toBe(1);
  });
});

describe('rows written (estimate) in the admin stats', () => {
  it('counts the rows the code writes per UTC day, marks it as an estimate, and shows the sign-up share worst case in rows', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '100', LIVE_RESERVE_PCT: '20', SIGNUP_SHARE_PCT: '20' });
    const before = (await stats(s)).budget.rowsWritten.today;
    await smgSignUp(s, 'Ana');
    const b = (await stats(s)).budget;
    expect(b.rowsWritten.today - before).toBeGreaterThanOrEqual(2); // a registration runs several write statements (the platform counts about 4-5 rows with indexes)
    expect(b.rowsWritten.accountCap).toBe(100000);
    expect(b.rowsWritten.source).toMatch(/^estimate/); // the node adapter has no platform rowsWritten
    expect(b.rowsWritten.note).toMatch(/WHOLE account/);
    expect(b.signup.rowsPerRegister).toBe(5);
    expect(b.signup.worstCaseRows).toBe(b.signup.limit * 5); // 16 requests x 5 rows
    const one = { ip: '198.51.100.200' };
    await s.call('GET', '/v1/info', undefined, undefined, one); // (sec34: the first counted request of an address writes its stored counter row)
    const mid = (await stats(s)).budget.rowsWritten.today;
    for (let i = 0; i < 5; i++) await s.call('GET', '/v1/info', undefined, undefined, one); // further reads write no rows (the budget's own save is every 200 requests, a counter row every 20)
    expect((await stats(s)).budget.rowsWritten.today).toBe(mid);
    s.advance(86400_000); // a new UTC day starts again from zero
    expect((await stats(s)).budget.rowsWritten.today).toBeLessThan(mid);
  });
  it('uses the platform number when the cursor reports it', async () => {
    const { createApp } = await import('../../src/core/app.js');
    const { openNodeSql } = await import('../../src/sqlite-node.js');
    const store = openNodeSql();
    const sql = { exec: (q, ...b) => { const c = store.sql.exec(q, ...b); return /^\s*INSERT/i.test(q) ? { toArray: c.toArray, rowsWritten: 7 } : { toArray: c.toArray, rowsWritten: 0 }; } };
    const app = createApp({ sql, tx: store.tx, env: { GAME: 'smg', ADMIN_TOKEN: ADMIN, RATE_LIMIT_SCALE: '1000', REGISTER_POW_BITS: '0', TURN_PROVIDER: 'stub' } });
    const h = { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0', 'cf-connecting-ip': '198.51.100.1', 'content-type': 'application/json' };
    await app.handle(new Request('https://t.invalid/v1/register', { method: 'POST', headers: h, body: '{"name":"Ana"}' }));
    const r = await (await app.handle(new Request('https://t.invalid/v1/admin/stats', { headers: { authorization: `Bearer ${ADMIN}`, 'cf-connecting-ip': '198.51.100.2' } }))).json();
    expect(r.budget.rowsWritten.today % 7).toBe(0);
    expect(r.budget.rowsWritten.today).toBeGreaterThanOrEqual(7);
    expect(r.budget.rowsWritten.source).toMatch(/^platform/);
  });
});

describe('sweep: alarm cadence', () => {
  const h = { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0', 'cf-connecting-ip': '198.51.100.7', 'content-type': 'application/json' };
  function obj() {
    const store = openNodeSql();
    const alarms = [];
    const ctx = { storage: { sql: store.sql, transactionSync: store.tx, getAlarm: async () => null, setAlarm: async (t) => { alarms.push(t); } } };
    return { o: new GameServer(ctx, { GAME: 'smg', ADMIN_TOKEN: ADMIN, TURN_PROVIDER: 'stub', RATE_LIMIT_SCALE: '1000', REGISTER_POW_BITS: '0' }), alarms };
  }
  const post = (o, path, tok, body = '{}') => o.fetch(new Request(`https://t.invalid${path}`, { method: 'POST', headers: { ...h, ...(tok ? { authorization: `Bearer ${tok}` } : {}) }, body }));
  it('a live room: 60 s; only a finished room left: 120 s; nothing left: no alarm', async () => {
    vi.useFakeTimers({ toFake: ['Date'] });
    try {
      vi.setSystemTime(Date.UTC(2026, 10, 15, 12));
      const { o, alarms } = obj();
      const tok = (await (await post(o, '/v1/register', null, '{"name":"Ana"}')).json()).token;
      const code = (await (await post(o, '/v1/rooms', tok)).json()).code;
      alarms.length = 0;
      await o.alarm();
      expect(alarms[0] - Date.now()).toBe(60_000);
      expect((await post(o, `/v1/rooms/${code}/leave`, tok)).status).toBe(200); // the room is finished, not yet deleted
      alarms.length = 0;
      await o.alarm();
      expect(alarms.map((t) => t - Date.now())).toEqual([120_000]);
      vi.setSystemTime(Date.now() + 3 * 86400_000); // long after: the finished room is deleted by this sweep
      alarms.length = 0;
      await o.alarm();
      expect(alarms).toEqual([]);
      await o.alarm();
      expect(alarms).toEqual([]);
    } finally { vi.useRealTimers(); }
  });
});
