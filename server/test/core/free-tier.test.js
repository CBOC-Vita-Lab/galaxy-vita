// Finding 9: free-tier amplification. Edge rejection, per-account daily caps, a brake that cannot be evicted.
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { makeServer, signUp } from '../helpers.js';
import { createApp } from '../../src/core/app.js';
import { edgeCheck, ROUTE_META } from '../../src/core/edge.js';
import { RateLimiter } from '../../src/core/ratelimit.js';

const req = (method, path, headers = {}) => new Request(`https://x.invalid${path}`, { method, headers });
const TOKEN = 'A'.repeat(43);

describe('edge checks (the Worker answers these without waking the Durable Object)', () => {
  const env = { ADMIN_TOKEN: 'secret-admin-token-123456789', REQUIRE_APP_VERSION: '0' };
  it('unknown paths are 404, wrong methods 405 with Allow', () => {
    expect(edgeCheck(req('GET', '/'), env)).toMatchObject({ status: 404, code: 'not_found' });
    expect(edgeCheck(req('GET', '/v1/nope'), env)).toMatchObject({ status: 404 });
    expect(edgeCheck(req('GET', '/v1/info/'), env)).toMatchObject({ status: 404 });
    expect(edgeCheck(req('GET', '/v1/rooms/' + 'A'.repeat(65)), env)).toMatchObject({ status: 404 });
    const m = edgeCheck(req('POST', '/v1/info'), env);
    expect(m).toMatchObject({ status: 405, code: 'method_not_allowed' });
    expect(m.headers.allow).toBe('GET');
    expect(edgeCheck(req('PATCH', '/v1/me', { authorization: `Bearer ${TOKEN}` }), env)).toMatchObject({ status: 405 });
    expect(edgeCheck(req('HEAD', '/v1/time'), env)).toMatchObject({ status: 405 });
    expect(edgeCheck(req('OPTIONS', '/v1/time'), env)).toMatchObject({ status: 405 });
    expect(edgeCheck(req('GET', '/v1/rooms'), env)).toMatchObject({ status: 405 }); // only POST exists there
  });
  it('valid requests pass', () => {
    expect(edgeCheck(req('GET', '/v1/info'), env)).toBeNull();
    expect(edgeCheck(req('GET', '/v1/me', { authorization: `Bearer ${TOKEN}` }), env)).toBeNull();
    expect(edgeCheck(req('POST', '/v1/matches/aB3dE5gH7jK9/result', { authorization: `Bearer ${TOKEN}` }), env)).toBeNull();
    expect(edgeCheck(req('GET', '/v1/admin/stats', { authorization: 'Bearer secret-admin-token-123456789' }), env)).toBeNull();
  });
  it('malformed Authorization headers are 401 at the edge', () => {
    for (const h of [undefined, '', 'Bearer', 'Bearer ', `bearer ${TOKEN}`, `Basic ${TOKEN}`, `Bearer ${TOKEN}x`, `Bearer ${TOKEN.slice(1)}`, `Bearer ${'+'.repeat(43)}`, `Bearer ${TOKEN} extra`]) {
      const e = edgeCheck(req('GET', '/v1/me', h === undefined ? {} : { authorization: h }), env);
      expect(e, String(h)).toMatchObject({ status: 401, code: 'unauthorized' });
    }
    expect(edgeCheck(req('GET', '/v1/admin/stats', { authorization: 'Bearer ' }), env)).toMatchObject({ status: 401 });
    expect(edgeCheck(req('GET', '/v1/admin/stats', { authorization: 'Bearer has space' }), env)).toMatchObject({ status: 401 });
    expect(edgeCheck(req('GET', '/v1/admin/stats', {}), env)).toMatchObject({ status: 401 });
  });
  it('admin routes do not exist without ADMIN_TOKEN; open routes need no Authorization', () => {
    expect(edgeCheck(req('GET', '/v1/admin/stats', { authorization: 'Bearer abc' }), {})).toMatchObject({ status: 404 });
    expect(edgeCheck(req('POST', '/v1/register'), env)).toBeNull();
  });
  it('oversized bodies are 413 by Content-Length', () => {
    expect(edgeCheck(req('POST', '/v1/register', { 'content-length': '8193' }), env)).toMatchObject({ status: 413, code: 'too_large' });
    expect(edgeCheck(req('POST', '/v1/register', { 'content-length': '8192' }), env)).toBeNull();
  });
  it('version headers: malformed is 400, missing is 400 when required (not on info/time/servers)', () => {
    const on = { ...env, REQUIRE_APP_VERSION: '1' };
    expect(edgeCheck(req('GET', '/v1/info', { 'x-msc-app': 'abc' }), env)).toMatchObject({ status: 400, code: 'bad_app_version' });
    expect(edgeCheck(req('GET', '/v1/info', { 'x-msc-proto': 'abc' }), env)).toMatchObject({ status: 400, code: 'bad_app_version' });
    expect(edgeCheck(req('GET', '/v1/info', { 'x-msc-build': 'zz' }), env)).toMatchObject({ status: 400 });
    expect(edgeCheck(req('GET', '/v1/info'), on)).toBeNull();
    expect(edgeCheck(req('POST', '/v1/register'), on)).toMatchObject({ status: 400, code: 'bad_app_version' });
    expect(edgeCheck(req('GET', '/v1/me', { authorization: `Bearer ${TOKEN}` }), on)).toMatchObject({ status: 400 });
    expect(edgeCheck(req('GET', '/v1/me', { authorization: `Bearer ${TOKEN}`, 'x-msc-app': '1.2.0' }), on)).toBeNull();
  });
  it('the edge table is exactly the application routing table (no drift)', () => {
    const s = makeServer();
    const key = (a) => a.join(' ');
    expect(s.app.routeMeta.map(key).sort()).toEqual(ROUTE_META.map(key).sort());
  });
  it('whatever the edge lets through is answered the same by the application', async () => {
    const s = makeServer();
    for (const [method, path, h] of [['GET', '/v1/nope', {}], ['POST', '/v1/info', {}], ['GET', '/v1/me', {}], ['GET', '/v1/me', { authorization: 'Bearer short' }]]) {
      const r = await s.app.handle(req(method, path, h));
      const e = edgeCheck(req(method, path, h), { ADMIN_TOKEN: 'x'.repeat(24), REQUIRE_APP_VERSION: '0' });
      expect(r.status, `${method} ${path}`).toBe(e.status);
    }
  });
});

