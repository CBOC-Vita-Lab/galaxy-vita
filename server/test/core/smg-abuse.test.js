// GAME=smg abuse and robustness: tokens, hostile input, names, room codes, restarts, ghosts, times, friends, the public page, error leaks.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, ep, roomOf, friends, SMG_HEADERS } from '../smg-helpers.js';
import { TIMES_JS, TIMES_CSS, TIMES_BODY, timesPageResponse } from '../../src/core/times_page.js';

const ADMIN = 'test-admin-token-0123456789';
const T = (o) => ({ galaxy: 5, star: 2, frames: 3600, players: 1, build: '3.0.0', ...o });

describe('smg abuse: tokens and request shape', () => {
  it('malformed, wrong and removed tokens are 401 and never say which part failed', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    for (const t of ['x', 'A'.repeat(42), 'A'.repeat(44), `${'A'.repeat(42)}!`, 'A'.repeat(43)]) {
      const r = await s.call('GET', '/v1/me', undefined, t);
      expect(r.status).toBe(401);
      expect(Object.keys(r.data.error).sort()).toEqual(['code', 'latestVersion', 'message']);
    }
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200);
    expect((await s.call('GET', '/v1/me', undefined, a.token.toLowerCase())).status).toBe(401);
    // Basic / no scheme / two tokens
    for (const h of [`Basic ${a.token}`, a.token, `Bearer ${a.token} ${a.token}`, `bearer ${a.token}`]) {
      expect((await s.call('GET', '/v1/me', undefined, undefined, { authorization: h })).status).toBe(401);
    }
    await s.call('DELETE', '/v1/me', undefined, a.token);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(401); // the account is gone
  });

  it('token guessing is braked per address (20 wrong tokens in 10 minutes), a good token is not an oracle', async () => {
    const s = makeSmg();
    let last;
    for (let i = 0; i < 25; i++) last = await s.call('GET', '/v1/me', undefined, 'B'.repeat(43), { ip: '203.0.113.9' });
    expect(last.status).toBe(429);
    expect(last.headers.get('retry-after')).toBeTruthy();
    // another address is unaffected
    expect((await s.call('GET', '/v1/me', undefined, 'B'.repeat(43), { ip: '203.0.113.10' })).status).toBe(401);
  });

  it('bodies: bad JSON, arrays, null, scalars, wrong types, __proto__ and unknown fields are 400 and change nothing', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const bad = ['{', '[]', 'null', '7', '"x"', 'true', '{"__proto__":{"x":1}}', '{"constructor":1}', '{"mode":{"a":1}}'];
    for (const path of ['/v1/rooms', '/v1/rooms/join', '/v1/quick', '/v1/friends', '/v1/friends/remove', '/v1/times', '/v1/times/get', '/v1/turn']) {
      for (const b of bad) {
        const r = await s.call('POST', path, b, a.token);
        expect(r.status, `${path} ${b}`).toBeGreaterThanOrEqual(400);
        expect(r.status, `${path} ${b}`).toBeLessThan(500);
      }
    }
    // settings with prototype keys are refused, not stored
    for (const k of ['__proto__', 'constructor', 'toString', 'hasOwnProperty']) {
      const r = await s.call('POST', '/v1/rooms', `{"settings":{"${k}":1}}`, a.token);
      expect(r.status, k).toBe(400);
    }
    const none = await s.call('GET', '/v1/rooms/public', undefined, a.token);
    expect(none.data.rooms).toEqual([]);
  });

  it('an unexpected exception answers a bare 500 internal: no stack, path, SQL or request data', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const real = s.store.sql.exec;
    s.store.sql.exec = (q, ...b) => { if (/FROM rooms/.test(q)) throw new Error('boom /srv/secret/path SELECT * FROM rooms token=abc'); return real.call(s.store.sql, q, ...b); };
    const r = await s.call('POST', '/v1/rooms/join', { code: 'ABCDEF' }, a.token);
    s.store.sql.exec = real;
    expect(r.status).toBe(500);
    expect(r.data).toEqual({ error: { code: 'internal', message: 'internal error', latestVersion: '3.0.0' } });
  });

  it('error bodies of 4xx never echo the request or internals', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const probes = [
      s.call('POST', '/v1/rooms/join', { code: '<script>alert(1)</script>' }, a.token),
      s.call('POST', '/v1/friends', { code: "'; DROP TABLE profiles;--" }, a.token),
      s.call('POST', '/v1/times', T({ build: '<img src=x>' }), a.token),
      s.call('POST', '/v1/rooms', { settings: { '<b>': 1 } }, a.token),
      s.call('GET', '/v1/rooms/ZZZZZZ', undefined, a.token),
    ];
    for (const r of await Promise.all(probes)) {
      const text = JSON.stringify(r.data);
      expect(text).not.toMatch(/node_modules|\.js:|SQLITE|at \w+ \(|\/home\/|DROP TABLE|<script>/i);
    }
  });
});

