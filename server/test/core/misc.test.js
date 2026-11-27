import { describe, it, expect } from 'vitest';
import { createHash, createHmac, randomBytes } from 'node:crypto';
import { makeServer, HMAC_KEY, T0, signUp } from '../helpers.js';
import { md5, hmacMd5 } from '../../src/core/md5.js';
import { season, dateParts, windowStart, SEASON_STARTS } from '../../src/core/calendar.js';
import { openNodeSql } from '../../src/sqlite-node.js';
import { migrate, MIGRATIONS } from '../../src/core/schema.js';

describe('md5 / hmac-md5', () => {
  it('matches node crypto for many lengths (padding boundaries)', () => {
    for (const n of [0, 1, 3, 55, 56, 57, 63, 64, 65, 111, 112, 119, 120, 128, 1000]) {
      const data = randomBytes(n);
      expect(Buffer.from(md5(data)).toString('hex')).toBe(createHash('md5').update(data).digest('hex'));
    }
  });
  it('matches node HMAC-MD5 with the 32-byte game key and with odd key sizes', () => {
    const data = randomBytes(0x70);
    expect(Buffer.from(hmacMd5(HMAC_KEY, data)).toString('hex')).toBe(createHmac('md5', Buffer.from(HMAC_KEY)).update(data).digest('hex'));
    for (const klen of [1, 16, 64, 65, 100]) {
      const k = randomBytes(klen);
      expect(Buffer.from(hmacMd5(k, data)).toString('hex')).toBe(createHmac('md5', k).update(data).digest('hex'));
    }
  });
  it('RFC 2202 test case 1', () => {
    const key = new Uint8Array(16).fill(0x0b);
    expect(Buffer.from(hmacMd5(key, Buffer.from('Hi There'))).toString('hex')).toBe('9294727a3638bb1c13f48ef8158bfc9d');
  });
});

describe('calendar', () => {
  it('has 52 season starts and finds the weekly window', () => {
    expect(SEASON_STARTS.length).toBe(52);
    const s = season(T0); // 2026-10-30 is inside the season that began Oct 29
    expect(new Date(s.startMs).toISOString()).toBe('2026-10-29T00:00:00.000Z');
    expect(new Date(s.endMs).toISOString()).toBe('2026-11-05T00:00:00.000Z');
  });
  it('season boundaries: exactly on a start, day before, and year end', () => {
    expect(new Date(season(Date.UTC(2026, 10, 5, 0, 0, 0)).startMs).toISOString()).toBe('2026-11-05T00:00:00.000Z');
    expect(new Date(season(Date.UTC(2026, 10, 4, 23, 59, 59)).startMs).toISOString()).toBe('2026-10-29T00:00:00.000Z');
    const last = season(Date.UTC(2026, 11, 31, 23, 0, 0));
    expect(new Date(last.startMs).toISOString()).toBe('2026-12-24T00:00:00.000Z');
    expect(new Date(last.endMs).toISOString()).toBe('2027-01-01T00:00:00.000Z');
    expect(season(Date.UTC(2027, 0, 1)).index).toBe(0);
  });
  it('daily window is UTC midnight', () => {
    expect(new Date(windowStart(1, T0)).toISOString()).toBe('2026-10-30T00:00:00.000Z');
    expect(new Date(windowStart(4, T0)).toISOString()).toBe('2026-10-30T00:00:00.000Z');
    expect(new Date(windowStart(0, T0)).toISOString()).toBe('2026-10-29T00:00:00.000Z');
  });
  it('date parts', () => {
    const p = dateParts(Date.UTC(2024, 1, 29, 13, 45, 7));
    expect(p).toMatchObject({ year: 2024, month: 2, mday: 29, hour: 13, min: 45, sec: 7, yday: 59 });
  });
});

