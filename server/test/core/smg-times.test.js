// GAME=smg star times: sanity, best rows, co-op / race verification, boards (top / near / friends), the public page.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, ep, roomOf, friends } from '../smg-helpers.js';

const T = (o) => ({ galaxy: 5, star: 2, frames: 3600, players: 1, build: '3.0.0', ...o });

async function startedRoom(s, players, mode) {
  const code = await roomOf(s, players, { mode });
  for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(i + 1), players[i].token);
  for (const p of players) await s.call('POST', `/v1/rooms/${code}/started`, undefined, p.token);
  return code;
}

describe('smg times: submit', () => {
  it('sanity: floor (global and per star), cap, splits, build allowlist, digest, strict fields', async () => {
    const s = makeSmg({ SMG_STAR_FLOORS: '{"5:2":1800}', SMG_TIME_BUILDS: '3.0.0,3.0.1' });
    const a = await smgSignUp(s, 'Ana');
    const put = (o) => s.call('POST', '/v1/times', T(o), a.token);
    expect((await put({ frames: 1799 })).data.error).toMatchObject({ code: 'bad_time', reason: 'too_fast' });
    expect((await put({ star: 3, frames: 599 })).data.error.reason).toBe('too_fast'); // global floor 600
    expect((await put({ star: 3, frames: 600 })).status).toBe(200);
    expect((await put({ frames: 432001 })).data.error.reason).toBe('too_slow');
    expect((await put({ frames: 3600.5 })).data.error.code).toBe('bad_time');
    expect((await put({ splits: [100, 90] })).data.error.code).toBe('bad_splits');
    expect((await put({ splits: [100, 4000] })).data.error.code).toBe('bad_splits');
    expect((await put({ build: '2.2.1' })).data.error.code).toBe('bad_build');
    expect((await put({ digest: 'xyz' })).data.error.field).toBe('digest');
    expect((await put({ cheat: true })).data.error.code).toBe('unknown_field');
    expect((await put({ galaxy: 64 })).data.error.field).toBe('galaxy');
    expect((await put({ players: 5 })).data.error.field).toBe('players');
    const ok = await put({ splits: [600, 1200, 3000], digest: '00112233445566778899aabbccddeeff', build: '3.0.1' });
    expect(ok.data).toMatchObject({ ok: true, best: true, verified: false, flagged: false, rank: 1, category: 1, personalBest: { frames: 3600, verified: false } });
  });

  it('keeps the best per category; a > 25 % improvement is kept but flagged and hidden from others', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    expect((await s.call('POST', '/v1/times', T({ frames: 4100 }), a.token)).data).toMatchObject({ best: false, personalBest: { frames: 4000 } });
    expect((await s.call('POST', '/v1/times', T({ frames: 3800 }), a.token)).data).toMatchObject({ best: true, flagged: false });
    const jump = await s.call('POST', '/v1/times', T({ frames: 2000 }), a.token);
    expect(jump.data).toMatchObject({ best: true, flagged: true });
    const other = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'top' }, b.token);
    expect(other.data.rows).toEqual([]);
    const own = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'top' }, a.token);
    expect(own.data.rows).toEqual([{ rank: 1, name: 'Ana', frames: 2000, players: 1, verified: false, you: true }]);
    const flags = await s.raw('GET', '/v1/admin/flags', undefined, 'test-admin-token-0123456789');
    expect(flags.data.flags[0]).toMatchObject({ pid: a.pid, kind: 'time_jump', detail: { before: 4000, after: 2000 } });
    await s.raw('POST', '/v1/admin/flags/clear', { pid: a.pid }, 'test-admin-token-0123456789');
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, b.token)).data.rows).toHaveLength(1);
  });

  it('30 submissions per player per hour (stored)', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    for (let i = 0; i < 30; i++) expect((await s.call('POST', '/v1/times', T({ frames: 5000 - i }), a.token)).status).toBe(200);
    const no = await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    expect(no.status).toBe(429);
    expect(no.data.error.reason).toBe('hourly_cap');
    s.advance(3600_001);
    expect((await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token)).status).toBe(200);
  });
});

