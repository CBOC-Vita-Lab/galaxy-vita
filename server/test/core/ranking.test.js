import { describe, it, expect } from 'vitest';
import { makeServer, signUp, makeBlob, T0 } from '../helpers.js';
import { b64decode } from '../../src/core/util.js';

const put = (s, p, o) => s.call('POST', '/v1/rank/put', { category: 0, region: 2, score: 0, data: makeBlob(p.name), ...o }, p.token);
const get = (s, p, o) => s.call('POST', '/v1/rank/get', { mode: 1, category: 0, region: 2, sort: 1, since: 100, limit: 10, ...o }, p.token);

/** Seed N players directly with the given scores (bypasses allowance). */
async function seed(s, scores, { category = 0, updated } = {}) {
  const ps = [];
  for (let i = 0; i < scores.length; i++) {
    const p = await signUp(s, `Plr${String(i).padStart(2, '0')}`);
    s.store.sql.exec(
      'INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (?,?,?,?,?,?)',
      category, p.pid, 2, scores[i], makeBlob(p.name), updated ?? s.clock.now - 1000 + i,
    );
    ps.push(p);
  }
  return ps;
}
const scoresOf = (r) => r.data.rows.map((x) => x.score);
const ordersOf = (r) => r.data.rows.map((x) => x.order);

describe('put validation', () => {
  it('accepts a valid 0x80 blob with the right digest and stores it byte for byte', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    const blob = makeBlob('Alpha', { wins: 3, losses: 1 });
    const r = await put(s, a, { score: 42, data: blob });
    expect(r.status).toBe(200);
    expect(r.data).toEqual({ ok: true, stored: 42, clamped: false });
    const g = await get(s, a, { mode: 1, limit: 1 });
    expect(g.data.rows[0].data).toBe(blob);
    expect(g.data.rows[0].size).toBe(128);
    expect(b64decode(g.data.rows[0].data).length).toBe(128);
  });

  it('rejects a wrong digest (any flipped bit in the first 0x70 bytes or in the digest)', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    const r = await put(s, a, { score: 1, data: makeBlob('Alpha', { corrupt: true }) });
    expect(r.status).toBe(400);
    expect(r.data.error.code).toBe('bad_digest');
    const raw = b64decode(makeBlob('Alpha'));
    raw[0x7f] ^= 0x80;
    const r2 = await put(s, a, { score: 1, data: Buffer.from(raw).toString('base64') });
    expect(r2.data.error.code).toBe('bad_digest');
  });

  it('blob must be exactly 0x80 bytes and base64; larger than 768 is refused outright', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    expect((await put(s, a, { data: Buffer.alloc(64).toString('base64') })).data.error.code).toBe('bad_blob');
    expect((await put(s, a, { data: Buffer.alloc(769).toString('base64') })).data.error.code).toBe('bad_blob');
    expect((await put(s, a, { data: '***' })).data.error.code).toBe('bad_blob');
    expect((await put(s, a, { data: 5 })).data.error.code).toBe('bad_blob');
    expect((await put(s, a, { data: undefined })).data.error.code).toBe('bad_blob');
  });

  it('with the digest check relaxed, any blob up to 768 bytes is stored opaque', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open', RANK_REQUIRE_HMAC: '0', RANK_CHECK_NAME: '0' });
    const a = await signUp(s, 'Alpha');
    const blob768 = Buffer.alloc(768, 7).toString('base64');
    expect((await put(s, a, { score: 5, data: blob768 })).status).toBe(200);
    const g = await get(s, a, { limit: 1 });
    expect(g.data.rows[0]).toMatchObject({ size: 768, data: blob768 });
  });

  it('category 0..100, region bit, score range', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    expect((await put(s, a, { category: 100 })).status).toBe(200);
    expect((await put(s, a, { category: 101 })).data.error.code).toBe('bad_category');
    expect((await put(s, a, { category: -1 })).data.error.code).toBe('bad_category');
    expect((await put(s, a, { category: 1.5 })).data.error.code).toBe('bad_category');
    expect((await put(s, a, { region: 3 })).data.error.code).toBe('bad_region');
    expect((await put(s, a, { region: 0xff })).data.error.code).toBe('bad_region');
    expect((await put(s, a, { score: -1 })).data.error.code).toBe('bad_score');
    expect((await put(s, a, { score: 1000000 })).data.error.code).toBe('bad_score');
    expect((await put(s, a, { score: 999999 })).status).toBe(200);
    expect((await put(s, a, { score: '5' })).data.error.code).toBe('bad_score');
  });

  it('the name inside the blob must be the registered name (either byte order)', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    expect((await put(s, a, { data: makeBlob('Mallory') })).data.error.code).toBe('bad_blob_name');
    expect((await put(s, a, { data: makeBlob('Alpha', { endian: 'be' }) })).status).toBe(200);
    expect((await put(s, a, { data: makeBlob('Alpha', { endian: 'le' }) })).status).toBe(200);
  });

  it('needs auth and a configured key', async () => {
    const s = makeServer({ RANK_HMAC_KEY_HEX: '' });
    const a = await signUp(s, 'Alpha');
    expect((await s.call('POST', '/v1/rank/put', { category: 0, region: 2, score: 0, data: makeBlob('Alpha') })).status).toBe(401);
    const r = await put(s, a, { score: 0 });
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('server_misconfigured');
  });

  it('never leaks the key or digest material in responses', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    const r = await put(s, a, { data: makeBlob('Alpha', { corrupt: true }) });
    expect(JSON.stringify(r.data)).not.toMatch(/[0-9a-f]{32}/);
  });
});

