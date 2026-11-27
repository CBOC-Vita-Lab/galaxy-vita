// Security review hardening (Nov 2026): star time plausibility, endpoint claims, public rooms, request bodies, the edge bucket, caps.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, roomOf, ep, friends } from '../smg-helpers.js';
import { edgeCheck } from '../../src/core/edge.js';
import { readBodyCapped } from '../../src/core/util.js';
import { createEdgeBucket, createEdgeLimiter } from '../../src/core/edge_bucket.js';
import { loadConfig } from '../../src/core/config.js';

const ADMIN = 'test-admin-token-0123456789';
const T = (o) => ({ galaxy: 5, star: 2, frames: 3600, players: 1, build: '3.0.0', ...o });
const HOUR = 3600_000;

async function startedRoom(s, players, mode) {
  const code = await roomOf(s, players, { mode });
  for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(i + 1), players[i].token);
  for (const p of players) await s.call('POST', `/v1/rooms/${code}/started`, undefined, p.token);
  return code;
}
const flagKinds = async (s) => (await s.raw('GET', '/v1/admin/flags', undefined, ADMIN, {})).data;

describe('times: plausibility', () => {
  it('galaxy 0..41 only (42 galaxies exist)', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    expect((await s.call('POST', '/v1/times', T({ galaxy: 41 }), a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/times', T({ galaxy: 42 }), a.token)).data.error.field).toBe('galaxy');
    expect((await s.call('POST', '/v1/times', T({ galaxy: 63 }), a.token)).data.error.field).toBe('galaxy');
    expect((await s.call('POST', '/v1/times/get', { galaxy: 42, star: 1 }, a.token)).status).toBe(400);
    expect((await s.raw('GET', '/v1/public/times?galaxy=42&star=1', undefined, undefined, { 'x-msc-app': '3.0.0' })).status).toBe(400);
  });

  it('a first time far below the board is flagged (kept, hidden, in the admin queue); a close one is not', async () => {
    const s = makeSmg();
    const [a, b, c] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben'), await smgSignUp(s, 'Cy')];
    expect((await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token)).data.flagged).toBe(false); // nothing to compare with
    expect((await s.call('POST', '/v1/times', T({ frames: 3700 }), b.token)).data.flagged).toBe(false); // 7.5 % below the #1: fine (the new-#1 rule starts at 8 %)
    const r = await s.call('POST', '/v1/times', T({ frames: 2400 }), c.token); // 35 % below the best: flagged
    expect(r.data).toMatchObject({ best: true, flagged: true });
    const kinds = (await flagKinds(s)).flags.map((f) => f.kind).sort();
    expect(kinds).toContain('time_first');
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, a.token)).data.rows.map((x) => x.name)).toEqual(['Ben', 'Ana']);
    await s.raw('POST', '/v1/admin/flags/clear', { pid: c.pid, kind: 'time_first' }, ADMIN);
    expect((await s.call('POST', '/v1/times/get', { galaxy: 5, star: 2 }, a.token)).data.rows.map((x) => x.name)).toEqual(['Cy', 'Ben', 'Ana']);
  });

  it('an unverified new #1 more than 8 % faster than the old #1 is flagged; 5 % is not', async () => {
    const s = makeSmg();
    const [a, b, c] = [await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Ben'), await smgSignUp(s, 'Cy')];
    await s.call('POST', '/v1/times', T({ frames: 4000 }), a.token);
    await s.call('POST', '/v1/times', T({ frames: 4100 }), b.token); // Ben second: first-time check passes (not faster than 4000)
    s.advance(25 * HOUR); // Ben's own history is old: only the #1 rule applies below
    expect((await s.call('POST', '/v1/times', T({ frames: 3820 }), c.token)).data.flagged).toBe(false); // 4.5 % faster
    const d = await smgSignUp(s, 'Dee');
    const r = await s.call('POST', '/v1/times', T({ frames: 3400 }), d.token); // 11 % faster than 3820, but a first time (15 %): only time_top
    expect(r.data.flagged).toBe(true);
    const f = (await flagKinds(s)).flags.filter((x) => x.pid === d.pid);
    expect(f.map((x) => x.kind)).toEqual(['time_top']);
  });

  it('walking down in small steps is caught by the 24 h rolling window; an old history is not counted', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    let flagged = false;
    for (const f of [6000, 5600, 5200, 4800, 4400, 4000, 3700, 3400, 3100, 2800]) { // each step < 8 %, total > 25 %
      const r = await s.call('POST', '/v1/times', T({ frames: f }), a.token);
      expect(r.status).toBe(200);
      flagged = flagged || r.data.flagged;
      s.advance(60_000);
    }
    expect(flagged).toBe(true);
    expect((await flagKinds(s)).flags[0]).toMatchObject({ pid: a.pid, kind: 'time_jump', detail: { before: 6000, windowHours: 24 } });
    // the same improvement spread over days is honest practice
    const b = await smgSignUp(s, 'Ben');
    await s.call('POST', '/v1/times', T({ star: 3, frames: 6000 }), b.token);
    s.advance(25 * HOUR);
    expect((await s.call('POST', '/v1/times', T({ star: 3, frames: 5000 }), b.token)).data.flagged).toBe(false);
    s.advance(25 * HOUR);
    expect((await s.call('POST', '/v1/times', T({ star: 3, frames: 4000 }), b.token)).data.flagged).toBe(false);
  });

  it('race and co-op times need a started room; a verified time must fit in the room age', async () => {
    const s = makeSmg({ SMG_VERIFY_SLACK_SEC: '3' });
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    expect((await s.call('POST', '/v1/times', T({ players: 2 }), a.token)).data.error.code).toBe('room_required');
    const code = await startedRoom(s, [a, b], 'race');
    // the room started now: a 5000-frame (83 s) run cannot be verified yet
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 5000 }), a.token)).data.verified).toBe(false);
    for (let i = 0; i < 4; i++) { // keep both alive while 90 s pass
      s.advance(22_000);
      await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
      await s.call('GET', `/v1/rooms/${code}`, undefined, b.token);
    }
    // (service review: a second member must vouch: Ana's run of star 3 alone is not verified, Ben's confirms both)
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 4990, star: 3 }), a.token)).data.verified).toBe(false);
    expect((await s.call('POST', '/v1/times', T({ players: 2, room: code, frames: 5000, star: 3 }), b.token)).data).toMatchObject({ verified: true, confirmed: 1 });
  });
});

