// Resumable sessions, identity codes, version gate, NAT steering, relay-only privacy, strict fields, idempotent results.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, makeBlob, pairInRoom, startMatch } from '../helpers.js';

const ep = (n, extra = {}) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAECAwQFBgc=', ...extra });
const adm = { authorization: 'Bearer test-admin-token-0123456789' };

describe('app version gate (A2/A13)', () => {
  const env = { MIN_APP_VERSION: '1.2.0', LATEST_APP_VERSION: '1.3.1' };
  it('refuses builds below the minimum with update_required, naming the latest version', async () => {
    const s = makeServer(env);
    const old = await s.call('POST', '/v1/register', { name: 'Oldie' }, undefined, { 'x-msc-app': '1.1.9' });
    expect(old.status).toBe(426);
    expect(old.data.error).toMatchObject({ code: 'update_required', minVersion: '1.2.0', latestVersion: '1.3.1' });
    expect((await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-app': '1.0' })).status).toBe(426);
    expect((await s.call('POST', '/v1/register', { name: 'Newer' }, undefined, { 'x-msc-app': '1.2.0' })).status).toBe(200);
    expect((await s.call('POST', '/v1/register', { name: 'Newest' }, undefined, { 'x-msc-app': '1.10.0' })).status).toBe(200); // numeric compare
    expect((await s.call('POST', '/v1/register', { name: 'NoHdr' })).status).toBe(200); // header optional unless REQUIRE_APP_VERSION=1
  });
  it('ordinary errors also carry latestVersion; info shows both; bad headers are 400', async () => {
    const s = makeServer(env);
    const e = await s.call('GET', '/v1/me', undefined, 'x'.repeat(43));
    expect(e.data.error.latestVersion).toBe('1.3.1');
    expect((await s.call('GET', '/v1/info')).data.app).toEqual({ min: '1.2.0', latest: '1.3.1' });
    expect((await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-app': 'abc' })).data.error.code).toBe('bad_app_version');
    expect((await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-build': 'zz' })).data.error.code).toBe('bad_app_version');
    expect((await s.call('GET', '/v1/info', undefined, undefined, { 'x-msc-build': 'abcdef0123456789' })).status).toBe(200);
  });
  it('REQUIRE_APP_VERSION=1 makes the header mandatory except on info/time/servers', async () => {
    const s = makeServer({ REQUIRE_APP_VERSION: '1' });
    expect((await s.call('POST', '/v1/register', { name: 'NoHdr' })).data.error.code).toBe('bad_app_version');
    expect((await s.call('GET', '/v1/info')).status).toBe(200);
    expect((await s.call('POST', '/v1/register', { name: 'HasHdr' }, undefined, { 'x-msc-app': '1.2.0' })).status).toBe(200);
  });
});

describe('identity: token + public code + display name (A3)', () => {
  it('every player has a short public code that finds them but grants nothing', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    const b = await signUp(s, 'Beta');
    expect(a.code).toMatch(/^[ABCDEFGHJKMNPQRSTUVWXYZ2-9]{4}-[ABCDEFGHJKMNPQRSTUVWXYZ2-9]{4}$/);
    expect(a.code).not.toBe(b.code);
    const found = await s.call('GET', `/v1/players/${a.code.toLowerCase().replace('-', '')}`, undefined, b.token);
    expect(found.data).toEqual({ pid: a.pid, code: a.code, name: 'Alpha', region: 2 });
    expect(JSON.stringify(found.data)).not.toContain(a.token);
    expect((await s.call('GET', '/v1/players/AAAA-AAAA', undefined, b.token)).data.error.code).toBe('player_not_found');
    expect((await s.call('GET', '/v1/players/short', undefined, b.token)).data.error.code).toBe('bad_code');
    expect((await s.call('GET', `/v1/players/${a.code}`)).status).toBe(401);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).data.code).toBe(a.code);
  });
  it('relayOnly is a profile setting', async () => {
    const s = makeServer();
    const a = await s.call('POST', '/v1/register', { name: 'Private1', relayOnly: true });
    expect(a.data.relayOnly).toBe(true);
    const off = await s.call('PUT', '/v1/me', { relayOnly: false }, a.data.token);
    expect(off.data.relayOnly).toBe(false);
    expect((await s.call('PUT', '/v1/me', { relayOnly: 'yes' }, a.data.token)).data.error.code).toBe('bad_request');
  });
});

describe('strict field allowlists (A5)', () => {
  it('unknown fields are refused everywhere a body is read', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    const b = await signUp(s, 'Beta');
    const code = (await s.call('POST', '/v1/rooms', undefined, a.token)).data.code;
    await s.call('POST', '/v1/rooms/join', { code }, b.token);
    const cases = [
      ['POST', '/v1/register', { name: 'Extra1', admin: true }],
      ['PUT', '/v1/me', { name: 'Extra2', pid: 1 }],
      ['POST', '/v1/rooms', { x: 1 }],
      ['POST', '/v1/rooms/join', { code, x: 1 }],
      ['POST', `/v1/rooms/${code}/endpoint`, { ...ep(1), extra: 1 }],
      ['POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', x: 1 }],
      ['POST', `/v1/rooms/${code}/started`, { x: 1 }],
      ['POST', '/v1/turn', { room: code, x: 1 }],
      ['POST', '/v1/turn/usage', { bytes: 1, x: 1 }],
      ['POST', '/v1/ranked/queue', { x: 1 }],
      ['POST', '/v1/ranked/queue/resume', { ticketId: 'a', ticketToken: 'b', x: 1 }],
      ['POST', '/v1/rank/put', { category: 0, region: 2, score: 0, data: makeBlob('Alpha'), x: 1 }],
      ['POST', '/v1/rank/get', { mode: 1, category: 0, limit: 1, x: 1 }],
      ['POST', '/v1/matches/abcdefghijkl/result', { result: 0, score0: 0, score1: 0, checksum: 'abcdef01', x: 1 }],
    ];
    for (const [m, path, body] of cases) {
      const r = await s.call(m, path, body, a.token);
      expect(r.status, `${m} ${path}`).toBe(400);
      expect(r.data.error.code, `${m} ${path}`).toBe('unknown_field');
    }
    expect((await s.call('POST', '/v1/admin/ban', { pid: 1, nope: 1 }, undefined, adm)).data.error.code).toBe('unknown_field');
    expect((await s.call('POST', '/v1/admin/switches', { kill: [], nope: 1 }, undefined, adm)).data.error.code).toBe('unknown_field');
    expect((await s.call('POST', '/v1/rooms', {}, a.token)).status).toBe(200); // an empty object is fine
  });

  it('blob fields must be sane in either byte order (date, wins/losses, region)', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha');
    const put = (blob) => s.call('POST', '/v1/rank/put', { category: 0, region: 2, score: 0, data: blob }, a.token);
    expect((await put(makeBlob('Alpha', { wins: 9999, losses: 9999 }))).status).toBe(200);
    expect((await put(makeBlob('Alpha', { wins: 20000 }))).data.error.code).toBe('bad_blob');
    expect((await put(makeBlob('Alpha', { wins: -1, endian: 'be' }))).data.error.code).toBe('bad_blob');
    expect((await put(makeBlob('Alpha', { wins: 5, endian: 'be' }))).status).toBe(200);
  });
});