describe('per-account daily caps on rooms and matches', () => {
  it('rooms: ROOMS_PER_ACCOUNT_DAY, stored (an evicted object does not reset it), reset at 00:00 UTC', async () => {
    const s = makeServer({ ROOMS_PER_ACCOUNT_DAY: '3' });
    const a = await signUp(s, 'Aaa1');
    for (let i = 0; i < 3; i++) expect((await s.call('POST', '/v1/rooms', undefined, a.token)).status).toBe(200);
    const r = await s.call('POST', '/v1/rooms', undefined, a.token);
    expect(r.status).toBe(429);
    expect(r.data.error).toMatchObject({ code: 'rate_limited', reason: 'daily_cap', scope: 'rooms' });
    expect(r.data.error.retryAfterSec).toBe(12 * 3600);
    expect(r.headers.get('retry-after')).toBe(String(12 * 3600));
    // a second application instance over the same database (= the object was evicted and restarted) still knows
    const s2 = await new Promise((resolve) => resolve(createApp({ sql: s.store.sql, tx: s.store.tx, env: s.env, now: () => s.clock.now })));
    const again = await s2.handle(new Request('https://x.invalid/v1/rooms', { method: 'POST', headers: { authorization: `Bearer ${a.token}`, 'cf-connecting-ip': '198.51.100.250' } }));
    expect(again.status).toBe(429);
    const other = await signUp(s, 'Bbb2');
    expect((await s.call('POST', '/v1/rooms', undefined, other.token)).status).toBe(200); // others unaffected
    s.advance(12 * 3600_000);
    expect((await s.call('POST', '/v1/rooms', undefined, a.token)).status).toBe(200);
  });
  it('matches: every match record counts for both players; host and guest are both checked; ranked queue too', async () => {
    const s = makeServer({ MATCHES_PER_ACCOUNT_DAY: '2' });
    const h = await signUp(s, 'Hosty1');
    const g1 = await signUp(s, 'Guest1');
    const g2 = await signUp(s, 'Guest2');
    const g3 = await signUp(s, 'Guest3');
    for (const g of [g1, g2]) {
      const room = (await s.call('POST', '/v1/rooms', undefined, h.token)).data.code;
      expect((await s.call('POST', '/v1/rooms/join', { code: room }, g.token)).status).toBe(200);
    }
    // the host is at the cap: cannot open another room
    const blocked = await s.call('POST', '/v1/rooms', undefined, h.token);
    expect(blocked.data.error).toMatchObject({ code: 'rate_limited', reason: 'daily_cap', scope: 'matches' });
    // a fresh host cannot be joined by a guest at the cap
    const fresh = await signUp(s, 'Fresh1');
    const room2 = (await s.call('POST', '/v1/rooms', undefined, fresh.token)).data.code;
    const other = await signUp(s, 'Other1');
    const g1again = await s.call('POST', `/v1/rooms/join`, { code: room2 }, h.token); // h is at the cap
    expect(g1again.status).toBe(429);
    expect(g1again.data.error.scope).toBe('matches');
    expect((await s.call('POST', `/v1/rooms/join`, { code: room2 }, other.token)).status).toBe(200);
    // and the ranked queue tells a player at the cap
    expect((await s.call('POST', '/v1/ranked/queue', undefined, h.token)).data.error).toMatchObject({ code: 'rate_limited', scope: 'ranked' });
    expect((await s.call('POST', '/v1/ranked/queue', undefined, g3.token)).status).toBe(200);
  });
  it('a host at the cap cannot be joined (guest gets a clear 429, host_matches)', async () => {
    const s = makeServer({ MATCHES_PER_ACCOUNT_DAY: '1', ROOMS_PER_ACCOUNT_DAY: '5' });
    const h = await signUp(s, 'Hosty1');
    const g1 = await signUp(s, 'Guest1');
    const g2 = await signUp(s, 'Guest2');
    const room = (await s.call('POST', '/v1/rooms', undefined, h.token)).data.code;
    await s.call('POST', '/v1/rooms/join', { code: room }, g1.token);
    // make h look like a host with an open room whose match cap is already used
    const open = (await s.call('POST', '/v1/rooms', undefined, g2.token)).data.code;
    const r = await s.call('POST', '/v1/rooms/join', { code: open }, h.token);
    expect(r.data.error).toMatchObject({ code: 'rate_limited', reason: 'daily_cap' });
  });
  it('defaults are generous for real play and finite for abuse', () => {
    const s = makeServer();
    expect(s.app.cfg.caps).toEqual({ roomsPerAccountDay: 60, matchesPerAccountDay: 60 });
  });
  it('deleting the account removes its counters', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    await s.call('POST', '/v1/rooms', undefined, a.token);
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM daily').toArray()[0].n).toBe(1);
    await s.call('DELETE', '/v1/me', undefined, a.token);
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM daily').toArray()[0].n).toBe(0);
  });
  it('the sweep prunes old daily rows', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    await s.call('POST', '/v1/rooms', undefined, a.token);
    s.advance(10 * 86400_000);
    s.app.sweep();
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM daily').toArray()[0].n).toBe(0);
  });
});

