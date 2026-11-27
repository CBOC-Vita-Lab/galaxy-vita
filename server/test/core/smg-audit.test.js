// Spin-up audit of every smg route (after the earlier hardening and the friends block routes): each route has a rate limit and, where it
// is a feature, a kill switch; junk bodies, junk path segments and junk queries never produce a 5xx; every limit really answers 429.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';

const ADMIN = 'test-admin-token-0123456789';
const BODIES = ['{', '[]', 'null', '7', '"x"', 'true', '{"__proto__":{"x":1}}', '{"constructor":1}', '{"code":{"a":1}}', '{"code":["A"]}', '{"code":null}',
  '{"galaxy":"1","star":1,"frames":1,"players":1,"build":"3.0.0"}', '{"x":' + '['.repeat(200) + ']'.repeat(200) + '}', '{"pid":1.5}', '{"pid":"1"}',
  '{"table":"profiles","rows":[{"__proto__":1}]}', '{"kill":["nope"]}', '{"kill":"times"}', '{"name":"\\u0000"}', '{"candidates":[1]}', JSON.stringify({ code: 'A'.repeat(5000) })];
const SEGMENTS = ['..', '%00', '%2e%2e', 'A'.repeat(64), 'ZZZZ-ZZZZ', 'é', 'null', '0', '-1', '1e999', 'constructor', '__proto__', "'%20OR%201=1"];
const QUERIES = ['', '?v=abc', '?v=-1', '?v=1e999', '?v=', '?limit=-1', '?limit=999999', '?limit=1e3', '?galaxy=1', '?galaxy=1&star=99', '?mode=%00', '?a=1&a=2', '?mode=race&mode=coop', '?category=9'];

const fill = (pattern, seg) => pattern.replace(/:\w+/g, seg);

describe('smg audit: every route', () => {
  it('has a rate limit (or is registration / admin, which have their own), and every feature route has a kill switch', async () => {
    const s = makeSmg();
    const KILLABLE = ['register', 'rooms', 'turn', 'friends', 'quick', 'times', 'report'];
    for (const [method, pattern, o] of s.app.routeOpts) {
      const what = `${method} ${pattern}`;
      if (o.admin) { expect(o.rl, what).toBeUndefined(); continue; } // admin: per-address limit + brake in app.js
      if (o.register) continue; // charged per address and per day in prepareRegister
      expect(Array.isArray(o.rl), `${what} needs rl`).toBe(true);
      expect(o.rl[1] > 0 && o.rl[2] > 0, what).toBe(true);
      expect(['ip', 'pid'], what).toContain(o.rl[3]);
      expect(o.auth ? o.rl[3] === 'pid' : o.rl[3] === 'ip', `${what}: scope follows auth`).toBe(true);
    }
    // every state-changing or costly authenticated route is switchable
    for (const [method, pattern, o] of s.app.routeOpts) {
      if (!o.auth || (method === 'GET' && /^\/v1\/(me|players)/.test(pattern))) continue;
      if (/^\/v1\/(me|rooms\/:code)/.test(pattern)) continue; // own account, and the room you are in: always reachable (leaving must work)
      expect(KILLABLE, `${method} ${pattern} kill`).toContain(o.kill);
    }
    expect(s.app.routeOpts.find((r) => r[1] === '/v1/public/times')[2]).toMatchObject({ kill: 'times', web: true });
  });

  it('junk bodies on every route are 4xx, never 5xx and never a change of state', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const before = JSON.stringify([a, b]);
    for (const [method, pattern, o] of s.app.routeOpts) {
      if (method === 'GET' || o.register || (method === 'DELETE' && pattern === '/v1/me')) continue;
      const path = fill(pattern, 'ABCDEFGH');
      for (const body of BODIES) {
        const r = o.admin ? await s.raw(method, path, body, ADMIN) : await s.call(method, path, body, o.auth ? a.token : undefined);
        expect(r.status, `${method} ${path} ${body.slice(0, 40)}`).toBeLessThan(500);
        if (!o.admin) expect(r.status, `${method} ${path} ${body.slice(0, 40)}`).toBeGreaterThanOrEqual(400);
      }
    }
    expect(JSON.stringify([a, b])).toBe(before);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).data.name).toBe('Ana'); // still there, not renamed by the junk
    expect((await s.call('GET', '/v1/friends', undefined, a.token)).data.friends).toEqual([]);
  });

  it('junk path segments and queries on every route are never 5xx', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    for (const [method, pattern, o] of s.app.routeOpts) {
      if (o.register || (method === 'DELETE' && pattern === '/v1/me')) continue;
      for (const seg of pattern.includes(':') ? SEGMENTS : ['x']) {
        const path = fill(pattern, encodeURIComponent(seg) === seg ? seg : seg.startsWith('%') ? seg : encodeURIComponent(seg));
        for (const q of method === 'GET' ? QUERIES : ['']) {
          const r = o.admin ? await s.raw(method, path + q, undefined, ADMIN) : await s.call(method, path + q, undefined, o.auth ? a.token : undefined);
          expect(r.status, `${method} ${path}${q}`).toBeLessThan(500);
        }
      }
    }
  });

  it('every rate limit really answers 429 with Retry-After once its bucket is empty', async () => {
    for (const [method, pattern, o] of makeSmg().app.routeOpts) {
      if (o.admin || o.register || !o.rl || (method === 'DELETE' && pattern === '/v1/me')) continue;
      const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
      const a = await smgSignUp(s, 'Ana');
      const path = fill(pattern, 'ABCDEFGH');
      const cap = o.rl[1];
      let hit = null;
      for (let i = 0; i < cap + 1 && !hit; i++) {
        const r = await s.call(method, path, method === 'GET' ? undefined : '{}', o.auth ? a.token : undefined, { ip: '203.0.113.7' });
        if (r.status === 429) hit = r;
      }
      expect(hit, `${method} ${pattern} (${cap} per ${o.rl[2]} s)`).not.toBeNull();
      expect(hit.headers.get('retry-after'), `${method} ${pattern}`).toMatch(/^\d+$/);
    }
  });

  it('every switchable route answers 503 service_disabled while its switch is on (and "all" stops them all)', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    for (const sw of ['rooms', 'turn', 'friends', 'quick', 'times', 'report', 'all']) {
      expect((await s.raw('POST', '/v1/admin/switches', { kill: [sw] }, ADMIN)).status).toBe(200);
      for (const [method, pattern, o] of s.app.routeOpts) {
        if (!o.auth || !o.kill || (sw !== 'all' && o.kill !== sw)) continue;
        const r = await s.call(method, fill(pattern, 'ABCDEFGH'), method === 'GET' ? undefined : '{}', a.token);
        expect(r.status, `${method} ${pattern} while ${sw} is off`).toBe(503);
        expect(r.data.error.code).toBe('service_disabled');
      }
      expect((await s.raw('GET', '/v1/public/times?galaxy=1&star=1')).status).toBe(sw === 'times' || sw === 'all' ? 503 : 200);
    }
    expect((await s.raw('POST', '/v1/admin/switches', { kill: [] }, ADMIN)).status).toBe(200);
  });
});
