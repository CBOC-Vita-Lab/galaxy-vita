// The public leaderboard through the real Worker + Durable Object: the page never wakes the object, and the board is held
// in the Worker for 60 s so repeated reads do not reach it.
import { describe, it, expect } from 'vitest';
import { env, exports } from 'cloudflare:workers';
import { listDurableObjectIds, runInDurableObject } from 'cloudflare:test';
import { hmacMd5 } from '../../src/core/md5.js';

const BASE = 'https://svc.example.invalid';
const f = (path, headers = {}) => exports.default.fetch(new Request(`${BASE}${path}`, { headers: { 'x-test-ip': '198.51.100.9', ...headers } }));

function blobFor(name, wins, losses) {
  const key = Uint8Array.from(env.RANK_HMAC_KEY_HEX.match(/../g).map((x) => parseInt(x, 16)));
  const b = new Uint8Array(0x80);
  const dv = new DataView(b.buffer);
  b[0] = 30; b[1] = 10; dv.setUint16(2, 2026, true); dv.setInt32(4, wins, true); dv.setInt32(8, losses, true); dv.setUint16(0x0c, 2, true);
  for (let i = 0; i < name.length; i++) dv.setUint16(0x0e + i * 2, name.charCodeAt(i), true);
  b.set(hmacMd5(key, b.subarray(0, 0x70)), 0x70);
  return btoa(String.fromCharCode(...b));
}

describe('worker: /leaderboard and /v1/public/leaderboard', () => {
  it('serves the page with its CSP from the Worker without creating or waking the object', async () => {
    expect((await listDurableObjectIds(env.GAME)).length).toBe(0);
    const r = await f('/leaderboard');
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toBe('text/html; charset=utf-8');
    expect(r.headers.get('content-security-policy')).toMatch(/^default-src 'none'; script-src 'sha256-[A-Za-z0-9+/=]+'; style-src 'sha256-[A-Za-z0-9+/=]+'; connect-src 'self'/);
    expect(r.headers.get('cache-control')).toBe('public, max-age=300');
    expect(await r.text()).toContain('SEASON LEADERS');
    expect((await f('/leaderboard', { 'x-msc-proto': 'abc' })).status).toBe(400);
    expect((await exports.default.fetch(new Request(`${BASE}/leaderboard`, { method: 'POST', body: '{}' }))).status).toBe(405);
    expect((await listDurableObjectIds(env.GAME)).length).toBe(0);
  });

  it('serves the board from the object, then from the Worker memo for 60 s; bad queries are never memoised', async () => {
    // wake the object and seed two players straight into storage (the credit rule would clamp real puts)
    const t = Date.now();
    expect((await f('/v1/time')).status).toBe(200);
    const [id] = await listDurableObjectIds(env.GAME);
    await runInDurableObject(env.GAME.get(id), (_inst, state) => {
      const sql = state.storage.sql;
      const mk = (pid, name, score, w, l) => {
        sql.exec(`INSERT INTO profiles (pid, code, name, name_key, token_hash, created_ms, last_seen_ms) VALUES (?,?,?,?,?,?,?)`, pid, `CODE${pid}`, name, name.toLowerCase(), `hash${pid}`, t, t);
        sql.exec(`INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (0, ?, 2, ?, ?, ?)`, pid, score, blobFor(name, w, l), t);
      };
      mk(100501, 'Sam', 120, 12, 3);
      mk(100502, 'Alex', 120, 9, 4);
      mk(100503, 'Robin', 80, 5, 5);
    });
    const a = await f('/v1/public/leaderboard?board=season&limit=7'); // a limit no other test uses: a fresh memo entry
    expect(a.status).toBe(200);
    expect(a.headers.get('x-msc-cache')).toBeNull();
    const body = await a.json();
    expect(body.rows).toEqual([
      { rank: 1, name: 'Sam', wins: 12, losses: 3, points: 120, lastPlayedMs: null },
      { rank: 1, name: 'Alex', wins: 9, losses: 4, points: 120, lastPlayedMs: null },
      { rank: 3, name: 'Robin', wins: 5, losses: 5, points: 80, lastPlayedMs: null },
    ]);
    expect(JSON.stringify(body)).not.toMatch(/CODE1005|hash1005|10050/);
    await runInDurableObject(env.GAME.get(id), (_inst, state) => { state.storage.sql.exec('DELETE FROM scores'); });
    const b = await f('/v1/public/leaderboard?limit=7&junk=1&board=season'); // same canonical key, different spelling
    expect(b.headers.get('x-msc-cache')).toBe('hit');
    expect((await b.json()).rows.length).toBe(3);
    const bad = await f('/v1/public/leaderboard?limit=0');
    expect(bad.status).toBe(400);
    expect((await f('/v1/public/leaderboard?limit=0')).headers.get('x-msc-cache')).toBeNull();
    const fresh = await f('/v1/public/leaderboard?limit=8'); // a different key reaches the object, which now has no rows
    expect((await fresh.json()).rows).toEqual([]);
  });

  it('the ranking kill switch reaches the Worker route too (a fresh key is refused by the object)', async () => {
    const admin = await exports.default.fetch(new Request(`${BASE}/v1/admin/switches`, {
      method: 'POST', headers: { authorization: `Bearer ${env.ADMIN_TOKEN}`, 'content-type': 'application/json', 'x-test-ip': '198.51.100.30' }, body: JSON.stringify({ kill: ['ranking'] }),
    }));
    expect(admin.status).toBe(200);
    const r = await f('/v1/public/leaderboard?limit=9');
    expect(r.status).toBe(503);
    expect((await r.json()).error.code).toBe('service_disabled');
    expect((await f('/leaderboard')).status).toBe(200);
    expect((await f('/v1/public/leaderboard?limit=9')).headers.get('x-msc-cache')).toBeNull(); // errors are not memoised
    await exports.default.fetch(new Request(`${BASE}/v1/admin/switches`, {
      method: 'POST', headers: { authorization: `Bearer ${env.ADMIN_TOKEN}`, 'content-type': 'application/json', 'x-test-ip': '198.51.100.30' }, body: JSON.stringify({ kill: [] }),
    }));
    expect((await f('/v1/public/leaderboard?limit=9')).status).toBe(200);
  });
});