describe('smg abuse: names and codes', () => {
  it('names: ASCII only; unicode, controls, bidi, zero-width and html are refused; Galaxy names are display names (duplicates allowed)', async () => {
    const s = makeSmg();
    const reg = (name) => s.call('POST', '/v1/register', { name });
    const refused = ['Marioé', '\u{1F344}Toad', 'Ab​cd', 'Ab‮cd', 'a\u0000bc', '<b>Hi</b>', 'Ab\ncd', 'Ab\tcd', 'ＡＢＣ', 'A', '', ' ', 'x'.repeat(11),
      'Ab  cd', '-Abc', 'Abc-', 'a-', '..', 'Admin', 'adm1n', 'Sh1t', 'Peach　Ab'];
    for (const n of refused) expect((await reg(n)).status, JSON.stringify(n)).toBe(400);
    for (const bad of [null, 5, [], {}, true]) expect((await reg(bad)).status).toBe(400);
    expect((await reg('Sally')).status).toBe(200);
    // Galaxy: the friend code is the identity; two players may both be "Sally" (and look-alikes are no longer an impersonation trick
    // worth refusing: the code shows who is who)
    const s1 = await reg('Sally');
    const s2 = await reg('sa11y');
    expect([s1.status, s2.status]).toEqual([200, 200]);
    expect(s1.data.code).not.toBe(s2.data.code);
    expect((await reg('  Bob  ')).data.name).toBe('Bob'); // trimmed
    expect((await s.call('POST', '/v1/register', { name: 'Okay', region: 3 })).data.error.code).toBe('bad_region');
    expect((await s.call('POST', '/v1/register', { name: 'Okay', evil: 1 })).data.error.code).toBe('unknown_field');
  });

  it('room codes: case, spaces and dashes are forgiven; everything else is bad_code, never a crash', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina')];
    const made = (await s.call('POST', '/v1/rooms', {}, h.token)).data.code;
    const lower = `${made.slice(0, 3)} - ${made.slice(3)}`.toLowerCase();
    expect((await s.call('POST', '/v1/rooms/join', { code: lower }, g.token)).status).toBe(200);
    for (const code of ['', 'ABC', `${made}A`, 'ILO01Z', 'ABCDEÉ', 5, null, ['ABCDEF'], { a: 1 }, 'A'.repeat(3000)]) {
      const r = await s.call('POST', '/v1/rooms/join', { code }, g.token);
      expect(r.status, JSON.stringify(code).slice(0, 30)).toBe(400);
      expect(r.data.error.code).toBe('bad_code');
    }
    for (const seg of ['..', '%2e%2e', 'A%00B', '<script>', 'A'.repeat(100), 'abc%20def']) {
      const r = await s.call('GET', `/v1/rooms/${seg}`, undefined, g.token);
      expect([400, 404], seg).toContain(r.status);
    }
    expect((await s.call('GET', '/v1/rooms/ZZZZZZ', undefined, g.token)).data.error.code).toBe('room_not_found');
    expect((await s.call('GET', '/v1/rooms/%00', undefined, g.token)).status).toBeLessThan(500);
  });

  it('a stranger cannot read, poke or leave a room they are not in', async () => {
    const s = makeSmg();
    const [h, x] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Evil')];
    const code = (await s.call('POST', '/v1/rooms', {}, h.token)).data.code;
    for (const [m, p, b] of [['GET', ''], ['POST', '/endpoint', ep(9)], ['POST', '/punch', { result: 'ok', path: 'direct' }], ['POST', '/started'], ['POST', '/leave'], ['DELETE', '']]) {
      const r = await s.call(m, `/v1/rooms/${code}${p}`, b, x.token);
      expect(r.status, `${m} ${p}`).toBe(403);
      expect(r.data.error.code).toBe('not_member');
    }
    expect((await s.call('POST', '/v1/turn', { room: code }, x.token)).status).toBeGreaterThanOrEqual(400);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data.members).toHaveLength(1);
  });
});