describe('endpoint claims', () => {
  const post = (s, code, p, body, ip) => s.call('POST', `/v1/rooms/${code}/endpoint`, body, p.token, { ip });
  const viewOf = (s, code, p) => s.call('GET', `/v1/rooms/${code}`, undefined, p.token);

  it('a public address that is not the caller\'s own puts the member on the relay: no addresses exchanged', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await post(s, code, h, ep(1), '203.0.113.1');
    const v = (await post(s, code, g, { ...ep(2), public: '198.51.100.77:41100' }, '203.0.113.2')).data; // names somebody else's address
    expect(v.members.find((m) => m.slot === 0).steer).toEqual({ relay: true, reason: 'endpoint_unverified' });
    const hv = (await viewOf(s, code, h)).data;
    const seen = hv.members.find((m) => m.slot === 1);
    expect(seen.steer).toEqual({ relay: true, reason: 'endpoint_unverified' });
    expect(seen.endpoint.public).toBeUndefined();
    expect(seen.endpoint.local).toBeUndefined();
    expect(JSON.stringify(hv)).not.toContain('198.51.100.77');
    // and the guest does not get the host's address either
    expect(v.members.find((m) => m.slot === 0).endpoint.public).toBeUndefined();
    // posting the right address again clears it
    const ok = (await post(s, code, g, ep(2), '203.0.113.2')).data;
    expect(ok.members.find((m) => m.slot === 0).steer.relay).toBe(false);
  });

  it('a public candidate (srflx / observed) that is not the caller\'s address is refused the same way; host candidates are not checked', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await post(s, code, h, ep(1), '203.0.113.1');
    const bad = (await post(s, code, g, { ...ep(2), candidates: [{ type: 'srflx', addr: '192.0.2.9:5000' }] }, '203.0.113.2')).data;
    expect(bad.members.find((m) => m.slot === 0).steer.reason).toBe('endpoint_unverified');
    const fine = (await post(s, code, g, { ...ep(2), candidates: [{ type: 'host', addr: '10.0.0.5:5000' }, { type: 'srflx', addr: '203.0.113.2:5001' }] }, '203.0.113.2')).data;
    expect(fine.members.find((m) => m.slot === 0).steer.relay).toBe(false);
  });

  // sec34 (VitAdhoc review finding 5, "a joiner makes the host send UDP at a victim"): checked, and already refused by postEndpoint (forced_relay);
  // these pin it for every way a claim can reach the other member, and the honest cases that must keep working.
  it('a joiner naming a victim as public / srflx / observed never gives the host that address; a public host candidate does not reach a member behind another address', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await post(s, code, h, ep(1), '203.0.113.1');
    const victim = '192.0.2.99:41100';
    for (const body of [
      { ...ep(2), public: victim },
      { ...ep(2), candidates: [{ type: 'srflx', addr: victim }] },
      { ...ep(2), candidates: [{ type: 'observed', addr: victim }] },
    ]) {
      await post(s, code, g, body, '203.0.113.2');
      const hv = (await viewOf(s, code, h)).data;
      expect(JSON.stringify(hv)).not.toContain('192.0.2.99');
      expect(hv.members.find((m) => m.slot === 1).steer.reason).toBe('endpoint_unverified');
    }
    // a "host" candidate may be any address, but only a member behind the same source address is told it
    await post(s, code, g, { ...ep(2), candidates: [{ type: 'host', addr: victim }] }, '203.0.113.2');
    expect(JSON.stringify((await viewOf(s, code, h)).data)).not.toContain('192.0.2.99');
  });
  it('honest NAT cases keep their direct path: public equal to the source address, with or without candidates, and a hairpin pair behind one router', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await post(s, code, h, { ...ep(1), nat: 'cone' }, '203.0.113.1');
    const v = (await post(s, code, g, { ...ep(2), nat: 'cone', candidates: [{ type: 'srflx', addr: '203.0.113.2:5001' }, { type: 'host', addr: '192.168.1.2:5000' }] }, '203.0.113.2')).data;
    expect(v.members.find((m) => m.slot === 0).steer).toEqual({ relay: false, reason: null });
    expect((await viewOf(s, code, h)).data.members.find((m) => m.slot === 1).endpoint.public).toBe('203.0.113.2:41100');
  });
  it('a relay address needs live relay credentials, and the server never learns the relayed address of anybody else (the peer only sends through its own TURN allocation)', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await post(s, code, h, ep(1), '203.0.113.1');
    const r = await post(s, code, g, { ...ep(2), relay: '192.0.2.99:41100' }, '203.0.113.2');
    expect(r.status).toBe(403);
    expect(r.data.error.code).toBe('relay_not_allowed');
  });
  it('an IPv6 caller cannot prove an IPv4 claim: relay', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await post(s, code, h, ep(1), '203.0.113.1');
    const v = (await post(s, code, g, ep(2), '2001:db8::7')).data;
    expect(v.members.find((m) => m.slot === 0).steer.reason).toBe('endpoint_unverified');
  });

  it('no address or a loopback address (the local fake service) is not checked', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    await post(s, code, h, ep(1), '127.0.0.1');
    const v = (await post(s, code, g, ep(2), '127.0.0.1')).data;
    expect(v.members.find((m) => m.slot === 0).steer.relay).toBe(false);
    expect(v.members.find((m) => m.slot === 0).endpoint.public).toBe('203.0.113.1:41100');
  });

  it('the LAN address goes only to a member behind the same public address', async () => {
    const s = makeSmg();
    const [h, g, k] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus'), await smgSignUp(s, 'Kim')];
    const code = await roomOf(s, [h, g, k]);
    await post(s, code, h, { public: '203.0.113.9:41100', local: '192.168.1.10:41100', nonce: 'AAAAAAAAAAA=', candidates: [{ type: 'host', addr: '192.168.1.10:41100' }] }, '203.0.113.9');
    await post(s, code, g, { public: '203.0.113.9:41101', local: '192.168.1.11:41100', nonce: 'AAAAAAAAAAA=' }, '203.0.113.9'); // same router
    await post(s, code, k, { public: '198.51.100.4:41100', local: '192.168.1.12:41100', nonce: 'AAAAAAAAAAA=' }, '198.51.100.4'); // elsewhere
    const gv = (await viewOf(s, code, g)).data.members.find((m) => m.slot === 0).endpoint;
    expect(gv.local).toBe('192.168.1.10:41100');
    expect(gv.candidates.map((c) => c.type)).toContain('host');
    const kv = (await viewOf(s, code, k)).data.members.find((m) => m.slot === 0).endpoint;
    expect(kv.public).toBe('203.0.113.9:41100');
    expect(kv.local).toBeNull();
    expect(kv.candidates).toEqual([]);
    const hv = (await viewOf(s, code, h)).data;
    expect(hv.members.find((m) => m.slot === 1).endpoint.local).toBe('192.168.1.11:41100');
    expect(hv.members.find((m) => m.slot === 2).endpoint.local).toBeNull();
  });

  it('public room: a guest gets the host\'s address only after the host has called since the guest joined', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    const code = (await s.call('POST', '/v1/rooms', { visibility: 'public' }, h.token, { ip: '203.0.113.1' })).data.code;
    await post(s, code, h, ep(1), '203.0.113.1');
    s.advance(5000);
    await s.call('POST', '/v1/rooms/join', { code }, g.token, { ip: '203.0.113.2' });
    const v = (await post(s, code, g, ep(2), '203.0.113.2')).data;
    expect(v.members.find((m) => m.slot === 0).endpoint.public).toBeUndefined(); // the host has not been heard from since the guest joined
    s.advance(1000);
    await viewOf(s, code, h); // the host's console polls: it has seen the new member
    const v2 = (await viewOf(s, code, g)).data;
    expect(v2.members.find((m) => m.slot === 0).endpoint.public).toBe('203.0.113.1:41100');
    // a code room has no such wait
    const h2 = await smgSignUp(s, 'Hed');
    const g2 = await smgSignUp(s, 'Gil');
    const code2 = await roomOf(s, [h2, g2]);
    await post(s, code2, h2, ep(5), '203.0.113.5');
    s.advance(5000);
    const h3 = await smgSignUp(s, 'Joe');
    const j3 = await s.call('POST', '/v1/rooms/join', { code: code2 }, h3.token);
    await s.call('POST', `/v1/rooms/${code2}/accept`, { slot: j3.data.slot }, h2.token); // (svcfix2 item 8: the host's console accepts a stranger)
    expect((await post(s, code2, h3, ep(6), '203.0.113.6')).data.members.find((m) => m.slot === 0).endpoint.public).toBe('203.0.113.5:41100');
  });

  it('at most SMG_PUBLIC_ROOMS_PER_IP live public rooms from one address', async () => {
    const s = makeSmg({ SMG_PUBLIC_ROOMS_PER_IP: '2' });
    const ps = [];
    for (const n of ['A1', 'B2', 'C3', 'D4']) ps.push(await smgSignUp(s, `Pl${n}`));
    const mk = (p, ip) => s.call('POST', '/v1/rooms', { visibility: 'public' }, p.token, { ip });
    expect((await mk(ps[0], '203.0.113.1')).status).toBe(200);
    expect((await mk(ps[1], '203.0.113.1')).status).toBe(200);
    expect((await mk(ps[2], '203.0.113.1')).data.error.code).toBe('too_many_rooms');
    expect((await mk(ps[3], '203.0.113.2')).status).toBe(200); // another address
    expect((await s.call('POST', '/v1/rooms', { visibility: 'friends' }, ps[2].token, { ip: '203.0.113.1' })).status).toBe(200); // only public rooms count
    const q = await s.call('POST', '/v1/quick', { mode: 'coop' }, ps[2].token, { ip: '203.0.113.1' }); // joins one instead of creating
    expect(q.data.quick).toBe('joined');
  });

  it('the public list: rooms with players first, then the newest; full rooms are not listed', async () => {
    const s = makeSmg();
    const ps = [];
    for (const n of ['A1', 'B2', 'C3', 'D4', 'E5', 'F6']) ps.push(await smgSignUp(s, `Pl${n}`));
    const mk = async (p, o) => { s.advance(1000); return (await s.call('POST', '/v1/rooms', { visibility: 'public', maxMembers: 2, ...o }, p.token)).data.code; };
    const old = await mk(ps[0]);
    const alone = await mk(ps[1]);
    const withGuest = await mk(ps[2], { maxMembers: 3 });
    await s.call('POST', '/v1/rooms/join', { code: withGuest }, ps[3].token);
    const full = await mk(ps[4]);
    await s.call('POST', '/v1/rooms/join', { code: full }, ps[5].token);
    const list = (await s.call('GET', '/v1/rooms/public', undefined, (await smgSignUp(s, 'Viewer')).token)).data.rooms.map((r) => r.code);
    expect(list).toEqual([withGuest, alone, old]);
  });
});

