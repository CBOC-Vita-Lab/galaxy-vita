// GAME=smg friends (mutual only), presence and join-by-friend.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, friends } from '../smg-helpers.js';

describe('smg friends', () => {
  it('add by code both ways makes a mutual friendship; one side only shows nothing but the name', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const add = await s.call('POST', '/v1/friends', { code: b.code }, a.token);
    expect(add.data).toEqual({ ok: true, added: true, friend: { code: b.code, name: 'Ben', mutual: false } });
    expect((await s.call('POST', '/v1/friends', { code: b.code.replace('-', '').toLowerCase() }, a.token)).data.added).toBe(false); // idempotent
    let la = await s.call('GET', '/v1/friends', undefined, a.token);
    expect(la.data.friends).toEqual([{ code: b.code, name: 'Ben', mutual: false, online: false, room: null }]);
    const lb = await s.call('GET', '/v1/friends', undefined, b.token);
    expect(lb.data.friends).toEqual([]);
    expect(lb.data.incoming).toEqual([{ code: a.code, name: 'Ana' }]);
    await s.call('POST', '/v1/friends', { code: a.code }, b.token);
    la = await s.call('GET', '/v1/friends', undefined, a.token);
    expect(la.data.friends[0]).toEqual({ code: b.code, name: 'Ben', mutual: true, online: true, room: null });
    expect(la.data).toMatchObject({ max: 50, onlineSec: 60, incoming: [] });
  });

  it('online = an authenticated call in the last 60 s; room shown for friends/public rooms only', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    await friends(s, a, b);
    s.advance(61_000);
    let la = await s.call('GET', '/v1/friends', undefined, a.token);
    expect(la.data.friends[0].online).toBe(false);
    const code = (await s.call('POST', '/v1/rooms', { mode: 'race' }, b.token)).data.code; // code only: hidden
    la = await s.call('GET', '/v1/friends', undefined, a.token);
    expect(la.data.friends[0]).toMatchObject({ online: true, room: null });
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, b.token);
    const fr = (await s.call('POST', '/v1/rooms', { mode: 'race', visibility: 'friends' }, b.token)).data.code;
    la = await s.call('GET', '/v1/friends', undefined, a.token);
    expect(la.data.friends[0].room).toEqual({ code: fr, joinable: true, mode: 'race' });
    const j = await s.call('POST', '/v1/rooms/join', { friend: b.code }, a.token);
    expect(j.data).toMatchObject({ code: fr, slot: 1 });
  });

  it('join by friend needs a mutual friendship and a friends/public room', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    await s.call('POST', '/v1/rooms', { visibility: 'public' }, b.token);
    await s.call('POST', '/v1/friends', { code: b.code }, a.token);
    expect((await s.call('POST', '/v1/rooms/join', { friend: b.code }, a.token)).data.error.code).toBe('not_friends');
    await s.call('POST', '/v1/friends', { code: a.code }, b.token);
    expect((await s.call('POST', '/v1/rooms/join', { friend: b.code }, a.token)).status).toBe(200);
    const c = await smgSignUp(s, 'Cy');
    await friends(s, c, b);
    await s.call('POST', '/v1/rooms', {}, b.token); // b now hosts a code-only room
    expect((await s.call('POST', '/v1/rooms/join', { friend: b.code }, c.token)).data.error.code).toBe('room_not_found');
    expect((await s.call('POST', '/v1/rooms/join', { friend: 'ZZZZ-ZZZZ' }, c.token)).data.error.code).toBe('player_not_found');
  });

  it('remove (POST and DELETE), own code, unknown code, the 50-friend cap', async () => {
    const s = makeSmg({ SMG_FRIENDS_MAX: '2' });
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const c = await smgSignUp(s, 'Cy');
    const d = await smgSignUp(s, 'Dee');
    expect((await s.call('POST', '/v1/friends', { code: a.code }, a.token)).data.error.code).toBe('own_code');
    expect((await s.call('POST', '/v1/friends', { code: 'ZZZZ-ZZZZ' }, a.token)).data.error.code).toBe('player_not_found');
    expect((await s.call('POST', '/v1/friends', { code: 'nope' }, a.token)).data.error.code).toBe('bad_code');
    await s.call('POST', '/v1/friends', { code: b.code }, a.token);
    await s.call('POST', '/v1/friends', { code: c.code }, a.token);
    expect((await s.call('POST', '/v1/friends', { code: d.code }, a.token)).data.error.code).toBe('friends_full');
    expect((await s.call('POST', '/v1/friends/remove', { code: b.code }, a.token)).data).toEqual({ ok: true, removed: true });
    expect((await s.call('DELETE', `/v1/friends/${c.code}`, undefined, a.token)).data).toEqual({ ok: true, removed: true });
    expect((await s.call('DELETE', `/v1/friends/${c.code}`, undefined, a.token)).data.removed).toBe(false);
    expect((await s.call('GET', '/v1/friends', undefined, a.token)).data.friends).toEqual([]);
  });

  it('rate buckets: 20 adds an hour, 6 presence polls a minute', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    for (let i = 0; i < 20; i++) expect((await s.call('POST', '/v1/friends', { code: b.code }, a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/friends', { code: b.code }, a.token)).status).toBe(429);
    for (let i = 0; i < 6; i++) expect((await s.call('GET', '/v1/friends', undefined, a.token)).status).toBe(200);
    const no = await s.call('GET', '/v1/friends', undefined, a.token);
    expect(no.status).toBe(429);
    expect(no.data.error.retryAfterSec).toBeGreaterThan(0);
  });

  it('deleting an account removes its friendships both ways', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    await friends(s, a, b);
    await s.call('DELETE', '/v1/me', undefined, b.token);
    expect((await s.call('GET', '/v1/friends', undefined, a.token)).data).toMatchObject({ friends: [], incoming: [] });
  });
});
