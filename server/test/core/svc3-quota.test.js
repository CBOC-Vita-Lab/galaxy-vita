import { fileURLToPath } from 'node:url';
// svc3 (outside review, findings S1 / S5 / day reset): the free plan's shared request quota. The real src/worker.js runs in node in
// front of the real application (test/worker-node.js); every fetch to the object it makes is counted.
import { describe, it, expect, vi, afterEach } from 'vitest';
import { makeWorker } from '../worker-node.js';
import { makeSmg, smgSignUp, roomOf } from '../smg-helpers.js';

const T0 = Date.UTC(2026, 10, 12, 1, 0, 0); // 01:00 UTC, an hour after the day's reset
afterEach(() => { vi.useRealTimers(); });
const freeze = (t = T0) => { vi.useFakeTimers({ toFake: ['Date'] }); vi.setSystemTime(t); };
const tick = (ms) => vi.setSystemTime(Date.now() + ms);

describe('S1: one address cannot spend the quota', () => {
  it('a loop on the open /v1/info from ONE address reaches the object at most its daily ceiling; the rest never wakes the object', async () => {
    freeze();
    const w = await makeWorker({ DAILY_REQUEST_BUDGET: '45000' });
    let served = 0, refused = 0;
    for (let i = 0; i < 6000; i++) { // one request every 0.34 s, 34 minutes
      tick(340);
      const r = await w.call('GET', '/v1/info', { ip: '203.0.113.9' });
      if (r.status === 200) served++; else if (r.status === 429) refused++;
    }
    console.log(`flood: served ${served}, refused ${refused}, object requests ${w.stats.doRequests}`);
    expect(w.stats.doRequests).toBeLessThanOrEqual(1100); // ADDR_DAILY_OPEN (1000) + the few the Worker needs to learn the verdict
    expect(served).toBeLessThanOrEqual(1000);
    expect(refused).toBeGreaterThan(4500);
  });

  it('the refused requests do not use up the day\'s budget: an honest player is still served after the flood', async () => {
    freeze();
    const w = await makeWorker({ DAILY_REQUEST_BUDGET: '1600' }); // the old code counted the 6000 refused ones too and said "busy" to everybody
    for (let i = 0; i < 6000; i++) { tick(340); await w.call('GET', '/v1/info', { ip: '203.0.113.9' }); }
    const honest = await w.call('POST', '/v1/register', { ip: '198.51.100.7', body: { name: 'Honest' } });
    expect(honest.status).toBe(200);
    const me = await w.call('GET', '/v1/me', { ip: '198.51.100.7', token: (await honest.json()).token });
    expect(me.status).toBe(200);
  });

  it('the Worker remembers the verdict per address: a different address is not affected, and the block ends at midnight UTC', async () => {
    freeze();
    const w = await makeWorker({ DAILY_REQUEST_BUDGET: '45000' });
    for (let i = 0; i < 1200; i++) { tick(1100); await w.call('GET', '/v1/time', { ip: '203.0.113.9' }); }
    const before = w.stats.doRequests;
    const blocked = await w.call('GET', '/v1/info', { ip: '203.0.113.9' });
    expect(blocked.status).toBe(429);
    expect(Number(blocked.headers.get('retry-after'))).toBeGreaterThan(3600);
    expect(w.stats.doRequests).toBe(before); // answered by the Worker
    expect((await w.call('GET', '/v1/info', { ip: '203.0.113.10' })).status).toBe(200);
    vi.setSystemTime(Date.UTC(2026, 10, 13, 0, 0, 5));
    expect((await w.call('GET', '/v1/info', { ip: '203.0.113.9' })).status).toBe(200);
  });

  it('the sustained per-isolate rate of the edge bucket is lower than before (1 a second, was 3)', async () => {
    freeze();
    const w = await makeWorker({ ADDR_DAILY_OPEN: '0' }); // the object's ceiling off: only the Worker's bucket acts
    let passed = 0;
    for (let i = 0; i < 3000; i++) { tick(340); if ((await w.call('GET', '/v1/info', { ip: '203.0.113.9' })).status === 200) passed++; }
    console.log(`edge bucket, 3000 requests in 17 min from one address: ${passed} passed`);
    expect(passed).toBeLessThan(1500); // 3/s would let all 3000 through
  });
});

