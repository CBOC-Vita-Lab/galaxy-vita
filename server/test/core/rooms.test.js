import { describe, it, expect } from 'vitest';
import { makeServer, signUp, pairInRoom, startMatch } from '../helpers.js';
import { CODE_ALPHABET } from '../../src/core/rooms.js';
import { b64decode } from '../../src/core/util.js';

const ep = (n, extra = {}) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAECAwQFBgc=', ...extra });

describe('room codes', () => {
  it('are 6 unambiguous characters', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const r = await s.call('POST', '/v1/rooms', undefined, a.token);
    expect(r.status).toBe(200);
    expect(r.data.code).toMatch(/^[ABCDEFGHJKMNPQRSTUVWXYZ2-9]{6}$/);
    for (const bad of ['I', 'L', 'O', '0', '1']) expect(CODE_ALPHABET).not.toContain(bad);
    expect(r.data).toMatchObject({ kind: 'unranked', state: 'open', role: 'host', slot: 0, peer: null, session: null });
  });

  it('join is case/space/dash-insensitive and rejects malformed codes', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    const bad = await s.call('POST', '/v1/rooms/join', { code: 'ABC' }, b.token);
    expect(bad.data.error.code).toBe('bad_code');
    expect((await s.call('POST', '/v1/rooms/join', { code: 'ABCDE0' }, b.token)).data.error.code).toBe('bad_code');
    expect((await s.call('POST', '/v1/rooms/join', {}, b.token)).data.error.code).toBe('bad_code');
    expect((await s.call('POST', '/v1/rooms/join', { code: 'ZZZZZZ' }, b.token)).data.error.code).toBe('room_not_found');
    const nice = `${code.slice(0, 3)}-${code.slice(3)}`.toLowerCase();
    expect((await s.call('POST', '/v1/rooms/join', { code: nice }, b.token)).status).toBe(200);
  });
});

