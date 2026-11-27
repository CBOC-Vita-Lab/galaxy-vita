// Online audit (1.3): accounts that never played were kept forever, so MAX_PLAYERS filled up (server_full for everybody) and names stayed
// taken. The sweep now removes accounts that never played (no match ever started, no score, no ban) after PRUNE_IDLE_DAYS (default 180).
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, pairInRoom, startMatch } from '../helpers.js';

const DAY = 86400_000;
const adm = { authorization: 'Bearer test-admin-token-0123456789' };

describe('idle account pruning', () => {
  it('removes never-played idle accounts and frees their names; keeps players, bans and recent sign-ups', async () => {
    const s = makeServer({ MAX_PLAYERS: '4' });
    const idle = await signUp(s, 'Idler');
    const a = await signUp(s, 'Player1');
    const b = await signUp(s, 'Player2');
    const banned = await signUp(s, 'Banned1');
    expect((await s.call('POST', '/v1/admin/ban', { pid: banned.pid, reason: 'test' }, undefined, adm)).status).toBe(200);
    expect((await s.call('POST', '/v1/register', { name: 'Late1' })).data.error.code).toBe('server_full');
    const { code, matchId } = await pairInRoom(s, a, b);
    await startMatch(s, a, b, code);
    expect(matchId).toBeTruthy();

    s.advance(179 * DAY);
    s.app.sweep();
    expect((await s.call('GET', '/v1/me', undefined, idle.token)).status).toBe(200); // seen just now: the clock restarts
    s.advance(181 * DAY);
    s.app.sweep();
    expect((await s.call('GET', '/v1/me', undefined, idle.token)).status).toBe(401); // gone
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200); // has a match record
    expect((await s.call('GET', '/v1/me', undefined, b.token)).status).toBe(200);
    const banRow = s.store.sql.exec('SELECT banned FROM profiles WHERE pid = ?', banned.pid).toArray();
    expect(banRow).toEqual([{ banned: 1 }]); // a ban must stay
    // the slot and the name are free again: the console that held the old token registers again with its stored name
    const again = await s.call('POST', '/v1/register', { name: 'Idler' });
    expect(again.status).toBe(200);
  });

  it('a recent sign-up stays and PRUNE_IDLE_DAYS=0 turns it off', async () => {
    const s = makeServer({ PRUNE_IDLE_DAYS: '0' });
    const x = await signUp(s, 'Keeper1');
    s.advance(400 * DAY);
    s.app.sweep();
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM profiles WHERE pid = ?', x.pid).toArray()[0].n).toBe(1);

    const t = makeServer();
    const y = await signUp(t, 'Fresh1');
    t.advance(170 * DAY);
    t.app.sweep();
    expect((await t.call('GET', '/v1/me', undefined, y.token)).status).toBe(200);
  });

  it('a player with a score but no match record left (records expire after 90 days) stays', async () => {
    const s = makeServer();
    const p = await signUp(s, 'Scorer1');
    s.store.sql.exec(`INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (0, ?, 2, 5, '', ?)`, p.pid, s.clock.now);
    s.advance(100 * DAY);
    s.app.sweep();
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM profiles WHERE pid = ?', p.pid).toArray()[0].n).toBe(1);
  });
});
