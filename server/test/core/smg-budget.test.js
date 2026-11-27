// The service's own daily request budget (DAILY_REQUEST_BUDGET): checked before anything else, the admin exempt, stored so a restart
// does not hand out a second day's worth, open again the next UTC day. (The Worker side, which then answers without waking the object,
// is in worker.js: covered by reading; the worker-smg suite runs with the default budget.)
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';
import { createApp } from '../../src/core/app.js';

describe('daily request budget', () => {
  it('beyond the budget every call is 503 over_budget (retry at midnight UTC); the admin still works; the next day it opens', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '5', LIVE_RESERVE_PCT: '0' }) // (one limit for everybody here; the reserve is tested in svc3-quota.test.js);
    const a = await smgSignUp(s, 'Ana'); // 1
    for (let i = 0; i < 4; i++) expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200); // 2..5
    const over = await s.call('GET', '/v1/me', undefined, a.token);
    expect(over.status).toBe(503);
    expect(over.data.error.code).toBe('over_budget');
    expect(over.headers.get('x-msc-budget')).toBe('over');
    expect(Number(over.headers.get('retry-after'))).toBeGreaterThanOrEqual(60);
    expect((await s.call('GET', '/v1/info')).status).toBe(503); // open routes too: the budget is checked first
    expect((await s.raw('GET', '/v1/admin/stats', undefined, 'test-admin-token-0123456789')).status).toBe(200);
    s.advance(86400_000);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200);
  });

  it('the count is stored (every 200 calls): a restarted object goes on counting', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '300', LIVE_RESERVE_PCT: '0' });
    for (let i = 0; i < 250; i++) await s.call('GET', '/v1/info');
    const again = createApp({ sql: s.store.sql, tx: s.store.tx, env: s.env, now: () => s.clock.now });
    let first503 = -1;
    for (let i = 0; i < 200 && first503 < 0; i++) {
      const r = await again.handle(new Request('https://test.invalid/v1/info', { headers: { 'cf-connecting-ip': `198.51.100.${i % 250}` } }));
      if (r.status === 503) first503 = i;
    }
    expect(first503).toBeGreaterThanOrEqual(99); // 200 stored + up to 100 more
    expect(first503).toBeLessThanOrEqual(101);
  });
});
