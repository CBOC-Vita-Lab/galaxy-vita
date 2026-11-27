// The same service, end to end inside the Workers runtime (workerd via miniflare): Worker -> Durable Object -> SQLite.
import { describe, it, expect } from 'vitest';
import { env, exports } from 'cloudflare:workers';
import { runDurableObjectAlarm, listDurableObjectIds } from 'cloudflare:test';

const BASE = 'https://svc.example.invalid';
const T0 = Date.UTC(2026, 9, 30, 12, 0, 0);
let ipn = 1;

async function call(method, path, body, token, extra = {}) {
  const headers = { 'x-test-time': String(T0 + (extra.dt || 0)), 'x-test-ip': extra.ip || `198.51.100.${(ipn++ % 250) + 1}` };
  if (token) headers.authorization = `Bearer ${token}`;
  if (extra.admin) headers.authorization = `Bearer ${env.ADMIN_TOKEN}`;
  const init = { method, headers };
  if (body !== undefined) {
    init.body = JSON.stringify(body);
    headers['content-type'] = 'application/json';
  }
  const resp = await exports.default.fetch(new Request(`${BASE}${path}`, init));
  const text = await resp.text();
  let data;
  try { data = JSON.parse(text); } catch { data = text; }
  return { status: resp.status, data, headers: resp.headers };
}

// Durable Object storage is shared by all tests in this file, so every test registers fresh names.
let seq = 0;
const uniq = (base) => `${base.slice(0, 6)}${String(++seq).padStart(3, '0')}`.slice(0, 10);
const signUp = async (name) => {
  const r = await call('POST', '/v1/register', { name: uniq(name) });
  if (r.status !== 200) throw new Error(JSON.stringify(r.data));
  return r.data;
};
const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAECAwQFBgc=' });

function hexToBytes(h) { return Uint8Array.from(h.match(/../g).map((x) => parseInt(x, 16))); }

// A tiny HMAC-MD5 through the server's own helper would be circular, so build the blob with WebCrypto-free JS here.
import { hmacMd5 } from '../../src/core/md5.js';
function blobFor(name) {
  const b = new Uint8Array(0x80);
  const dv = new DataView(b.buffer);
  b[0] = 30; b[1] = 10; dv.setUint16(2, 2026, true);
  for (let i = 0; i < name.length; i++) dv.setUint16(0x0e + i * 2, name.charCodeAt(i), true);
  b.set(hmacMd5(hexToBytes(env.RANK_HMAC_KEY_HEX), b.subarray(0, 0x70)), 0x70);
  return btoa(String.fromCharCode(...b));
}