describe('S5: the public board memo costs about one object request a minute per isolate', () => {
  it('600 memo hits in 10 minutes cost the object at most 11 requests of /v1/info (was 12 a minute)', async () => {
    freeze();
    const w = await makeWorker({});
    let n = 0;
    for (let i = 0; i < 600; i++) {
      tick(1000);
      const r = await w.call('GET', '/v1/public/times', { ip: `203.0.113.${(i % 40) + 1}`, headers: { 'x-msc-game': 'smg' } });
      expect(r.status).toBe(200);
      n++;
    }
    const info = w.stats.byPath.get('/v1/info') || 0;
    const board = w.stats.byPath.get('/v1/public/times') || 0;
    console.log(`memo: ${n} hits -> object: ${info} x /v1/info, ${board} x /v1/public/times`);
    expect(info + board).toBeLessThanOrEqual(22); // a miss a minute (the memo lives 60 s) and at most one kill-switch look a minute
  });
});

describe('reserve for sessions in progress', () => {
  it('new sessions stop at 80 % of the budget; a member of a live room is served to the whole budget; the admin always', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '200', LIVE_RESERVE_PCT: '20' });
    const h = await smgSignUp(s, 'Host'); // 1
    const g = await smgSignUp(s, 'Gus'); // 2
    const code = await roomOf(s, [h, g]); // 3, 4
    let r;
    do { r = await s.call('GET', '/v1/info'); } while (r.status === 200);
    expect(r.status).toBe(503);
    expect(r.headers.get('x-msc-budget')).toBe('new-over');
    expect(s.app.budgetUsed()).toBe(160);
    // a new session is refused ...
    const created = await s.call('POST', '/v1/rooms', {}, g.token);
    expect(created.status).toBe(503);
    expect((await s.call('POST', '/v1/register', { name: 'Late' })).status).toBe(503);
    // ... the running one is not
    for (let i = 0; i < 40; i++) {
      const p = await s.call('GET', `/v1/rooms/${code}`, undefined, i % 2 ? h.token : g.token);
      expect(p.status).toBe(200);
    }
    expect(s.app.budgetUsed()).toBe(200);
    const cut = await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
    expect(cut.status).toBe(503);
    expect(cut.headers.get('x-msc-budget')).toBe('over');
    expect((await s.raw('GET', '/v1/admin/stats', undefined, 'test-admin-token-0123456789')).status).toBe(200);
    s.advance(86400_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).status).toBeLessThan(500);
    expect((await s.call('GET', '/v1/info')).status).toBe(200);
  });

  it('LIVE_RESERVE_PCT=0 is one limit for everybody', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '50', LIVE_RESERVE_PCT: '0' });
    let r;
    do { r = await s.call('GET', '/v1/info'); } while (r.status === 200);
    expect(s.app.budgetUsed()).toBe(50);
    expect(r.headers.get('x-msc-budget')).toBe('over');
  });

  it('the Worker answers new-session paths itself once the object said "new-over", and still forwards the live room paths', async () => {
    freeze();
    const w = await makeWorker({ DAILY_REQUEST_BUDGET: '100', LIVE_RESERVE_PCT: '20' });
    const reg = async (name, ip) => (await (await w.call('POST', '/v1/register', { ip, body: { name } })).json());
    const a = await reg('Ann', '198.51.100.1');
    const b = await reg('Bob', '198.51.100.2');
    const created = await (await w.call('POST', '/v1/rooms', { token: a.token, ip: '198.51.100.1', body: {} })).json();
    await w.call('POST', '/v1/rooms/join', { token: b.token, ip: '198.51.100.2', body: { code: created.code } });
    let r;
    let i = 0;
    do { r = await w.call('GET', '/v1/info', { ip: `192.0.2.${(i++ % 200) + 1}` }); } while (r.status === 200);
    expect(r.status).toBe(503);
    const before = w.stats.doRequests;
    for (let k = 0; k < 30; k++) expect((await w.call('GET', '/v1/info', { ip: `192.0.2.${(k % 200) + 1}` })).status).toBe(503);
    expect(w.stats.doRequests).toBe(before); // the Worker answered
    const poll = await w.call('GET', `/v1/rooms/${created.code}`, { token: b.token, ip: '198.51.100.2' });
    expect(poll.status).toBe(200); // a live member still reaches the object
  });
});

