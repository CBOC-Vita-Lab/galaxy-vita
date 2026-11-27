// secreview32 (F1): no request shape may reach the Durable Object without a ceiling. Before the fix, from ONE address without an account:
//   a) /v1/admin/* with wrong bearers        -> every request reached the object (the Worker never consulted a verdict for admin routes)
//   b) a player route with a too-old X-Msc-App -> 426 from the object before any per-address ceiling
//   c) a player route with X-Msc-Proto: 0     -> the same
//   d) a player route of a switched-off feature -> 503 from the object before any per-address ceiling
// 50,000 of them spent DAILY_OBJECT_BUDGET and every honest player got 503 over_budget until 00:00 UTC.
// The real src/worker.js runs in node in front of the real application (test/worker-node.js).
import { describe, it, expect, vi, afterEach } from 'vitest';
import { makeWorker } from '../worker-node.js';

const T0 = Date.UTC(2026, 10, 20, 0, 5, 0);
afterEach(() => { vi.useRealTimers(); });
const freeze = (t = T0) => { vi.useFakeTimers({ toFake: ['Date'] }); vi.setSystemTime(t); };
const tick = (ms) => vi.setSystemTime(Date.now() + ms);
const PROD = { DAILY_REQUEST_BUDGET: '45000', DAILY_OBJECT_BUDGET: '50000', LIVE_RESERVE_PCT: '20', ADDR_DAILY_OPEN: '1000', ADDR_DAILY_PLAYER: '3000',
  ADDR_DAILY_LIVE: '10000', PID_DAILY_MAX: '4000', ADDR_BAD_DAY: '2000', EDGE_IP_BURST: '120', EDGE_IP_PER_SEC: '1', EDGE_IP_AGG_BURST: '300', EDGE_IP_AGG_PER_SEC: '5' };
const tok = (i) => `x${String(i).padStart(42, '0')}`;
const ATTACKER = '203.0.113.66';
const ADMIN = 'test-admin-token-0123456789';

async function flood(w, n, make) {
  const reached0 = w.stats.doRequests, by = {};
  for (let i = 0; i < n; i++) {
    tick(200); // 5 a second: under the Worker's aggregate bucket and far under the zone's rate-limiting rule
    const r = await make(i);
    by[r.status] = (by[r.status] || 0) + 1;
  }
  return { reached: w.stats.doRequests - reached0, by };
}