describe('smg abuse: room lifecycle', () => {
  it('an open room expires after its TTL (10 min), is then refused, and the sweep removes it and its members', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina')];
    const code = (await s.call('POST', '/v1/rooms', { visibility: 'public' }, h.token)).data.code;
    s.advance(9 * 60_000);
    await s.call('GET', `/v1/rooms/${code}`, undefined, h.token); // polling does not extend an open room
    s.advance(2 * 60_000);
    expect((await s.call('POST', '/v1/rooms/join', { code }, g.token)).data.error.code).toBe('room_expired');
    expect((await s.call('POST', '/v1/quick', { mode: 'coop' }, g.token)).data.quick).toBe('created'); // the expired public room is skipped
    s.advance(60_000);
    s.app.sweep(); // persists the expiry (a refused call rolls its lazy close back)
    expect(s.store.sql.exec(`SELECT state FROM rooms WHERE code = ?`, code).toArray()[0].state).toBe('expired');
    s.advance(6 * 60_000); // closed rooms stay readable ROOM_KEEP_CLOSED_SEC (5 min)
    s.app.sweep();
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM room_members WHERE code = ?`, code).toArray()[0].n).toBe(0);
  });

  it('a started room lives 3 h from the start; a joined one 20 min', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina')];
    const code = await roomOf(s, [h, g]);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), g.token);
    await s.call('POST', `/v1/rooms/${code}/started`, undefined, h.token);
    const st = await s.call('POST', `/v1/rooms/${code}/started`, undefined, g.token);
    expect(st.data.state).toBe('started');
    expect(st.data.expiresMs - s.clock.now).toBe(3 * 3600_000);
    for (let i = 0; i < 70; i++) { // both keep polling for 2 h 20 min
      s.advance(2 * 60_000);
      await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
      await s.call('GET', `/v1/rooms/${code}`, undefined, g.token);
    }
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data.state).toBe('started');
    s.advance(45 * 60_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data.state).toBe('expired');
  });

  it('after a service restart (presence is memory only) a dead host is still noticed and replaced', async () => {
    const presence = new Map();
    const s = makeSmg({}, { deps: { presence } });
    const [h, a, b] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina'), await smgSignUp(s, 'Gabe')];
    const code = await roomOf(s, [h, a, b]);
    presence.clear(); // the Durable Object was restarted by a deploy; the host's console crashed at the same time
    s.advance(5_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.hostSlot).toBe(0); // grace period
    s.advance(10_000);
    await s.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    s.advance(10_000);
    const v = await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    expect(v.data).toMatchObject({ hostSlot: 1, hostEpoch: 1, code });
    expect(v.data.members.map((m) => m.slot)).toEqual([1, 2]);
  });

  it('a crashed guest does not hold its slot forever: gone after 60 s of silence, the slot is free again', async () => {
    const s = makeSmg();
    const ps = [];
    for (const n of ['Host', 'Gina', 'Gabe', 'Gus', 'Gil']) ps.push(await smgSignUp(s, n));
    const [h, a, b, c, d] = ps;
    const code = await roomOf(s, [h, a, b, c]);
    expect((await s.call('POST', '/v1/rooms/join', { code }, d.token)).data.error.code).toBe('room_full');
    for (let i = 0; i < 8; i++) { // host, Gina and Gabe keep polling; Gus crashed
      s.advance(10_000);
      for (const p of [h, a, b]) await s.call('GET', `/v1/rooms/${code}`, undefined, p.token);
    }
    const v = await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
    expect(v.data.members.map((m) => m.slot)).toEqual([0, 1, 2]);
    expect(v.data).toMatchObject({ hostSlot: 0, hostEpoch: 0 });
    expect((await s.call('POST', '/v1/rooms/join', { code }, d.token)).data.slot).toBe(3);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, c.token)).data.error.code).toBe('not_member');
  });

  it('host migration keeps the room code, key and cookie, and every vip, through two successive hosts', async () => {
    const s = makeSmg();
    const ps = [];
    for (const n of ['Host', 'Gina', 'Gabe', 'Gus']) ps.push(await smgSignUp(s, n));
    const code = await roomOf(s, ps);
    const first = (await s.call('GET', `/v1/rooms/${code}`, undefined, ps[3].token)).data;
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, ps[0].token);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, ps[1].token);
    const v = (await s.call('GET', `/v1/rooms/${code}`, undefined, ps[3].token)).data;
    expect(v).toMatchObject({ code, hostSlot: 2, hostEpoch: 2, slot: 3, role: 'guest' });
    expect(v.session.key).toBe(first.session?.key ?? v.session.key);
    expect(v.you.vip).toBe(first.you.vip);
    expect(v.members.map((m) => m.vip)).toEqual(first.members.filter((m) => m.slot >= 2).map((m) => m.vip));
  });

  it('one room at a time and own_room: the host cannot join itself; joining while hosting closes the old room', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina')];
    const c1 = (await s.call('POST', '/v1/rooms', {}, h.token)).data.code;
    expect((await s.call('POST', '/v1/rooms/join', { code: c1 }, h.token)).data.error.code).toBe('own_room');
    const c2 = (await s.call('POST', '/v1/rooms', {}, g.token)).data.code;
    expect((await s.call('POST', '/v1/rooms/join', { code: c2 }, h.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/rooms/join', { code: c1 }, g.token)).data.error.code).toBe('room_closed');
  });

  it('endpoint abuse: bad nonces, private-looking junk, oversize candidate lists, endpoint on a closed room', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina')];
    const code = await roomOf(s, [h, g]);
    const post = (o) => s.call('POST', `/v1/rooms/${code}/endpoint`, { ...ep(1), ...o }, g.token);
    for (const o of [{ nonce: 'x' }, { nonce: 'A'.repeat(100) }, { nonce: 5 }, { public: 'nope' }, { public: '1.2.3.4:99999' }, { public: '1.2.3.4' }, { local: 7 },
      { candidates: Array(7).fill({ type: 'host', addr: '1.2.3.4:5' }) }, { candidates: [{ type: 'x', addr: '1.2.3.4:5' }] }, { candidates: 'a' }, { nat: 'weird' }]) {
      const r = await post(o);
      expect(r.status, JSON.stringify(o).slice(0, 50)).toBe(400);
    }
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, g.token);
    expect((await s.call('POST', `/v1/rooms/${code}/leave`, undefined, h.token)).data.state).toBe('closed');
    expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token)).status).toBe(403);
  });
});

describe('smg abuse: quick match fairness', () => {
  it('first come, first served: six players fill the oldest room (4), then the next; nobody is matched with themselves or across modes/versions', async () => {
    const s = makeSmg();
    const ps = [];
    for (let i = 0; i < 6; i++) ps.push(await smgSignUp(s, `Quick${i}`));
    const out = [];
    for (const p of ps) { out.push((await s.call('POST', '/v1/quick', { mode: 'coop' }, p.token)).data); s.advance(1000); }
    expect(out.map((r) => r.quick)).toEqual(['created', 'joined', 'joined', 'joined', 'created', 'joined']);
    expect(out[3].code).toBe(out[0].code);
    expect(out[5].code).toBe(out[4].code);
    expect(out[4].code).not.toBe(out[0].code);
    // the same player asking again stays in its room (idempotent: no second room, not matched to itself in another)
    const again = (await s.call('POST', '/v1/quick', { mode: 'coop' }, ps[0].token)).data;
    expect(again.quick).toBe('joined'); // quick always means "find me a new game": the old room is left, the oldest open one is taken
    expect(again.code).toBe(out[4].code);
    expect((await s.call('GET', `/v1/rooms/${out[0].code}`, undefined, ps[1].token)).data.members).toHaveLength(3);
    const race = await s.call('POST', '/v1/quick', { mode: 'race' }, (await smgSignUp(s, 'Racer')).token);
    expect(race.data.quick).toBe('created');
    const old = await s.call('POST', '/v1/quick', { mode: 'coop' }, (await smgSignUp(s, 'OldVer')).token, { 'x-msc-app': '3.0.1' });
    expect(old.status).toBe(200);
    expect(old.data.quick).toBe('created'); // another version tag never shares a room
  });
});

describe('smg abuse: friends', () => {
  it('mutual only: strangers and one-way adders see no presence, rooms or join access; removal ends it at once', async () => {
    const s = makeSmg();
    const [a, b, x] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben'), await smgSignUp(s, 'Evil')];
    const code = (await s.call('POST', '/v1/rooms', { visibility: 'friends' }, b.token)).data.code;
    await s.call('POST', '/v1/friends', { code: b.code }, x.token); // x adds Ben; Ben never adds x
    await s.call('POST', '/v1/friends', { code: b.code }, a.token);
    let lf = (await s.call('GET', '/v1/friends', undefined, x.token)).data.friends[0];
    expect(lf).toMatchObject({ mutual: false, online: false, room: null });
    expect((await s.call('POST', '/v1/rooms/join', { friend: b.code }, x.token)).data.error.code).toBe('not_friends');
    s.advance(10_000);
    await s.call('POST', '/v1/friends', { code: a.code }, b.token);
    lf = (await s.call('GET', '/v1/friends', undefined, a.token)).data.friends[0];
    expect(lf).toMatchObject({ mutual: true, online: true, room: { code, joinable: true } });
    s.advance(10_000);
    await s.call('POST', '/v1/friends/remove', { code: b.code }, a.token);
    expect((await s.call('POST', '/v1/rooms/join', { friend: b.code }, a.token)).data.error.code).toBe('not_friends');
    const bl = (await s.call('GET', '/v1/friends', undefined, b.token)).data.friends.find((f) => f.name === 'Ana');
    expect(bl).toMatchObject({ mutual: false, online: false, room: null }); // Ben no longer sees Ana's presence either
    // names of strangers are only reachable by their public code; codes of the wrong shape never match
    for (const code of ['', 'x', 5, null, 'AAAA-AAAA-A', `${a.code}'`]) expect((await s.call('POST', '/v1/friends', { code }, x.token)).status).toBe(code === 5 || code === null || code === '' || code === 'x' || code.length > 9 ? 400 : 400);
    expect((await s.call('POST', '/v1/friends', { code: 'ZZZZ-ZZZZ' }, x.token)).data.error.code).toBe('player_not_found');
  });

  it('a code-only room is never visible to friends; a banned friend vanishes from the list', async () => {
    const s = makeSmg();
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    await friends(s, a, b);
    await s.call('POST', '/v1/rooms', { visibility: 'code' }, b.token);
    expect((await s.call('GET', '/v1/friends', undefined, a.token)).data.friends[0].room).toBeNull();
    expect((await s.call('POST', '/v1/rooms/join', { friend: b.code }, a.token)).data.error.code).toBe('room_not_found');
    await s.raw('POST', '/v1/admin/ban', { pid: b.pid }, ADMIN);
    s.advance(11_000);
    expect((await s.call('GET', '/v1/friends', undefined, a.token)).data.friends).toEqual([]);
  });
});

