// Security review finding 13 (leaderboard forgery), with the PRODUCTION defaults (REVIEW_TIMES): unverified rows hidden from everyone
// but their owner (friends board excepted), the first visible time of a star flagged, verification only through a second member who
// submitted the star from another address with an account at least 48 h old, per-account and per-address submission limits, and the
// admin's tools (list, remove, approve, keep off the boards).
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, ep, roomOf, friends, REVIEW_TIMES } from '../smg-helpers.js';

const T = (o) => ({ galaxy: 5, star: 2, frames: 3600, players: 1, build: '3.0.0', ...o });
const ADMIN = 'test-admin-token-0123456789';
const HOUR = 3600_000;
const board = async (s, who, o = {}) => (await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, ...o }, who.token)).data;

/** A started room whose members call from the given addresses (endpoint and join both record the address). */
async function startedRoom(s, players, mode, ips) {
  const made = await s.call('POST', '/v1/rooms', { mode }, players[0].token, { ip: ips[0] });
  const code = made.data.code;
  for (let i = 1; i < players.length; i++) await s.call('POST', '/v1/rooms/join', { code }, players[i].token, { ip: ips[i] });
  for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/endpoint`, { ...ep(i + 1), public: `${ips[i]}:41100` }, players[i].token, { ip: ips[i] });
  for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/started`, undefined, players[i].token, { ip: ips[i] });
  return code;
}

/** `ms` of play: every member polls every 15 s (a guest silent for 60 s leaves the room). */
async function play(s, code, players, ips, ms) {
  for (let t = 0; t < ms; t += 15_000) {
    s.advance(Math.min(15_000, ms - t));
    for (let i = 0; i < players.length; i++) await s.call('GET', `/v1/rooms/${code}`, undefined, players[i].token, { ip: ips[i] });
  }
}

async function aged(s, names) {
  const out = [];
  for (const n of names) out.push(await smgSignUp(s, n));
  s.advance(49 * HOUR);
  return out;
}

describe('finding 13: what the public sees', () => {
  it('a solo time (never verifiable) is seen by its owner only; others, the public page and the star list do not see it', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const [a, b, c] = await aged(s, ['Ana', 'Ben', 'Cy']);
    // Cy's time was approved by the admin (the star is not empty, so Ana's is not a time_new_star)
    await s.call('POST', '/v1/times', T({ frames: 4100 }), c.token);
    await s.raw('POST', '/v1/admin/times/approve', { pid: c.pid, galaxy: 5, star: 2, category: 1 }, ADMIN);
    expect((await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token)).data).toMatchObject({ ok: true, verified: false, flagged: false });
    expect((await board(s, a)).rows.map((r) => [r.name, r.you])).toEqual([['Ana', true], ['Cy', false]]);
    expect((await board(s, b)).rows.map((r) => r.name)).toEqual(['Cy']);
    expect((await board(s, b)).total).toBe(1);
    expect((await s.raw('GET', '/v1/public/times?galaxy=5&star=2')).data.rows.map((r) => r.name)).toEqual(['Cy']);
    expect((await s.raw('GET', '/v1/public/times')).data.stars).toEqual([{ galaxy: 5, star: 2, players: 1, best: 4100 }]);
    // a mutual friend sees it on the friends board (people you chose); top and near stay clean
    await friends(s, a, b);
    expect((await board(s, b, { mode: 'friends' })).rows.map((r) => r.name)).toContain('Ana');
    expect((await board(s, b, { mode: 'near' })).rows.map((r) => r.name)).toEqual(['Cy']);
  });

  it('the admin approves a row: it is shown (verified 2 counts as vouched); a removed time is gone; a board ban refuses new times', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const [a, b] = await aged(s, ['Ana', 'Ben']);
    await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    const list = await s.raw('GET', '/v1/admin/times?galaxy=5&star=2', undefined, ADMIN);
    expect(list.data.rows).toEqual([expect.objectContaining({ pid: a.pid, frames: 4000, verified: 0, flagged: 1 })]); // time_new_star
    expect((await s.raw('POST', '/v1/admin/times/approve', { pid: a.pid, galaxy: 5, star: 2, category: 1 }, ADMIN)).status).toBe(200);
    expect((await board(s, b)).rows).toEqual([expect.objectContaining({ name: 'Ana', verified: true })]);
    expect((await s.raw('GET', '/v1/public/times?galaxy=5&star=2')).data.rows).toHaveLength(1);
    expect((await s.raw('POST', '/v1/admin/times/remove', { pid: a.pid, galaxy: 5 }, ADMIN)).data).toEqual({ ok: true, removed: 1 });
    expect((await board(s, b)).rows).toEqual([]);
    expect((await s.raw('POST', '/v1/admin/times/ban', { pid: a.pid, reason: 'forged' }, ADMIN)).data).toEqual({ ok: true, banned: true });
    const refused = await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    expect(refused.status).toBe(403);
    expect(refused.data.error.code).toBe('board_banned');
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200); // the account still plays
    await s.raw('POST', '/v1/admin/times/ban', { pid: a.pid, banned: false }, ADMIN);
    expect((await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token)).status).toBe(200);
    expect((await s.raw('POST', '/v1/admin/times/approve', { pid: b.pid, galaxy: 5, star: 2, category: 1 }, ADMIN)).status).toBe(404);
    expect((await s.raw('POST', '/v1/admin/times/remove', { pid: a.pid, star: 9 }, ADMIN)).status).toBe(400);
    expect((await s.raw('POST', '/v1/admin/times/remove', { pid: a.pid }, 'wrong-token')).status).toBe(401);
  });
});