describe('score credit (anti-cheat policy "credit")', () => {
  it('a new player cannot raise a score without verified matches (clamped, not rejected)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    const r = await put(s, a, { score: 500 });
    expect(r.status).toBe(200);
    expect(r.data).toEqual({ ok: true, stored: 0, clamped: true });
  });

  it('credit allows exactly what verified matches earned; decreases are always allowed', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    s.store.sql.exec('INSERT INTO allowance (pid, category, amount) VALUES (?, 0, 18)', a.pid);
    expect((await put(s, a, { score: 13 })).data).toEqual({ ok: true, stored: 13, clamped: false });
    expect((await put(s, a, { score: 8 })).data).toEqual({ ok: true, stored: 8, clamped: false }); // -5 at match start
    // 5 credit is left (18 - 13): exactly the "restore" amount, so the score returns to 13 and no further
    expect((await put(s, a, { score: 21 })).data).toEqual({ ok: true, stored: 13, clamped: true });
    expect((await put(s, a, { score: 99 })).data).toEqual({ ok: true, stored: 13, clamped: true });
  });

  it('credit is per category', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    s.store.sql.exec('INSERT INTO allowance (pid, category, amount) VALUES (?, 0, 10)', a.pid);
    expect((await put(s, a, { category: 1, score: 10 })).data.stored).toBe(0);
    expect((await put(s, a, { category: 0, score: 10 })).data.stored).toBe(10);
  });

  it('a new day (category 1) or season starts the baseline at zero again', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    s.store.sql.exec('INSERT INTO allowance (pid, category, amount) VALUES (?, 1, 30)', a.pid);
    expect((await put(s, a, { category: 1, score: 20 })).data.stored).toBe(20);
    s.advance(24 * 3600_000); // next UTC day: the stored 20 is stale
    expect((await put(s, a, { category: 1, score: 10 })).data.stored).toBe(10); // 10 more credit used, baseline 0
    expect((await put(s, a, { category: 1, score: 20 })).data).toEqual({ ok: true, stored: 10, clamped: true }); // credit is gone
  });

  it('policy "delta" caps one put at +25; "open" accepts anything in range', async () => {
    const d = makeServer({ RANK_PUT_POLICY: 'delta' });
    const a = await signUp(d, 'Alpha');
    expect((await put(d, a, { score: 100 })).data).toEqual({ ok: true, stored: 25, clamped: true });
    expect((await put(d, a, { score: 50 })).data.stored).toBe(50);
    const o = makeServer({ RANK_PUT_POLICY: 'open' });
    const b = await signUp(o, 'Alpha');
    expect((await put(o, b, { score: 5000 })).data.stored).toBe(5000);
  });
});

