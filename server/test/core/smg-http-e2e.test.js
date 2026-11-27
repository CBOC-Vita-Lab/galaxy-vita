// GAME=smg over real HTTP against the local fake server (the same application code as the Worker): the whole happy path a client walks,
// plus the wire-level abuse that only shows with a real HTTP stack (huge bodies, wrong methods, wrong headers, odd paths).
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { startFake } from '../../fake/server.mjs';

let fake;
let base;
beforeAll(async () => {
  fake = await startFake({ httpPort: 0, https: false, env: { GAME: 'smg', SMG_VERIFY_SLACK_SEC: '600', REGISTER_POW_BITS: '0', SMG_BOARD_UNVERIFIED: 'show', SMG_FLAG_NEW_STAR: '0', SMG_VERIFY_MIN_AGE_HOURS: '0', SMG_VERIFY_DISTINCT_ADDR: '0' } });
  base = fake.httpUrl;
});
afterAll(async () => { await fake.close(); });

const H = { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0', 'x-msc-proto': '1', 'content-type': 'application/json' };
async function api(method, path, body, token, headers = {}) {
  const hh = { ...H, ...(token ? { authorization: `Bearer ${token}` } : {}), ...headers };
  for (const k of Object.keys(hh)) if (hh[k] === undefined) delete hh[k]; // (undefined = leave the header out)
  const r = await fetch(base + path, {
    method, headers: hh,
    body: body === undefined ? undefined : typeof body === 'string' ? body : JSON.stringify(body),
  });
  const text = await r.text();
  let data = text;
  try { data = JSON.parse(text); } catch { /* html */ }
  return { status: r.status, data, headers: r.headers };
}
const reg = async (name) => (await api('POST', '/v1/register', { name })).data;
const ep = (n) => ({ public: `203.0.113.${n}:41100`, local: `192.168.1.${n}:41100`, nonce: 'AAAAAAAAAAA=' });

describe('smg over HTTP: the happy path', () => {
  it('register, host, join by code, endpoints, start, quick match, friends, times, boards, public page', async () => {
    const [h, g, k] = [await reg('Hanna'), await reg('Gus'), await reg('Kim')];
    expect(h).toMatchObject({ name: 'Hanna', token: expect.stringMatching(/^[A-Za-z0-9_-]{43}$/), code: expect.stringMatching(/^[A-Z2-9]{4}-[A-Z2-9]{4}$/) });
    expect((await api('GET', '/v1/me', undefined, h.token)).data.name).toBe('Hanna');

    for (const [x, y] of [[h, g], [g, h]]) await api('POST', '/v1/friends', { code: y.code }, x.token); // (svcfix3: a friends room opens to the host's friends, also by code)
    const made = (await api('POST', '/v1/rooms', { mode: 'race', visibility: 'friends', settings: { galaxy: 5, scenario: 2 } }, h.token)).data;
    expect(made).toMatchObject({ state: 'open', slot: 0, role: 'host' });
    const joined = (await api('POST', '/v1/rooms/join', { code: made.code.toLowerCase() }, g.token)).data;
    expect(joined).toMatchObject({ slot: 1, role: 'guest', state: 'joined' });
    await api('POST', `/v1/rooms/${made.code}/endpoint`, ep(1), h.token);
    const ready = (await api('POST', `/v1/rooms/${made.code}/endpoint`, ep(2), g.token)).data;
    expect(ready.state).toBe('ready');
    expect((await api('POST', `/v1/rooms/${made.code}/punch`, { result: 'ok', path: 'direct', ms: 120 }, g.token)).status).toBe(200);
    await api('POST', `/v1/rooms/${made.code}/started`, undefined, h.token);
    expect((await api('POST', `/v1/rooms/${made.code}/started`, undefined, g.token)).data.state).toBe('started');
    const poll = await api('GET', `/v1/rooms/${made.code}?v=${ready.version}`, undefined, h.token);
    expect(poll.data.changed).toBe(true);

    // friends: mutual, then join the friend's room
    await api('POST', '/v1/friends', { code: h.code }, k.token);
    await api('POST', '/v1/friends', { code: k.code }, h.token);
    const fl = (await api('GET', '/v1/friends', undefined, k.token)).data;
    expect(fl.friends[0]).toMatchObject({ name: 'Hanna', mutual: true, online: true, room: { code: made.code, joinable: true, mode: 'race' } });
    expect((await api('POST', '/v1/rooms/join', { friend: h.code }, k.token)).data.slot).toBe(2); // drop-in to a started room

    // times: both finish the race, verified; boards
    const t = (p, o) => api('POST', '/v1/times', { galaxy: 5, star: 2, frames: 3600, players: 3, build: '3.0.0', room: made.code, ...o }, p.token);
    // (service review: the first finisher is verified once a second member's result arrives)
    expect((await t(h, { frames: 3500 })).data).toMatchObject({ ok: true, best: true, verified: false, rank: 1, category: 3 });
    expect((await t(g, { frames: 3700 })).data).toMatchObject({ verified: true, confirmed: 1, rank: 2 });
    const board = (await api('POST', '/v1/times/get', { galaxy: 5, star: 2, category: 3 }, g.token)).data;
    expect(board.rows.map((r) => [r.rank, r.name, r.frames, r.you])).toEqual([[1, 'Hanna', 3500, false], [2, 'Gus', 3700, true]]);
    expect((await api('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'friends' }, h.token)).data.rows.map((r) => r.name)).toEqual(['Hanna', 'Gus']);
    const pub = await api('GET', '/v1/public/times?galaxy=5&star=2', undefined, undefined, { 'x-msc-game': undefined });
    expect(pub.status).toBe(200);
    expect(pub.data.rows).toHaveLength(2);
    const page = await fetch(`${base}/times`);
    expect(page.status).toBe(200);
    expect(page.headers.get('content-security-policy')).toMatch(/default-src 'none'/);

    // leaving: the host goes, the room moves on with the same code
    const left = (await api('POST', `/v1/rooms/${made.code}/leave`, undefined, h.token)).data;
    expect(left).toMatchObject({ ok: true, hostSlot: 1, hostEpoch: 1, state: 'started' });
    expect((await api('GET', `/v1/rooms/${made.code}`, undefined, g.token)).data).toMatchObject({ role: 'host', code: made.code });
  });

  it('quick match over HTTP: two players meet in one public room', async () => {
    const [a, b] = [await reg('Quincy'), await reg('Quinn')];
    const one = (await api('POST', '/v1/quick', { mode: 'coop' }, a.token)).data;
    expect(one.quick).toBe('created');
    const two = (await api('POST', '/v1/quick', { mode: 'coop' }, b.token)).data;
    expect(two).toMatchObject({ quick: 'joined', code: one.code });
    const list = (await api('GET', '/v1/rooms/public?mode=coop', undefined, (await reg('Looker')).token)).data.rooms;
    expect(list.find((r) => r.code === one.code)).toMatchObject({ players: 2, host: 'Quincy' });
  });
});

describe('smg over HTTP: wire-level abuse', () => {
  it('headers: no game / wrong game / old app / bad version / bad proto are refused with stable codes', async () => {
    const a = await reg('Wirey');
    const get = (headers) => api('GET', '/v1/me', undefined, a.token, headers);
    expect((await get({ 'x-msc-game': undefined })).data.error.code).toBe('game_mismatch');
    expect((await get({ 'x-msc-game': 'msc' })).data.error.code).toBe('game_mismatch');
    expect((await get({ 'x-msc-game': 'xyz' })).data.error.code).toBe('bad_game');
    expect((await get({ 'x-msc-app': '2.2.1' })).status).toBe(426);
    expect((await get({ 'x-msc-app': 'banana' })).data.error.code).toBe('bad_app_version');
    expect((await get({ 'x-msc-proto': 'abc' })).data.error.code).toBe('bad_app_version');
    expect((await get({ 'x-msc-build': 'zz' })).data.error.code).toBe('bad_app_version');
    expect((await get({ 'x-msc-build': 'abcdef12' })).status).toBe(200);
  });

  it('methods, paths and sizes: wrong method 405, unknown 404, Strikers-only routes 404, huge body 413, junk JSON 400', async () => {
    const a = await reg('Sizey');
    expect((await api('PATCH', '/v1/me', undefined, a.token)).status).toBe(405);
    expect((await api('GET', '/v1/rooms', undefined, a.token)).status).toBe(405);
    for (const p of ['/v1/ranked/queue', '/v1/rank/get', '/v1/matches/abc', '/v1/me/matches', '/leaderboard', '/v1/public/leaderboard', '/v1/admin/stats/../x', '/', '/%00', '/v1/../etc/passwd']) {
      const r = await api('GET', p, undefined, a.token);
      expect([404, 400, 401], p).toContain(r.status);
      expect(r.status, p).toBeLessThan(500);
    }
    expect((await api('POST', '/v1/register', { name: 'x'.repeat(5000) })).status).toBe(413);
    expect((await api('POST', '/v1/rooms', 'x'.repeat(100_000), a.token)).status).toBe(413);
    expect((await api('POST', '/v1/rooms', '{"mode":', a.token)).data.error.code).toBe('bad_json');
    expect((await api('POST', '/v1/rooms', '\u0000\u0001', a.token)).status).toBe(400);
    // admin without a token or with a wrong one
    expect([401, 404]).toContain((await api('GET', '/v1/admin/stats', undefined, undefined, {})).status);
    expect([401, 404]).toContain((await api('GET', '/v1/admin/stats', undefined, 'nope')).status);
    expect((await api('POST', '/v1/admin/ban', { pid: 1 }, a.token)).status).toBeGreaterThanOrEqual(401); // a player token is not an admin token
  });

  it('every error answer is JSON with a code, never HTML or a stack, and never sets cookies or CORS wildcards for tokens', async () => {
    const a = await reg('Jsony');
    for (const [m, p, b] of [['GET', '/nope'], ['POST', '/v1/rooms/join', { code: 'x' }], ['GET', '/v1/rooms/ZZZZZZ'], ['POST', '/v1/times', { galaxy: 99 }], ['DELETE', '/v1/friends/QQQQ-QQQQ']]) {
      const r = await api(m, p, b, a.token);
      expect(r.headers.get('content-type')).toMatch(/application\/json/);
      expect(r.data.error.code).toMatch(/^[a-z_]+$/);
      expect(r.headers.get('set-cookie')).toBeNull();
      expect(r.headers.get('access-control-allow-origin')).toBeNull();
      expect(r.headers.get('cache-control')).toBe('no-store');
    }
  });
});