describe('join and exchange', () => {
  it('host and guest get distinct vips, same cookie and key; key is 32 bytes', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code, joined } = await pairInRoom(s, a, b);
    expect(joined.status).toBe(200);
    const ha = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data;
    const gb = joined.data;
    expect(ha.state).toBe('joined');
    expect(ha.you.vip).not.toBe(gb.you.vip);
    expect(ha.you.vip).toBe(gb.peer.vip);
    expect(ha.peer.vip).toBe(gb.you.vip);
    expect(ha.session.key).toBe(gb.session.key);
    expect(ha.session.cookie).toBe(gb.session.cookie);
    expect(ha.session.cookie).toBeGreaterThan(0);
    expect(b64decode(ha.session.key).length).toBe(32);
    expect(ha.peer.name).toBe('Guesty');
    expect(gb.peer.name).toBe('Hosty');
    expect(ha.you.vip).toMatch(/^10\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d+\.[12]$/);
    expect(ha.matchId).toBeTruthy();
  });

  it('the key is withheld until the second player joined', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const r = (await s.call('POST', '/v1/rooms', undefined, a.token)).data;
    expect(r.session).toBeNull();
    expect(JSON.stringify(r)).not.toContain('key');
  });

  it('endpoints are revealed only when both have posted (reciprocity)', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    const r1 = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
    expect(r1.status).toBe(200);
    expect(r1.data.peer.endpoint).toBeNull();
    // the guest still sees nothing of the host's address, nor the punch time, before posting their own
    const g0 = (await s.call('GET', `/v1/rooms/${code}`, undefined, b.token)).data;
    expect(g0.peer.endpoint).toBeNull();
    expect(JSON.stringify(g0)).not.toContain('203.0.113.1');
    const r2 = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2, { nat: 'cone' }), b.token);
    expect(r2.data.peer.endpoint).toMatchObject({ public: '203.0.113.1:41000', local: '192.168.1.1:41000', nonce: 'AAECAwQFBgc=', nat: null, relay: null, candidates: [], stale: false });
    expect(r2.data.state).toBe('ready');
    const h = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data;
    expect(h.peer.endpoint).toMatchObject({ public: '203.0.113.2:41000', nat: 'cone' });
    expect(h.session.punchAtMs).toBe(s.clock.now + 1000);
    expect(h.session.punch).toEqual({ intervalMs: 700, tries: 7, lingerMs: 5000 });
  });

  it('host may post an endpoint before the guest arrives', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token)).status).toBe(200);
    await s.call('POST', '/v1/rooms/join', { code }, b.token);
    const r = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    expect(r.data.peer.endpoint.public).toBe('203.0.113.1:41000');
    expect(r.data.state).toBe('ready');
  });

  it('validates endpoints', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    const post = (o) => s.call('POST', `/v1/rooms/${code}/endpoint`, { ...ep(1), ...o }, a.token);
    for (const bad of ['1.2.3.4', '1.2.3.4:0', '1.2.3.4:70000', '999.1.1.1:5', '1.2.3:5', '::1', 'host:41000', '127.0.0.1:41000', '0.0.0.0:41000', '224.0.0.1:41000', '169.254.1.1:41000', '01.2.3.4:5', '1.2.3.4:041000']) {
      const r = await post({ public: bad });
      expect(r.status, bad).toBe(400);
      expect(r.data.error.code).toBe('bad_endpoint');
    }
    expect((await post({ local: 'nope' })).data.error.code).toBe('bad_endpoint');
    expect((await post({ nonce: 'x' })).data.error.code).toBe('bad_nonce');
    expect((await post({ nonce: 'AAAA' })).data.error.code).toBe('bad_nonce'); // 3 bytes
    expect((await post({ nonce: undefined })).data.error.code).toBe('bad_nonce');
    expect((await post({ nat: 'a b' })).data.error.code).toBe('bad_request');
    expect((await post({})).status).toBe(200);
  });

  it('polling with ?v= returns a tiny unchanged answer', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    const full = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data;
    expect(full.changed).toBe(true);
    const same = (await s.call('GET', `/v1/rooms/${code}?v=${full.version}`, undefined, a.token)).data;
    expect(same).toEqual({ code, state: 'joined', version: full.version, changed: false });
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    const after = (await s.call('GET', `/v1/rooms/${code}?v=${full.version}`, undefined, a.token)).data;
    expect(after.changed).toBe(true);
    expect(after.version).toBeGreaterThan(full.version);
  });
});

describe('access rules', () => {
  it('only members can read; full rooms and own rooms refuse', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const c = await signUp(s, 'Third');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    expect((await s.call('POST', '/v1/rooms/join', { code }, a.token)).data.error.code).toBe('own_room');
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, b.token)).data.error.code).toBe('not_member');
    await s.call('POST', '/v1/rooms/join', { code }, b.token);
    expect((await s.call('POST', '/v1/rooms/join', { code }, c.token)).data.error.code).toBe('room_full');
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, c.token)).data.error.code).toBe('not_member');
    expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(3), c.token)).status).toBe(403);
    expect((await s.call('GET', `/v1/rooms/ZZZZZZ`, undefined, c.token)).status).toBe(404);
    expect((await s.call('GET', `/v1/rooms/${code}`)).status).toBe(401);
  });

  it('joining twice is idempotent; creating a new room closes the old open one', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    const j1 = await s.call('POST', '/v1/rooms/join', { code }, b.token);
    const j2 = await s.call('POST', '/v1/rooms/join', { code }, b.token);
    expect(j2.status).toBe(200);
    expect(j2.data.session.key).toBe(j1.data.session.key);
    const code2 = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    expect(code2).not.toBe(code);
    const old = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data;
    expect(old.state).toBe('closed');
    expect(old.closedReason).toBe('replaced');
  });

  it('unique vips across many simultaneous rooms', async () => {
    const s = makeServer();
    const seen = new Set();
    for (let i = 0; i < 40; i++) {
      const a = await signUp(s, `Host${i}`);
      const b = await signUp(s, `Gest${i}`);
      const { joined } = await pairInRoom(s, a, b);
      for (const v of [joined.data.you.vip, joined.data.peer.vip]) {
        expect(seen.has(v), v).toBe(false);
        seen.add(v);
      }
    }
    expect(seen.size).toBe(80);
  });
});

