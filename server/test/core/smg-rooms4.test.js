// GAME=smg rooms of up to 4: slots, the members view (star topology), legacy peer, punch per pair, start, leave, host migration.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, ep, roomOf, friends } from '../smg-helpers.js';

async function four(s) {
  const ps = [];
  for (const n of ['Host', 'Gina', 'Gabe', 'Gus']) ps.push(await smgSignUp(s, n));
  return ps;
}

describe('smg rooms: create and join', () => {
  it('create takes mode, size, visibility and allowlisted settings; join takes the first free slot', async () => {
    const s = makeSmg();
    const [h, a, b, c] = await four(s);
    for (const p of [a, b, c]) await friends(s, h, p); // (svcfix3: a friends room opens to the host's friends, also by code)
    const made = await s.call('POST', '/v1/rooms', { mode: 'race', maxMembers: 3, visibility: 'friends', settings: { rivals: 1, raceWait: 18, galaxy: 4, scenario: 2 } }, h.token);
    expect(made.status).toBe(200);
    expect(made.data).toMatchObject({ game: 'smg', mode: 'race', maxMembers: 3, visibility: 'friends', topology: 'star', state: 'open', role: 'host', slot: 0, hostSlot: 0, hostEpoch: 0, session: null, peer: null });
    expect(made.data.settings).toEqual({ rivals: 1, raceWait: 18, galaxy: 4, scenario: 2, dropIn: true });
    expect(made.data.members).toHaveLength(1);
    expect(made.data.you.vip).toMatch(/^10\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d+\.1$/);
    const code = made.data.code;
    const ja = await s.call('POST', '/v1/rooms/join', { code }, a.token);
    expect(ja.data).toMatchObject({ slot: 1, role: 'guest', state: 'joined' });
    const base = ja.data.you.vip.replace(/\.\d+$/, '');
    expect(ja.data.you.vip).toBe(`${base}.2`);
    expect(ja.data.peer).toMatchObject({ slot: 0, name: 'Host', vip: `${base}.1`, endpoint: null }); // legacy 2-member peer
    expect(ja.data.session).toMatchObject({ key: expect.any(String), cookie: expect.any(Number), punchAtMs: null });
    const jb = await s.call('POST', '/v1/rooms/join', { code }, b.token);
    expect(jb.data).toMatchObject({ slot: 2, peer: null });
    expect(jb.data.you.vip).toBe(`${base}.3`);
    expect(jb.data.members.map((m) => m.slot)).toEqual([0, 1, 2]);
    expect(jb.data.session.key).toBe(ja.data.session.key);
    const full = await s.call('POST', '/v1/rooms/join', { code }, c.token);
    expect(full.status).toBe(409);
    expect(full.data.error.code).toBe('room_full');
    // a guest leaving frees its slot; the next joiner takes it
    expect((await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token)).data).toMatchObject({ ok: true });
    const jc = await s.call('POST', '/v1/rooms/join', { code }, c.token);
    expect(jc.data.slot).toBe(1);
  });

  it('refuses unknown fields, bad settings, oversized settings, wrong versions and bad sizes', async () => {
    const s = makeSmg();
    const [h, a] = await four(s);
    expect((await s.call('POST', '/v1/rooms', { mode: 'party' }, h.token)).data.error.code).toBe('bad_mode');
    expect((await s.call('POST', '/v1/rooms', { maxMembers: 5 }, h.token)).data.error).toMatchObject({ code: 'bad_request', field: 'maxMembers' });
    expect((await s.call('POST', '/v1/rooms', { maxMembers: 1 }, h.token)).status).toBe(400);
    expect((await s.call('POST', '/v1/rooms', { colour: 1 }, h.token)).data.error.code).toBe('unknown_field');
    expect((await s.call('POST', '/v1/rooms', { settings: { evil: 1 } }, h.token)).data.error).toMatchObject({ code: 'unknown_field', field: 'settings.evil' });
    expect((await s.call('POST', '/v1/rooms', { settings: { rivals: 9 } }, h.token)).data.error.code).toBe('bad_settings');
    expect((await s.call('POST', '/v1/rooms', { settings: { dropIn: 1 } }, h.token)).data.error.code).toBe('bad_settings');
    const code = await roomOf(s, [h]);
    const other = await s.call('POST', '/v1/rooms/join', { code }, a.token, { 'x-msc-app': '3.0.1' });
    expect(other.status).toBe(409);
    expect(other.data.error.code).toBe('version_mismatch');
    expect((await s.call('POST', '/v1/rooms/join', { code }, h.token)).data.error.code).toBe('own_room');
    expect((await s.call('POST', '/v1/rooms/join', { code, friend: 'AAAA-AAAA' }, a.token)).data.error.code).toBe('bad_request');
  });

  it('a body over the smg cap (4096) is refused', async () => {
    const s = makeSmg();
    const [h] = await four(s);
    const r = await s.call('POST', '/v1/rooms', 'x'.repeat(5000), h.token);
    expect(r.status).toBe(413);
  });
});