describe('smg times: verification', () => {
  it('co-op: two members of the same started room within +-2 frames verify each other', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const c = await smgSignUp(s, 'Cy');
    const code = await startedRoom(s, [a, b], 'coop');
    const first = await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 5000 }), a.token);
    expect(first.data.verified).toBe(false);
    const second = await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 5002 }), b.token);
    expect(second.data).toMatchObject({ verified: true, confirmed: 1 });
    const board = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, category: 2 }, c.token);
    expect(board.data.rows.map((r) => [r.name, r.frames, r.verified])).toEqual([['Ana', 5000, true], ['Ben', 5002, true]]);
    // 3 frames apart: no
    const third = await s.call('POST', '/v1/times', T({ star: 3, players: 2, room: code, frames: 5000 }), a.token);
    const fourth = await s.call('POST', '/v1/times', T({ star: 3, players: 2, room: code, frames: 5003 }), b.token);
    expect([third.data.verified, fourth.data.verified]).toEqual([false, false]);
    // not a member: refused
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code }), c.token)).data.error.code).toBe('not_member');
  });

  it('race: another member of the started room must submit the star too (membership alone is not enough); an unstarted room or a solo run never', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const code = await startedRoom(s, [a, b], 'race');
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 6000 }), a.token)).data.verified).toBe(false);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 6400 }), b.token)).data.verified).toBe(true);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, category: 2 }, b.token)).data.rows.map((r) => r.verified)).toEqual([true, true]);
    const c = await smgSignUp(s, 'Cy');
    const d = await smgSignUp(s, 'Dee');
    const lobby = await roomOf(s, [c, d], { mode: 'race' });
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: lobby }), c.token)).data.error.code).toBe('room_not_started'); // (was: accepted, unverified)
    expect((await s.call('POST', '/v1/times', T({ players: 1 }), d.token)).data.verified).toBe(false);
    expect((await s.call('POST', '/v1/times', T({ players: 1, room: lobby }), c.token)).data.verified).toBe(false); // a solo time may name a lobby: never verified
  });
});

describe('smg times: boards', () => {
  it('top, near and friends with competition ranks; you', async () => {
    const s = makeSmg();
    const ps = [];
    for (let i = 0; i < 12; i++) {
      const p = await smgSignUp(s, ['Ada', 'Bob', 'Cal', 'Dot', 'Eve', 'Fox', 'Gem', 'Hal', 'Ivy', 'Jon', 'Kay', 'Max'][i]);
      await s.call('POST', '/v1/times', T({ frames: i === 3 ? 1200 + 2 * 100 : 1200 + i * 100 }), p.token); // C and D tie at 1400
      s.advance(10);
      ps.push(p);
    }
    const top = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'top', limit: 5 }, ps[9].token);
    expect(top.data.rows.map((r) => r.rank)).toEqual([1, 2, 3, 3, 5]);
    expect(top.data).toMatchObject({ total: 12, you: { rank: 10, frames: 2100 } });
    const near = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'near', limit: 4 }, ps[9].token);
    expect(near.data.rows.map((r) => [r.rank, r.you])).toEqual([[8, false], [9, false], [10, true], [11, false]]);
    const nearTop = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'near', limit: 4 }, ps[11].token);
    expect(nearTop.data.rows.map((r) => r.rank)).toEqual([9, 10, 11, 12]);
    await friends(s, ps[9], ps[2]);
    await s.call('POST', '/v1/friends', { code: ps[0].code }, ps[9].token); // one-sided: not on the friends board
    const fr = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'friends' }, ps[9].token);
    expect(fr.data.rows.map((r) => [r.rank, r.name])).toEqual([[1, 'Cal'], [2, 'Jon']]);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, mode: 'world' }, ps[0].token)).data.error.code).toBe('bad_mode');
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, limit: 51 }, ps[0].token)).status).toBe(400);
  });

  it('without a category each player appears once with the best of every category', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const code = await startedRoom(s, [a, b], 'race');
    await s.call('POST', '/v1/times', T({ frames: 5000 }), a.token);
    await s.call('POST', '/v1/times', T({ frames: 4500, players: 2, room: code }), a.token);
    await s.call('POST', '/v1/times', T({ frames: 4700, players: 2, room: code }), b.token); // Ben vouches (and has his own row)
    const all = await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, b.token);
    expect(all.data.rows.map((r) => [r.name, r.frames, r.verified])).toEqual([['Ana', 4500, true], ['Ben', 4700, true]]);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, category: 1 }, b.token)).data.rows[0].frames).toBe(5000);
  });

  it('public times (open, no token, names only) and the /times page', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    const list = await s.raw('GET', '/v1/public/times');
    expect(list.status).toBe(200);
    expect(list.data.stars).toEqual([{ galaxy: 5, star: 2, players: 1, best: 4000 }]);
    const one = await s.raw('GET', '/v1/public/times?galaxy=5&star=2');
    expect(one.data.rows).toEqual([{ rank: 1, name: 'Ana', frames: 4000, players: 1, verified: false, dateMs: expect.any(Number) }]);
    expect(JSON.stringify(one.data)).not.toMatch(/pid|code|token/);
    expect((await s.raw('GET', '/v1/public/times?galaxy=5')).status).toBe(400);
    const page = await s.raw('GET', '/times');
    expect(page.status).toBe(200);
    expect(page.headers.get('content-security-policy')).toMatch(/script-src 'sha256-/);
    expect(page.data).toMatch(/Star Race Times/);
    expect(page.data).not.toMatch(/innerHTML/);
  });
});