describe('secreview32 F1: every path to the object has a ceiling', () => {
  it('a) wrong admin bearers from one address: the Worker answers the loop itself; honest players are still served', async () => {
    freeze();
    const w = await makeWorker(PROD);
    const reg = await w.call('POST', '/v1/register', { ip: '198.51.100.7', body: { name: 'Honest' } });
    const token = (await reg.json()).token;
    const f = await flood(w, 50200, (i) => w.call('GET', '/v1/admin/stats', { ip: ATTACKER, token: tok(i) }));
    // 2.8 hours of guessing: 5 wrong bearers at once, then one per brake refill (120 s) plus the request that learns each verdict
    expect(f.reached).toBeLessThan(400);
    expect(f.by[200]).toBeUndefined();
    const st = w.app.budget.status(Date.now());
    expect(st.objectRequests).toBeLessThan(500);
    expect((await w.call('GET', '/v1/me', { ip: '198.51.100.7', token })).status).toBe(200);
    expect((await w.call('POST', '/v1/register', { ip: '198.51.100.8', body: { name: 'Newcomer' } })).status).toBe(200);
  }, 240000);

  it('a2) the admin verdict is per address and ends: the owner (another address, the right token) is never affected, and the blocked address is asked again later', async () => {
    freeze();
    const w = await makeWorker(PROD);
    await flood(w, 200, (i) => w.call('GET', '/v1/admin/stats', { ip: ATTACKER, token: tok(i) }));
    const before = w.stats.doRequests;
    const blocked = await w.call('GET', '/v1/admin/stats', { ip: ATTACKER, token: ADMIN }); // even the right token, from the blocked address, waits
    expect(blocked.status).toBe(429);
    expect(w.stats.doRequests).toBe(before);
    expect(Number(blocked.headers.get('retry-after'))).toBeGreaterThan(0);
    const owner = await w.call('GET', '/v1/admin/stats', { ip: '198.51.100.20', token: ADMIN });
    expect(owner.status).toBe(200);
    tick(11 * 60_000); // the brake has refilled
    expect((await w.call('GET', '/v1/admin/stats', { ip: ATTACKER, token: ADMIN })).status).toBe(200);
  });

  it('a3) an honest admin run (a backup paging 25 requests) is not slowed', async () => {
    freeze();
    const w = await makeWorker(PROD);
    for (let i = 0; i < 25; i++) { tick(300); expect((await w.call('GET', '/v1/admin/export', { ip: '198.51.100.20', token: ADMIN })).status).toBe(200); }
  });

  it('b) a too-old X-Msc-App: 426 from the Worker, the object is never woken', async () => {
    freeze();
    const w = await makeWorker(PROD);
    const f = await flood(w, 3000, (i) => w.call('GET', '/v1/me', { ip: ATTACKER, token: tok(i), headers: { 'x-msc-app': '2.9.9' } }));
    expect(f.by[426]).toBe(3000);
    expect(f.reached).toBe(0);
    const one = await w.call('GET', '/v1/me', { ip: '198.51.100.9', token: tok(1), headers: { 'x-msc-app': '2.9.9' } });
    const j = await one.json();
    expect(j.error).toMatchObject({ code: 'update_required', minVersion: '3.0.0' });
  }, 120000);

  it('c) X-Msc-Proto: 0: 426 from the Worker, the object is never woken (open routes too)', async () => {
    freeze();
    const w = await makeWorker(PROD);
    const f = await flood(w, 3000, (i) => w.call('POST', '/v1/quick', { ip: ATTACKER, token: tok(i), body: { mode: 'coop' }, headers: { 'x-msc-proto': '0' } }));
    expect(f.by[426]).toBe(3000);
    expect(f.reached).toBe(0);
    const info = await w.call('GET', '/v1/info', { ip: ATTACKER, headers: { 'x-msc-proto': '0' } });
    expect(info.status).toBe(426);
    expect((await info.json()).error).toMatchObject({ code: 'upgrade_required', minProto: 1 });
    expect((await w.call('GET', '/v1/info', { ip: ATTACKER, headers: { 'x-msc-proto': '1' } })).status).toBe(200);
  }, 120000);

  it('d) a switched-off feature: refusals count against the address like failed sign-ins and stop near ADDR_BAD_DAY', async () => {
    freeze();
    const w = await makeWorker({ ...PROD, KILL_SWITCH: 'friends' });
    const f = await flood(w, 6000, (i) => w.call('GET', '/v1/friends', { ip: ATTACKER, token: tok(i) }));
    // 2,000 refusals (503), then the `bad` verdict: the Worker lets 3 unknown tokens a minute through (20 minutes of flood: 60) and answers the rest
    expect(f.by[503]).toBe(2000);
    expect(f.reached).toBeLessThan(2100);
    // an honest console elsewhere reads the real answer
    const reg = await w.call('POST', '/v1/register', { ip: '198.51.100.7', body: { name: 'Honest' } });
    const token = (await reg.json()).token;
    const fr = await w.call('GET', '/v1/friends', { ip: '198.51.100.7', token });
    expect(fr.status).toBe(503);
    expect((await fr.json()).error.code).toBe('service_disabled');
    expect((await w.call('GET', '/v1/me', { ip: '198.51.100.7', token })).status).toBe(200);
  }, 120000);

  it('the app keeps its own version gate (the fake server and defence in depth): same codes as the Worker', async () => {
    freeze();
    const w = await makeWorker(PROD);
    const direct = (headers) => w.app.handle(new Request('https://smg.test.invalid/v1/me', { headers: { 'cf-connecting-ip': '198.51.100.30', 'x-msc-game': 'smg', authorization: `Bearer ${tok(7)}`, ...headers } }));
    const a = await direct({ 'x-msc-app': '2.9.9' });
    expect(a.status).toBe(426);
    expect((await a.json()).error.code).toBe('update_required');
    const b = await direct({ 'x-msc-app': '3.0.0', 'x-msc-proto': '0' });
    expect(b.status).toBe(426);
    expect((await b.json()).error.code).toBe('upgrade_required');
  });
});
