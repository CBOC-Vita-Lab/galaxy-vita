// The Worker memo guard: kill switch and rate limit apply to memoised hits too.
import { describe, it, expect } from 'vitest';
import { createMemoGuard, RECHECK_MS, HIT_CAP } from '../../src/core/memo_guard.js';

describe('memo guard', () => {
  it('static KILL_SWITCH and the object\'s switches turn the route off; "all" too; other names do not', () => {
    const g = createMemoGuard();
    expect(g.off({}, 'times')).toBe(false);
    expect(g.off({ KILL_SWITCH: 'quick, times' }, 'times')).toBe(true);
    expect(g.off({ KILL_SWITCH: 'all' }, 'times')).toBe(true);
    expect(g.off({ KILL_SWITCH: 'quick' }, 'times')).toBe(false);
    g.seen(['times'], 1000);
    expect(g.off({}, 'times')).toBe(true);
    expect(g.off({}, 'ranking')).toBe(false);
    g.seen([], 2000);
    expect(g.off({}, 'times')).toBe(false);
    g.seen(['all'], 3000);
    expect(g.off({}, 'times')).toBe(true);
  });

  it('the view of the switches goes stale after RECHECK_MS, can be forgotten, retried soon, and survives a clock step back', () => {
    const g = createMemoGuard();
    expect(g.stale(10_000)).toBe(true);
    g.seen([], 10_000);
    expect(g.stale(10_000 + RECHECK_MS - 1)).toBe(false);
    expect(g.stale(10_000 + RECHECK_MS)).toBe(true);
    expect(g.stale(9_000)).toBe(true);
    g.routeOn(20_000);
    expect(g.stale(20_001)).toBe(false);
    g.forget();
    expect(g.stale(20_001)).toBe(true);
    g.retrySoon(30_000);
    expect(g.stale(30_500)).toBe(false);
    expect(g.stale(31_000)).toBe(true);
  });

  it('rate limits hits per address (IPv6 by /64), scaled by RATE_LIMIT_SCALE, separately per route', () => {
    const g = createMemoGuard();
    for (let i = 0; i < HIT_CAP; i++) expect(g.take({}, 'times', '203.0.113.5', 1000)).toBe(0);
    expect(g.take({}, 'times', '203.0.113.5', 1000)).toBeGreaterThan(0);
    expect(g.take({}, 'times', '203.0.113.6', 1000)).toBe(0);
    expect(g.take({}, 'ranking', '203.0.113.5', 1000)).toBe(0);
    for (let i = 0; i < HIT_CAP; i++) g.take({}, 'times', '2001:db8:1:2:aaaa::1', 1000);
    expect(g.take({}, 'times', '2001:db8:1:2:bbbb::2', 1000)).toBeGreaterThan(0);
    for (let i = 0; i < 3 * HIT_CAP; i++) expect(g.take({ RATE_LIMIT_SCALE: '1000' }, 'times', '203.0.113.9', 1000)).toBe(0);
    expect(g.take({}, 'times', '203.0.113.5', 1000 + 60_000)).toBe(0); // refilled
  });
});