describe('smg rooms: signaling in a star', () => {
  it('a guest sees only the host endpoint, the host sees each guest after both of the pair posted; punchAtMs per pair', async () => {
    const s = makeSmg();
    const [h, a, b] = await four(s);
    const code = await roomOf(s, [h, a, b]);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token);
    let va = await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    expect(va.data.members.find((m) => m.slot === 0).endpoint).toBeNull(); // a has not posted yet
    s.advance(500);
    va = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), a.token);
    const hostSeenByA = va.data.members.find((m) => m.slot === 0);
    // (different public addresses: the LAN address is not handed over; see the LAN tests in smg-hardening.test.js)
    expect(hostSeenByA.endpoint).toMatchObject({ public: '203.0.113.1:41100', local: null, stale: false });
    expect(hostSeenByA.punchAtMs).toBe(s.clock.now + 1000);
    expect(va.data.members.find((m) => m.slot === 2).endpoint).toBeNull(); // never another guest's address
    expect(va.data.session.punchAtMs).toBe(hostSeenByA.punchAtMs);
    expect(va.data.state).toBe('joined'); // b has not posted
    const vb = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(3), b.token);
    expect(vb.data.state).toBe('ready');
    expect(vb.data.members.find((m) => m.slot === 1).endpoint).toBeNull();
    const vh = await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
    expect(vh.data.members.find((m) => m.slot === 1).endpoint.public).toBe('203.0.113.2:41100');
    expect(vh.data.members.find((m) => m.slot === 2).endpoint.public).toBe('203.0.113.3:41100');
    expect(vh.data.members.find((m) => m.slot === 0).endpoint).toBeNull(); // self
    expect(vh.data.peer).toBeNull(); // 3 members: no legacy peer
    expect(vh.data.session.punchAtMs).toBeNull(); // the host has two pairs: per member only
  });

  it('punch reports name the pair; failures per (host, guest) steer that pair to the relay', async () => {
    const s = makeSmg();
    const [h, a, b] = await four(s);
    for (let round = 0; round < 2; round++) {
      const code = await roomOf(s, [h, a, b]);
      for (const [p, n] of [[h, 1], [a, 2], [b, 3]]) await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(n), p.token);
      const amb = await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'fail', path: 'direct' }, h.token);
      expect(amb.data.error).toMatchObject({ code: 'bad_request', field: 'peer' }); // two guests: say which
      expect((await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'fail', path: 'direct', peer: 1 }, h.token)).status).toBe(200);
      expect((await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', peer: 0 }, b.token)).status).toBe(200);
      expect((await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', peer: 2 }, b.token)).data.error.code).toBe('bad_request');
      await s.call('POST', `/v1/rooms/${code}/leave`, undefined, h.token);
      await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
      await s.call('POST', `/v1/rooms/${code}/leave`, undefined, b.token);
    }
    const code = await roomOf(s, [h, a, b]);
    const vh = await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
    expect(vh.data.members.find((m) => m.slot === 1).steer).toEqual({ relay: true, reason: 'pair_failures' });
    expect(vh.data.members.find((m) => m.slot === 2).steer).toEqual({ relay: false, reason: null });
    const va = await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    expect(va.data.session.steer).toEqual({ relay: true, reason: 'pair_failures' });
  });

  it('started: host + one guest starts the room; drop-in joins a started room unless the settings forbid it', async () => {
    const s = makeSmg();
    const [h, a, b, c] = await four(s);
    const code = await roomOf(s, [h, a]);
    expect((await s.call('POST', `/v1/rooms/${code}/started`, undefined, h.token)).data.error.code).toBe('room_not_ready');
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), a.token);
    expect((await s.call('POST', `/v1/rooms/${code}/started`, undefined, h.token)).data.state).toBe('ready');
    const st = await s.call('POST', `/v1/rooms/${code}/started`, undefined, a.token);
    expect(st.data).toMatchObject({ state: 'started', started: { you: true, peer: true, room: true } });
    const drop = await s.call('POST', '/v1/rooms/join', { code }, b.token);
    expect(drop.data).toMatchObject({ state: 'started', slot: 2 });
    expect((await s.call('POST', `/v1/rooms/${code}/started`, undefined, b.token)).data.error.code).toBe('room_not_ready');
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(3), b.token);
    expect((await s.call('POST', `/v1/rooms/${code}/started`, undefined, b.token)).data.started.you).toBe(true);

    const closed = await roomOf(s, [c], { settings: { dropIn: false } });
    const x = await smgSignUp(s, 'Xena');
    const y = await smgSignUp(s, 'Yuri');
    await s.call('POST', '/v1/rooms/join', { code: closed }, x.token);
    await s.call('POST', `/v1/rooms/${closed}/endpoint`, ep(4), c.token);
    await s.call('POST', `/v1/rooms/${closed}/endpoint`, ep(5), x.token);
    await s.call('POST', `/v1/rooms/${closed}/started`, undefined, c.token);
    await s.call('POST', `/v1/rooms/${closed}/started`, undefined, x.token);
    const no = await s.call('POST', '/v1/rooms/join', { code: closed }, y.token);
    expect(no.status).toBe(409);
    expect(no.data.error.code).toBe('room_started');
  });

  it('TURN credentials are for members of a live smg room only', async () => {
    const s = makeSmg();
    const [h, a, b] = await four(s);
    const code = await roomOf(s, [h, a]);
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/turn', { room: code }, b.token)).data.error.code).toBe('turn_not_allowed');
  });

  it('a player is in one room at a time: creating another room leaves the first', async () => {
    const s = makeSmg();
    const [h, a] = await four(s);
    const first = await roomOf(s, [h, a]);
    await s.call('POST', '/v1/rooms', {}, a.token);
    const v = await s.call('GET', `/v1/rooms/${first}`, undefined, h.token);
    expect(v.data.members.map((m) => m.slot)).toEqual([0]);
    expect(v.data.state).toBe('open');
  });
});