describe('resumable queue tickets (A2)', () => {
  it('a ticket is issued once, resumable with its secret, refused with a wrong one or after the drop', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const q = await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect(q.data.ticket.id).toBeTruthy();
    expect(q.data.ticket.token).toMatch(/^[A-Za-z0-9_-]{20,}$/);
    expect(q.data.link).toBe('ok');
    const poll = await s.call('GET', '/v1/ranked/queue', undefined, a.token);
    expect(poll.data.ticket).toBeUndefined(); // secret only at creation
    expect(poll.data.ticketId).toBe(q.data.ticket.id);
    s.advance(7000);
    expect((await s.call('POST', '/v1/ranked/queue/resume', { ticketId: q.data.ticket.id, ticketToken: q.data.ticket.token }, a.token)).data).toMatchObject({ status: 'queued', link: 'ok' });
    const bad = await s.call('POST', '/v1/ranked/queue/resume', { ticketId: q.data.ticket.id, ticketToken: 'nope' }, a.token);
    expect(bad.status).toBe(410);
    expect(bad.data.error.code).toBe('ticket_expired');
    s.advance(31_000);
    expect((await s.call('POST', '/v1/ranked/queue/resume', { ticketId: q.data.ticket.id, ticketToken: q.data.ticket.token }, a.token)).data.error.code).toBe('ticket_expired');
  });

  it('link health: quiet after 5 s, lost after 15 s; a lost ticket is not matched but resumes', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, a.token)).data;
    s.advance(6000);
    // the entry is only observable through its owner, so peek at the stored silence through a resume
    expect((await s.call('GET', '/v1/ranked/queue', undefined, a.token)).data.link).toBe('ok'); // polling refreshed it
    s.advance(16_000); // silent for 16 s: a ghost for the matchmaker
    const qb = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    expect(qb.status).toBe('queued');
    s.advance(1000);
    const back = await s.call('POST', '/v1/ranked/queue/resume', { ticketId: q.ticket.id, ticketToken: q.ticket.token }, a.token);
    expect(back.data.status).toBe('matched');
  });
});