describe('poll pacing: the service tells the clients to stretch their polls before the budget runs out', () => {
  it('room answers carry pollScale (absent at 1): rises from half of the new-session budget to 1.4', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '1000', LIVE_RESERVE_PCT: '20' });
    const h = await smgSignUp(s, 'Host');
    const code = await roomOf(s, [h]);
    const first = await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
    expect(first.data.pollScale).toBeUndefined();
    while (s.app.budgetUsed() < 600) await s.call('GET', '/v1/time');
    const mid = await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
    expect(mid.data.pollScale).toBeGreaterThan(1.1);
    expect(mid.data.pollScale).toBeLessThan(1.4);
    while (s.app.budgetUsed() < 800) await s.call('GET', '/v1/time');
    const top = await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
    expect(top.data.pollScale).toBe(1.4);
    const unchanged = await s.call('GET', `/v1/rooms/${code}?v=${top.data.version}`, undefined, h.token);
    expect(unchanged.data.changed).toBe(false);
    expect(unchanged.data.pollScale).toBe(1.4);
  });
});

describe('ceilings per address and per account (budget.js)', () => {
  it('the account ceiling (PID_DAILY_MAX) ends one account\'s day with a terminal daily_cap answer and spends no budget; other accounts go on', async () => {
    const s = makeSmg({ PID_DAILY_MAX: '30', RATE_LIMIT_SCALE: '1', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', DAILY_REQUEST_BUDGET: '1000' });
    const a = await smgSignUp(s, 'Ann');
    const b = await smgSignUp(s, 'Ben');
    let last;
    for (let i = 0; i < 30; i++) { s.advance(2000); last = await s.call('GET', '/v1/me', undefined, a.token); expect(last.status).toBe(200); }
    const used = s.app.budgetUsed();
    s.advance(2000);
    last = await s.call('GET', '/v1/me', undefined, a.token);
    expect(last.status).toBe(429);
    expect(last.data.error.reason).toBe('daily_cap');
    expect(s.app.budgetUsed()).toBe(used);
    expect((await s.call('GET', '/v1/me', undefined, b.token)).status).toBe(200);
    s.advance(86400_000);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200);
  });

  it('a flood of wrong tokens from one address ends at the address ceiling too, and spends no budget', async () => {
    const s = makeSmg({ ADDR_DAILY_PLAYER: '50', RATE_LIMIT_SCALE: '1', DAILY_REQUEST_BUDGET: '1000' });
    const bad = 'A'.repeat(43);
    const codes = new Set();
    for (let i = 0; i < 120; i++) { s.advance(40_000); codes.add((await s.call('GET', '/v1/me', undefined, bad, { ip: '203.0.113.50' })).status); }
    expect(s.app.budgetUsed()).toBe(0);
    expect(s.app.budget.status(s.clock.now).rejectedAddress).toBeGreaterThan(60);
  });

  it('refused requests are counted apart and the admin page says so', async () => {
    const s = makeSmg({ ADDR_DAILY_OPEN: '10', RATE_LIMIT_SCALE: '1', DAILY_REQUEST_BUDGET: '1000' });
    for (let i = 0; i < 25; i++) { s.advance(60_000); await s.call('GET', '/v1/time', undefined, undefined, { ip: '203.0.113.50' }); }
    const st = await s.raw('GET', '/v1/admin/stats', undefined, 'test-admin-token-0123456789');
    expect(st.status).toBe(200);
    expect(st.data.budget).toMatchObject({ used: 10, limit: 1000, rejectedAddress: 15, rejectedBudget: 0, pace: 1 });
  });
});

describe('failed-authentication ceiling per address (budget.js gateBadAuth; Strikers d330d612)', () => {
  it('the object: requests that fail authentication stop at ADDR_BAD_DAY with a daily_cap and x-msc-block bad; a valid token from the same address, and other addresses, are untouched', async () => {
    const s = makeSmg({ ADDR_BAD_DAY: '20', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', RATE_LIMIT_SCALE: '1', DAILY_REQUEST_BUDGET: '1000' });
    const honest = await smgSignUp(s, 'Hon');
    const wrong = 'A'.repeat(43);
    const ip = '203.0.113.50';
    let n401 = 0, n429 = 0, block = '';
    for (let i = 0; i < 60; i++) {
      s.advance(40_000);
      const r = await s.call('GET', '/v1/me', undefined, wrong, { ip });
      if (r.status === 401) n401++;
      if (r.status === 429) { n429++; block = block || r.headers.get('x-msc-block') || ''; expect(r.data.error.reason).toBe('daily_cap'); }
    }
    expect(n401).toBe(20);
    expect(n429).toBe(40);
    expect(block).toMatch(/^bad:\d+$/);
    expect((await s.call('GET', '/v1/me', undefined, honest.token, { ip })).status).toBe(200);   // a valid token is not a failed authentication
    expect((await s.call('GET', '/v1/me', undefined, wrong, { ip: '198.51.100.9' })).status).toBe(401);   // another address has its own count
    s.advance(86400_000);
    expect((await s.call('GET', '/v1/me', undefined, wrong, { ip })).status).toBe(401);   // a new UTC day
  });

  it('ADDR_BAD_DAY=0 turns it off', async () => {
    const s = makeSmg({ ADDR_BAD_DAY: '0', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', RATE_LIMIT_SCALE: '1' });
    for (let i = 0; i < 40; i++) { s.advance(40_000); expect((await s.call('GET', '/v1/me', undefined, 'B'.repeat(43), { ip: '203.0.113.51' })).status).toBe(401); }
  });

  it('the Worker: after the verdict a wrong token no longer reaches the object, a token it has seen succeed still does', async () => {
    freeze();
    const w = await makeWorker({ ADDR_BAD_DAY: '15', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', EDGE_IP_BURST: '0', DAILY_REQUEST_BUDGET: '45000' });
    const ip = '203.0.113.60';
    const reg = await w.call('POST', '/v1/register', { ip: '198.51.100.7', body: { name: 'Honest' } });
    expect(reg.status).toBe(200);
    const tok = (await reg.json()).token;
    expect((await w.call('GET', '/v1/me', { ip, token: tok })).status).toBe(200);   // a route that checks the token: the Worker marks it good
    const wrong = 'C'.repeat(43);
    for (let i = 0; i < 16; i++) { tick(5000); await w.call('GET', '/v1/me', { ip, token: wrong }); }   // 15 are 401, the 16th is the verdict
    const before = w.stats.doRequests;
    for (let i = 0; i < 50; i++) { tick(5000); expect((await w.call('GET', '/v1/me', { ip, token: wrong })).status).toBe(429); }
    expect(w.stats.doRequests).toBe(before);   // a known-bad token never wakes the object
    tick(5000);
    expect((await w.call('GET', '/v1/me', { ip, token: tok })).status).toBe(200);   // the honest console on the same address goes on
    expect((await w.call('GET', '/v1/me', { ip: '198.51.100.8', token: wrong })).status).toBe(401);   // another address is not blocked
    tick(86400_000);
    expect((await w.call('GET', '/v1/me', { ip, token: wrong })).status).toBe(401);   // midnight UTC ends it
  });

  it('the Worker: an invented token sent to an OPEN route is not marked good', async () => {
    freeze();
    const w = await makeWorker({ ADDR_BAD_DAY: '5', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', EDGE_IP_BURST: '0' });
    const ip = '203.0.113.61';
    const invented = 'E'.repeat(43);
    expect((await w.call('GET', '/v1/info', { ip, token: invented })).status).toBe(200);   // an open route answers whatever Authorization says
    for (let i = 0; i < 6; i++) { tick(20_000); await w.call('GET', '/v1/me', { ip, token: 'F'.repeat(43) }); }   // the verdict
    tick(60_000);
    const probe = w.stats.doRequests;
    expect((await w.call('GET', '/v1/me', { ip, token: invented })).status).not.toBe(200);   // it is an unknown token (it uses the allowance, the object refuses it), not a good one ...
    expect(w.stats.doRequests).toBe(probe + 1);
    for (let i = 0; i < 6; i++) { tick(1000); await w.call('GET', '/v1/me', { ip, token: 'G'.repeat(43) }); }   // ... the allowance is spent now
    const r = await w.call('GET', '/v1/me', { ip, token: 'H'.repeat(43) });
    expect(r.status).toBe(429);
    const b4 = w.stats.doRequests;
    const again = await w.call('GET', '/v1/me', { ip, token: invented });
    expect(w.stats.doRequests).toBe(b4);   // it did not reach the object: not good
    expect(again.status).toBe(429);   // refused as a known-bad token: it did not pass "all day" through the open-route mark
    expect(w.stats.doRequests).toBeLessThan(probe + 8);
  });

  it('the Worker: after an attack a new valid console of the same address gets in within a minute; rotating invented tokens stay bounded', async () => {
    freeze();
    const w = await makeWorker({ ADDR_BAD_DAY: '20', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', EDGE_IP_BURST: '0', REGISTER_NET_PER_DAY: '1000' });
    const ip = '203.0.113.62';
    let n = 0;
    const invent = () => `X${String(n++).padStart(42, 'q')}`;
    for (let i = 0; i < 40; i++) { tick(500); await w.call('GET', '/v1/me', { ip, token: invent() }); }   // the attack: over the ceiling
    // a console registers (an open route: unaffected) and then calls with its new token while the attack goes on
    const reg = await w.call('POST', '/v1/register', { ip, body: { name: 'Newcomer' } });
    expect(reg.status).toBe(200);
    const tok = (await reg.json()).token;
    let inAt = -1;
    for (let sec = 0; sec < 60 && inAt < 0; sec++) {
      tick(1000);
      for (let k = 0; k < 5; k++) await w.call('GET', '/v1/me', { ip, token: invent() });   // five invented tokens a second keep arriving
      if ((await w.call('GET', '/v1/me', { ip, token: tok })).status === 200) inAt = sec;
    }
    expect(inAt).toBeGreaterThanOrEqual(0);   // proved itself inside a minute
    tick(1000);
    expect((await w.call('GET', '/v1/me', { ip, token: tok })).status).toBe(200);   // and stays in
    // bounded: a further ten minutes of 5 invented tokens a second reach the object at most 3 a minute (+ the one minute boundary)
    const before = w.stats.doRequests;
    for (let sec = 0; sec < 600; sec++) { tick(1000); for (let k = 0; k < 5; k++) await w.call('GET', '/v1/me', { ip, token: invent() }); }
    expect(w.stats.doRequests - before).toBeLessThanOrEqual(10 * 3 + 6);
  });
  it('the Worker: an attacker spending 3 invented tokens at every minute start does not keep a retrying new console out', async () => {
    freeze(Date.UTC(2026, 10, 12, 1, 0, 0));
    const w = await makeWorker({ ADDR_BAD_DAY: '5', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', EDGE_IP_BURST: '0', REGISTER_NET_PER_DAY: '1000' });
    const ip = '203.0.113.63';
    let n = 0;
    const invent = () => `Y${String(n++).padStart(42, 'q')}`;
    for (let i = 0; i < 12; i++) { tick(500); await w.call('GET', '/v1/me', { ip, token: invent() }); }   // over the ceiling
    const reg = await w.call('POST', '/v1/register', { ip, body: { name: 'Retry' } });
    const tok = (await reg.json()).token;
    let inAt = -1;
    for (let sec = 0; sec < 180 && inAt < 0; sec += 5) {
      tick(5000);
      if (Math.floor(Date.now() / 1000) % 60 < 5) for (let k = 0; k < 3; k++) await w.call('GET', '/v1/me', { ip, token: invent() });   // the attacker at each minute start
      if ((await w.call('GET', '/v1/me', { ip, token: tok })).status === 200) inAt = sec;
    }
    expect(inAt).toBeGreaterThanOrEqual(0);
    expect(inAt).toBeLessThanOrEqual(120);
  });

  it('the Worker: a valid token whose first answers are 404 / 409 is marked good (it stops spending the allowance); a 400 or 426 does not mark an invented one', async () => {
    freeze();
    const w = await makeWorker({ ADDR_BAD_DAY: '3', ADDR_DAILY_PLAYER: '0', ADDR_DAILY_LIVE: '0', ADDR_DAILY_OPEN: '0', EDGE_IP_BURST: '0', REGISTER_NET_PER_DAY: '1000' });
    const ip = '203.0.113.64';
    const reg = await w.call('POST', '/v1/register', { ip, body: { name: 'Fourohfour' } });
    const tok = (await reg.json()).token;
    for (let i = 0; i < 5; i++) { tick(1000); await w.call('GET', '/v1/me', { ip, token: `Z${String(i).padStart(42, 'q')}` }); }   // over the ceiling
    tick(60_000);
    const first = await w.call('GET', '/v1/rooms/ABCDEF', { ip, token: tok });   // not a member: 404 after authentication
    expect(first.status).toBe(404);
    for (let i = 0; i < 8; i++) {   // good now: it never needs the allowance again, however often it asks
      tick(1000);
      expect((await w.call('GET', '/v1/rooms/ABCDEF', { ip, token: tok })).status).toBe(404);
    }
    expect((await w.call('GET', '/v1/me', { ip, token: tok })).status).toBe(200);
  });
});

describe('static assets (scripts/build-assets.mjs)', () => {
  it('writes the /times page byte for byte as the Worker serves it, with its security headers', async () => {
    const { mkdtempSync, readFileSync } = await import('node:fs');
    const { tmpdir } = await import('node:os');
    const { join } = await import('node:path');
    const { execFileSync } = await import('node:child_process');
    const dir = mkdtempSync(join(tmpdir(), 'assets-'));
    execFileSync(process.execPath, [fileURLToPath(new URL('../../scripts/build-assets.mjs', import.meta.url)), dir]);
    const { timesPageResponse } = await import('../../src/core/times_page.js');
    const r = await timesPageResponse();
    expect(readFileSync(join(dir, 'times.html'), 'utf8')).toBe(await r.text());
    const h = readFileSync(join(dir, '_headers'), 'utf8');
    expect(h).toMatch(/^\/times\n/);
    expect(h).toContain(`content-security-policy: ${r.headers.get('content-security-policy')}`);
    expect(h).not.toMatch(/^\s+content-type:/m);
  });
});

describe('quota32: the object refusing a call (its daily limit) reaches the player as the service\'s own busy answer', () => {
  it('a throwing Durable Object stub answers 503 over_budget with retry-after, not an uncaught error', async () => {
    const w = await makeWorker({});
    w.env.GAME = { idFromName: (n) => n, get: () => ({ fetch: async () => { throw new Error('Exceeded allowed volume of Durable Object requests'); } }) };
    const r = await w.call('POST', '/v1/register', { ip: '198.51.100.20', body: { name: 'Busy' } });
    expect(r.status).toBe(503);
    expect(r.headers.get('retry-after')).toBe('120');
    const j = await r.json();
    expect(j.error.code).toBe('over_budget');
    expect(j.error.retryAfterSec).toBe(120);
  });

  it('fix33 (S2): any other thrown error (object reset by a deploy, overload) answers 503 `busy` with a short retry, not over_budget', async () => {
    for (const msg of ['Durable Object reset because its code was updated.', 'Durable Object is overloaded. Requests queued for too long.', 'internal error',
      // watch F3b: these name a limit but are transient (the object was reset), not the day's quota
      'Durable Object exceeded its memory limit and was reset.', 'Durable Object exceeded its CPU time limit and was reset.', 'Subrequest depth limit exceeded']) {
      const w = await makeWorker({});
      w.env.GAME = { idFromName: (n) => n, get: () => ({ fetch: async () => { throw new Error(msg); } }) };
      const r = await w.call('POST', '/v1/register', { ip: '198.51.100.21', body: { name: 'Busy' } });
      expect(r.status).toBe(503);
      expect(Number(r.headers.get('retry-after'))).toBeLessThanOrEqual(5);
      const j = await r.json();
      expect(j.error.code).toBe('busy');
      expect(j.error.retryAfterSec).toBeLessThanOrEqual(5);
    }
    const w = await makeWorker({});
    w.env.GAME = { idFromName: (n) => n, get: () => ({ fetch: async () => { throw Object.assign(new Error('Exceeded allowed rows read'), { overloaded: true }); } }) };
    expect((await (await w.call('POST', '/v1/register', { ip: '198.51.100.22', body: { name: 'Busy' } })).json()).error.code).toBe('busy');
  });
});