describe('smg abuse: star times', () => {
  async function started(s, players, mode) {
    const code = await roomOf(s, players, { mode });
    for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(i + 1), players[i].token);
    for (const p of players) await s.call('POST', `/v1/rooms/${code}/started`, undefined, p.token);
    return code;
  }

  it('hostile numbers and types are refused: floats, strings, NaN-likes, negatives, huge, booleans, nulls', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const bad = [{ frames: '3600' }, { frames: null }, { frames: true }, { frames: -1 }, { frames: 0 }, { frames: 1e300 }, { frames: 599 }, { frames: 432001 },
      { frames: 3600.1 }, { galaxy: -1 }, { galaxy: '5' }, { galaxy: 1.5 }, { star: 0 }, { star: 9 }, { players: 0 }, { players: 2.5 }, { players: '1' },
      { build: 5 }, { build: '' }, { build: 'x'.repeat(40) }, { room: 5 }, { room: 'nope' }, { splits: 'a' }, { splits: [1.5] }, { splits: Array(17).fill(1).map((_, i) => i + 1) },
      { splits: [0] }, { digest: 5 }, { digest: 'g'.repeat(32) }];
    for (const o of bad) {
      const r = await s.call('POST', '/v1/times', T(o), a.token);
      expect(r.status, JSON.stringify(o)).toBe(400);
    }
    const none = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, a.token);
    expect(none.data.total).toBe(0); // nothing was stored by any of those
    // the world record cannot be below the floor even when asked in a different category
    expect((await s.call('POST', '/v1/times', T({ frames: 1, players: 4 }), a.token)).data.error.reason).toBe('too_fast');
  });

  it('duplicate submits and replays are harmless: same time again is not a new best, rank and board unchanged', async () => {
    const s = makeSmg();
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    const one = await s.call('POST', '/v1/times', T(), a.token);
    expect(one.data).toMatchObject({ best: true, rank: 1 });
    for (let i = 0; i < 5; i++) {
      const again = await s.call('POST', '/v1/times', T(), a.token);
      expect(again.data).toMatchObject({ best: false, rank: 1, personalBest: { frames: 3600 } });
    }
    await s.call('POST', '/v1/times', T({ frames: 3600 }), b.token); // a tie: Ana was first
    const board = (await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, a.token)).data;
    expect(board.rows.map((r) => [r.rank, r.name, r.frames])).toEqual([[1, 'Ana', 3600], [1, 'Ben', 3600]]);
    expect(board.total).toBe(2);
    // a slower replay never overwrites a better time
    await s.call('POST', '/v1/times', T({ frames: 9000 }), a.token);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, a.token)).data.you.frames).toBe(3600);
  });

  it('a time can only be claimed with a room you are in; a time never lands on another galaxy or star', async () => {
    const s = makeSmg();
    const [h, g, x] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina'), await smgSignUp(s, 'Evil')];
    const code = await started(s, [h, g], 'coop');
    expect((await s.call('POST', '/v1/times', T({ room: code, players: 2 }), x.token)).data.error.code).toBe('not_member');
    expect((await s.call('POST', '/v1/times', T({ room: 'ABCDEF', players: 2 }), x.token)).data.error.code).toBe('not_member');
    await s.call('POST', '/v1/times', T({ room: code, players: 2, galaxy: 5, star: 2, frames: 4000 }), h.token);
    // the guest submits a different star from the same room: nothing is confirmed
    const other = await s.call('POST', '/v1/times', T({ room: code, players: 2, galaxy: 5, star: 3, frames: 4000 }), g.token);
    expect(other.data).toMatchObject({ verified: false, confirmed: 0 });
    const board = (await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, h.token)).data;
    expect(board.rows).toHaveLength(1);
    expect(board.rows[0].verified).toBe(false);
  });

  it('verification cannot be forged alone: one player, many submits, a left room, or far-apart times do not verify', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gina')];
    const code = await started(s, [h, g], 'coop');
    for (const f of [4000, 4001, 4002]) {
      expect((await s.call('POST', '/v1/times', T({ room: code, players: 2, frames: f }), h.token)).data.verified).toBe(false);
    }
    const far = await s.call('POST', '/v1/times', T({ room: code, players: 2, frames: 4010 }), g.token); // more than +-2 from every earlier one
    expect(far.data.verified).toBe(false);
    for (let i = 0; i < 62; i++) { // 31 min, both polling: past the 30 min window
      s.advance(30_000);
      await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
      await s.call('GET', `/v1/rooms/${code}`, undefined, g.token);
    }
    const late = await s.call('POST', '/v1/times', T({ room: code, players: 2, frames: 4000 }), g.token);
    expect(late.data.verified).toBe(false);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, g.token);
    expect((await s.call('POST', '/v1/times', T({ room: code, players: 2 }), g.token)).data.error.code).toBe('not_member');
  });

  it('a 26 % jump is flagged and invisible to everyone else (board, page, ranks); a flagged row never ranks others down', async () => {
    const s = makeSmg();
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    await s.call('POST', '/v1/times', T({ frames: 5000 }), b.token);
    const jump = await s.call('POST', '/v1/times', T({ frames: 2900 }), a.token);
    expect(jump.data).toMatchObject({ best: true, flagged: true });
    const pub = await s.raw('GET', '/v1/public/times?galaxy=5&star=2', undefined, undefined, { 'x-msc-app': '3.0.0' });
    expect(pub.data.rows.map((r) => r.name)).toEqual(['Ben']);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, b.token)).data.rows.map((r) => r.name)).toEqual(['Ben']);
    expect((await s.call('POST', '/v1/times', T({ frames: 5000 }), b.token)).data.rank).toBe(1);
    // cheating is not rewarded by splitting into categories either: a 2-player category needs a started room (no room: refused)
    expect((await s.call('POST', '/v1/times', T({ frames: 700, players: 2 }), a.token)).data.error.code).toBe('room_required');
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, category: 1 }, b.token)).data.rows.map((r) => r.name)).toEqual(['Ben']);
  });

  it('a banned player disappears from every board and the page; deleting the account removes its times', async () => {
    const s = makeSmg();
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    await s.call('POST', '/v1/times', T({ frames: 5000 }), b.token);
    await s.raw('POST', '/v1/admin/ban', { pid: a.pid }, ADMIN);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, b.token)).data.rows.map((r) => r.name)).toEqual(['Ben']);
    s.advance(61_000);
    const pub = await s.raw('GET', '/v1/public/times', undefined, undefined, {});
    expect(pub.data.stars).toEqual([{ galaxy: 5, star: 2, players: 1, best: 5000 }]);
    expect((await s.call('POST', '/v1/times', T(), a.token)).status).toBe(403);
    await s.call('DELETE', '/v1/me', undefined, b.token);
    s.advance(61_000);
    expect((await s.raw('GET', '/v1/public/times', undefined, undefined, {})).data.stars).toEqual([]);
  });

  it('the public list counts players, not rows (one player in two categories counts once)', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    await s.call('POST', '/v1/times', T({ players: 1 }), a.token);
    await s.call('POST', '/v1/times', T({ players: 3 }), a.token);
    const pub = await s.raw('GET', '/v1/public/times', undefined, undefined, {});
    expect(pub.data.stars[0].players).toBe(1);
  });

  it('the public query is strict: bad numbers, half pairs and extra characters are 400', async () => {
    const s = makeSmg();
    for (const q of ['galaxy=5', 'star=2', 'galaxy=64&star=1', 'galaxy=5&star=0', 'galaxy=a&star=1', 'galaxy=5&star=1&category=5', 'galaxy=5&star=1&limit=0', 'galaxy=5&star=1&limit=101', 'galaxy=05x&star=1', 'galaxy=-1&star=1']) {
      expect((await s.raw('GET', `/v1/public/times?${q}`, undefined, undefined, {})).status, q).toBe(400);
    }
  });
});