describe('join acknowledgement (A2: 5 s)', () => {
  it('a pairing nobody confirms in time is voided: the confirming player is requeued, the other dropped', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const qb = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data; // b receives the pairing = acks
    expect(qb.status).toBe('matched');
    s.advance(6000);
    const pa = (await s.call('GET', '/v1/ranked/queue', undefined, a.token)).data; // a answers too late
    expect(pa).toMatchObject({ status: 'none', reason: 'ack_timeout' });
    const pb = (await s.call('GET', '/v1/ranked/queue', undefined, b.token)).data;
    expect(pb.status).toBe('queued');
    expect((await s.call('GET', `/v1/matches/${qb.room.matchId}`, undefined, b.token)).data.status).toBe('void');
    expect((await s.call('GET', `/v1/rooms/${qb.roomCode}`, undefined, b.token)).data.closedReason).toBe('ack_timeout');
  });
  it('confirming inside the window keeps the pairing; any room call counts as the confirmation', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const qb = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    s.advance(4000);
    expect((await s.call('GET', `/v1/rooms/${qb.roomCode}`, undefined, a.token)).data.state).toBe('joined');
    s.advance(10_000);
    expect((await s.call('GET', '/v1/ranked/queue', undefined, b.token)).data.status).toBe('matched');
  });
  it('the housekeeping sweep also settles an unconfirmed pairing', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const qb = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    s.advance(6000);
    s.app.sweep();
    expect(s.store.sql.exec('SELECT state FROM rooms WHERE code = ?', qb.roomCode).one().state).toBe('closed');
  });
});

describe('presence and stale endpoints (A2/A6)', () => {
  it('the partner link reads ok / quiet / lost from how recently they called', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    await s.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.peer.link).toBe('ok');
    s.advance(6000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.peer.link).toBe('quiet');
    s.advance(10_000);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.peer.link).toBe('lost');
    await s.call('GET', `/v1/rooms/${code}?v=1`, undefined, b.token); // the cheap poll is a ping
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.peer.link).toBe('ok');
  });

  it('an endpoint older than the NAT mapping life (30 s) is marked stale until re-posted', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
    s.advance(31_000);
    const g = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    expect(g.data.peer.endpoint).toMatchObject({ stale: true, ageMs: 31_000 });
    expect(g.data.session.natLifeSec).toBe(30);
    const fresh = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
    expect(fresh.data.peer.endpoint.stale).toBe(false);
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, b.token)).data.peer.endpoint).toMatchObject({ stale: false, public: '203.0.113.1:41000' });
  });

  it('candidates (host, srflx, observed) are validated and exchanged', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    const cands = [{ type: 'host', addr: '192.168.1.1:41000' }, { type: 'srflx', addr: '203.0.113.1:41000' }, { type: 'observed', addr: '203.0.113.1:50111' }];
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1, { candidates: cands, nat: 'cone' }), a.token);
    const r = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    expect(r.data.peer.endpoint.candidates).toEqual(cands);
    const bad = (c) => s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1, { candidates: c }), a.token);
    expect((await bad([{ type: 'weird', addr: '1.2.3.4:5' }])).data.error.code).toBe('bad_endpoint');
    expect((await bad([{ type: 'host', addr: 'x' }])).data.error.code).toBe('bad_endpoint');
    expect((await bad(Array(7).fill({ type: 'host', addr: '1.2.3.4:5' }))).data.error.code).toBe('bad_endpoint');
    expect((await bad([{ type: 'host', addr: '1.2.3.4:5', x: 1 }])).data.error.code).toBe('unknown_field');
    expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1, { nat: 'weird' }), a.token)).data.error.code).toBe('bad_request');
  });
});