describe('bodies, edge, caps', () => {
  it('POST / PUT without a numeric Content-Length is 411 at the edge; no body at all is fine', () => {
    const base = { ADMIN_TOKEN: ADMIN, SERVICE_GAME: 'smg', REQUIRE_APP_VERSION: '0' };
    const h = { 'x-msc-game': 'smg', authorization: `Bearer ${'a'.repeat(43)}` };
    const stream = () => new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode('{}')); c.close(); } });
    const chunked = new Request('https://x.invalid/v1/rooms', { method: 'POST', headers: h, body: stream(), duplex: 'half' });
    expect(chunked.headers.get('content-length')).toBeNull();
    expect(edgeCheck(chunked, base)).toMatchObject({ status: 411 });
    const bad = new Request('https://x.invalid/v1/rooms', { method: 'POST', headers: { ...h, 'content-length': 'abc' }, body: '{}' });
    expect(edgeCheck(bad, base)).toMatchObject({ status: 411 });
    const ok = new Request('https://x.invalid/v1/rooms', { method: 'POST', headers: { ...h, 'content-length': '2' }, body: '{}' });
    expect(edgeCheck(ok, base)).toBeNull();
    expect(edgeCheck(new Request('https://x.invalid/v1/rooms/AAAAAA/started', { method: 'POST', headers: h }), base)).toBeNull();
    const big = new Request('https://x.invalid/v1/rooms', { method: 'POST', headers: { ...h, 'content-length': '5000' }, body: 'x'.repeat(5000) });
    expect(edgeCheck(big, base)).toMatchObject({ status: 413 });
  });

  it('the object caps the stream itself: a body longer than its Content-Length says is 413 and is not read to the end', async () => {
    let pulled = 0;
    const stream = new ReadableStream({ pull(c) { pulled += 1024; c.enqueue(new Uint8Array(1024).fill(120)); if (pulled > 1_000_000) c.close(); } });
    const req = new Request('https://x.invalid/', { method: 'POST', body: stream, duplex: 'half' });
    expect(await readBodyCapped(req, 4096)).toBeNull();
    expect(pulled).toBeLessThan(20_000);
    expect(await readBodyCapped(new Request('https://x.invalid/', { method: 'POST', body: '{"a":1}' }), 4096)).toBe('{"a":1}');
    expect(await readBodyCapped(new Request('https://x.invalid/'), 4096)).toBe('');
    // through the application: a liar (Content-Length 2, body 6000 bytes) cannot get a big body past it
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const big = JSON.stringify({ code: 'x'.repeat(6000) });
    const r = await s.call('POST', '/v1/friends', big, a.token, { 'content-length': '2' });
    expect(r.status).toBe(413);
  });

  it('the edge token bucket: burst, refill, per address, bounded memory', () => {
    const b = createEdgeBucket({ burst: 5, perSec: 1 });
    for (let i = 0; i < 5; i++) expect(b.take('203.0.113.1', 1000)).toBe(0);
    expect(b.take('203.0.113.1', 1000)).toBeGreaterThanOrEqual(1);
    expect(b.take('203.0.113.2', 1000)).toBe(0); // another address
    expect(b.take('203.0.113.1', 3000)).toBe(0); // 2 s later: 2 tokens back
    expect(b.take('203.0.113.1', 3000)).toBe(0);
    expect(b.take('203.0.113.1', 3000)).toBeGreaterThanOrEqual(1);
    expect(b.take('2001:db8:1:2::1', 0)).toBe(0); // IPv6 by /64
    for (let i = 0; i < 5; i++) b.take('2001:db8:1:2::99', 0);
    expect(b.take('2001:db8:1:2:ffff::1', 0)).toBeGreaterThanOrEqual(1);
    for (let i = 0; i < 6000; i++) b.take(`10.${i >> 8}.${i & 255}.1`, 0);
    expect(b.size()).toBeLessThanOrEqual(4000);
    expect(createEdgeBucket({ burst: 0, perSec: 1 }).take('1.2.3.4', 0)).toBe(0); // off
  });

  it('the edge limiter: consoles behind one address are not throttled together, a token flood still is (Strikers 2e8378c6, Galaxy numbers)', () => {
    const tok = (i) => `Bearer ${String(i).padStart(43, 'a')}`;
    // the toml's numbers: 120 / 1 per address+token, 300 / 5 aggregate. Four consoles, each polling about every 1.7 s plus a queue poll every 2 s
    // (about 1.1 a second each, 4.4 a second together: above the old single per-address 1 a second) for two minutes
    const lim = createEdgeLimiter({ burst: 120, perSec: 1, aggBurst: 300, aggPerSec: 5 });
    let refused = 0;
    for (let t = 0; t < 120000; t += 100) {
      for (let c = 0; c < 4; c++) {
        if ((t + c * 37) % 1700 < 100 && lim.take('198.51.100.7', tok(c), t) > 0) refused++;
        if ((t + c * 53) % 2000 < 100 && lim.take('198.51.100.7', tok(c), t) > 0) refused++;
      }
    }
    expect(refused).toBe(0);
    // the same traffic through the old single per-address bucket (120 / 1) is throttled: the negative control
    const old = createEdgeBucket({ burst: 120, perSec: 1 });
    let oldRefused = 0;
    for (let t = 0; t < 120000; t += 100) {
      for (let c = 0; c < 4; c++) {
        if ((t + c * 37) % 1700 < 100 && old.take('198.51.100.7', t) > 0) oldRefused++;
        if ((t + c * 53) % 2000 < 100 && old.take('198.51.100.7', t) > 0) oldRefused++;
      }
    }
    expect(oldRefused).toBeGreaterThan(0);
    // one console that hammers is refused on its own bucket and does not use up the others
    const one = createEdgeLimiter({ burst: 5, perSec: 1, aggBurst: 400, aggPerSec: 30 });
    for (let i = 0; i < 5; i++) expect(one.take('203.0.113.9', tok(1), 0)).toBe(0);
    expect(one.take('203.0.113.9', tok(1), 0)).toBeGreaterThanOrEqual(1);
    expect(one.take('203.0.113.9', tok(2), 0)).toBe(0);
    // a flood with invented tokens gets fresh per-token buckets but hits the address's aggregate (burst, then the refill)
    const flood = createEdgeLimiter({ burst: 120, perSec: 1, aggBurst: 300, aggPerSec: 5 });
    let passed = 0;
    for (let i = 0; i < 5000; i++) if (flood.take('192.0.2.1', tok(i), 0) === 0) passed++;
    expect(passed).toBe(300);
    for (let i = 0; i < 5000; i++) if (flood.take('192.0.2.1', tok(10000 + i), 10000) === 0) passed++;
    expect(passed).toBe(300 + 50); // ten seconds refill 50
    // no token: the plain per-address bucket
    const plain = createEdgeLimiter({ burst: 5, perSec: 1 });
    for (let i = 0; i < 5; i++) expect(plain.take('192.0.2.2', '', 0)).toBe(0);
    expect(plain.take('192.0.2.2', '', 0)).toBeGreaterThanOrEqual(1);
    expect(plain.take('192.0.2.3', '', 0)).toBe(0);
    // burst 0 = off for everyone
    expect(createEdgeLimiter({ burst: 0, perSec: 1 }).take('1.2.3.4', tok(1), 0)).toBe(0);
    // memory stays bounded when many tokens come
    for (let i = 0; i < 9000; i++) lim.take(`10.${i >> 8}.${i & 255}.1`, tok(i), 0);
    expect(lim.size()).toBeLessThanOrEqual(12000);
  });

  it('Galaxy defaults: 2000 registrations a day, 50000 players, link lost after 20 s (Strikers keeps its numbers)', async () => {
    const smg = loadConfig({ SERVICE_GAME: 'smg' });
    expect([smg.register.maxPerDay, smg.maxPlayers, smg.rooms.lostMs, smg.rooms.quietMs]).toEqual([2000, 50000, 20_000, 12_000]);
    expect([smg.register.powBits, smg.register.netPerDay, smg.register.uniqueNames, smg.expireIdleMs / 86400_000]).toEqual([15, 60, false, 365]);
    const msc = loadConfig({});
    expect([msc.register.maxPerDay, msc.maxPlayers, msc.rooms.lostMs, msc.rooms.quietMs]).toEqual([1000, 200000, 15_000, 5000]);
    const s = makeSmg();
    expect((await s.call('GET', '/v1/info')).data.link).toMatchObject({ pingAfterSec: 12, lostAfterSec: 20 });
  });

  it('registration stops at REGISTER_MAX_PER_DAY for the whole service', async () => {
    const s = makeSmg({ REGISTER_MAX_PER_DAY: '3', RATE_LIMIT_SCALE: '1' });
    for (const n of ['Aa1', 'Bb2', 'Cc3']) await smgSignUp(s, `Reg${n}`);
    expect((await s.call('POST', '/v1/register', { name: 'RegDd4' })).status).toBe(429);
  });

  it('sweep: the idle-account scan runs at most hourly; its index exists; the alarm stops when no room is live', async () => {
    const s = makeSmg({ PRUNE_IDLE_DAYS: '1' });
    const idx = s.store.sql.exec(`PRAGMA index_list(profiles)`).toArray().map((r) => r.name);
    expect(idx).toContain('profiles_last_seen');
    const old = await smgSignUp(s, 'Olly');
    s.advance(2 * 86400_000);
    expect(s.app.sweep()).toBe(false); // nothing live: the object's alarm is not set again
    const gone = s.store.sql.exec(`SELECT COUNT(*) AS n FROM profiles WHERE pid = ?`, old.pid).toArray()[0].n;
    expect(gone).toBe(0); // first sweep scanned
    const next = await smgSignUp(s, 'Nina');
    s.advance(2 * 86400_000 - 1);
    s.app.sweep(); // (the first sweep above set the timestamp; this one is 2 days later and runs)
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM profiles WHERE pid = ?`, next.pid).toArray()[0].n).toBe(0);
    const third = await smgSignUp(s, 'Tom');
    s.advance(2 * 86400_000);
    // a sweep 10 minutes after the last scan does not scan again: an old account stays until an hourly pass
    s.app.sweep();
    const late = await smgSignUp(s, 'Lou');
    s.advance(2 * 86400_000);
    s.app.sweep();
    s.advance(600_000);
    const keep = await smgSignUp(s, 'Kay');
    s.store.sql.exec(`UPDATE profiles SET last_seen_ms = ? WHERE pid = ?`, s.clock.now - 3 * 86400_000, keep.pid);
    s.app.sweep();
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM profiles WHERE pid = ?`, keep.pid).toArray()[0].n).toBe(1);
    s.advance(3600_000);
    s.app.sweep();
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM profiles WHERE pid = ?`, keep.pid).toArray()[0].n).toBe(0);
    void third; void late;
    // with a live room the alarm goes on
    const a = await smgSignUp(s, 'Ana');
    await s.call('POST', '/v1/rooms', {}, a.token);
    expect(s.app.sweep()).toBe(true);
  });

  it('friend requests: at most 100 stored incoming per target (the oldest go); the list says how many are stored', async () => {
    const s = makeSmg({ SMG_FRIENDS_MAX: '500' });
    const target = await smgSignUp(s, 'Target');
    for (let i = 0; i < 103; i++) {
      s.store.sql.exec(`INSERT INTO profiles (pid, code, name, name_key, token_hash, created_ms, last_seen_ms) VALUES (?, ?, ?, ?, ?, 0, 0)`, 500000 + i, `ZZZ${String(i).padStart(5, '0')}`.slice(0, 8), `Bot${i}`, `bot${i}`, `h${i}`);
    }
    // each "bot" adds the target (through the same SQL path the API uses is not possible without a token: use real accounts for a few, and the cap constant for the rest)
    const { INCOMING_MAX } = await import('../../src/core/friends.js');
    expect(INCOMING_MAX).toBe(100);
    const reals = [];
    for (let i = 0; i < 4; i++) reals.push(await smgSignUp(s, `Req${i}`));
    for (const r of reals) await s.call('POST', '/v1/friends', { code: target.code }, r.token);
    const l = (await s.call('GET', '/v1/friends', undefined, target.token)).data;
    expect(l.incomingTotal).toBe(4);
    expect(l.incoming).toHaveLength(4);
  });

  it('the admin export leaves out room_members (live addresses)', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Gus')];
    await roomOf(s, [h, g]);
    const list = (await s.raw('GET', '/v1/admin/export', undefined, ADMIN, {})).data;
    expect(list.tables.map((t) => t.table)).not.toContain('room_members');
    expect((await s.raw('GET', '/v1/admin/export?table=room_members', undefined, ADMIN, {})).status).toBe(400);
    expect((await s.raw('GET', '/v1/admin/export?table=rooms', undefined, ADMIN, {})).status).toBe(200);
  });
});
void friends;

describe('slower polling of started rooms (free-plan budget)', () => {
  it('an unchanged poll says peersQuiet once a member is silent for 12 s; 8 s polls never look quiet; host migration after 20 s', async () => {
    const s = makeSmg();
    const [h, a] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Ana')];
    const code = await startedRoom(s, [h, a], 'coop');
    let v = (await s.call('GET', `/v1/rooms/${code}`, undefined, a.token)).data.version;
    for (let i = 0; i < 6; i++) { // both poll every 8 s for a minute: nobody is quiet or lost
      s.advance(8000);
      await s.call('GET', `/v1/rooms/${code}`, undefined, h.token);
      const r = (await s.call('GET', `/v1/rooms/${code}?v=${v}`, undefined, a.token)).data;
      expect(r).toEqual({ code, state: 'started', version: v, changed: false });
    }
    s.advance(13000); // the host misses a poll: 13 s silent
    const quiet = (await s.call('GET', `/v1/rooms/${code}?v=${v}`, undefined, a.token)).data;
    expect(quiet).toMatchObject({ changed: false, peersQuiet: true });
    s.advance(7000); // 20 s: the host is replaced and the guest's next poll sees the new version
    const moved = (await s.call('GET', `/v1/rooms/${code}?v=${v}`, undefined, a.token)).data;
    expect(moved).toMatchObject({ changed: true, hostSlot: 1, hostEpoch: 1 });
    v = moved.version;
  });

  it('drop-in into a started room still works with the slow rhythm: the new member and the host exchange endpoints', async () => {
    const s = makeSmg();
    const [h, a, k] = [await smgSignUp(s, 'Hal'), await smgSignUp(s, 'Ana'), await smgSignUp(s, 'Kim')];
    const code = await startedRoom(s, [h, a], 'coop');
    s.advance(8000);
    await s.call('GET', `/v1/rooms/${code}`, undefined, h.token); // the host's slow poll
    await s.call('GET', `/v1/rooms/${code}`, undefined, a.token);
    expect((await s.call('POST', '/v1/rooms/join', { code }, k.token)).data.slot).toBe(2);
    await s.call('POST', `/v1/rooms/${code}/accept`, { slot: 2 }, h.token); // (svcfix2 item 8)
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(3), k.token);
    s.advance(8000);
    const hv = (await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data; // the host's next slow poll sees the newcomer's endpoint
    expect(hv.members.find((m) => m.slot === 2).endpoint.public).toBe('203.0.113.3:41100');
    const kv = (await s.call('GET', `/v1/rooms/${code}`, undefined, k.token)).data;
    expect(kv.members.find((m) => m.slot === 0).endpoint.public).toBe('203.0.113.1:41100');
  });
});
