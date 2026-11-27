// svcfix2 item 6 (independent review, recommendation 1 and 3): the free plan buys about 135 player-hours a day at 354 requests per player-hour.
// Once every peer of a started room exchanges direct heartbeats, the clients stop polling the service except about once a minute (the host's
// held keep-alive) and at once when a peer is lost; forming rooms hold their polls 25 s instead of 8. The SERVICE dictates the rhythm
// ({poll: {holdSec, nextSec}} in the room answer); this file is the accounting model of what a client that obeys it costs.
//
// A virtual-time model: the application is called directly (no real waiting); a held poll is modelled as a request whose answer arrives
// `hold` seconds later, with the presence stamps the application's own ticker would make every 4 s during the hold.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, ep } from '../smg-helpers.js';

const SEC = 1000, HOUR = 3600 * SEC;
const NOT_LIMITED = { RATE_LIMIT_SCALE: '1000', DAILY_REQUEST_BUDGET: '0' };

async function startedRoom(s, n) {
  const ps = []; for (let i = 0; i < n; i++) ps.push(await smgSignUp(s, `Cad${i}Name`));
  const made = await s.call('POST', '/v1/rooms', {}, ps[0].token);
  const code = made.data.code;
  for (const p of ps.slice(1)) expect((await s.call('POST', '/v1/rooms/join', { code }, p.token)).status).toBe(200);
  for (const [i, p] of ps.entries()) expect((await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(i + 1), p.token)).status).toBe(200);
  for (const p of ps) expect((await s.call('POST', `/v1/rooms/${code}/started`, undefined, p.token)).status).toBe(200);
  return { ps, code };
}

/** One simulated console: obeys the plan (new) or polls the old way (hold 8 s, 0.3 s gap). */
function consoleModel(s, presence, room, slot, { newClient, linksUp }) {
  const m = { slot, requests: 0, nextAt: s.clock.now, holdUntil: 0, dead: false, version: 0, hold: 8, next: 0.3, lastMask: null, plans: [] };
  m.mask = () => linksUp(slot);
  m.step = async (forceNow) => {
    const t = s.clock.now;
    const token = room.ps[slot].token;
    const mask = m.mask();
    const q = newClient ? `&p=1&l=${mask.toString(16)}` : ''; // (no `wait`: the application answers at once; the hold is modelled below)
    const r = await s.call('GET', `/v1/rooms/${room.code}?v=${m.version || 1}${q}`, undefined, token);
    m.requests++;
    if (r.status !== 200) return r;
    // (the app answered at once because the model gives no real time to wait; the hold is virtual)
    m.version = r.data.version;
    const poll = r.data.poll;
    m.plans.push(poll);
    const hold = newClient && poll ? poll.holdSec : (r.data.changed === false ? 8 : 0);
    m.holdUntil = t + hold * SEC;
    m.holdFrom = t;
    m.nextAt = t + hold * SEC + (newClient && poll ? poll.nextSec : 0.3) * SEC;
    m.hold = newClient && poll ? poll.holdSec : 8;
    return r;
  };
  return m;
}

async function run(s, presence, room, models, untilMs, { onTick } = {}) {
  const end = s.clock.now + untilMs;
  for (;;) {
    const live = models.filter((x) => !x.dead);
    const next = Math.min(...live.map((x) => x.nextAt));
    if (!isFinite(next) || next > end) break;
    // presence stamps the held polls make every 4 s (HELD_TICK_MS) up to `next`
    for (const x of live) for (let ts = Math.ceil((x.holdFrom || 0) / 4000) * 4000; ts <= Math.min(next, x.holdUntil || 0); ts += 4000) {
      if (ts >= (x.holdFrom || 0)) presence.set(`${room.code}:${x.slot}`, Math.max(presence.get(`${room.code}:${x.slot}`) || 0, ts));
    }
    s.clock.now = Math.max(s.clock.now, next);
    if (onTick) await onTick(s.clock.now);
    for (const x of live.filter((y) => y.nextAt <= s.clock.now)) await x.step();
  }
  s.clock.now = end;
}

const allLinked = (n) => (slot) => ((1 << n) - 1) & ~(1 << slot);

