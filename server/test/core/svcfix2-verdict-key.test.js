// svcfix2 item 1 (independent review, P1): the Worker remembers the object's per-address verdict. It used to key that memory on the raw
// address string while the object counts by /64, so an attacker with a /64 who rotates the low 64 bits sent every request to the object.
import { describe, it, expect, vi, afterEach } from 'vitest';
import { makeWorker } from '../worker-node.js';

const T0 = Date.UTC(2026, 10, 12, 1, 0, 0);
afterEach(() => { vi.useRealTimers(); });
const freeze = () => { vi.useFakeTimers({ toFake: ['Date'] }); vi.setSystemTime(T0); };
const tick = (ms) => vi.setSystemTime(Date.now() + ms);

describe('verdict cache is keyed like the object (ipKey)', () => {
  it('rotating the low 64 bits of an IPv6 address costs the object no more requests than one fixed address', async () => {
    const out = {};
    for (const mode of ['fixed', 'rotate']) {
      freeze();
      const w = await makeWorker({ ADDR_DAILY_OPEN: '100' });
      for (let i = 0; i < 1500; i++) {
        tick(1050); // 1 a second: the edge bucket lets it through
        const ip = mode === 'fixed' ? '2001:db8:aaaa:bbbb::1' : `2001:db8:aaaa:bbbb::${(i + 1).toString(16)}`;
        await w.call('GET', '/v1/info', { ip });
      }
      out[mode] = w.stats.doRequests;
    }
    expect(out.fixed).toBeLessThanOrEqual(110);
    expect(out.rotate).toBeLessThanOrEqual(out.fixed + 2);
  });
  it('sec34: rotating the /64 inside one /48 costs the object no more requests than one fixed address (the daily ceilings count the /48)', async () => {
    const out = {};
    for (const mode of ['fixed', 'rotate64']) {
      freeze();
      const w = await makeWorker({ ADDR_DAILY_OPEN: '100' });
      for (let i = 0; i < 1500; i++) {
        tick(1050);
        const ip = mode === 'fixed' ? '2001:db8:aaaa:1::1' : `2001:db8:aaaa:${(i + 1).toString(16)}::1`;
        await w.call('GET', '/v1/info', { ip });
      }
      out[mode] = w.stats.doRequests;
    }
    expect(out.fixed).toBeLessThanOrEqual(110);
    expect(out.rotate64).toBeLessThanOrEqual(out.fixed + 2);
  });
  it('a block set from one spelling of an address also covers an equivalent spelling (IPv4-mapped IPv6)', async () => {
    freeze();
    const w = await makeWorker({ ADDR_DAILY_OPEN: '20' });
    for (let i = 0; i < 60; i++) { tick(1100); await w.call('GET', '/v1/time', { ip: '203.0.113.9' }); }
    const before = w.stats.doRequests;
    const r = await w.call('GET', '/v1/time', { ip: '::ffff:203.0.113.9' });
    expect(r.status).toBe(429);
    expect(w.stats.doRequests).toBe(before);
  });
});
