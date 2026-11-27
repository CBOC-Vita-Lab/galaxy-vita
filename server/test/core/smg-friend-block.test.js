// GAME=smg: dismiss an incoming friend request, block / unblock a player.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, friends } from '../smg-helpers.js';

async function trio() {
  const s = makeSmg();
  return { s, a: await smgSignUp(s, 'Ana'), b: await smgSignUp(s, 'Ben'), c: await smgSignUp(s, 'Cy') };
}
const list = (s, p) => s.call('GET', '/v1/friends', undefined, p.token);

describe('smg friends: dismiss', () => {
  it('removes a pending request from incoming, idempotently; they may ask again', async () => {
    const { s, a, b } = await trio();
    await s.call('POST', '/v1/friends', { code: a.code }, b.token);
    expect((await list(s, a)).data.incoming).toEqual([{ code: b.code, name: 'Ben' }]);
    expect((await s.call('POST', '/v1/friends/dismiss', { code: b.code }, a.token)).data).toEqual({ ok: true, dismissed: true });
    expect((await list(s, a)).data.incoming).toEqual([]);
    expect((await s.call('POST', '/v1/friends/dismiss', { code: b.code }, a.token)).data).toEqual({ ok: true, dismissed: false });
    expect((await list(s, b)).data.friends).toEqual([]); // their request is gone, they see nothing about it
    expect((await s.call('POST', '/v1/friends', { code: a.code }, b.token)).data.added).toBe(true);
    expect((await list(s, a)).data.incoming.length).toBe(1);
  });

  it('does not touch a mutual friend; validates the code', async () => {
    const { s, a, b } = await trio();
    await friends(s, a, b);
    expect((await s.call('POST', '/v1/friends/dismiss', { code: b.code }, a.token)).data.dismissed).toBe(false);
    expect((await list(s, a)).data.friends[0].mutual).toBe(true);
    expect((await s.call('POST', '/v1/friends/dismiss', { code: a.code }, a.token)).data.error.code).toBe('own_code');
    expect((await s.call('POST', '/v1/friends/dismiss', { code: 'ZZZZ-ZZZZ' }, a.token)).data.error.code).toBe('player_not_found');
    expect((await s.call('POST', '/v1/friends/dismiss', { code: 5 }, a.token)).status).toBe(400);
    expect((await s.call('POST', '/v1/friends/dismiss', { code: b.code, x: 1 }, a.token)).data.error.code).toBe('unknown_field');
    expect((await s.call('POST', '/v1/friends/dismiss', undefined, a.token)).status).toBe(400);
    expect((await s.call('POST', '/v1/friends/dismiss', { code: b.code }, undefined)).status).toBe(401);
  });
});

describe('smg friends: block', () => {
  it('ends the friendship both ways, drops further requests silently and shares no presence', async () => {
    const { s, a, b } = await trio();
    await friends(s, a, b);
    expect((await s.call('POST', '/v1/friends/block', { code: b.code }, a.token)).data).toEqual({ ok: true, blocked: true });
    expect((await s.call('POST', '/v1/friends/block', { code: b.code }, a.token)).data.blocked).toBe(false); // idempotent
    let la = await list(s, a);
    expect(la.data.friends).toEqual([]);
    expect(la.data.incoming).toEqual([]);
    expect(la.data.blocked).toEqual([{ code: b.code, name: 'Ben' }]);
    expect((await list(s, b)).data.friends).toEqual([]); // her row of him is gone too
    // Ben asks again: the answer looks like any pending request, nothing is stored, Ana sees nothing
    const again = await s.call('POST', '/v1/friends', { code: a.code }, b.token);
    expect(again.data).toEqual({ ok: true, added: true, friend: { code: a.code, name: 'Ana', mutual: false } });
    expect((await list(s, b)).data.friends).toEqual([]);
    la = await list(s, a);
    expect(la.data.incoming).toEqual([]);
    // Ana cannot add him without unblocking
    expect((await s.call('POST', '/v1/friends', { code: b.code }, a.token)).data.error.code).toBe('blocked');
    // no presence, no join-by-friend, even though a room exists
    await s.call('POST', '/v1/rooms', { visibility: 'friends' }, b.token);
    expect((await s.call('POST', '/v1/rooms/join', { friend: b.code }, a.token)).data.error.code).toBe('not_friends');
  });

  it('unblock lets requests through again (and does not restore the old friendship)', async () => {
    const { s, a, b } = await trio();
    await s.call('POST', '/v1/friends/block', { code: b.code }, a.token);
    expect((await s.call('POST', '/v1/friends/unblock', { code: b.code }, a.token)).data).toEqual({ ok: true, unblocked: true });
    expect((await s.call('POST', '/v1/friends/unblock', { code: b.code }, a.token)).data.unblocked).toBe(false);
    expect((await list(s, a)).data.blocked).toEqual([]);
    await s.call('POST', '/v1/friends', { code: a.code }, b.token);
    expect((await list(s, a)).data.incoming).toEqual([{ code: b.code, name: 'Ben' }]);
  });

  it('validates, caps the list, follows the friends kill switch and account deletion', async () => {
    const s = makeSmg({ SMG_BLOCKS_MAX: '2' });
    const [a, b, c, d] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben'), await smgSignUp(s, 'Cy'), await smgSignUp(s, 'Di')];
    expect((await s.call('POST', '/v1/friends/block', { code: a.code }, a.token)).data.error.code).toBe('own_code');
    expect((await s.call('POST', '/v1/friends/block', { code: 'ZZZZ-ZZZZ' }, a.token)).data.error.code).toBe('player_not_found');
    expect((await s.call('POST', '/v1/friends/block', { code: b.code }, a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/friends/block', { code: c.code }, a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/friends/block', { code: d.code }, a.token)).data.error.code).toBe('blocks_full');
    expect((await s.call('POST', '/v1/friends/block', { code: b.code }, a.token)).status).toBe(200); // already blocked: still fine at the cap
    await s.raw('POST', '/v1/admin/switches', { kill: ['friends'] }, 'test-admin-token-0123456789');
    expect((await s.call('POST', '/v1/friends/block', { code: d.code }, a.token)).data.error.code).toBe('service_disabled');
    expect((await s.call('POST', '/v1/friends/dismiss', { code: d.code }, a.token)).data.error.code).toBe('service_disabled');
  });

  it('is rate limited per player and removed with the account', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    await s.call('POST', '/v1/friends/block', { code: b.code }, a.token);
    let last;
    for (let i = 0; i < 40; i++) last = await s.call('POST', '/v1/friends/unblock', { code: b.code }, a.token);
    expect(last.status).toBe(429);
    const t = makeSmg();
    const x = await smgSignUp(t, 'Xe');
    const y = await smgSignUp(t, 'Yo');
    await t.call('POST', '/v1/friends/block', { code: y.code }, x.token);
    await t.call('DELETE', '/v1/me', undefined, y.token);
    expect(t.store.sql.exec('SELECT COUNT(*) AS n FROM friend_blocks').toArray()[0].n).toBe(0);
  });
});
