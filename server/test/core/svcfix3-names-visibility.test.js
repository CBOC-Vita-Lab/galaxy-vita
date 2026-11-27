// svcfix3 (review S10): duplicate display names are told apart in a room view, and a join by code respects the room's visibility.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, friends } from '../smg-helpers.js';

describe('S10: duplicate names', () => {
  it('two members both called Mario get a label with their friend-code tail and nameDup; a unique name keeps label == name', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Mario');
    const b = await smgSignUp(s, 'mario');
    const c = await smgSignUp(s, 'Luigi');
    const made = await s.call('POST', '/v1/rooms', {}, a.token);
    await s.call('POST', '/v1/rooms/join', { code: made.data.code }, b.token);
    await s.call('POST', '/v1/rooms/join', { code: made.data.code }, c.token);
    const v = (await s.call('GET', `/v1/rooms/${made.data.code}`, undefined, a.token)).data;
    const byName = Object.fromEntries(v.members.map((m) => [m.name, m]));
    expect(byName.Mario.nameDup).toBe(true);
    expect(byName.mario.nameDup).toBe(true);
    expect(byName.Mario.label).toBe(`Mario #${a.code.replace(/-/g, '').slice(-4)}`);
    expect(byName.mario.label).toBe(`mario #${b.code.replace(/-/g, '').slice(-4)}`);
    expect(byName.Mario.label).not.toBe(byName.mario.label);
    expect(byName.Luigi).toMatchObject({ nameDup: false, label: 'Luigi' });
  });
});

describe('S10: join by code and visibility', () => {
  const mk = async (visibility) => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const pal = await smgSignUp(s, 'Pal');
    const stranger = await smgSignUp(s, 'Stranger');
    await friends(s, host, pal);
    const made = await s.call('POST', '/v1/rooms', { visibility }, host.token);
    return { s, host, pal, stranger, code: made.data.code };
  };

  it('a friends room: a mutual friend joins by code, a stranger with the code gets 404 room_not_found', async () => {
    const { s, pal, stranger, code } = await mk('friends');
    const no = await s.call('POST', '/v1/rooms/join', { code }, stranger.token);
    expect(no.status).toBe(404);
    expect(no.data.error.code).toBe('room_not_found');
    expect((await s.call('POST', '/v1/rooms/join', { code }, pal.token)).status).toBe(200);
  });

  it('code and public rooms still take anyone with the code', async () => {
    for (const vis of ['code', 'public']) {
      const { s, stranger, code } = await mk(vis);
      expect((await s.call('POST', '/v1/rooms/join', { code }, stranger.token)).status).toBe(200);
    }
  });

  it('a member of a friends room who is no longer a friend can still rejoin idempotently; join by friend is unchanged', async () => {
    const { s, host, pal, code } = await mk('friends');
    await s.call('POST', '/v1/rooms/join', { code }, pal.token);
    expect((await s.call('POST', '/v1/rooms/join', { code }, pal.token)).status).toBe(200);
    const s2 = makeSmg();
    const h = await smgSignUp(s2, 'H2'); const p2 = await smgSignUp(s2, 'P2');
    await friends(s2, h, p2);
    await s2.call('POST', '/v1/rooms', { visibility: 'friends' }, h.token);
    expect((await s2.call('POST', '/v1/rooms/join', { friend: h.code }, p2.token)).status).toBe(200);
    void host;
  });
});

describe('S10 review fix: a former member comes back by code', () => {
  it('after a host change, a former member who is no friend of the NEW host rejoins a friends room by code; a stranger still gets 404', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const guest = await smgSignUp(s, 'Guest');   // becomes the host
    const y = await smgSignUp(s, 'Yan');         // friend of the old host only
    const stranger = await smgSignUp(s, 'Stranger');
    await friends(s, host, guest);
    await friends(s, host, y);
    const code = (await s.call('POST', '/v1/rooms', { visibility: 'friends' }, host.token)).data.code;
    await s.call('POST', '/v1/rooms/join', { code }, guest.token);
    await s.call('POST', '/v1/rooms/join', { code }, y.token);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, host.token); // host change: Guest hosts now
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, y.token);    // Yan leaves, no friend of Guest
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, guest.token)).data.role).toBe('host');
    expect((await s.call('POST', '/v1/rooms/join', { code }, stranger.token)).status).toBe(404);
    expect((await s.call('POST', '/v1/rooms/join', { code }, y.token)).status).toBe(200);
  });

  it('a member who came in through a guest\'s friend path rejoins by code although he is no friend of the host', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const guest = await smgSignUp(s, 'Guest');
    const x = await smgSignUp(s, 'Xan');         // friend of the guest only
    await friends(s, host, guest);
    await friends(s, guest, x);
    const code = (await s.call('POST', '/v1/rooms', { visibility: 'friends' }, host.token)).data.code;
    await s.call('POST', '/v1/rooms/join', { code }, guest.token);
    expect((await s.call('POST', '/v1/rooms/join', { friend: guest.code }, x.token)).status).toBe(200);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, x.token);
    expect((await s.call('POST', '/v1/rooms/join', { code }, x.token)).status).toBe(200);
    // not a former member and no friend of the host: still refused
    const z = await smgSignUp(s, 'Zed');
    await friends(s, guest, z);
    expect((await s.call('POST', '/v1/rooms/join', { code }, z.token)).status).toBe(404);
  });
});