describe('smg rooms: host migration (DESIGN.md section 8)', () => {
  it('host leaves a 4-member room: slot 1 becomes host, epoch 1, same code, every guest pair re-scheduled', async () => {
    const s = makeSmg();
    const [h, a, b, c] = await four(s);
    const code = await roomOf(s, [h, a, b, c]);
    for (const [p, n] of [[h, 1], [a, 2], [b, 3], [c, 4]]) await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(n), p.token);
    await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', peer: 2 }, h.token);
    s.advance(40_000);
    const left = await s.call('POST', `/v1/rooms/${code}/leave`, undefined, h.token);
    expect(left.data).toMatchObject({ ok: true, hostSlot: 1, hostEpoch: 1 });
    const va = await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    expect(va.data).toMatchObject({ code, hostSlot: 1, hostEpoch: 1, role: 'host', slot: 1, state: 'ready' });
    expect(va.data.members.map((m) => m.slot)).toEqual([1, 2, 3]);
    // the new host sees both guests; their pairs punch from now on (not at the old post time)
    for (const g of [2, 3]) {
      const m = va.data.members.find((x) => x.slot === g);
      expect(m.endpoint).not.toBeNull();
      expect(m.punchAtMs).toBe(s.clock.now + 1000);
      expect(m.endpoint.stale).toBe(true); // addresses older than the NAT life and no punch since the change: re-post
    }
    const vb = await s.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    expect(vb.data.role).toBe('guest');
    expect(vb.data.members.find((m) => m.slot === 1).endpoint.public).toBe('203.0.113.2:41100');
    expect(vb.data.members.find((m) => m.slot === 3).endpoint).toBeNull();
    expect(vb.data.session.punchAtMs).toBe(s.clock.now + 1000);
    // the guest's report now names the new host
    expect((await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', peer: 1 }, b.token)).status).toBe(200);
    expect((await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', peer: 0 }, b.token)).status).toBe(400);
  });

  it('host leaves twice: epoch 2, the lowest remaining slot hosts; the last member closes the room', async () => {
    const s = makeSmg();
    const [h, a, b, c] = await four(s);
    const code = await roomOf(s, [h, a, b, c]);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, h.token);
    const second = await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
    expect(second.data).toMatchObject({ hostSlot: 2, hostEpoch: 2, state: 'joined' });
    // a guest (not the host) leaving changes nothing about the host
    expect((await s.call('POST', `/v1/rooms/${code}/leave`, undefined, c.token)).data).toMatchObject({ hostSlot: 2, hostEpoch: 2, state: 'open' });
    const last = await s.call('POST', `/v1/rooms/${code}/leave`, undefined, b.token);
    expect(last.data.state).toBe('closed');
    const gone = await s.call('POST', '/v1/rooms/join', { code }, h.token);
    expect(gone.data.error.code).toBe('room_closed');
  });

  it('a host whose link is lost for 20 s is replaced on the next member call', async () => {
    const s = makeSmg();
    const [h, a, b] = await four(s);
    const code = await roomOf(s, [h, a, b]);
    s.advance(10_000);
    await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    await s.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    s.advance(9_000); // host silent 19 s (Galaxy: lost after 20 s, so two missed 8 s polls still count as alive), guests 9 s
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, b.token)).data).toMatchObject({ hostSlot: 0, hostEpoch: 0 });
    s.advance(1_000);
    const v = await s.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    expect(v.data).toMatchObject({ hostSlot: 1, hostEpoch: 1 });
    expect(v.data.members.map((m) => m.slot)).toEqual([1, 2]);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data.error.code).toBe('not_member');
  });

  it('a banned or deleted host is replaced like a leaving one', async () => {
    const s = makeSmg();
    const [h, a, b] = await four(s);
    const code = await roomOf(s, [h, a, b]);
    await s.raw('POST', '/v1/admin/ban', { pid: h.pid }, 'test-admin-token-0123456789');
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, b.token)).data).toMatchObject({ hostSlot: 1, hostEpoch: 1, state: 'joined' });
    await s.call('DELETE', '/v1/me', undefined, a.token);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, b.token)).data).toMatchObject({ hostSlot: 2, hostEpoch: 2, state: 'open', role: 'host' });
  });

  it('a rejoin within 15 s takes the old slot back; later it takes the first free slot', async () => {
    const s = makeSmg();
    const [h, a, b, c] = await four(s);
    const code = await roomOf(s, [h, a, b]);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token); // slot 1 free
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, h.token); // host -> slot 2
    s.advance(5_000);
    const back = await s.call('POST', '/v1/rooms/join', { code }, h.token);
    expect(back.data).toMatchObject({ slot: 0, role: 'guest', hostSlot: 2 }); // its slot, not the host role
    s.advance(20_000);
    await s.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    const late = await s.call('POST', '/v1/rooms/join', { code }, a.token);
    expect(late.data.slot).toBe(1);
    const cj = await s.call('POST', '/v1/rooms/join', { code }, c.token);
    expect(cj.data.slot).toBe(3);
  });
});