describe('request cost of a started room (requests per player-hour)', () => {
  for (const n of [2, 4]) {
    it(`${n} players, one hour, all peers linked directly: a client that obeys the plan costs under 60 requests per player-hour; the old rhythm costs ~430`, async () => {
      const out = {};
      for (const newClient of [false, true]) {
        const presence = new Map(), direct = new Map();
        const s = makeSmg(NOT_LIMITED, { deps: { presence, direct } });
        const room = await startedRoom(s, n);
        const models = room.ps.map((_, slot) => consoleModel(s, presence, room, slot, { newClient, linksUp: allLinked(n) }));
        await run(s, presence, room, models, HOUR);
        const total = models.reduce((a, x) => a + x.requests, 0);
        out[newClient ? 'new' : 'old'] = { perPlayerHour: total / n, host: models[0].requests, guest: models[1].requests };
        // nobody was ever dropped and the host never moved in a healthy hour
        const v = await s.call('GET', `/v1/rooms/${room.code}`, undefined, room.ps[1].token);
        expect(v.data.members).toHaveLength(n);
        expect(v.data.hostSlot).toBe(0);
        expect(v.data.state).toBe('started');
      }
      console.log(`cadence ${n} players:`, JSON.stringify(out));
      expect(out.old.perPlayerHour).toBeGreaterThan(400);
      expect(out.new.perPlayerHour).toBeLessThan(60);
      expect(out.new.host).toBeLessThan(75);   // the host's keep-alive: one held poll a minute
      expect(out.new.guest).toBeLessThan(16);  // a guest: every 5 minutes
    });
  }

  it('forming rooms: held polls of 25 s (was 8 s) cost 144 requests an hour', async () => {
    const presence = new Map(), direct = new Map();
    const s = makeSmg(NOT_LIMITED, { deps: { presence, direct } });
    const ps = [await smgSignUp(s, 'FormHost'), await smgSignUp(s, 'FormGuest')];
    const code = (await s.call('POST', '/v1/rooms', {}, ps[0].token)).data.code;
    await s.call('POST', '/v1/rooms/join', { code }, ps[1].token);
    const room = { ps, code };
    const models = ps.map((_, slot) => consoleModel(s, presence, room, slot, { newClient: true, linksUp: () => 0 }));
    await run(s, presence, room, models, HOUR);
    for (const x of models) { expect(x.requests).toBeGreaterThan(120); expect(x.requests).toBeLessThan(160); expect(x.plans[1].holdSec).toBe(25); }
  });
});

describe('losses are noticed at once', () => {
  it('the host dies: guests report the lost link at once and the lowest guest becomes host within the old 20 s limit (not minutes)', async () => {
    const presence = new Map(), direct = new Map();
    const s = makeSmg(NOT_LIMITED, { deps: { presence, direct } });
    const room = await startedRoom(s, 4);
    let hostDeadAt = 0;
    const linksUp = (slot) => {
      const all = 0xf & ~(1 << slot);
      return hostDeadAt && s.clock.now >= hostDeadAt + 3000 && slot !== 0 ? all & ~1 : all; // guests notice the silent host after ~3 s of heartbeats
    };
    const models = room.ps.map((_, slot) => consoleModel(s, presence, room, slot, { newClient: true, linksUp }));
    await run(s, presence, room, models, 20 * 60 * SEC); // twenty quiet minutes
    hostDeadAt = s.clock.now; models[0].dead = true;
    // a client re-asks at once when it loses a peer (client side: np_signal_http.cpp); the model does the same
    const t0 = s.clock.now;
    for (const g of models.slice(1)) g.nextAt = t0 + 3500;
    let movedAt = null;
    await run(s, presence, room, models.slice(1), 60 * SEC, {
      onTick: async (t) => { if (movedAt === null) { const v = await s.call('GET', `/v1/rooms/${room.code}`, undefined, room.ps[1].token); if (v.data.hostSlot === 1) movedAt = t; } },
    });
    expect(movedAt).not.toBeNull();
    expect(movedAt - t0).toBeLessThanOrEqual(25 * SEC);
  });
  it('a guest that stopped heartbeating with the host is dropped within a minute, while the others stay', async () => {
    const presence = new Map(), direct = new Map();
    const s = makeSmg(NOT_LIMITED, { deps: { presence, direct } });
    const room = await startedRoom(s, 3);
    let deadAt = 0;
    const linksUp = (slot) => (slot === 0 && deadAt && s.clock.now >= deadAt + 3000 ? 0b010 : 0xf & ~(1 << slot) & 0b111);
    const models = room.ps.map((_, slot) => consoleModel(s, presence, room, slot, { newClient: true, linksUp }));
    await run(s, presence, room, models, 10 * 60 * SEC);
    deadAt = s.clock.now; models[2].dead = true;
    models[0].nextAt = s.clock.now + 3500;
    await run(s, presence, room, models, 80 * SEC);
    const v = await s.call('GET', `/v1/rooms/${room.code}`, undefined, room.ps[1].token);
    expect(v.data.members.map((m) => m.slot)).toEqual([0, 1]);
  });
  it('the hold is the service\'s: a client asking for 55 s in a forming room is held no longer than the plan says; an old client is held at most 8 s', async () => {
    const s = makeSmg({ ...NOT_LIMITED, POLL_FORMING_HOLD_SEC: '1' });
    const ps = [await smgSignUp(s, 'HoldHost'), await smgSignUp(s, 'HoldGuest')];
    const code = (await s.call('POST', '/v1/rooms', {}, ps[0].token)).data.code;
    const v = (await s.call('GET', `/v1/rooms/${code}`, undefined, ps[0].token)).data.version;
    const t0 = Date.now();
    const r = await s.call('GET', `/v1/rooms/${code}?v=${v}&wait=55&p=1`, undefined, ps[0].token);
    expect(r.data.changed).toBe(false);
    expect(r.data.poll.holdSec).toBe(1);
    expect(Date.now() - t0).toBeLessThan(2500);
    // 21 s is only accepted with the plan (old clients: 1..20 s as before)
    const t1 = Date.now();
    await s.call('GET', `/v1/rooms/${code}?v=${v}&wait=21`, undefined, ps[0].token);
    expect(Date.now() - t1).toBeLessThan(500);
  });
  it('a client that does not ask for the plan (an old build) is served exactly as before', async () => {
    const s = makeSmg(NOT_LIMITED);
    const room = await startedRoom(s, 2);
    const r = await s.call('GET', `/v1/rooms/${room.code}`, undefined, room.ps[1].token);
    expect(r.data.poll).toBeUndefined();
  });
});
