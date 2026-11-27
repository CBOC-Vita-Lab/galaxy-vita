// Outside review (Nov 2026): S1 relay claims, S2 held-poll presence, S3 board scans, S6 board category, S9, S10.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, roomOf, ep } from '../smg-helpers.js';

const HOUR = 3600_000;

describe('S1: relay claims', () => {
  async function two() {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token);
    return { s, h, g, code };
  }
  const post = (s, code, p, body) => s.call('POST', `/v1/rooms/${code}/endpoint`, body, p.token);

  it('refuses private, LAN, loopback and link-local relay addresses, even from an account with credentials', async () => {
    const { s, g, code } = await two();
    await s.call('POST', '/v1/turn', { room: code }, g.token);
    for (const bad of ['192.168.1.1:80', '10.0.0.5:3478', '172.16.4.4:3478', '127.0.0.1:3478', '169.254.1.1:3478', '100.64.0.9:3478', '0.0.0.0:1']) {
      const r = await post(s, code, g, { ...ep(2), relay: bad });
      expect(r.status, bad).toBe(400);
      expect(r.data.error.code).toBe('bad_endpoint');
    }
    const c = await post(s, code, g, { ...ep(2), candidates: [{ type: 'relay', addr: '192.168.1.1:80' }] });
    expect(c.status).toBe(400);
  });

  it('accepts a relay claim only from an account issued credentials within their lifetime', async () => {
    const { s, h, g, code } = await two();
    const victim = '198.51.100.200:9999';
    const r1 = await post(s, code, g, { ...ep(2), relay: victim });
    expect(r1.status).toBe(403);
    expect(r1.data.error.code).toBe('relay_not_allowed');
    expect((await post(s, code, g, { ...ep(2), candidates: [{ type: 'relay', addr: victim }] })).status).toBe(403);
    // and nothing was stored: the host never sees the victim's address
    const hv = (await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data;
    expect(JSON.stringify(hv)).not.toContain('198.51.100.200');
    // minted: accepted
    expect((await s.call('POST', '/v1/turn', { room: code }, g.token)).status).toBe(200);
    expect((await post(s, code, g, { ...ep(2), relay: '198.51.100.201:50000' })).status).toBe(200);
    // an honest post without a relay claim is never affected
    expect((await post(s, code, g, ep(2))).status).toBe(200);
    // the credential's lifetime (3600 s) is over: refused again
    s.advance(HOUR + 5000);
    expect((await post(s, code, g, { ...ep(2), relay: '198.51.100.201:50000' })).status).toBe(403);
  });

  it('a host candidate may still be private (passed on only behind the same address)', async () => {
    const { s, g, code } = await two();
    const r = await post(s, code, g, { ...ep(2), candidates: [{ type: 'host', addr: '192.168.1.2:41100' }] });
    expect(r.status).toBe(200);
    const bad = await post(s, code, g, { ...ep(2), candidates: [{ type: 'srflx', addr: '192.168.1.2:41100' }] });
    expect(bad.status).toBe(400);
  });
});

describe('S2: held polls keep presence', () => {
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  async function pair(opts) {
    const s = makeSmg({}, opts);
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), g.token);
    const v = (await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data.version;
    return { s, h, g, code, v };
  }

  it('a host holding a poll for longer than LINK_LOST_SEC is not taken for lost by another member\'s call', async () => {
    const { s, h, g, code, v } = await pair({ deps: { heldTickMs: 40 } });
    const held = s.call('GET', `/v1/rooms/${code}?v=${v}&wait=2`, undefined, h.token);
    await sleep(100);
    s.advance(15_000); await sleep(120); // the holder looks quiet (LINK_PING 12 s) unless the hold keeps stamping
    s.advance(15_000); await sleep(120); // 30 s since the poll began: over LINK_LOST_SEC (20 s)
    const gv = (await s.call('GET', `/v1/rooms/${code}`, undefined, g.token)).data;
    expect(gv).toMatchObject({ hostSlot: 0, hostEpoch: 0 });
    await held;
  });

  it('wait is capped at 8 s', async () => {
    const { s, h, code, v } = await pair();
    const t0 = Date.now();
    await s.call('GET', `/v1/rooms/${code}?v=${v}&wait=20`, undefined, h.token);
    const ms = Date.now() - t0;
    expect(ms).toBeGreaterThanOrEqual(7900);
    expect(ms).toBeLessThan(9500);
  }, 15000);

  it('at most 4 held polls per account; the fifth answers at once', async () => {
    const { s, h, code, v } = await pair();
    const held = [];
    for (let i = 0; i < 4; i++) held.push(s.call('GET', `/v1/rooms/${code}?v=${v}&wait=1`, undefined, h.token));
    await sleep(100);
    expect(s.app.waiting()).toBe(4);
    const t0 = Date.now();
    await s.call('GET', `/v1/rooms/${code}?v=${v}&wait=1`, undefined, h.token);
    expect(Date.now() - t0).toBeLessThan(500);
    expect(s.app.waiting()).toBe(4);
    await Promise.all(held);
    expect(s.app.waiting()).toBe(0);
  });
});

