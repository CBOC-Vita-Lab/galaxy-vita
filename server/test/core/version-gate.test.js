// Finding 10: version gating must fail closed.
import { describe, it, expect } from 'vitest';
import { makeServer } from '../helpers.js';

describe('X-Msc-Proto', () => {
  const s = makeServer();
  it('a header that is not a whole number is refused (it used to pass as "no header")', async () => {
    for (const v of ['abc', '1.5', '-1', '1e1', '0x1', '', ' ', '99999', '1 2', 'NaN', 'Infinity']) {
      const r = await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': v });
      // fetch() drops an empty header value on some runtimes; whatever reaches the server must not be waved through
      if (v === '') continue;
      expect(r.status, JSON.stringify(v)).toBe(400);
      expect(r.data.error).toMatchObject({ code: 'bad_app_version', field: 'x-msc-proto' });
    }
  });
  it('a whole number is fine, and older than MIN_CLIENT_PROTO is 426', async () => {
    expect((await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': '1' })).status).toBe(200);
    expect((await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': '2' })).status).toBe(200); // a newer client: the server is tolerant
    const strictServer = makeServer({ MIN_CLIENT_PROTO: '2' });
    expect((await strictServer.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': '1' })).data.error.code).toBe('upgrade_required');
    expect((await strictServer.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': '0' })).data.error.code).toBe('upgrade_required');
    expect((await strictServer.call('GET', '/v1/info')).data.error.code).toBe('upgrade_required'); // no header = protocol 1
    expect((await strictServer.call('GET', '/v1/info', undefined, undefined, { 'x-msc-proto': '2' })).status).toBe(200);
  });
  it('applies to every route including the authenticated ones (before the token is looked at)', async () => {
    const r = await s.call('GET', '/v1/me', undefined, 'A'.repeat(43), { 'x-msc-proto': 'abc' });
    expect(r.data.error.code).toBe('bad_app_version');
  });
});

describe('REQUIRE_APP_VERSION', () => {
  it('on: every endpoint but info / time / servers needs X-Msc-App; a malformed one is 400 as before', async () => {
    const s = makeServer({ REQUIRE_APP_VERSION: '1' });
    for (const [m, p] of [['POST', '/v1/register'], ['GET', '/v1/register/challenge'], ['GET', '/v1/me'], ['POST', '/v1/rooms'], ['POST', '/v1/turn'], ['POST', '/v1/rank/get']]) {
      const r = await s.call(m, p, undefined, 'A'.repeat(43));
      expect(r.data.error.code, `${m} ${p}`).toBe('bad_app_version');
    }
    for (const p of ['/v1/info', '/v1/time', '/v1/servers']) expect((await s.call('GET', p)).status).toBe(200);
    expect((await s.call('GET', '/v1/me', undefined, 'A'.repeat(43), { 'x-msc-app': '1.2.0' })).data.error.code).toBe('unauthorized'); // got past the gate
    expect((await s.call('GET', '/v1/me', undefined, 'A'.repeat(43), { 'x-msc-app': 'abc' })).data.error.code).toBe('bad_app_version');
  });
  it('the admin API is not gated by the game version', async () => {
    const s = makeServer({ REQUIRE_APP_VERSION: '1' });
    expect((await s.call('GET', '/v1/admin/stats', undefined, undefined, { authorization: 'Bearer test-admin-token-0123456789' })).status).toBe(200);
  });
  it('only the literal "1" turns it on, as documented; the tests run with it explicitly off', async () => {
    expect(makeServer({ REQUIRE_APP_VERSION: '0' }).app.cfg.requireApp).toBe(false);
    expect(makeServer({ REQUIRE_APP_VERSION: '1' }).app.cfg.requireApp).toBe(true);
    expect(makeServer().env.REQUIRE_APP_VERSION).toBe('0');
  });
});