describe('smg room view: the exact shape the client reads', () => {
  it('keys of the room view, a member and an endpoint', async () => {
    const s = makeSmg();
    const [h, a] = await four(s);
    const code = await roomOf(s, [h, a]);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token);
    const v = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), a.token);
    expect(Object.keys(v.data)).toEqual(['code', 'game', 'kind', 'mode', 'visibility', 'topology', 'maxMembers', 'settings', 'state', 'version', 'role', 'slot',
      'hostSlot', 'hostEpoch', 'createdMs', 'expiresMs', 'closedReason', 'matchId', 'you', 'peer', 'members', 'session', 'started']); // (+ 'changed' on GET)
    expect(Object.keys(v.data.members[0])).toEqual(['slot', 'pid', 'name', 'label', 'nameDup', 'code', 'vip', 'accepted', 'link', 'endpoint', 'punchAtMs', 'steer']);
    expect(Object.keys(v.data.members[0].endpoint)).toEqual(['public', 'local', 'candidates', 'relay', 'nonce', 'nat', 'ageMs', 'stale']);
    expect(Object.keys(v.data.peer)).toEqual(['pid', 'name', 'slot', 'vip', 'link', 'endpoint']);
    expect(Object.keys(v.data.session)).toEqual(['key', 'cookie', 'punchAtMs', 'punch', 'stun', 'natLifeSec', 'steer']);
    // each member's public player code (the roster's friend code), as GET /v1/me gives it to its owner
    const me0 = await s.call('GET', '/v1/me', undefined, h.token);
    expect(v.data.members[0].code).toBe(me0.data.code);
    expect(v.data.members[0].code).toMatch(/^[A-HJKMNP-Z2-9]{4}-[A-HJKMNP-Z2-9]{4}$/);
    const same = await s.call('GET', `/v1/rooms/${code}?v=${v.data.version}`, undefined, a.token);
    expect(same.data).toEqual({ code, state: 'ready', version: v.data.version, changed: false });
  });
});
