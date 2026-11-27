// svcfix2 item 2 (independent review, P3): the Worker looked for a 'live' verdict on /v1/rooms/:code while the object answers 'player' to
// a non-member, so a valid account asking at 1 a second sent ~86,000 requests a day to the object and the budget never saw them.
// Now: verdicts are keyed (address key, account) with the classes the object uses, every request that reaches the object is counted in the
// budget's own accounting (budget.objects()), and a rate-limited answer is served by the Worker without the object.
import { describe, it, expect, vi, afterEach } from 'vitest';
import { makeWorker } from '../worker-node.js';

const T0 = Date.UTC(2026, 10, 12, 0, 0, 5);
afterEach(() => { vi.useRealTimers(); });
const freeze = (t = T0) => { vi.useFakeTimers({ toFake: ['Date'] }); vi.setSystemTime(t); };
const tick = (ms) => vi.setSystemTime(Date.now() + ms);
async function signUp(w, name, ip) {
  const r = await w.call('POST', '/v1/register', { ip, body: { name } });
  if (r.status !== 200) throw new Error('register ' + r.status);
  return r.json();
}

describe('non-member spam on a room route', () => {
  for (const mode of ['fixed', 'rotate64']) {
    it(`one valid account polling a room it is not in at 1 a second for 12 hours (${mode}): the object sees at most the daily ceiling`, async () => {
      freeze();
      const w = await makeWorker({});
      const me = await signUp(w, 'Spammer', '203.0.113.77');
      const base = w.stats.doRequests;
      let ok = 0;
      for (let i = 0; i < 43200; i++) {
        tick(1000);
        const ip = mode === 'fixed' ? '203.0.113.77' : `2001:db8:1:2::${(i + 1).toString(16)}`;
        const r = await w.call('GET', '/v1/rooms/ZZZZZZ', { ip, token: me.token });
        if (r.status !== 429) ok++;
      }
      const reached = w.stats.doRequests - base;
      // ADDR_DAILY_PLAYER is 3,000: a wrong-room poll is a player-class request. (The old Worker looked for 'live', never found it, and sent all 43,200.)
      expect(reached).toBeLessThanOrEqual(3100);
      expect(ok).toBeLessThanOrEqual(3000);
      // the budget's own accounting of object requests agrees with what the object really received
      expect(w.app.budget.objects()).toBe(w.stats.doRequests);
    }, 120000);
  }

  it('a member of the room is a live-class caller and is not blocked by the non-member verdict of the same address', async () => {
    freeze();
    const w = await makeWorker({ ADDR_DAILY_PLAYER: '50' });
    const host = await signUp(w, 'HostOne', '198.51.100.1');
    const spam = await signUp(w, 'SpamTwo', '198.51.100.1'); // same address (a venue)
    const code = (await (await w.call('POST', '/v1/rooms', { ip: '198.51.100.1', token: host.token, body: {} })).json()).code;
    for (let i = 0; i < 120; i++) { tick(1100); await w.call('GET', '/v1/rooms/ZZZZZZ', { ip: '198.51.100.1', token: spam.token }); }
    expect((await w.call('GET', '/v1/rooms/ZZZZZZ', { ip: '198.51.100.1', token: spam.token })).status).toBe(429);
    const mine = await w.call('GET', `/v1/rooms/${code}`, { ip: '198.51.100.1', token: host.token });
    expect(mine.status).toBe(200);
  });
});

describe('rate-limited answers are served by the Worker', () => {
  it('after the object says 429 slow down, the same caller\'s requests for the wait are answered without the object', async () => {
    freeze();
    const w = await makeWorker({ RATE_LIMIT_SCALE: '1', EDGE_IP_BURST: '0' });
    const me = await signUp(w, 'Limiter1', '198.51.100.5');
    let first429 = -1;
    for (let i = 0; i < 400 && first429 < 0; i++) {
      const r = await w.call('GET', '/v1/rooms/ZZZZZZ', { ip: '198.51.100.5', token: me.token });
      if (r.status === 429) first429 = i;
    }
    expect(first429).toBeGreaterThan(0);
    const before = w.stats.doRequests;
    for (let i = 0; i < 50; i++) expect((await w.call('GET', '/v1/rooms/ZZZZZZ', { ip: '198.51.100.5', token: me.token })).status).toBe(429);
    expect(w.stats.doRequests).toBe(before);
    tick(70_000); // the wait is over: the object is asked again
    expect((await w.call('GET', '/v1/rooms/ZZZZZZ', { ip: '198.51.100.5', token: me.token })).status).not.toBe(429);
    expect(w.stats.doRequests).toBe(before + 1);
  });
  it('the account ceiling is remembered per account, whichever address it comes from', async () => {
    freeze();
    const w = await makeWorker({ PID_DAILY_MAX: '40' });
    const me = await signUp(w, 'Pidcap1', '198.51.100.6');
    for (let i = 0; i < 80; i++) { tick(1100); await w.call('GET', '/v1/me', { ip: `192.0.2.${i + 1}`, token: me.token }); }
    const before = w.stats.doRequests;
    const r = await w.call('GET', '/v1/me', { ip: '192.0.2.200', token: me.token });
    expect(r.status).toBe(429);
    expect(w.stats.doRequests).toBe(before);
  });
});
