import { describe, it, expect } from 'vitest';
import { makeServer, signUp } from '../helpers.js';
import { RateLimiter } from '../../src/core/ratelimit.js';

describe('token bucket', () => {
  it('allows a burst, then reports a wait, then refills', () => {
    const l = new RateLimiter();
    for (let i = 0; i < 3; i++) expect(l.take('k', 3, 60, 0)).toBe(0);
    const wait = l.take('k', 3, 60, 0);
    expect(wait).toBeGreaterThan(0);
    expect(l.take('k', 3, 60, 20_000)).toBe(0); // one token per 20 s
    expect(l.take('k', 3, 60, 20_000)).toBeGreaterThan(0);
    expect(l.take('other', 3, 60, 0)).toBe(0); // buckets are independent
  });
  it('bounded memory: sweeps when full', () => {
    const l = new RateLimiter(100);
    for (let i = 0; i < 500; i++) l.take(`k${i}`, 1, 60, i);
    expect(l.buckets.size).toBeLessThanOrEqual(100);
  });
});

describe('route limits (RATE_LIMIT_SCALE=1)', () => {
  const strict = () => makeServer({ RATE_LIMIT_SCALE: '1' });

  it('registration: a burst of 10 per address, then 3 per hour, then 429 with Retry-After; other addresses unaffected', async () => {
    const s = strict();
    // a burst of 10 (a venue behind one NAT), then 3 per hour
    for (let i = 0; i < 10; i++) expect((await s.call('POST', '/v1/register', { name: `Plr${i}x` }, undefined, { ip: '203.0.113.5' })).status).toBe(200);
    const r = await s.call('POST', '/v1/register', { name: 'PlrZz' }, undefined, { ip: '203.0.113.5' });
    expect(r.status).toBe(429);
    expect(r.data.error.code).toBe('rate_limited');
    expect(Number(r.headers.get('retry-after'))).toBeGreaterThan(0);
    expect(r.data.error.retryAfterSec).toBeGreaterThan(0);
    expect((await s.call('POST', '/v1/register', { name: 'PlrZz' }, undefined, { ip: '203.0.113.6' })).status).toBe(200);
    s.advance(21 * 60_000); // refilled one token (3 per hour)
    expect((await s.call('POST', '/v1/register', { name: 'PlrYy' }, undefined, { ip: '203.0.113.5' })).status).toBe(200);
  });

  it('global registration cap per day (1,000: README.md free-tier arithmetic)', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1' });
    let blocked = 0;
    for (let i = 0; i < 1020; i++) {
      const r = await s.call('POST', '/v1/register', { name: `P${String(i).padStart(4, '0')}` }, undefined, { ip: `10.0.${i >> 8}.${i & 255}` });
      if (r.status === 429) blocked++;
    }
    expect(blocked).toBe(20);
  });

  it('room codes cannot be guessed quickly: 20 joins per 10 minutes per player', async () => {
    const s = strict();
    const a = await signUp(s, 'Alpha');
    let statuses = [];
    for (let i = 0; i < 22; i++) statuses.push((await s.call('POST', '/v1/rooms/join', { code: 'ZZZZZZ' }, a.token)).status);
    expect(statuses.slice(0, 20).every((x) => x === 404)).toBe(true);
    expect(statuses.slice(20)).toEqual([429, 429]);
  });

  it('ranking put: 12 per minute per player', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1', RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    const out = [];
    for (let i = 0; i < 14; i++) out.push((await s.call('POST', '/v1/rank/put', { category: 0, region: 2, score: 0, data: 'x' }, a.token)).status);
    expect(out.filter((x) => x === 429)).toHaveLength(2);
    s.advance(60_000);
    expect((await s.call('POST', '/v1/rank/put', { category: 0, region: 2, score: 0, data: 'x' }, a.token)).status).not.toBe(429);
  });

  it('wrong tokens are throttled per address', async () => {
    const s = strict();
    let n429 = 0;
    for (let i = 0; i < 25; i++) {
      const r = await s.call('GET', '/v1/me', undefined, 'A'.repeat(43), { ip: '203.0.113.77' });
      if (r.status === 429) n429++;
    }
    expect(n429).toBe(5);
  });

  it('limits are per player, not shared', async () => {
    const s = strict();
    const a = await signUp(s, 'Alpha');
    const b = await signUp(s, 'Beta');
    for (let i = 0; i < 20; i++) await s.call('POST', '/v1/rooms/join', { code: 'ZZZZZZ' }, a.token);
    expect((await s.call('POST', '/v1/rooms/join', { code: 'ZZZZZZ' }, a.token)).status).toBe(429);
    expect((await s.call('POST', '/v1/rooms/join', { code: 'ZZZZZZ' }, b.token)).status).toBe(404);
  });

  it('polling at 1-2 s intervals stays within limits', async () => {
    const s = strict();
    const a = await signUp(s, 'Alpha');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    for (let i = 0; i < 300; i++) {
      s.advance(1500);
      expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).status).toBe(200);
    }
  });
});