describe('punch reports and relay steering (A6)', () => {
  const room = async (s, a, b) => {
    const { code } = await pairInRoom(s, a, b);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    return code;
  };
  const steer = async (s, code, p) => (await s.call('GET', `/v1/rooms/${code}`, undefined, p.token)).data.session.steer;

  it('a symmetric NAT goes straight to the relay', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    expect(await steer(s, code, a)).toEqual({ relay: false, reason: null });
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1, { nat: 'symmetric' }), a.token);
    expect(await steer(s, code, b)).toEqual({ relay: true, reason: 'nat' });
  });

  it('two failed punches for the same pair steer their next room to the relay; a success clears the debt', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    for (let i = 0; i < 2; i++) {
      const code = await room(s, a, b);
      expect(await steer(s, code, a)).toEqual({ relay: false, reason: null });
      const r = await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'fail', path: 'direct', ms: 6000 }, a.token);
      expect(r.status).toBe(200);
      await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'fail', path: 'direct' }, b.token); // both report: still one failure
      await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
    }
    const row = s.store.sql.exec('SELECT fails, oks FROM pair_stats').one();
    expect(row).toEqual({ fails: 2, oks: 0 });
    const third = await room(s, a, b);
    expect(await steer(s, third, b)).toEqual({ relay: true, reason: 'pair_failures' });
    // the relay path works: that is a "fail" for punching purposes only if reported as such; a direct success helps
    await s.call('POST', `/v1/rooms/${third}/leave`, undefined, a.token);
    const fourth = await room(s, a, b);
    await s.call('POST', `/v1/rooms/${fourth}/punch`, { result: 'ok', path: 'direct' }, a.token);
    await s.call('POST', `/v1/rooms/${fourth}/leave`, undefined, a.token);
    const fifth = await room(s, a, b);
    expect(await steer(s, fifth, a)).toEqual({ relay: false, reason: null }); // 2 fails - 1 ok = 1 < limit
  });

  it('"fail" from either side wins over "ok"; relay path counts as a punch failure; failures expire after 7 days', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    let code = await room(s, a, b);
    await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct' }, a.token);
    await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'fail', path: 'direct' }, b.token);
    expect(s.store.sql.exec('SELECT fails, oks FROM pair_stats').one()).toEqual({ fails: 1, oks: 0 });
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
    code = await room(s, a, b);
    await s.call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'relay' }, a.token);
    expect(s.store.sql.exec('SELECT fails FROM pair_stats').one().fails).toBe(2);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
    code = await room(s, a, b);
    expect((await steer(s, code, a)).relay).toBe(true);
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
    s.advance(8 * 86400_000);
    code = await room(s, a, b);
    expect((await steer(s, code, a)).relay).toBe(false);
  });

  it('validates the report', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const code = await room(s, a, b);
    const post = (o) => s.call('POST', `/v1/rooms/${code}/punch`, o, a.token);
    expect((await post({ result: 'maybe', path: 'direct' })).data.error.code).toBe('bad_request');
    expect((await post({ result: 'ok', path: 'tunnel' })).data.error.code).toBe('bad_request');
    expect((await post({ result: 'ok', path: 'direct', ms: -1 })).data.error.code).toBe('bad_request');
    const solo = await signUp(s, 'Solo');
    const open = (await s.call('POST', '/v1/rooms', undefined, solo.token)).data.code;
    expect((await s.call('POST', `/v1/rooms/${open}/punch`, { result: 'ok', path: 'direct' }, solo.token)).data.error.code).toBe('room_not_ready');
  });
});

describe('relay-only players (C5)', () => {
  it('exchange only relayed addresses: no raw address in either direction, steer says relay_only', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const p = (await s.call('POST', '/v1/register', { name: 'Private1', relayOnly: true })).data;
    const { code } = await pairInRoom(s, a, p);
    // the relay-only account may not send direct addresses, even by mistake
    const leak = await s.call('POST', `/v1/rooms/${code}/endpoint`, { ...ep(2), relay: '198.51.100.9:50000' }, p.token);
    expect(leak.data.error.code).toBe('relay_only_account');
    expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, { nonce: 'AAECAwQFBgc=' }, p.token)).data.error.code).toBe('bad_endpoint');
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1, { relay: '198.51.100.8:50001' }), a.token);
    const rp = await s.call('POST', `/v1/rooms/${code}/endpoint`, { relay: '198.51.100.9:50000', nonce: 'AAECAwQFBgc=' }, p.token);
    expect(rp.status).toBe(200);
    expect(rp.data.session.steer).toEqual({ relay: true, reason: 'relay_only' });
    expect(rp.data.peer.endpoint).toMatchObject({ relay: '198.51.100.8:50001' });
    expect(rp.data.peer.endpoint.public).toBeUndefined();
    expect(JSON.stringify(rp.data)).not.toContain('203.0.113.1');
    const ha = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data;
    expect(ha.peer.endpoint).toMatchObject({ relay: '198.51.100.9:50000' });
    expect(ha.peer.endpoint.public).toBeUndefined();
    expect(ha.peer.endpoint.local).toBeUndefined();
    expect(ha.session.steer.reason).toBe('relay_only');
  });
  it('a normal player can add a relayed address next to the direct ones', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Hosty');
    const b = await signUp(s, 'Guesty');
    const { code } = await pairInRoom(s, a, b);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1, { relay: '198.51.100.8:50001' }), a.token);
    const r = await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
    expect(r.data.peer.endpoint).toMatchObject({ public: '203.0.113.1:41000', relay: '198.51.100.8:50001' });
  });
});