describe('TOPLIST', () => {
  it('returns the best rows, best first, with competition ranks (ties share a rank)', async () => {
    const s = makeServer();
    const ps = await seed(s, [50, 70, 70, 10, 90, 30]);
    const r = await get(s, ps[0], { mode: 1, limit: 4 });
    expect(r.status).toBe(200);
    expect(scoresOf(r)).toEqual([90, 70, 70, 50]);
    expect(ordersOf(r)).toEqual([1, 2, 2, 4]);
    expect(r.data).toMatchObject({ mode: 1, count: 4, total: 6 });
    expect(r.data.rows[0].pid).toBe(ps[4].pid);
    expect(r.data.rows[0]).toMatchObject({ region: 2, size: 128 });
    expect(r.data.rows[0].lastupdate).toBe(Math.floor((T0 - 1000 + 4) / 1000));
  });

  it('limit 1..10 only; sort 0 reverses; empty table is an empty list', async () => {
    const s = makeServer();
    const empty = await signUp(s, 'Lonely');
    expect((await get(s, empty, { mode: 1, limit: 3 })).data.rows).toEqual([]);
    const ps = await seed(s, [5, 6, 7]);
    expect((await get(s, ps[0], { mode: 1, limit: 0 })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { mode: 1, limit: 11 })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { mode: 1, limit: 10 })).status).toBe(200);
    expect((await get(s, ps[0], { mode: 1, limit: 1 })).status).toBe(200);
    const asc = await get(s, ps[0], { mode: 1, limit: 3, sort: 0 });
    expect(scoresOf(asc)).toEqual([5, 6, 7]);
    expect(ordersOf(asc)).toEqual([1, 2, 3]);
    expect((await get(s, ps[0], { mode: 1, limit: 3, sort: 2 })).data.error.code).toBe('bad_param');
  });

  it('ten players: returns exactly ten', async () => {
    const s = makeServer();
    const ps = await seed(s, Array.from({ length: 25 }, (_, i) => i + 1));
    const r = await get(s, ps[0], { mode: 1, limit: 10 });
    expect(scoresOf(r)).toEqual([25, 24, 23, 22, 21, 20, 19, 18, 17, 16]);
    expect(ordersOf(r)).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
  });
});

