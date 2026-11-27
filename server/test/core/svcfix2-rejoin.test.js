// svcfix2 item 4 (independent review): between 48,000 and 60,000 requests (the reserve) a dropped player could not rejoin his own game
// (/v1/rooms/join was not a live route) and a player in a live room could not post a time. A join is live when the room is started and the
// caller is a member or has just left it (a room_left row); /v1/times is live for a member of a live room. Strangers stay refused.
import { describe, it, expect, vi, afterEach } from 'vitest';
import { makeWorker } from '../worker-node.js';

const T0 = Date.UTC(2026, 10, 12, 1, 0, 0);
afterEach(() => { vi.useRealTimers(); });
const tick = (ms) => vi.setSystemTime(Date.now() + ms);
async function signUp(w, name, ip) { const r = await w.call('POST', '/v1/register', { ip, body: { name } }); if (r.status !== 200) throw new Error('register ' + r.status); return r.json(); }
const epb = (ip) => ({ public: `${ip}:41100`, local: '192.168.1.5:41100', nonce: 'AAAAAAAAAAA=' });

async function setup() {
  vi.useFakeTimers({ toFake: ['Date'] }); vi.setSystemTime(T0);
  const w = await makeWorker({ DAILY_REQUEST_BUDGET: '400', LIVE_RESERVE_PCT: '20', RESERVE_ROOM_MAX: '0', RESERVE_ADDR_MAX: '0' });
  const ips = ['198.51.100.1', '198.51.100.2', '198.51.100.3'];
  const [a, b, c3] = [await signUp(w, 'PlayerAA', ips[0]), await signUp(w, 'PlayerBB', ips[1]), await signUp(w, 'PlayerCC', ips[2])];
  const stranger = await signUp(w, 'Stranger', '198.51.100.8');
  const code = (await (await w.call('POST', '/v1/rooms', { ip: ips[0], token: a.token, body: {} })).json()).code;
  for (const [x, ip] of [[b, ips[1]], [c3, ips[2]]]) await w.call('POST', '/v1/rooms/join', { ip, token: x.token, body: { code } });
  for (const [x, ip] of [[a, ips[0]], [b, ips[1]], [c3, ips[2]]]) await w.call('POST', `/v1/rooms/${code}/endpoint`, { ip, token: x.token, body: epb(ip) });
  for (const [x, ip] of [[a, ips[0]], [b, ips[1]], [c3, ips[2]]]) await w.call('POST', `/v1/rooms/${code}/started`, { ip, token: x.token });
  // the crowd arrives: strangers' info calls from many addresses until the new-session part of the budget (320) is spent
  for (let i = 0; w.app.budgetUsed() < 324 && i < 600; i++) { tick(1100); await w.call('GET', '/v1/info', { ip: `192.0.2.${(i % 250) + 1}` });
    for (const [x, ip] of [[a, ips[0]], [b, ips[1]], [c3, ips[2]]]) await w.call('GET', `/v1/rooms/${code}`, { ip, token: x.token }); // the game goes on
  }
  expect(w.app.budgetUsed()).toBeGreaterThanOrEqual(320);
  return { w, a, b, c3, stranger, code, ips };
}

describe('live traffic in the reserve band', () => {
  it('a dropped player rejoins his started game; a stranger with the code does not', async () => {
    const { w, a, b, stranger, code, ips } = await setup();
    expect((await w.call('GET', '/v1/info', { ip: '192.0.2.250' })).status).toBe(503); // new sessions are refused now
    expect((await w.call('POST', `/v1/rooms/${code}/leave`, { ip: ips[1], token: b.token })).status).toBe(200);
    tick(5000);
    const rj = await w.call('POST', '/v1/rooms/join', { ip: ips[1], token: b.token, body: { code } });
    expect(rj.status).toBe(200);
    const stranger_ = await w.call('POST', '/v1/rooms/join', { ip: '198.51.100.8', token: stranger.token, body: { code } });
    expect(stranger_.status).toBe(503);
    expect(Number(stranger_.headers.get('retry-after'))).toBeLessThanOrEqual(900);
    // the stranger's retries do not each cost an object request
    const before = w.stats.doRequests;
    for (let i = 0; i < 10; i++) await w.call('POST', '/v1/rooms/join', { ip: '198.51.100.8', token: stranger.token, body: { code } });
    expect(w.stats.doRequests).toBe(before);
    void a;
  });
  it('a member of a live room can post a time; an account in no room cannot (new session)', async () => {
    const { w, a, stranger } = await setup();
    const tm = await w.call('POST', '/v1/times', { ip: '198.51.100.1', token: a.token, body: { galaxy: 0, star: 1, frames: 3000, players: 3, room: 'AAAAAA', build: '3.0.0' } });
    expect(tm.status).not.toBe(503); // refused for its content maybe, not for the budget
    const st = await w.call('POST', '/v1/times', { ip: '198.51.100.8', token: stranger.token, body: { galaxy: 0, star: 1, frames: 3000, players: 1, build: '3.0.0' } });
    expect(st.status).toBe(503);
  });
});
