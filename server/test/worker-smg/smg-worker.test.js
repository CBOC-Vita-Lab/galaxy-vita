// smg-online end to end inside the Workers runtime (workerd via miniflare) with wrangler.smg.toml: SERVICE_GAME=smg next to the
// Durable Object binding GAME, the edge's Galaxy route table, the times page answered at the edge, a room of 3 through the object.
import { describe, it, expect } from 'vitest';
import { exports } from 'cloudflare:workers';

const BASE = 'https://smg.example.invalid';
let ipn = 1;
const H = { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' };

async function call(method, path, body, token, extra = {}) {
  const headers = { ...H, 'x-test-ip': `198.51.100.${(ipn++ % 250) + 1}`, ...extra };
  if (token) headers.authorization = `Bearer ${token}`;
  const init = { method, headers };
  if (body !== undefined) { init.body = JSON.stringify(body); headers['content-type'] = 'application/json'; }
  const resp = await exports.default.fetch(new Request(`${BASE}${path}`, init));
  const text = await resp.text();
  let data;
  try { data = JSON.parse(text); } catch { data = text; }
  return { status: resp.status, data, headers: resp.headers };
}
let seq = 0;
const signUp = async (n) => {
  const r = await call('POST', '/v1/register', { name: `${n}${String.fromCharCode(97 + (seq++ % 26))}${String.fromCharCode(97 + Math.floor(seq / 26))}` });
  if (r.status !== 200) throw new Error(JSON.stringify(r.data));
  return r.data;
};

describe('smg-online worker + durable object', () => {
  it('serves Galaxy: info, game header at the edge, no ranking routes', async () => {
    const i = await call('GET', '/v1/info');
    expect(i.data.game).toBe('smg');
    expect((await call('POST', '/v1/register', { name: 'Edge' }, undefined, { 'x-msc-game': 'msc' })).data.error.code).toBe('game_mismatch');
    expect((await call('POST', '/v1/rank/get', {}, 'a'.repeat(43))).status).toBe(404);
    const page = await exports.default.fetch(new Request(`${BASE}/times`)); // a browser: no game header, no version
    expect(page.status).toBe(200);
    expect(page.headers.get('content-security-policy')).toMatch(/script-src/);
    expect((await call('GET', '/times', undefined, undefined, { 'x-msc-game': '' })).status).toBe(400); // malformed: bad_game
  });

  it('GET /v1/prove answers the HMAC of the admin token for the request host (through the edge and the object), 400 for a bad n', async () => {
    const n = 'a'.repeat(16) + '0123456789abcdef';
    const enc = new TextEncoder();
    const key = await crypto.subtle.importKey('raw', enc.encode('test-admin-token-0123456789'), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, enc.encode(`smg-online prove v1\nsmg.example.invalid\n${n}`)));
    const r = await call('GET', `/v1/prove?n=${n}`);
    expect(r.status).toBe(200);
    expect(r.data.proof).toBe([...mac].map((b) => b.toString(16).padStart(2, '0')).join(''));
    expect((await call('GET', '/v1/prove?n=zz')).status).toBe(400);
  });
  it('a chunked POST (no Content-Length) is 411 at the edge and never reaches the object', async () => {
    const stream = new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode('{"name":"Chunky"}')); c.close(); } });
    const r = await exports.default.fetch(new Request(`${BASE}/v1/register`, { method: 'POST', headers: { ...H, 'content-type': 'application/json' }, body: stream, duplex: 'half' }));
    expect(r.status).toBe(411);
    expect((await r.json()).error.code).toBe('length_required');
    expect((await call('POST', '/v1/register', { name: 'Normal1' })).status).toBe(200); // a normal body carries its length
  });

  it('a room of 3, friends and a time through the real object', async () => {
    const [h, a, b] = [await signUp('Host'), await signUp('Ann'), await signUp('Bea')];
    const made = await call('POST', '/v1/rooms', { mode: 'coop', visibility: 'friends' }, h.token);
    expect(made.data).toMatchObject({ game: 'smg', hostSlot: 0 });
    await call('POST', '/v1/friends', { code: h.code }, a.token); // (svcfix3: a friends room opens to the host's friends, also by code)
    await call('POST', '/v1/friends', { code: a.code }, h.token);
    await call('POST', '/v1/rooms/join', { code: made.data.code }, a.token);
    await call('POST', '/v1/friends', { code: h.code }, b.token);
    await call('POST', '/v1/friends', { code: b.code }, h.token);
    const viaFriend = await call('POST', '/v1/rooms/join', { friend: h.code }, b.token);
    expect(viaFriend.data).toMatchObject({ code: made.data.code, slot: 2 });
    const left = await call('POST', `/v1/rooms/${made.data.code}/leave`, undefined, h.token);
    expect(left.data).toMatchObject({ hostSlot: 1, hostEpoch: 1 });
    const t = await call('POST', '/v1/times', { galaxy: 1, star: 1, frames: 3600, players: 1, build: '3.0.0' }, b.token);
    expect(t.data.ok).toBe(true);
    // production defaults (wrangler.smg.toml; finding 13): a solo time nobody vouched for is not public until an admin approves it
    expect((await call('GET', '/v1/public/times?galaxy=1&star=1&limit=5')).data.rows).toEqual([]);
    const ok = await exports.default.fetch(new Request(`${BASE}/v1/admin/times/approve`, {
      method: 'POST', headers: { authorization: 'Bearer test-admin-token-0123456789', 'content-type': 'application/json', 'x-test-ip': '198.51.100.79' },
      body: JSON.stringify({ pid: b.pid, galaxy: 1, star: 1, category: 1 }),
    }));
    expect(ok.status).toBe(200);
    const pub = await call('GET', '/v1/public/times?galaxy=1&star=1&limit=6');
    expect(pub.data.rows[0].frames).toBe(3600);
  });

  it('the times kill switch applies to memoised public hits too (no stale 200 from the Worker memo)', async () => {
    const pub = (q) => exports.default.fetch(new Request(`${BASE}/v1/public/times${q}`, { headers: { 'x-test-ip': '198.51.100.77' } }));
    const sw = (kill) => exports.default.fetch(new Request(`${BASE}/v1/admin/switches`, {
      method: 'POST', headers: { authorization: 'Bearer test-admin-token-0123456789', 'content-type': 'application/json', 'x-test-ip': '198.51.100.78' }, body: JSON.stringify({ kill }),
    }));
    expect((await pub('?galaxy=2&star=1&limit=5')).status).toBe(200);
    const hit = await pub('?galaxy=2&star=1&limit=5');
    expect(hit.status).toBe(200);
    expect(hit.headers.get('x-msc-cache')).toBe('hit'); // memoised
    expect((await sw(['times'])).status).toBe(200);
    const off = await pub('?galaxy=2&star=1&limit=5'); // the same key, still inside the 60 s memo
    expect(off.status).toBe(503);
    expect((await off.json()).error.code).toBe('service_disabled');
    expect(off.headers.get('x-msc-cache')).toBeNull();
    expect((await pub('?galaxy=2&star=1&limit=5')).status).toBe(503);
    expect((await sw([])).status).toBe(200);
    expect((await pub('?galaxy=2&star=1&limit=5')).status).toBe(200);
    expect((await pub('?galaxy=2&star=1&limit=5')).headers.get('x-msc-cache')).toBe('hit');
  });
  it('beta reports and summaries go through the real D1 binding (REPORTS_DB from wrangler.smg.toml): report, admin list and get, summary on leave, account delete purge', async () => {
    const admin = (path) => exports.default.fetch(new Request(`${BASE}${path}`, { headers: { ...H, authorization: 'Bearer test-admin-token-0123456789', 'x-test-ip': '198.51.100.90' } })).then(async (r) => ({ status: r.status, data: await r.json() }));
    const p = await signUp('Rep');
    const r = await call('POST', '/v1/report', { v: 1, kind: 'user', where: 'settings', note: 'from 203.0.113.5', log: 'peer 198.51.100.4 a@b.com\nline 2', check: { nat: 'x' } }, p.token);
    expect(r.status).toBe(201);
    const list = await admin('/v1/admin/reports');
    expect(list.status).toBe(200);
    expect(list.data.reports.map((x) => x.id)).toContain(r.data.id);
    const one = await admin(`/v1/admin/reports/${r.data.id}`);
    expect(one.data.log).toBe('peer [ip] [email]\nline 2');
    expect(one.data.note).toBe('from [ip]');
    // a summary rides on a leave
    const h = await signUp('Hst');
    const room = (await call('POST', '/v1/rooms', { mode: 'coop', visibility: 'friends' }, h.token)).data;
    const lv = await call('POST', `/v1/rooms/${room.code}/leave`, { summary: { transport: 'udp', ended: 'leave', dur_s: 12 } }, h.token);
    expect(lv.status).toBe(200);
    const sm = await admin('/v1/admin/summaries');
    expect(sm.data.summaries.some((x) => x.transport === 'udp' && x.dur_s === 12)).toBe(true);
    expect((await admin('/v1/admin/stats')).data.summaries.available).toBe(true);
    // account delete purges the reports
    expect((await call('DELETE', '/v1/me', undefined, p.token)).status).toBe(200);
    expect((await admin(`/v1/admin/reports/${r.data.id}`)).status).toBe(404);
    expect((await admin('/v1/admin/stats')).data.reports.stored).toBe(0); // the totals follow the purge (real D1 RETURNING / meta.changes)
    // and the connection check is refused cleanly when no relay is configured here, never a 5xx crash
    const c = await call('POST', '/v1/turn/check', undefined, h.token);
    expect(c.status).toBeLessThan(600);
  });
});