describe('NEAR (must contain the caller with order >= 1)', () => {
  it('centres on the caller and always includes them', async () => {
    const s = makeServer();
    const ps = await seed(s, Array.from({ length: 30 }, (_, i) => (i + 1) * 10)); // 10..300, ps[i] has (i+1)*10
    const me = ps[14]; // 150, rank 16
    const r = await get(s, me, { mode: 2, limit: 5 });
    expect(r.status).toBe(200);
    expect(scoresOf(r)).toEqual([170, 160, 150, 140, 130]);
    expect(ordersOf(r)).toEqual([14, 15, 16, 17, 18]);
    expect(r.data.rows.map((x) => x.pid)).toContain(me.pid);
    expect(r.data.rows.every((x) => x.order >= 1)).toBe(true);
  });

  it('at the top, bottom and with limits 2 and 10', async () => {
    const s = makeServer();
    const ps = await seed(s, Array.from({ length: 12 }, (_, i) => (i + 1) * 10));
    const top = await get(s, ps[11], { mode: 2, limit: 5 });
    expect(scoresOf(top)).toEqual([120, 110, 100, 90, 80]);
    expect(ordersOf(top)[0]).toBe(1);
    const bottom = await get(s, ps[0], { mode: 2, limit: 5 });
    expect(scoresOf(bottom)).toEqual([50, 40, 30, 20, 10]);
    expect(ordersOf(bottom)).toEqual([8, 9, 10, 11, 12]);
    const two = await get(s, ps[5], { mode: 2, limit: 2 });
    expect(two.data.rows.length).toBe(2);
    expect(two.data.rows.map((x) => x.pid)).toContain(ps[5].pid);
    const ten = await get(s, ps[5], { mode: 2, limit: 10 });
    expect(ten.data.rows.length).toBe(10);
    expect(ten.data.rows.map((x) => x.pid)).toContain(ps[5].pid);
    expect((await get(s, ps[5], { mode: 2, limit: 1 })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[5], { mode: 2, limit: 11 })).data.error.code).toBe('bad_param');
  });

  it('fewer players than the limit returns them all', async () => {
    const s = makeServer();
    const ps = await seed(s, [30, 20, 10]);
    const r = await get(s, ps[1], { mode: 2, limit: 10 });
    expect(scoresOf(r)).toEqual([30, 20, 10]);
    expect(ordersOf(r)).toEqual([1, 2, 3]);
  });

  it('ties around the caller keep competition ranks and the caller stays in', async () => {
    const s = makeServer();
    const ps = await seed(s, [10, 20, 20, 20, 30]);
    const r = await get(s, ps[2], { mode: 2, limit: 3 });
    expect(r.data.rows.map((x) => x.pid)).toContain(ps[2].pid);
    expect(ordersOf(r).every((o) => o >= 1)).toBe(true);
    const all = await get(s, ps[2], { mode: 2, limit: 10 });
    expect(ordersOf(all)).toEqual([1, 2, 2, 2, 5]);
  });

  it('a caller with no row (or only a stale one) gets a synthesized score-0 row with order >= 1', async () => {
    const s = makeServer();
    const ps = await seed(s, [30, 20, 10]);
    const fresh = await signUp(s, 'Newbie');
    const r = await get(s, fresh, { mode: 2, limit: 3 });
    expect(r.data.rows.length).toBe(3);
    const mine = r.data.rows.find((x) => x.pid === fresh.pid);
    expect(mine).toBeTruthy();
    expect(mine).toMatchObject({ score: 0, size: 128 });
    expect(mine.order).toBe(4);
    expect(b64decode(mine.data)).toEqual(new Uint8Array(128));
    // one stored row + newcomer: both come back
    const s3 = makeServer();
    const [solo] = await seed(s3, [40]);
    const nb3 = await signUp(s3, 'Newbie');
    const r3 = await get(s3, nb3, { mode: 2, limit: 5 });
    expect(r3.data.rows.map((x) => [x.pid, x.score, x.order])).toEqual([[solo.pid, 40, 1], [nb3.pid, 0, 2]]);
    // alone on an empty board: exactly the own row
    const s2 = makeServer();
    const only = await signUp(s2, 'Only');
    const r2 = await get(s2, only, { mode: 2, limit: 5 });
    expect(r2.data.rows).toHaveLength(1);
    expect(r2.data.rows[0]).toMatchObject({ pid: only.pid, order: 1, score: 0 });
    // a row from last season does not count
    s.advance(8 * 24 * 3600_000);
    const stale = await get(s, ps[0], { mode: 2, limit: 3 });
    expect(stale.data.rows).toHaveLength(1);
    expect(stale.data.rows[0]).toMatchObject({ pid: ps[0].pid, score: 0, order: 1 });
  });
});