// ------------------------------------------------------------------ S3
import { REVIEW_TIMES } from '../smg-helpers.js';
const T = (o) => ({ galaxy: 5, star: 2, frames: 3600, players: 1, build: '3.0.0', ...o });
const getBoard = (s, who, o = {}) => s.call('POST', '/v1/times/get', { galaxy: 5, star: 2, ...o }, who.token);

describe('S3: board reads', () => {
  function spy(s) {
    const log = [];
    const real = s.store.sql.exec;
    s.store.sql.exec = (q, ...b) => { log.push(q); return real.call(s.store.sql, q, ...b); };
    return { boardReads: () => log.filter((q) => /^\s*WITH b AS/.test(q)).length, reset: () => { log.length = 0; } };
  }

  it('an answer is reused for 20 s per viewer class, then read again', async () => {
    const s = makeSmg();
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    await s.call('POST', '/v1/times', T({ frames: 3600 }), a.token);
    const sp = spy(s);
    const r1 = await getBoard(s, b);
    expect(sp.boardReads()).toBeGreaterThan(0);
    sp.reset();
    const r2 = await getBoard(s, b);
    expect(sp.boardReads()).toBe(0);
    expect(r2.data).toEqual(r1.data);
    s.advance(21_000);
    await getBoard(s, b);
    expect(sp.boardReads()).toBeGreaterThan(0);
  });

  it('a viewer with no row shares one answer; a hidden row is never shown through the memo', async () => {
    const s = makeSmg(REVIEW_TIMES);
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    await s.call('POST', '/v1/times', T({ frames: 3600 }), a.token); // solo: unverified, hidden from everyone but Ana
    expect((await getBoard(s, b)).data.rows).toEqual([]);
    const mine = (await getBoard(s, a)).data;
    expect(mine.rows.map((r) => r.name)).toEqual(['Ana']);
    expect(mine.you).toMatchObject({ rank: 1 });
    expect((await getBoard(s, b)).data.rows).toEqual([]); // still nothing for Ben, from the memo
  });

  it('a submission shows at once in its owner\'s next read', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    await s.call('POST', '/v1/times', T({ frames: 3600 }), a.token);
    expect((await getBoard(s, a)).data.you.frames).toBe(3600);
    await s.call('POST', '/v1/times', T({ frames: 3500 }), a.token);
    expect((await getBoard(s, a)).data.you.frames).toBe(3500);
  });

  it('migration 8: the board index carries verified and flagged; the scan of one board is bounded', async () => {
    const s = makeSmg();
    const cols = s.store.sql.exec(`PRAGMA index_info(star_times_board)`).toArray().map((r) => r.name);
    expect(cols).toEqual(['galaxy', 'star', 'category', 'frames', 'updated_ms', 'pid', 'verified', 'flagged']);
    expect(s.store.sql.exec(`SELECT 1 FROM sqlite_master WHERE name = 'star_times_rank'`).toArray()).toEqual([]);
    const a = await smgSignUp(s, 'Ana');
    s.store.tx(() => {
      for (let i = 0; i < 1100; i++) {
        const pid = 200000 + i;
        s.store.sql.exec(`INSERT INTO profiles (pid, code, name, name_key, token_hash, created_ms, last_seen_ms) VALUES (?, ?, ?, ?, ?, 0, 0)`, pid, `c${i}`, `N${i}`, `n${i}`, `h${i}`);
        s.store.sql.exec(`INSERT INTO star_times (pid, galaxy, star, category, frames, players, verified, flagged, updated_ms) VALUES (?, 5, 2, 1, ?, 1, 1, 0, 1)`, pid, 1000 + i);
      }
    });
    const r = (await getBoard(s, a)).data;
    expect(r.total).toBe(1000);
    expect(r.capped).toBe(true);
    s.advance(21_000);
    await s.call('POST', '/v1/times', T({ frames: 9000 }), a.token);
    const mine = (await getBoard(s, a, { mode: 'near' })).data;
    expect(mine.you.rank).toBe(1001); // a floor: the count stops at the cap
  });
});