describe('expiry', () => {
  it('open rooms expire after the open TTL and answer 410', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    s.advance(9 * 60_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.state).toBe('open');
    s.advance(2 * 60_000);
    const g = await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    expect(g.data.state).toBe('expired');
    const j = await s.call('POST', '/v1/rooms/join', { code }, b.token);
    expect(j.status).toBe(410);
    expect(j.data.error.code).toBe('room_expired');
    expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token)).status).toBe(410);
  });

  it('joined rooms last longer; the pending match is voided on expiry', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code, matchId } = await pairInRoom(s, a, b);
    s.advance(15 * 60_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.state).toBe('joined');
    s.advance(6 * 60_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.state).toBe('expired');
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token)).data.status).toBe('void');
  });

  it('sweep deletes closed rooms after the grace period and reports remaining work', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    await s.call('POST', '/v1/rooms', undefined, a.token);
    expect(s.app.sweep()).toBe(true);
    s.advance(11 * 60_000);
    expect(s.app.sweep()).toBe(true); // expired, kept for the grace period
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM rooms').one().n).toBe(1);
    s.advance(6 * 60_000);
    expect(s.app.sweep()).toBe(false);
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM rooms').one().n).toBe(0);
  });
});

describe('leave and start', () => {
  it('guest leaving reopens the room with a fresh key; host leaving closes it', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const c = await signUp(s, 'Third');
    const { code, joined } = await pairInRoom(s, a, b);
    expect((await s.call('POST', `/v1/rooms/${code}/leave`, undefined, b.token)).status).toBe(200);
    const h = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data;
    expect(h.state).toBe('open');
    expect(h.session).toBeNull();
    const j = await s.call('POST', '/v1/rooms/join', { code }, c.token);
    expect(j.status).toBe(200);
    expect(j.data.session.key).not.toBe(joined.data.session.key);
    expect((await s.call('DELETE', `/v1/rooms/${code}`, undefined, a.token)).status).toBe(200);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, c.token)).data.state).toBe('closed');
    expect((await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token)).data.state).toBe('closed');
  });

  it('started needs exchanged endpoints; becomes started only when both confirm', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code, matchId } = await pairInRoom(s, a, b);
    expect((await s.call('POST', `/v1/rooms/${code}/started`, undefined, a.token)).data.error.code).toBe('room_not_ready');
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    const r1 = await s.call('POST', `/v1/rooms/${code}/started`, undefined, a.token);
    expect(r1.data.state).toBe('ready');
    expect(r1.data.started).toEqual({ you: true, peer: false });
    const r2 = await s.call('POST', `/v1/rooms/${code}/started`, undefined, b.token);
    expect(r2.data.state).toBe('started');
    expect(r2.data.started).toEqual({ you: true, peer: true });
    // endpoints may be re-posted after a reconnect without leaving "started"
    expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(9), a.token)).data.state).toBe('started');
    expect(s.store.sql.exec('SELECT status FROM matches WHERE match_id = ?', matchId).one().status).toBe('started');
  });

  it('a started room lives for the long TTL', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    await startMatch(s, a, b, code);
    s.advance(2 * 3600_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.state).toBe('started');
    s.advance(2 * 3600_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.state).toBe('expired');
  });
});

describe('kill switch for rooms', () => {
  it('rooms off -> 503 service_disabled, polling existing rooms still works', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    const adm = { authorization: 'Bearer test-admin-token-0123456789' };
    await s.call('POST', '/v1/admin/switches', { kill: ['rooms'], message: 'maintenance' }, undefined, adm);
    const r = await s.call('POST', '/v1/rooms', undefined, a.token);
    expect(r.status).toBe(503);
    expect(r.data.error).toMatchObject({ code: 'service_disabled', message: 'maintenance' });
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).status).toBe(200);
    expect((await s.call('GET', '/v1/info')).data.kill).toEqual(['rooms']);
  });
});