describe('FRIENDS', () => {
  it('returns the caller plus listed friends, best first; unknown ids are ignored', async () => {
    const s = makeServer();
    const ps = await seed(s, [10, 40, 25, 70, 5]);
    const r = await get(s, ps[0], { mode: 3, limit: 10, friends: [ps[1].pid, ps[2].pid, 999999, 0, 0] });
    expect(r.status).toBe(200);
    expect(scoresOf(r)).toEqual([40, 25, 10]);
    expect(ordersOf(r)).toEqual([1, 2, 3]);
    expect(r.data.rows.map((x) => x.pid)).not.toContain(ps[3].pid);
  });

  it('no friends: own row only; no row at all: synthesized own row; limit enforced', async () => {
    const s = makeServer();
    const ps = await seed(s, [10, 40]);
    expect((await get(s, ps[0], { mode: 3, limit: 2, friends: [] })).data.rows.map((x) => x.pid)).toEqual([ps[0].pid]);
    const nb = await signUp(s, 'Newbie');
    const r = await get(s, nb, { mode: 3, limit: 5, friends: [ps[1].pid] });
    expect(r.data.rows.map((x) => x.pid)).toEqual([ps[1].pid, nb.pid]);
    expect(r.data.rows[1]).toMatchObject({ score: 0, order: 2 });
    const tight = await get(s, nb, { mode: 3, limit: 2, friends: [ps[1].pid, ps[0].pid] });
    expect(tight.data.rows).toHaveLength(2);
    expect(tight.data.rows.map((x) => x.pid)).toContain(nb.pid);
  });

  it('limits 2..65, at most 64 friends, sane ids', async () => {
    const s = makeServer();
    const ps = await seed(s, [10]);
    expect((await get(s, ps[0], { mode: 3, limit: 1, friends: [] })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { mode: 3, limit: 66, friends: [] })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { mode: 3, limit: 65, friends: [] })).status).toBe(200);
    const sixtyFive = Array.from({ length: 65 }, (_, i) => i + 1);
    expect((await get(s, ps[0], { mode: 3, limit: 10, friends: sixtyFive })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { mode: 3, limit: 10, friends: Array.from({ length: 64 }, (_, i) => i + 1) })).status).toBe(200);
    expect((await get(s, ps[0], { mode: 3, limit: 10, friends: [-5] })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { mode: 3, limit: 10, friends: 'x' })).data.error.code).toBe('bad_param');
  });
});

describe('ORDER and mode handling', () => {
  it('ORDER returns the caller\'s rank and score', async () => {
    const s = makeServer();
    const ps = await seed(s, [10, 20, 30]);
    const r = await get(s, ps[0], { mode: 0 });
    expect(r.data).toMatchObject({ mode: 0, order: 3, score: 10, total: 3, rows: [] });
  });
  it('rejects unknown modes and bad parameters; mode names work', async () => {
    const s = makeServer();
    const ps = await seed(s, [10]);
    for (const mode of [4, -1, 'DEATHMATCH', null, 1.5]) {
      expect((await get(s, ps[0], { mode })).data.error.code, String(mode)).toBe('bad_mode');
    }
    expect((await get(s, ps[0], { mode: 'toplist', limit: 3 })).status).toBe(200);
    expect((await get(s, ps[0], { category: 101 })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { since: -1 })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { since: 2 ** 32 })).data.error.code).toBe('bad_param');
    expect((await get(s, ps[0], { region: 0 })).data.error.code).toBe('bad_param');
  });
});