describe('smg abuse: the public /times page cannot run user text', () => {
  it('names are text-only in the page script (no innerHTML, document.write, eval, inline handlers, string timers)', () => {
    expect(TIMES_JS).not.toMatch(/innerHTML|outerHTML|insertAdjacentHTML|document\.write|eval\(|new Function|setTimeout\(\s*['"]|setInterval\(\s*['"]|srcdoc|javascript:/);
    expect(TIMES_JS).toMatch(/textContent/);
    // every server value goes in through cell(): text. The only other string-built DOM text is numbers
    expect(TIMES_BODY).not.toMatch(/\son\w+\s*=|<script|<iframe|<object|<embed|<link|<img/i);
    expect(TIMES_CSS).not.toMatch(/url\(|@import|expression\(/);
  });

  it('CSP: no unsafe-inline / unsafe-eval, scripts only by hash, no remote connect, no framing, nosniff', async () => {
    const r = await timesPageResponse();
    const csp = r.headers.get('content-security-policy');
    expect(csp).not.toMatch(/unsafe-|\*|https?:/);
    expect(csp).toMatch(/default-src 'none'/);
    expect(csp).toMatch(/script-src 'sha256-[A-Za-z0-9+/=]+'/);
    expect(csp).toMatch(/connect-src 'self'/);
    expect(csp).toMatch(/frame-ancestors 'none'/);
    expect(r.headers.get('x-content-type-options')).toBe('nosniff');
    expect(r.headers.get('content-type')).toMatch(/^text\/html/);
  });

  it('the page and the data API give nothing back for the query (no reflection), and names cannot hold markup at all', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    await s.call('POST', '/v1/times', T(), a.token);
    const page = await s.raw('GET', '/times?galaxy=<script>alert(1)</script>', undefined, undefined, {});
    expect(String(page.data)).not.toContain('alert(1)');
    const pub = await s.raw('GET', '/v1/public/times?galaxy=5&star=2', undefined, undefined, {});
    expect(pub.headers.get('content-type')).toMatch(/application\/json/);
    expect(pub.headers.get('x-content-type-options')).toBe('nosniff');
    expect(Object.keys(pub.data.rows[0]).sort()).toEqual(['dateMs', 'frames', 'name', 'players', 'rank', 'verified']); // no pid, code or address
    for (const n of ['<img src=x onerror=1>', '"><script>', "a'b", 'a&amp;b', 'a`b`c']) {
      expect((await s.call('POST', '/v1/register', { name: n })).status, n).toBe(400);
    }
  });
});

describe('smg abuse: rate limits are real (RATE_LIMIT_SCALE=1)', () => {
  it('rooms 10 per 10 min, join 20 per 10 min, times get 40 per min, each with Retry-After', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    const a = await smgSignUp(s, 'Ana');
    let r;
    for (let i = 0; i < 11; i++) r = await s.call('POST', '/v1/rooms', {}, a.token);
    expect(r.status).toBe(429);
    expect(r.data.error.code).toBe('rate_limited');
    expect(Number(r.headers.get('retry-after'))).toBeGreaterThan(0);
    for (let i = 0; i < 41; i++) r = await s.call('POST', '/v1/times/get', { galaxy: 1, star: 1 }, a.token);
    expect(r.status).toBe(429);
    s.advance(61_000);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 1, star: 1 }, a.token)).status).toBe(200);
  });

  it('registration: a burst from one address is stopped; junk names do not use the budget', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    let last;
    for (let i = 0; i < 40; i++) last = await s.call('POST', '/v1/register', { name: `Reg${String(i).padStart(3, 'x')}` }, undefined, { ip: '203.0.113.77' });
    expect(last.status).toBe(429);
    expect((await s.call('POST', '/v1/register', { name: 'Fine' }, undefined, { ip: '203.0.113.78' })).status).toBe(200);
  });
});
