// svcfix2 item 3 (independent review, P2/P2b): fake "live" rooms drained the 20 % reserve in 5.8 hours, after which an honest mid-game poll
// and even /leave got 503 with a retry-after of 17 hours. Now: per-room and per-address caps inside the reserve, leave / close always
// allowed, and every wait the budget names is at most 15 minutes.
import { describe, it, expect, vi, afterEach } from 'vitest';
import { makeWorker } from '../worker-node.js';

const T0 = Date.UTC(2026, 10, 12, 1, 0, 0);
afterEach(() => { vi.useRealTimers(); });
const freeze = () => { vi.useFakeTimers({ toFake: ['Date'] }); vi.setSystemTime(T0); };
const tick = (ms) => vi.setSystemTime(Date.now() + ms);
async function signUp(w, name, ip) {
  const r = await w.call('POST', '/v1/register', { ip, body: { name } });
  if (r.status !== 200) throw new Error('register ' + r.status + ' ' + await r.text());
  return r.json();
}
const epb = (ip) => ({ public: `${ip}:41100`, local: '192.168.1.5:41100', nonce: 'AAAAAAAAAAA=' });
async function startedRoom(w, ip, accts, ips = []) {
  const at = (i) => ips[i] || ip;
  const m = await w.call('POST', '/v1/rooms', { ip: at(0), token: accts[0].token, body: {} });
  const code = (await m.json()).code;
  for (const [i, a] of accts.entries()) if (i) { const j = await w.call('POST', '/v1/rooms/join', { ip: at(i), token: a.token, body: { code } }); if (j.status !== 200) throw new Error('join ' + j.status); }
  for (const [i, a] of accts.entries()) { const e = await w.call('POST', `/v1/rooms/${code}/endpoint`, { ip: at(i), token: a.token, body: epb(at(i)) }); if (e.status !== 200) throw new Error('ep ' + e.status); }
  for (const [i, a] of accts.entries()) { const s = await w.call('POST', `/v1/rooms/${code}/started`, { ip: at(i), token: a.token }); if (s.status !== 200) throw new Error('st ' + s.status); }
  return { code, accts, ip };
}

describe('the reserve is spent fairly', () => {
  it('three addresses polling their own started rooms cannot drain the reserve: an honest game in progress is served to the end of the day', async () => {
    freeze();
    const w = await makeWorker({ DAILY_REQUEST_BUDGET: '4000', LIVE_RESERVE_PCT: '20', RESERVE_ROOM_MAX: '150', RESERVE_ADDR_MAX: '200', ADDR_DAILY_LIVE: '0', PID_DAILY_MAX: '0' });
    const honestAccts = [await signUp(w, 'HonestA1', '198.51.100.1'), await signUp(w, 'HonestB1', '198.51.100.2')];
    const hroom = await startedRoom(w, '198.51.100.1', honestAccts, ['198.51.100.1', '198.51.100.2']);
    const rooms = [];
    for (const [i, ip] of ['203.0.113.50', '203.0.113.51', '203.0.113.52'].entries()) {
      const accts = []; for (let k = 0; k < 3; k++) accts.push(await signUp(w, `Atk${i}${k}`, ip));
      rooms.push(await startedRoom(w, ip, accts));
    }
    // attackers poll (live class, started rooms) until the whole new-session part of the budget is gone and well into the reserve
    let honestBad = 0, honestPolls = 0, firstReserveRefusal = null;
    for (let step = 0; step < 6000; step++) {
      tick(1200);
      for (const r of rooms) {
        const res = await w.call('GET', `/v1/rooms/${r.code}`, { ip: r.ip, token: r.accts[step % 3].token });
        if (res.status === 503 && firstReserveRefusal === null) firstReserveRefusal = step;
        if (res.status === 503) expect(Number(res.headers.get('retry-after'))).toBeLessThanOrEqual(900);
      }
      if (step % 4 === 0 && honestPolls < 50) { // a member polls every ~5 s
        const inReserve = w.app.budgetUsed() > 3200; // the honest member polls inside the reserve
        const r = await w.call('GET', `/v1/rooms/${hroom.code}`, { ip: '198.51.100.2', token: honestAccts[1].token });
        if (inReserve) { honestPolls++; if (r.status !== 200) honestBad++; }
        await w.call('GET', `/v1/rooms/${hroom.code}`, { ip: '198.51.100.1', token: honestAccts[0].token });
      }
    }
    expect(firstReserveRefusal).not.toBeNull(); // the fake rooms were stopped...
    expect(honestPolls).toBeGreaterThan(20); // (50 polls: well inside one room's share)
    expect(honestBad).toBe(0);                   // ...and the honest game never was
    expect(w.app.budgetUsed()).toBeLessThan(4000);
  }, 120000);

  it('/leave and closing a room are served when the whole budget is spent, and the refusal names a wait of at most 15 minutes', async () => {
    freeze();
    const w = await makeWorker({ DAILY_REQUEST_BUDGET: '400', LIVE_RESERVE_PCT: '20', RESERVE_ROOM_MAX: '0', RESERVE_ADDR_MAX: '0', RATE_LIMIT_SCALE: '1000' });
    const a = await signUp(w, 'LeaverAA', '198.51.100.1'), b = await signUp(w, 'LeaverBB', '198.51.100.2');
    const room = await startedRoom(w, '198.51.100.1', [a, b], ['198.51.100.1', '198.51.100.2']);
    let r;
    for (let i = 0; i < 600; i++) { tick(1100); r = await w.call('GET', `/v1/rooms/${room.code}`, { ip: '198.51.100.1', token: a.token }); await w.call('GET', `/v1/rooms/${room.code}`, { ip: '198.51.100.2', token: b.token }); if (r.status === 503) break; }
    expect(r.status).toBe(503);
    expect(Number(r.headers.get('retry-after'))).toBeLessThanOrEqual(900);
    expect((await r.json()).error.code).toBe('over_budget');
    const before = w.stats.doRequests;
    const lv = await w.call('POST', `/v1/rooms/${room.code}/leave`, { ip: '198.51.100.2', token: b.token });
    if (lv.status !== 200) console.log('LEAVE', lv.status, await lv.clone().text());
    expect(lv.status).toBe(200);          // went through the Worker's own "over" answer to the object
    expect(w.stats.doRequests).toBe(before + 1);
    const close = await w.call('DELETE', `/v1/rooms/${room.code}`, { ip: '198.51.100.1', token: a.token });
    expect([200, 204]).toContain(close.status);
  });

  it('an honest room needs far less than its share: a 4-player game for 3 hours at the client cadence fits RESERVE_ROOM_MAX', async () => {
    const { default: cfgOf } = await import('../../src/core/config.js').then((m) => ({ default: m.loadConfig }));
    const c = cfgOf({ SERVICE_GAME: 'smg' });
    expect(c.reserve.roomMax).toBeGreaterThanOrEqual(4 * 3 * 60);
    expect(c.reserve.addrMax).toBeGreaterThanOrEqual(2 * c.reserve.roomMax);
  });
});