describe('windows and regions are decided by the server', () => {
  it('daily categories (1, 4) only count rows from today (UTC); season categories from this week', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    const ins = (cat, updated) => s.store.sql.exec(
      'INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (?,?,?,?,?,?)', cat, a.pid, 2, 77, makeBlob('Alpha'), updated);
    const yesterday = T0 - 13 * 3600_000; // 2026-10-29 23:00 UTC
    ins(1, yesterday);
    ins(0, yesterday);
    expect((await get(s, a, { category: 1, limit: 5 })).data.rows).toEqual([]);
    expect((await get(s, a, { category: 0, limit: 5 })).data.rows).toHaveLength(1);
    s.advance(6 * 24 * 3600_000); // 2026-11-05: the season turned over on Nov 5 00:00
    expect((await get(s, a, { category: 0, limit: 5 })).data.rows).toEqual([]);
  });

  it('the client "since" value does not widen the window', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    s.store.sql.exec('INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (1,?,2,5,?,?)', a.pid, makeBlob('Alpha'), T0 - 86400_000);
    expect((await get(s, a, { category: 1, limit: 5, since: 4_000_000 })).data.rows).toEqual([]);
  });

  it('region pool mode (default) shows everyone; mask mode filters by the requested bits', async () => {
    const pool = makeServer();
    const mask = makeServer({ RANK_REGION_MODE: 'mask' });
    for (const srv of [pool, mask]) {
      const a = await signUp(srv, 'Alpha');
      const b = await signUp(srv, 'Beta');
      srv.store.sql.exec('INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (0,?,2,5,?,?)', a.pid, makeBlob('Alpha'), srv.clock.now);
      srv.store.sql.exec('INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (0,?,4,9,?,?)', b.pid, makeBlob('Beta'), srv.clock.now);
      srv.p = [a, b];
    }
    expect((await get(pool, pool.p[0], { limit: 5, region: 2 })).data.rows).toHaveLength(2);
    expect((await get(mask, mask.p[0], { limit: 5, region: 2 })).data.rows.map((x) => x.score)).toEqual([5]);
    expect((await get(mask, mask.p[0], { limit: 5, region: 6 })).data.rows.map((x) => x.score)).toEqual([9, 5]);
    expect((await get(mask, mask.p[0], { limit: 5, region: 255 })).data.rows).toHaveLength(2);
  });

  it('a row updated at the very start of the window counts; one millisecond earlier does not', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    const b = await signUp(s, 'Beta');
    const dayStart = Date.UTC(2026, 9, 30);
    s.store.sql.exec('INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (1,?,2,5,?,?)', a.pid, makeBlob('Alpha'), dayStart);
    s.store.sql.exec('INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (1,?,2,6,?,?)', b.pid, makeBlob('Beta'), dayStart - 1);
    expect((await get(s, a, { category: 1, limit: 5 })).data.rows.map((x) => x.pid)).toEqual([a.pid]);
  });
});

describe('end to end: put, then read back', () => {
  it('rows survive put -> toplist/near with the exact userdata', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const names = ['Alpha', 'Beta', 'Gamma'];
    const ps = [];
    for (const [i, n] of names.entries()) {
      const p = await signUp(s, n);
      await put(s, p, { score: (i + 1) * 100, data: makeBlob(n, { wins: i }) });
      ps.push(p);
    }
    const top = await get(s, ps[0], { mode: 1, limit: 3 });
    expect(top.data.rows.map((r) => r.pid)).toEqual([ps[2].pid, ps[1].pid, ps[0].pid]);
    expect(top.data.rows[0].data).toBe(makeBlob('Gamma', { wins: 2 }));
    const near = await get(s, ps[0], { mode: 2, limit: 3 });
    expect(near.data.rows.map((r) => r.order)).toEqual([1, 2, 3]);
  });

  it('a later put replaces the row (score, blob, lastupdate)', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    await put(s, a, { score: 10, data: makeBlob('Alpha', { wins: 1 }) });
    s.advance(5000);
    await put(s, a, { score: 7, data: makeBlob('Alpha', { wins: 2 }) });
    const r = await get(s, a, { mode: 1, limit: 1 });
    expect(r.data.rows[0]).toMatchObject({ score: 7, data: makeBlob('Alpha', { wins: 2 }), lastupdate: Math.floor((T0 + 5000) / 1000) });
    expect(r.data.total).toBe(1);
  });

  it('kill switch "ranking" stops both calls', async () => {
    const s = makeServer({ KILL_SWITCH: 'ranking' });
    const a = await signUp(s, 'Alpha');
    expect((await put(s, a, { score: 0 })).status).toBe(503);
    expect((await get(s, a, {})).status).toBe(503);
  });
});