describe('finding 13: verification', () => {
  it('one person, two accounts, one address: a race never verifies; from two addresses with aged accounts it does', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const [a, b, c, d] = await aged(s, ['Ana', 'Alt', 'Ben', 'Cy']);
    const same = await startedRoom(s, [a, b], 'race', ['203.0.113.5', '203.0.113.5']);
    await play(s, same, [a, b], ['203.0.113.5', '203.0.113.5'], 70_000);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: same, frames: 4000 }), a.token)).data.verified).toBe(false);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: same, frames: 4100 }), b.token)).data.verified).toBe(false);
    const two = await startedRoom(s, [c, d], 'race', ['203.0.113.6', '198.51.100.6']);
    await play(s, two, [c, d], ['203.0.113.6', '198.51.100.6'], 70_000);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: two, frames: 4000 }), c.token, { ip: '203.0.113.6' })).data.verified).toBe(false);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: two, frames: 4100 }), d.token, { ip: '198.51.100.6' })).data).toMatchObject({ verified: true, confirmed: 1 });
    expect((await board(s, a, { category: 2 })).rows.filter((r) => !r.you)).toEqual([]); // the first on the star: flagged for review
    for (const p of [c, d]) await s.raw('POST', '/v1/admin/flags/clear', { pid: p.pid }, ADMIN);
    const rows = (await board(s, a, { category: 2 })).rows;
    expect(rows.filter((r) => !r.you).map((r) => [r.name, r.verified])).toEqual([['Ben', true], ['Cy', true]]);
  });

  it('fresh accounts (under 48 h) do not verify; a time longer than the room has run does not; a left member does not vouch', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const ips = ['203.0.113.7', '198.51.100.7'];
    const code = await startedRoom(s, [a, b], 'race', ips);
    await play(s, code, [a, b], ips, 70_000);
    await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 4000 }), a.token);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 4100 }), b.token)).data.verified).toBe(false); // too young
    s.advance(49 * HOUR); // (the room expired meanwhile: a new one)
    const r2 = await startedRoom(s, [a, b], 'race', ips);
    // 70 s of play cannot hold a 100 s run
    await play(s, r2, [a, b], ips, 70_000);
    await s.call('POST', '/v1/times', T({ star: 3, players: 2, room: r2, frames: 6000 }), a.token);
    expect((await s.call('POST', '/v1/times', T({ star: 3, players: 2, room: r2, frames: 6000 }), b.token)).data.verified).toBe(false);
    // Ana submits, then leaves: Ben's later result has nobody to vouch for it
    await s.call('POST', '/v1/times', T({ star: 4, players: 2, room: r2, frames: 3000 }), a.token);
    await s.call('POST', `/v1/rooms/${r2}/leave`, undefined, a.token);
    expect((await s.call('POST', '/v1/times', T({ star: 4, players: 2, room: r2, frames: 3100 }), b.token)).data.verified).toBe(false);
  });

  it('the first visible time on a star is flagged even when verified (an empty board has nothing to compare against)', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const [a, b, alt] = await aged(s, ['Ana', 'Ben', 'Alt']);
    // a hidden unverified time from a second account does not make the star look taken
    await s.call('POST', '/v1/times', T({ frames: 9000 }), alt.token);
    const code = await startedRoom(s, [a, b], 'race', ['203.0.113.8', '198.51.100.8']);
    await play(s, code, [a, b], ['203.0.113.8', '198.51.100.8'], 70_000);
    await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 700 }), a.token);
    const second = await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 710 }), b.token);
    expect(second.data).toMatchObject({ verified: true, flagged: true });
    const flags = (await s.raw('GET', '/v1/admin/flags?kind=time_new_star', undefined, ADMIN)).data.flags;
    expect(flags.map((f) => f.pid).sort()).toEqual([a.pid, alt.pid, b.pid].sort());
    expect((await board(s, alt, { category: 2 })).rows).toEqual([]); // flagged: hidden until reviewed
  });
});