// ------------------------------------------------------------------ S6
describe('S6: the board category is tied to the room', () => {
  async function started(s, players, mode) {
    const code = await roomOf(s, players, { mode });
    for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(i + 1), players[i].token);
    for (const p of players) await s.call('POST', `/v1/rooms/${code}/started`, undefined, p.token);
    return code;
  }
  async function run(s, code, players, ms) {
    for (let t = 0; t < ms; t += 10_000) { s.advance(10_000); for (const p of players) await s.call('GET', `/v1/rooms/${code}`, undefined, p.token); }
  }
  it('two members cannot post on the 3- or 4-player boards; a member who left does not lower the peak', async () => {
    const s = makeSmg({ SMG_VERIFY_DISTINCT_ADDR: '0' });
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    const two = await started(s, [a, b], 'coop');
    await run(s, two, [a, b], 70_000);
    for (const players of [3, 4]) {
      const r = await s.call('POST', '/v1/times', T({ players, room: two, frames: 3000 }), a.token);
      expect(r.status).toBe(400);
      expect(r.data.error.reason).toBe('players_exceed_room');
    }
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: two, frames: 3000 }), a.token)).status).toBe(200);
    const [d, e, f] = [await smgSignUp(s, 'Dee'), await smgSignUp(s, 'Eli'), await smgSignUp(s, 'Fay')];
    const three = await started(s, [d, e, f], 'coop');
    await run(s, three, [d, e, f], 70_000);
    await s.call('POST', `/v1/rooms/${three}/leave`, undefined, f.token);
    expect((await s.call('POST', '/v1/times', T({ star: 3, players: 3, room: three, frames: 3000 }), d.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/times', T({ star: 3, players: 4, room: three, frames: 3000 }), d.token)).status).toBe(400);
  });

  it('race mode: both times must fit the time the room has run, or nothing verifies', async () => {
    const s = makeSmg({ SMG_VERIFY_DISTINCT_ADDR: '0', SMG_VERIFY_SLACK_SEC: '3' });
    const [a, b] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben')];
    const code = await started(s, [a, b], 'race');
    await run(s, code, [a, b], 30_000); // 1800 frames of play
    await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 5000 }), a.token);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 5100 }), b.token)).data.verified).toBe(false);
    await s.call('POST', '/v1/times', T({ star: 3, players: 2, room: code, frames: 1500 }), a.token);
    expect((await s.call('POST', '/v1/times', T({ star: 3, players: 2, room: code, frames: 1700 }), b.token)).data.verified).toBe(true);
  });
});

// ------------------------------------------------------------------ S9
import { makeServer } from '../helpers.js';
import { edgeCheck } from '../../src/core/edge.js';

describe('S9: a short ADMIN_TOKEN leaves the admin routes off', () => {
  const short = 'a'.repeat(23);
  const ok = 'a'.repeat(24);
  it('the application answers 404 with a 23-character token, and works with 24', async () => {
    const a = makeServer({ GAME: 'smg', ADMIN_TOKEN: short });
    expect((await a.call('GET', '/v1/admin/stats', undefined, short, { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' })).status).toBe(404);
    const b = makeServer({ GAME: 'smg', ADMIN_TOKEN: ok });
    expect((await b.call('GET', '/v1/admin/stats', undefined, ok, { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' })).status).toBe(200);
  });
  it('the edge agrees', () => {
    const req = () => new Request('https://t.invalid/v1/admin/stats', { headers: { authorization: `Bearer ${ok}` } });
    expect(edgeCheck(req(), { ADMIN_TOKEN: short })).toMatchObject({ status: 404 });
    expect(edgeCheck(req(), { ADMIN_TOKEN: ok })).toBeNull();
  });
});

// ------------------------------------------------------------------ S10
import { validateName } from '../../src/core/names.js';

describe('S10: the name filter does not block innocent names', () => {
  it('Grape, Peacock, Dickson (and friends) are allowed', () => {
    for (const n of ['Grape', 'Peacock', 'Dickson', 'Therapist', 'Janus', 'Wankel', 'Cockatiel', 'Sussex', 'Assassin']) {
      expect(() => validateName(n), n).not.toThrow();
    }
  });
  it('the words themselves, with the usual disguises, are still blocked', () => {
    for (const n of ['Dick', 'DICK', 'd1ck', 'Dick99', 'Dick Smith', 'xx_dick', 'GrapeRape', 'Rape', 'Cocks', 'p0rn', 'Slut', 'FuckYou', 'xFuckx', 'Shithead', 'N1gger']) {
      expect(() => validateName(n), n).toThrow(/not allowed/);
    }
  });
});