describe('idempotent results with retry metadata (A4/A9)', () => {
  const body = (o = {}) => ({ result: 0, score0: 2, score1: 1, checksum: 'abcdef0123456789', ...o });
  async function started(s) {
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    await startMatch(s, a, b, q.roomCode);
    return { a, b, id: q.room.matchId };
  }
  it('retries from a persistent queue are safe: same answer flagged duplicate, attempts tracked, nothing double-counted', async () => {
    const s = makeServer();
    const { a, b, id } = await started(s);
    const first = await s.call('POST', `/v1/matches/${id}/result`, body({ attempt: 1, build: 'a1b2c3d4e5f60718', mode: 'ranked' }), a.token);
    expect(first.data).toMatchObject({ status: 'awaiting_peer', attempts: 1 });
    expect(first.data.duplicate).toBeUndefined();
    const again = await s.call('POST', `/v1/matches/${id}/result`, body({ attempt: 4, build: 'a1b2c3d4e5f60718', mode: 'ranked' }), a.token);
    expect(again.status).toBe(200);
    expect(again.data).toMatchObject({ status: 'awaiting_peer', duplicate: true });
    const done = await s.call('POST', `/v1/matches/${id}/result`, body({ build: 'a1b2c3d4e5f60718' }), b.token);
    expect(done.data).toMatchObject({ status: 'verified', buildsMatch: true });
    // a retry that arrives after the match settled still gets a clean answer (client can drop it from its queue)
    const late = await s.call('POST', `/v1/matches/${id}/result`, body({ attempt: 6 }), a.token);
    expect(late.data).toMatchObject({ status: 'verified', duplicate: true, final: true, attempts: 6 });
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM results').one().n).toBe(2);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).data.wins).toBe(1);
  });
  it('differing build hashes are reported; a result is committed only when scores AND end-of-match hashes agree', async () => {
    const s = makeServer();
    const { a, b, id } = await started(s);
    await s.call('POST', `/v1/matches/${id}/result`, body({ build: '1111111111111111' }), a.token);
    const r = await s.call('POST', `/v1/matches/${id}/result`, body({ build: '2222222222222222', checksum: 'abcdef0123456780' }), b.token);
    expect(r.data).toMatchObject({ status: 'disputed', buildsMatch: false, scores: null });
  });
  it('mode and field validation', async () => {
    const s = makeServer();
    const { a, id } = await started(s);
    const post = (o) => s.call('POST', `/v1/matches/${id}/result`, body(o), a.token);
    expect((await post({ mode: 'unranked' })).data.error.code).toBe('mode_mismatch');
    expect((await post({ mode: 'casual' })).data.error.code).toBe('bad_request');
    expect((await post({ attempt: 0 })).data.error.code).toBe('bad_request');
    expect((await post({ attempt: 'x' })).data.error.code).toBe('bad_request');
    expect((await post({ build: 'nothex!!' })).data.error.code).toBe('bad_request');
  });
});

describe('server list (C5)', () => {
  it('serves the configured mirrors, https only, so the Worker URL can move without a client update', async () => {
    const s = makeServer({ SERVER_LIST_JSON: JSON.stringify([{ url: 'https://a.example.org/' }, { url: 'http://insecure.example.org' }, { url: 'https://b.example.org:8443', priority: 5 }, { nope: 1 }]), LATEST_APP_VERSION: '1.3.0' });
    const r = await s.call('GET', '/v1/servers');
    expect(r.status).toBe(200);
    expect(r.data.servers).toEqual([{ url: 'https://a.example.org', priority: 0 }, { url: 'https://b.example.org:8443', priority: 5 }]);
    expect(r.data).toMatchObject({ version: 1, ttlSec: 86400, app: { min: null, latest: '1.3.0' }, stun: ['stun.cloudflare.com:3478'] });
    expect((await s.call('GET', '/v1/info')).data.servers).toHaveLength(2);
    expect((await makeServer().call('GET', '/v1/servers')).data.servers).toEqual([]);
  });
});