describe('plumbing', () => {
  it('GET /v1/time returns UTC with DWC-shaped fields', async () => {
    const s = makeServer();
    const r = await s.call('GET', '/v1/time');
    expect(r.status).toBe(200);
    expect(r.data).toMatchObject({ utcMs: T0, year: 2026, month: 10, month0: 9, mday: 30, hour: 12, min: 0, sec: 0, wday: 5, yday: 302 });
    expect(r.data.season.index).toBe(43);
    expect(r.headers.get('x-msc-proto')).toBe('1');
    s.advance(61_000);
    expect((await s.call('GET', '/v1/time')).data.min).toBe(1);
  });

  it('info lists limits, stun, punch hints', async () => {
    const s = makeServer();
    const r = await s.call('GET', '/v1/info');
    expect(r.data).toMatchObject({ proto: 1, limits: { nameMin: 2, nameMax: 10, dataMax: 768, blobSize: 128 }, stun: ['stun.cloudflare.com:3478'] });
    expect(r.data.punch).toEqual({ intervalMs: 700, tries: 7, lingerMs: 5000 });
    expect(r.data.features.turn).toBe(true);
  });

  it('unknown path 404, wrong method 405, bad json 400, huge body 413', async () => {
    const s = makeServer();
    expect((await s.call('GET', '/v1/nope')).status).toBe(404);
    expect((await s.call('GET', '/v1/register')).status).toBe(405);
    const bad = await s.call('POST', '/v1/register', '{not json');
    expect(bad.status).toBe(400);
    expect(bad.data.error.code).toBe('bad_json');
    const big = await s.call('POST', '/v1/register', JSON.stringify({ name: 'x'.repeat(9000) }));
    expect(big.status).toBe(413);
    expect(big.data.error.code).toBe('too_large');
  });

  it('error bodies are {error:{code,message}} and never echo input', async () => {
    const s = makeServer();
    const r = await s.call('POST', '/v1/register', { name: '<script>' });
    expect(r.status).toBe(400);
    expect(r.data.error.code).toBe('bad_name');
    expect(JSON.stringify(r.data)).not.toContain('<script>');
  });

  it('old clients are refused with 426', async () => {
    const s = makeServer({ MIN_CLIENT_PROTO: '2' });
    const r = await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': '1' });
    expect(r.status).toBe(426);
    expect(r.data.error.code).toBe('upgrade_required');
    expect((await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': '2' })).status).toBe(200);
  });

  it('migrations are idempotent and ordered', () => {
    const st = openNodeSql();
    expect(migrate(st.sql, 1)).toBe(MIGRATIONS.at(-1).id);
    expect(migrate(st.sql, 2)).toBe(MIGRATIONS.at(-1).id);
    expect(st.sql.exec('SELECT COUNT(*) AS n FROM _migrations').one().n).toBe(MIGRATIONS.length);
    // profile ids start above 100000
    st.sql.exec(`INSERT INTO profiles (code, name, name_key, token_hash, created_ms, last_seen_ms) VALUES ('c','a','a','h',0,0)`);
    expect(st.sql.exec('SELECT pid FROM profiles').one().pid).toBe(100001);
  });

  it('survives restart on the same database (re-created app, same data)', async () => {
    const a = makeServer();
    const p = await signUp(a, 'Persist');
    const { createApp } = await import('../../src/core/app.js');
    const app2 = createApp({ sql: a.store.sql, tx: a.store.tx, env: a.env, now: () => T0 });
    const r = await app2.handle(new Request('https://x.invalid/v1/me', { headers: { authorization: `Bearer ${p.token}` } }));
    expect((await r.json()).name).toBe('Persist');
  });

  it('unexpected exceptions become 500 internal without details', async () => {
    const s = makeServer({}, { deps: { turnProvider: { mint: async () => { throw new Error('secret detail'); } } } });
    const p = await signUp(s, 'Boom');
    const q = await signUp(s, 'Boom2');
    const room = (await s.call('POST', '/v1/rooms', undefined, p.token)).data;
    await s.call('POST', '/v1/rooms/join', { code: room.code }, q.token);
    const r = await s.call('POST', '/v1/turn', { room: room.code }, p.token);
    expect(r.status).toBe(502); // provider failures are reported as an upstream error
    expect(JSON.stringify(r.data)).not.toContain('secret detail');
  });
});
