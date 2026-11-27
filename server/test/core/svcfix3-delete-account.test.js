// svcfix3: a player deletes their own online account from the game: DELETE /v1/me removes everything that names the account.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, friends, roomOf } from '../smg-helpers.js';

const PID_COLS = ['pid', 'friend_pid', 'blocked_pid', 'lo_pid', 'hi_pid'];
// tables that may still mention a deleted pid: counters (keys), rooms (a closed room's host_pid, swept in minutes), matches (not used by Galaxy)
const KEEP = new Set(['rooms', 'matches', 'counters', 'sqlite_sequence']);
function refs(s, pid) {
  const out = [];
  const tables = s.store.sql.exec(`SELECT name FROM sqlite_master WHERE type = 'table'`).toArray().map((r) => r.name);
  for (const t of tables) {
    if (KEEP.has(t) || t.startsWith('_')) continue;
    const cols = s.store.sql.exec(`PRAGMA table_info(${t})`).toArray().map((r) => r.name).filter((n) => PID_COLS.includes(n));
    for (const col of cols) {
      const n = s.store.sql.exec(`SELECT COUNT(*) AS n FROM ${t} WHERE ${col} = ?`, pid).toArray()[0].n;
      if (n) out.push(`${t}.${col}=${n}`);
    }
  }
  return out;
}

describe('DELETE /v1/me', () => {
  it('removes the account, its friend code, both sides of its friend edges, pending requests, blocks, times, room membership and invites', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Leaver');
    const pal = await smgSignUp(s, 'Pal');
    const asked = await smgSignUp(s, 'Asked');     // Leaver sent Asked a request: a pending (one-way) edge
    const asker = await smgSignUp(s, 'Asker');     // Asker sent Leaver a request
    const blocked = await smgSignUp(s, 'Blocked');
    await friends(s, a, pal);
    await s.call('POST', '/v1/friends', { code: asked.code }, a.token);
    await s.call('POST', '/v1/friends', { code: a.code }, asker.token);
    expect((await s.call('POST', '/v1/friends/block', { code: blocked.code }, a.token)).status).toBeLessThan(500);
    const code = await roomOf(s, [pal, a]);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token); // leaves a room_left row
    const code2 = await roomOf(s, [a, pal]);                              // hosts a room that closes
    s.store.sql.exec(`INSERT INTO invites (code, pid, blob, app, created_ms, expires_ms) VALUES ('ZZZZ9', ?, 'x', '', ?, ?)`, a.pid, s.clock.now, s.clock.now + 600_000);
    expect(refs(s, a.pid).length).toBeGreaterThan(0);

    const del = await s.call('DELETE', '/v1/me', undefined, a.token);
    expect(del.status).toBe(200);
    expect(del.data).toEqual({ ok: true });
    expect(refs(s, a.pid)).toEqual([]);
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM profiles WHERE code = ?`, a.code.replace(/-/g, '')).toArray()[0].n).toBe(0);
    // the room it hosted goes on with the remaining member as host (it closes only when the last member leaves)
    const rr = s.store.sql.exec(`SELECT state, host_pid FROM rooms WHERE code = ?`, code2).toArray()[0];
    expect(rr.host_pid).toBe(pal.pid);
    // the others lost nothing but the edges to the deleted player
    expect((await s.call('GET', '/v1/me', undefined, pal.token)).status).toBe(200);
    const list = (await s.call('GET', '/v1/friends', undefined, pal.token)).data;
    expect(JSON.stringify(list)).not.toContain('Leaver');
    // the friend code is gone for everyone: a lookup is a 404
    expect((await s.call('GET', `/v1/players/${a.code}`, undefined, pal.token)).status).toBe(404);
  });

  it('is idempotent: the token no longer exists, so a second delete (or any call) is 401 unauthorized = "already gone"', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Leaver');
    expect((await s.call('DELETE', '/v1/me', undefined, a.token)).status).toBe(200);
    const again = await s.call('DELETE', '/v1/me', undefined, a.token);
    expect(again.status).toBe(401);
    expect(again.data.error.code).toBe('unauthorized');
  });

  it('takes no body (400 bad_request)', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Leaver');
    expect((await s.call('DELETE', '/v1/me', { x: 1 }, a.token)).status).toBe(400);
  });

  it('the player can sign up again afterwards with the same name, as a new account with a new friend code', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Leaver');
    await s.call('DELETE', '/v1/me', undefined, a.token);
    const b = await smgSignUp(s, 'Leaver');
    expect(b.pid).not.toBe(a.pid);
    expect(b.code).not.toBe(a.code);
  });
});
