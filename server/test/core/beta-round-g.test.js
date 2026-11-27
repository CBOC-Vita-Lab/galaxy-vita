// Round g (package m): GET /v1/prove, the service's proof that it holds the admin token for a host name.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { createHmac } from 'node:crypto';
import { makeSmg } from '../smg-helpers.js';
import { SMG_ROUTE_META } from '../../src/core/edge.js';

const TOKEN = 'test-admin-token-0123456789';
const N = '0123456789abcdef0123456789abcdef';
const want = (host, n) => createHmac('sha256', TOKEN).update(`smg-online prove v1\n${host}\n${n}`).digest('hex');
let lines; let spy;
beforeEach(() => { lines = []; spy = vi.spyOn(console, 'log').mockImplementation((x) => { lines.push(String(x)); }); });
afterEach(() => spy.mockRestore());
const GET = (s, url, ip = '198.51.100.7') => s.app.handle(new Request(url, { headers: { 'cf-connecting-ip': ip } })).then(async (r) => ({ status: r.status, data: await r.json() }));

describe('GET /v1/prove', () => {
  it('the proof is HMAC-SHA256(admin token, "smg-online prove v1\\n" + host + "\\n" + n); another host or nonce gives a different proof; the host is lower-cased', async () => {
    const s = makeSmg();
    const a = await s.call('GET', `/v1/prove?n=${N}`);
    expect(a.status).toBe(200);
    expect(a.data).toEqual({ proof: want('test.invalid', N) });
    const b = await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${N}`);
    expect(b.data.proof).toBe(want('galaxy.psvita.dev', N));
    expect(b.data.proof).not.toBe(a.data.proof);
    expect((await GET(s, `https://GALAXY.psvita.dev/v1/prove?n=${N}`)).data.proof).toBe(b.data.proof);
    const c = await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${'f'.repeat(32)}`);
    expect(c.data.proof).toBe(want('galaxy.psvita.dev', 'f'.repeat(32)));
    expect(c.data.proof).not.toBe(b.data.proof);
  });
  it('it needs no token and no version header, and is in the route table as an open route', async () => {
    const s = makeSmg();
    expect((await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${N}`)).status).toBe(200);
    expect(SMG_ROUTE_META.some((r) => r[0] === 'GET' && r[1] === '/v1/prove' && r[2] === 'none' && r[3] === true)).toBe(true);
  });
  it('404 not_found, exactly like an admin route, when ADMIN_TOKEN is missing or too short', async () => {
    for (const env of [{ ADMIN_TOKEN: '' }, { ADMIN_TOKEN: 'short' }]) {
      const s = makeSmg(env);
      const r = await s.call('GET', `/v1/prove?n=${N}`);
      const adm = await s.call('GET', '/v1/admin/stats', undefined, 'whatever-whatever-whatever-1');
      expect(r.status).toBe(404);
      expect(r.data.error.code).toBe('not_found');
      expect(r.data.error.code).toBe(adm.data.error.code);
      expect(JSON.stringify(r.data)).not.toMatch(/proof/);
    }
  });
  it('a missing or malformed n is 400', async () => {
    const s = makeSmg();
    for (const q of ['', '?n=', '?n=abc', `?n=${N.toUpperCase()}`, `?n=${N}0`, `?n=${N.slice(1)}`, '?n=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz', `?m=${N}`]) {
      const r = await s.call('GET', `/v1/prove${q}`);
      expect(r.status, q).toBe(400);
      expect(r.data.error.code).toBe('bad_request');
    }
  });
  it('10 a minute per address, then 429; another address is not affected; the next minute opens again', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    for (let i = 0; i < 10; i++) expect((await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${N}`, '198.51.100.50')).status).toBe(200);
    expect((await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${N}`, '198.51.100.50')).status).toBe(429);
    expect((await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${N}`, '198.51.100.51')).status).toBe(200);
    s.advance(61_000);
    expect((await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${N}`, '198.51.100.50')).status).toBe(200);
  });
  it('no log line holds n or the proof', async () => {
    const s = makeSmg();
    const r = await GET(s, `https://galaxy.psvita.dev/v1/prove?n=${N}`);
    await s.call('GET', `/v1/prove?n=${N.replace(/0/g, '1')}`);
    await s.call('GET', '/v1/prove?n=bad');
    const all = lines.join('\n');
    expect(all).not.toContain(N);
    expect(all).not.toContain(r.data.proof);
  });
});
