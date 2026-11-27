import { describe, it, expect } from 'vitest';
import { makeServer, signUp, makeBlob } from '../helpers.js';

const ADM = { authorization: 'Bearer test-admin-token-0123456789' };
const adm = (s, method, path, body) => s.call(method, path, body, undefined, ADM);

describe('admin access', () => {
  it('requires the secret; wrong token 401; disabled when no secret is configured', async () => {
    const s = makeServer();
    expect((await s.call('GET', '/v1/admin/stats')).status).toBe(401);
    expect((await s.call('GET', '/v1/admin/stats', undefined, undefined, { authorization: 'Bearer nope' })).status).toBe(401);
    // a player token is not an admin token
    const a = await signUp(s, 'Alpha');
    expect((await s.call('GET', '/v1/admin/stats', undefined, a.token)).status).toBe(401);
    expect((await adm(s, 'GET', '/v1/admin/stats')).status).toBe(200);
    const off = makeServer({ ADMIN_TOKEN: '' });
    expect((await off.call('GET', '/v1/admin/stats', undefined, undefined, { authorization: 'Bearer ' })).status).toBe(404);
  });

  it('brute-forcing the token is rate limited', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1' });
    let last;
    for (let i = 0; i < 8; i++) last = await s.call('GET', '/v1/admin/stats', undefined, undefined, { authorization: `Bearer guess${i}`, ip: '203.0.113.9' });
    expect(last.status).toBe(429);
    expect(last.data.error.code).toBe('rate_limited');
    // even the right token from that address waits for the penalty to expire? (the failure bucket only gates failures)
    expect((await s.call('GET', '/v1/admin/stats', undefined, undefined, { ...ADM, ip: '203.0.113.9' })).status).toBe(200);
  });

  it('stats summarise the service', async () => {
    const s = makeServer();
    await signUp(s, 'Alpha');
    const r = await adm(s, 'GET', '/v1/admin/stats');
    expect(r.data).toMatchObject({ profiles: 1, roomsLive: 0, queue: 0, scores: 0, kill: [] });
  });
});

describe('export / import (backup and restore)', () => {
  it('lists tables, pages through rows, and restores into a fresh server', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const ps = [];
    for (let i = 0; i < 5; i++) {
      const p = await signUp(s, `Plr${i}`);
      await s.call('POST', '/v1/rank/put', { category: 0, region: 2, score: i * 10, data: makeBlob(p.name) }, p.token);
      ps.push(p);
    }
    const list = await adm(s, 'GET', '/v1/admin/export');
    expect(list.data.tables.find((t) => t.table === 'profiles').rows).toBe(5);
    expect(list.data.tables.find((t) => t.table === 'scores').rows).toBe(5);

    const dump = {};
    for (const t of list.data.tables) {
      dump[t.table] = [];
      let after = 0;
      for (;;) {
        const page = await adm(s, 'GET', `/v1/admin/export?table=${t.table}&after=${after}&limit=2`);
        expect(page.status).toBe(200);
        dump[t.table].push(...page.data.rows);
        if (page.data.next === null) break;
        after = page.data.next;
      }
      expect(dump[t.table].length).toBe(t.rows);
    }

    const fresh = makeServer({ RANK_PUT_POLICY: 'open' });
    for (const [table, rows] of Object.entries(dump)) {
      if (!rows.length) continue;
      const r = await adm(fresh, 'POST', '/v1/admin/import', { table, rows });
      expect(r.data).toEqual({ ok: true, imported: rows.length });
    }
    // the restored server knows the same players: their old tokens still work and the board is intact
    const me = await fresh.call('GET', '/v1/me', undefined, ps[3].token);
    expect(me.data).toMatchObject({ pid: ps[3].pid, name: 'Plr3' });
    const top = await fresh.call('POST', '/v1/rank/get', { mode: 1, category: 0, region: 2, sort: 1, since: 1, limit: 3 }, ps[0].token);
    expect(top.data.rows.map((r) => r.score)).toEqual([40, 30, 20]);
  });

  it('refuses unknown tables and columns', async () => {
    const s = makeServer();
    expect((await adm(s, 'GET', '/v1/admin/export?table=sqlite_master')).data.error.code).toBe('bad_table');
    expect((await adm(s, 'GET', '/v1/admin/export?table=profiles;DROP')).data.error.code).toBe('bad_table');
    expect((await adm(s, 'POST', '/v1/admin/import', { table: 'profiles', rows: [{ nope: 1 }] })).data.error.code).toBe('bad_request');
    expect((await adm(s, 'POST', '/v1/admin/import', { table: 'users', rows: [] })).data.error.code).toBe('bad_request');
    expect((await adm(s, 'POST', '/v1/admin/import', { table: 'profiles', rows: [{ name: { x: 1 } }] })).data.error.code).toBe('bad_request');
  });
});

describe('switches', () => {
  it('kill switches apply at once, survive a new app instance, and can be cleared', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    expect((await adm(s, 'POST', '/v1/admin/switches', { kill: ['register', 'ranking'] })).data.kill).toEqual(['register', 'ranking']);
    expect((await s.call('POST', '/v1/register', { name: 'Beta' })).data.error.code).toBe('service_disabled');
    expect((await s.call('POST', '/v1/rank/get', { mode: 1, category: 0, limit: 1 }, a.token)).status).toBe(503);
    expect((await s.call('POST', '/v1/rooms', undefined, a.token)).status).toBe(200); // not killed
    const { createApp } = await import('../../src/core/app.js');
    const again = createApp({ sql: s.store.sql, tx: s.store.tx, env: s.env, now: () => s.clock.now });
    const r = await again.handle(new Request('https://x.invalid/v1/info'));
    expect((await r.json()).kill).toEqual(['register', 'ranking']);
    await adm(s, 'POST', '/v1/admin/switches', { kill: [] });
    expect((await s.call('POST', '/v1/register', { name: 'Beta' })).status).toBe(200);
  });

  it('"all" blocks everything but info, time and admin', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    await adm(s, 'POST', '/v1/admin/switches', { kill: ['all'], message: 'back soon' });
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(503);
    expect((await s.call('GET', '/v1/info')).data).toMatchObject({ message: 'back soon', kill: ['all'] });
    expect((await s.call('GET', '/v1/time')).status).toBe(200);
    expect((await adm(s, 'GET', '/v1/admin/stats')).status).toBe(200);
  });

  it('rejects unknown switch names; env KILL_SWITCH applies without admin', async () => {
    const s = makeServer({ KILL_SWITCH: 'ranked' });
    expect((await adm(s, 'POST', '/v1/admin/switches', { kill: ['everything'] })).status).toBe(400);
    const a = await signUp(s, 'Alpha');
    expect((await s.call('POST', '/v1/ranked/queue', undefined, a.token)).data.error.code).toBe('service_disabled');
  });
});

describe('ban', () => {
  it('removes the player from the board and the queue and can be undone', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    await s.call('POST', '/v1/rank/put', { category: 0, region: 2, score: 5, data: makeBlob('Alpha') }, a.token);
    await adm(s, 'POST', '/v1/admin/ban', { pid: a.pid, reason: 'cheating' });
    expect((await adm(s, 'GET', '/v1/admin/stats')).data).toMatchObject({ banned: 1, scores: 0 });
    await adm(s, 'POST', '/v1/admin/ban', { pid: a.pid, banned: false });
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200);
    expect((await adm(s, 'POST', '/v1/admin/ban', { pid: 'x' })).status).toBe(400);
  });
});