describe('finding 13: limits', () => {
  it('per account: 30 an hour and 120 a day (stored); per address: 60 an hour across accounts', async () => {
    const s = makeSmg({ ...REVIEW_TIMES, SMG_TIMES_PER_DAY: '40', RATE_LIMIT_SCALE: '1' });
    const a = await smgSignUp(s, 'Ana');
    for (let i = 0; i < 30; i++) expect((await s.call('POST', '/v1/times', T({ star: 1 + (i % 8), frames: 9000 - i }), a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/times', T({ frames: 5000 }), a.token)).data.error.code).toBe('rate_limited'); // (memory bucket or the stored count)
    s.advance(HOUR + 1);
    for (let i = 0; i < 10; i++) await s.call('POST', '/v1/times', T({ star: 1 + (i % 8), frames: 8000 - i }), a.token);
    expect((await s.call('POST', '/v1/times', T({ frames: 5000 }), a.token)).data.error.reason).toBe('daily_cap');
    // many accounts behind one address
    const ip = { ip: '203.0.113.99' };
    let ok = 0;
    for (let k = 0; k < 4; k++) {
      const p = await smgSignUp(s, `Bot${k}`);
      for (let i = 0; i < 20; i++) ok += (await s.call('POST', '/v1/times', T({ galaxy: 7, star: 1 + (i % 8), frames: 9000 - i }), p.token, ip)).status === 200 ? 1 : 0;
    }
    expect(ok).toBe(60);
  });
});

describe('svcfix3 (review S6): a race time needs the other member to agree within a tolerance', () => {
  const ips = ['203.0.113.5', '198.51.100.5'];
  it('race: two times far apart (more than SMG_VERIFY_RACE_SEC, 60 s) do not verify each other; times within it do', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const [a, b] = await aged(s, ['Ana', 'Ben']);
    const code = await startedRoom(s, [a, b], 'race', ips);
    await play(s, code, [a, b], ips, 150_000); // 9000 frames of room time: both times fit
    await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 700 }), a.token, { ip: ips[0] });
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 5000 }), b.token, { ip: ips[1] })).data).toMatchObject({ verified: false, confirmed: 0 });
    // a time near Ana's (a tenth of the distance) does: Ben's second try is a new best and the two agree
    const near = await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 2000 }), b.token, { ip: ips[1] });
    expect(near.data).toMatchObject({ verified: true, confirmed: 1 });
  });

  it('co-op is unchanged: still within SMG_VERIFY_FRAMES (2) of each other', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const [a, b] = await aged(s, ['Ana', 'Ben']);
    const code = await startedRoom(s, [a, b], 'coop', ips);
    await play(s, code, [a, b], ips, 150_000);
    await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 3000 }), a.token, { ip: ips[0] });
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 3100 }), b.token, { ip: ips[1] })).data.verified).toBe(false);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 3001 }), b.token, { ip: ips[1] })).data).toMatchObject({ verified: true, confirmed: 1 });
  });
});