describe('rate-limiter eviction never touches the admin-fail brake', () => {
  it('flooding the ordinary limiter with junk keys does not reset the brake (separate map)', async () => {
    const limiter = new RateLimiter(30); // tiny, so the flood evicts constantly
    const s = makeServer({ RATE_LIMIT_SCALE: '1' }, { deps: { limiter } });
    const ip = '203.0.113.9';
    const bad = (i) => s.call('GET', '/v1/admin/stats', undefined, undefined, { authorization: `Bearer guess${i}`, ip });
    for (let i = 0; i < 5; i++) expect((await bad(i)).status).toBe(401);
    for (let i = 0; i < 200; i++) await s.call('GET', '/v1/info', undefined, undefined, { ip: `198.51.${i >> 8}.${i & 255}` });
    expect(limiter.buckets.size).toBeLessThanOrEqual(30);
    expect([...limiter.buckets.keys()].some((k) => k.startsWith('adminfail'))).toBe(false); // it never lived there
    expect((await bad(6)).status).toBe(429); // the brake remembered the five failures
  });
  it('the same for the wrong-player-token brake', async () => {
    const limiter = new RateLimiter(30);
    const s = makeServer({ RATE_LIMIT_SCALE: '1' }, { deps: { limiter } });
    const ip = '203.0.113.9';
    for (let i = 0; i < 20; i++) await s.call('GET', '/v1/me', undefined, 'B'.repeat(43), { ip });
    for (let i = 0; i < 200; i++) await s.call('GET', '/v1/info', undefined, undefined, { ip: `198.51.${i >> 8}.${i & 255}` });
    expect((await s.call('GET', '/v1/me', undefined, 'B'.repeat(43), { ip })).status).toBe(429);
  });
  it('the brake itself only drops buckets that are back to full, and routes new keys to one shared bucket when saturated', () => {
    const b = new RateLimiter(10, { protect: true });
    for (let i = 0; i < 10; i++) b.take(`k${i}`, 5, 600, 0); // ten live buckets (4 tokens left each)
    expect(b.take('newcomer', 5, 600, 1000)).toBe(0); // goes to the overflow bucket, evicts nobody
    expect(b.buckets.size).toBe(11);
    expect([...b.buckets.keys()].filter((k) => k.startsWith('k'))).toHaveLength(10);
    // the overflow bucket is shared: five more unknown keys exhaust it
    const waits = [];
    for (let i = 0; i < 6; i++) waits.push(b.take(`other${i}`, 5, 600, 1000));
    expect(waits.some((w) => w > 0)).toBe(true);
    // after enough time everything has refilled completely: that is safe to drop
    b.take('later', 5, 600, 10 * 600_000);
    expect([...b.buckets.keys()].filter((k) => k.startsWith('k'))).toHaveLength(0);
    expect(b.buckets.has('later')).toBe(true);
  });
  it('the ordinary limiter still evicts when full (bounded memory) and accepts a burst larger than its rate', () => {
    const l = new RateLimiter(5);
    for (let i = 0; i < 50; i++) l.take(`k${i}`, 1, 60, i);
    expect(l.buckets.size).toBeLessThanOrEqual(5);
    const b = new RateLimiter();
    let ok = 0;
    for (let i = 0; i < 12; i++) if (b.take('x', 3, 3600, 0, 10) === 0) ok++;
    expect(ok).toBe(10); // burst 10 ...
    expect(b.take('x', 3, 3600, 20 * 60_000, 10)).toBe(0); // ... then one token per 20 minutes
    expect(b.take('x', 3, 3600, 20 * 60_000, 10)).toBeGreaterThan(0);
  });
});

describe('wrangler.toml production defaults', () => {
  const toml = readFileSync(new URL('../../wrangler.toml', import.meta.url), 'utf8');
  const v = (name) => new RegExp(`^${name}\\s*=\\s*"([^"]*)"`, 'm').exec(toml)?.[1];
  it('version gating is required in production', () => {
    expect(v('REQUIRE_APP_VERSION')).toBe('1');
  });
  it('the shipped TURN limits match the code defaults', () => {
    const cfg = makeServer().app.cfg.turn;
    expect(Number(v('TURN_MAX_CREDS_PER_PLAYER_DAY'))).toBe(cfg.maxPerPlayerDay);
    expect(Number(v('TURN_MAX_CREDS_PER_DAY'))).toBe(cfg.maxPerDay);
  });
});
