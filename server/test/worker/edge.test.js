// The Worker answers junk itself: the Durable Object (and with it a free-tier request and storage work) is never woken.
import { describe, it, expect } from 'vitest';
import { exports } from 'cloudflare:workers';
import { listDurableObjectIds } from 'cloudflare:test';
import { env } from 'cloudflare:workers';

const BASE = 'https://svc.example.invalid';
const f = (method, path, headers = {}, body) => exports.default.fetch(new Request(`${BASE}${path}`, { method, headers, body }));

describe('Worker edge rejection', () => {
  it('unknown paths, bad methods, malformed Authorization and oversized bodies never create or wake the object', async () => {
    expect((await listDurableObjectIds(env.GAME)).length).toBe(0);
    const checks = [
      [await f('GET', '/'), 404, 'not_found'],
      [await f('GET', '/v1/nothing'), 404, 'not_found'],
      [await f('POST', '/v1/info'), 405, 'method_not_allowed'],
      [await f('DELETE', '/v1/time'), 405, 'method_not_allowed'],
      [await f('GET', '/v1/me'), 401, 'unauthorized'],
      [await f('GET', '/v1/me', { authorization: 'Bearer nope' }), 401, 'unauthorized'],
      [await f('GET', '/v1/admin/stats', { authorization: 'Basic abc' }), 401, 'unauthorized'],
      [await f('POST', '/v1/register', { 'content-length': '9000' }, 'x'.repeat(9000)), 413, 'too_large'],
      [await f('GET', '/v1/info', { 'x-msc-proto': 'abc' }), 400, 'bad_app_version'],
    ];
    for (const [r, status, code] of checks) {
      expect(r.status).toBe(status);
      expect((await r.json()).error.code).toBe(code);
    }
    expect((await listDurableObjectIds(env.GAME)).length).toBe(0); // still no object: nothing reached it
    expect((await (await f('GET', '/v1/time')).json()).year).toBeGreaterThan(2000);
    expect((await listDurableObjectIds(env.GAME)).length).toBe(1); // a real request does
  });
});