describe('worker + durable object', () => {
  it('rejects paths outside /v1 at the edge, serves info and time from the object', async () => {
    const r = await exports.default.fetch(new Request(`${BASE}/`));
    expect(r.status).toBe(404);
    const i = await call('GET', '/v1/info');
    expect(i.status).toBe(200);
    expect(i.data.proto).toBe(1);
    const t = await call('GET', '/v1/time');
    expect(t.data).toMatchObject({ utcMs: T0, year: 2026, month: 10 });
  });

  it('sign-in persists in the object between requests', async () => {
    const a = await signUp('Alpha');
    expect(a.pid).toBeGreaterThan(100000);
    expect((await call('GET', '/v1/me', undefined, a.token)).data.name).toBe(a.name);
    expect((await call('POST', '/v1/register', { name: a.name.toLowerCase() })).data.error.code).toBe('name_taken');
    expect((await call('GET', '/v1/me', undefined, 'z'.repeat(43))).status).toBe(401);
  });

  it('invite room flow with unique vips and endpoint exchange', async () => {
    const a = await signUp('Hosty');
    const b = await signUp('Guesty');
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    expect(room.code).toMatch(/^[A-Z2-9]{6}$/);
    const j = await call('POST', '/v1/rooms/join', { code: room.code }, b.token);
    expect(j.status).toBe(200);
    await call('POST', `/v1/rooms/${room.code}/endpoint`, ep(1), a.token);
    const done = await call('POST', `/v1/rooms/${room.code}/endpoint`, ep(2), b.token);
    expect(done.data.state).toBe('ready');
    expect(done.data.peer.endpoint.public).toBe('203.0.113.1:41000');
    expect(done.data.you.vip).not.toBe(done.data.peer.vip);
    expect((await call('POST', `/v1/rooms/${room.code}/started`, undefined, a.token)).status).toBe(200);
    const st = await call('POST', `/v1/rooms/${room.code}/started`, undefined, b.token);
    expect(st.data.state).toBe('started');
  });

  it('ranked: queue, pair, agree on a result, rank rows come back with the opaque blob', async () => {
    const a = await signUp('Anna');
    const b = await signUp('Bert');
    await call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    expect(q.status).toBe('matched');
    for (const [p, n] of [[a, 1], [b, 2]]) await call('POST', `/v1/rooms/${q.roomCode}/endpoint`, ep(n), p.token);
    for (const p of [a, b]) await call('POST', `/v1/rooms/${q.roomCode}/started`, undefined, p.token);
    const res = { result: 0, score0: 3, score1: 1, checksum: 'abcdef0123456789' };
    await call('POST', `/v1/matches/${q.room.matchId}/result`, res, a.token);
    expect((await call('POST', `/v1/matches/${q.room.matchId}/result`, res, b.token)).data.status).toBe('verified');
    const blob = blobFor(a.name);
    const put = await call('POST', '/v1/rank/put', { category: 0, region: 2, score: 13, data: blob }, a.token);
    expect(put.data).toEqual({ ok: true, stored: 13, clamped: false });
    const near = await call('POST', '/v1/rank/get', { mode: 2, category: 0, region: 2, sort: 1, since: 1, limit: 5 }, b.token);
    expect(near.data.rows.find((r) => r.pid === a.pid).score).toBe(13);
    expect(near.data.rows.find((r) => r.pid === b.pid).score).toBe(0);
    expect(near.data.rows[0].data).toBe(blob);
    expect(near.data.rows.every((r) => r.order >= 1)).toBe(true);
  });

  it('time-dependent behaviour works in the runtime: room expiry via the test clock', async () => {
    const a = await signUp('Expiry');
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    expect((await call('GET', `/v1/rooms/${room.code}`, undefined, a.token, { dt: 60_000 })).data.state).toBe('open');
    expect((await call('GET', `/v1/rooms/${room.code}`, undefined, a.token, { dt: 11 * 60_000 })).data.state).toBe('expired');
  });

  it('admin: stats, export pages and kill switch through the real object', async () => {
    const who = await signUp('Alpha');
    expect((await call('GET', '/v1/admin/stats')).status).toBe(401);
    const st = await call('GET', '/v1/admin/stats', undefined, undefined, { admin: true });
    expect(st.data.profiles).toBeGreaterThanOrEqual(1);
    const ex = await call('GET', '/v1/admin/export', undefined, undefined, { admin: true });
    // migration 4 (Galaxy tables, additive; Strikers rows untouched) runs in every deployment of this code
    expect(ex.data.schema).toEqual([{ id: 1, name: 'initial' }, { id: 2, name: 'abuse_controls' }, { id: 3, name: 'status_that_lasts' },
      { id: 4, name: 'galaxy_rooms_friends_times' }, { id: 5, name: 'galaxy_friend_blocks' }, { id: 6, name: 'galaxy_hardening' },
      { id: 7, name: 'galaxy_service_review' }, { id: 8, name: 'galaxy_board_index' }, { id: 9, name: 'galaxy_room_peak' }, { id: 10, name: 'galaxy_join_accept' }, { id: 11, name: 'galaxy_invites' }, { id: 12, name: 'turn_pass_usernames' }, { id: 13, name: 'addr_day_counters' }]);
    const rows = await call('GET', '/v1/admin/export?table=profiles&after=0&limit=1000', undefined, undefined, { admin: true });
    const row = rows.data.rows.find((r) => r.pid === who.pid);
    expect(row.name).toBe(who.name);
    expect(row.token_hash).toMatch(/^[0-9a-f]{64}$/);
    await call('POST', '/v1/admin/switches', { kill: ['register'] }, undefined, { admin: true });
    expect((await call('POST', '/v1/register', { name: 'Betaxx' })).data.error.code).toBe('service_disabled');
    await call('POST', '/v1/admin/switches', { kill: [] }, undefined, { admin: true });
  });

  it('the housekeeping alarm is scheduled by traffic and runs', async () => {
    await signUp('Alarm');
    const ids = await listDurableObjectIds(env.GAME);
    expect(ids.length).toBe(1);
    const stub = env.GAME.get(ids[0]);
    const ran = await runDurableObjectAlarm(stub);
    expect(ran).toBe(true);
  });

  it('turn credentials come from the stub provider configured for tests', async () => {
    const a = await signUp('Hosty');
    const b = await signUp('Guesty');
    const room = (await call('POST', '/v1/rooms', undefined, a.token)).data;
    await call('POST', '/v1/rooms/join', { code: room.code }, b.token);
    const t = await call('POST', '/v1/turn', { room: room.code }, a.token);
    expect(t.status).toBe(200);
    expect(t.data.servers.some((s) => s.username)).toBe(true);
  });
});
