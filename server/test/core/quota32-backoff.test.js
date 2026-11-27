// quota32: a started room whose links never settle (relay / unlinked) used to re-ask every ~8.3 s for the whole session. The service's poll
// plan now backs the held poll off while nothing changes (8 -> 15 after 30 s -> 30 after 90 s) and snaps back to 8 s on any change.
// A virtual-time model like svcfix2-cadence.test.js: a held poll is a request whose answer arrives `hold` seconds later, with the presence
// stamps the application's ticker makes every 4 s.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, ep } from '../smg-helpers.js';

const SEC = 1000;
const NOT_LIMITED = { RATE_LIMIT_SCALE: '1000', DAILY_REQUEST_BUDGET: '0' };

async function startedRoom(s, n) {
  const ps = []; for (let i = 0; i < n; i++) ps.push(await smgSignUp(s, `Bo${i}Name`));
  const code = (await s.call('POST', '/v1/rooms', {}, ps[0].token)).data.code;
  for (const p of ps.slice(1)) await s.call('POST', '/v1/rooms/join', { code }, p.token);
  for (const [i, p] of ps.entries()) await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(i + 1), p.token);
  for (const p of ps) await s.call('POST', `/v1/rooms/${code}/started`, undefined, p.token);
  return { ps, code };
}

function model(s, presence, room, slot, mask = () => 0) {
  const m = { slot, n: 0, version: 0, nextAt: s.clock.now, holdFrom: 0, holdUntil: 0, dead: false, holds: [] };
  m.step = async () => {
    const t = s.clock.now;
    const r = await s.call('GET', `/v1/rooms/${room.code}?v=${m.version || 1}&p=1&l=${mask(slot).toString(16)}`, undefined, room.ps[slot].token);
    m.n++;
    m.version = r.data.version;
    m.holds.push(r.data.peersQuiet ? 'q' : r.data.poll.holdSec);
    const hold = r.data.peersQuiet ? 0 : r.data.poll.holdSec;
    m.holdFrom = t; m.holdUntil = t + hold * SEC;
    m.nextAt = t + hold * SEC + r.data.poll.nextSec * SEC;
    return r;
  };
  return m;
}

async function run(s, presence, room, models, ms) {
  const end = s.clock.now + ms;
  for (;;) {
    const live = models.filter((x) => !x.dead);
    const next = Math.min(...live.map((x) => x.nextAt));
    if (!isFinite(next) || next > end) break;
    for (const x of live) for (let ts = Math.ceil(x.holdFrom / 4000) * 4000; ts <= Math.min(next, x.holdUntil); ts += 4000) {
      if (ts >= x.holdFrom) presence.set(`${room.code}:${x.slot}`, Math.max(presence.get(`${room.code}:${x.slot}`) || 0, ts));
    }
    s.clock.now = Math.max(s.clock.now, next);
    for (const x of live.filter((y) => y.nextAt <= s.clock.now)) await x.step();
  }
  s.clock.now = end;
}

const fresh = () => { const presence = new Map(), direct = new Map(); return { presence, direct, s: makeSmg(NOT_LIMITED, { deps: { presence, direct } }) }; };

describe('quota32: unsettled-link polling backs off', () => {
  it('2 players that never link: holds climb 8 -> 15 -> 30 and a 30-minute session costs under 90 requests a player (was ~215)', async () => {
    const { s, presence } = fresh();
    const room = await startedRoom(s, 2);
    const models = room.ps.map((_, i) => model(s, presence, room, i));
    await run(s, presence, room, models, 30 * 60 * SEC);
    for (const m of models) {
      expect(m.holds.slice(0, 3)).toEqual([8, 8, 8]);
      expect(m.holds).toContain(15);
      expect(m.holds[m.holds.length - 1]).toBe(30);
      expect(Math.max(...m.holds)).toBe(30);
      expect(m.n).toBeLessThan(90);
      expect(m.n).toBeGreaterThan(55);
    }
    console.log('quota32 busy 30 min requests/player:', models.map((m) => m.n).join(','));
  });

  it('a version change (somebody posts an endpoint) snaps the hold back to 8 s, then it climbs again', async () => {
    const { s, presence } = fresh();
    const room = await startedRoom(s, 2);
    const models = room.ps.map((_, i) => model(s, presence, room, i));
    await run(s, presence, room, models, 5 * 60 * SEC);
    expect(models[1].holds[models[1].holds.length - 1]).toBe(30);
    await s.call('POST', `/v1/rooms/${room.code}/endpoint`, ep(9), room.ps[0].token);
    const k = models[1].holds.length;
    await run(s, presence, room, models, 2 * 60 * SEC);
    const after = models[1].holds.slice(k);
    expect(after[0]).toBe(8);
    expect(after[after.length - 1]).toBe(30);
  });

  it('a different direct-link report snaps back to 8 s', async () => {
    const { s, presence } = fresh();
    const room = await startedRoom(s, 3);
    let up = 0;
    const models = room.ps.map((_, i) => model(s, presence, room, i, (slot) => (slot === 1 ? up : 0)));
    await run(s, presence, room, models, 4 * 60 * SEC);
    const g = models[1];
    expect(g.holds[g.holds.length - 1]).toBe(30);
    up = 0b0001;   // the guest links to the host: a new report
    const k = g.holds.length;
    g.nextAt = s.clock.now;
    await run(s, presence, room, [g], 1);
    expect(g.holds[k]).toBe(8);
  });

  it('a backed-off held poll ends early (about one ticker step) when the other member goes quiet, and the next plan is the base hold', async () => {
    const presence = new Map(), direct = new Map();
    const s = makeSmg({ ...NOT_LIMITED, POLL_BUSY_HOLD_SEC: '1', POLL_BUSY_MID_HOLD_SEC: '8', POLL_BUSY_MID_AFTER_SEC: '1' }, { deps: { presence, direct, heldTickMs: 50 } });
    const room = await startedRoom(s, 2);
    const v = (await s.call('GET', `/v1/rooms/${room.code}?v=1&p=1&l=0`, undefined, room.ps[1].token)).data.version;
    s.clock.now += 3000;   // idle for 3 s on the virtual clock: the plan is now the 8 s hold
    const t0 = Date.now();
    const held = s.call('GET', `/v1/rooms/${room.code}?v=${v}&p=1&l=0&wait=30`, undefined, room.ps[1].token);
    await new Promise((r) => setTimeout(r, 200));
    s.clock.now += 15000;   // the host has not called for 18 s of virtual time: quiet
    const r = await held;
    expect(Date.now() - t0).toBeLessThan(3000);   // not the 8 s hold
    expect(r.data.changed).toBe(false);
    expect(r.data.peersQuiet).toBe(true);   // the answer says so (the client goes to the fast rhythm)
    expect(r.data.poll.holdSec).toBe(1);
    const again = await s.call('GET', `/v1/rooms/${room.code}?v=${v}&p=1&l=0`, undefined, room.ps[1].token);
    expect(again.data.peersQuiet).toBe(true);
  });

  it('fix33 (S1): a settled host\'s 55 s hold is not ended early by a quiet guest (the guest polls every 300 s): no extra requests', async () => {
    const presence = new Map(), direct = new Map();
    const s = makeSmg({ ...NOT_LIMITED }, { deps: { presence, direct, heldTickMs: 50 } });
    const room = await startedRoom(s, 2);
    const g = await s.call('GET', `/v1/rooms/${room.code}?v=1&p=1&l=1`, undefined, room.ps[1].token);
    const h0 = await s.call('GET', `/v1/rooms/${room.code}?v=1&p=1&l=2`, undefined, room.ps[0].token);
    expect(h0.data.poll).toEqual({ holdSec: 55, nextSec: 1 });
    const v = h0.data.version;
    let done = null;
    const held = s.call('GET', `/v1/rooms/${room.code}?v=${v}&p=1&l=2&wait=55`, undefined, room.ps[0].token).then((r) => { done = r; return r; });
    await new Promise((r) => setTimeout(r, 150));
    for (let k = 0; k < 4; k++) {   // 4 s of virtual time per step: 16 s > the 12 s quiet limit; the guest does not call (settled: 300 s)
      s.clock.now += 4000;
      await new Promise((r) => setTimeout(r, 120));
    }
    expect(done).toBe(null);   // still held: it was not woken by "quiet"
    await s.call('POST', `/v1/rooms/${room.code}/endpoint`, ep(9), room.ps[1].token);   // a real change ends it at once
    const r = await held;
    expect(r.data.changed).not.toBe(false);
  });

  it('a settled direct link still gets the settled plan (55 s host / 300 s guest), untouched by the backoff', async () => {
    const { s } = fresh();
    const room = await startedRoom(s, 2);
    const g = await s.call('GET', `/v1/rooms/${room.code}?v=1&p=1&l=1`, undefined, room.ps[1].token);
    const h = await s.call('GET', `/v1/rooms/${room.code}?v=1&p=1&l=2`, undefined, room.ps[0].token);
    expect(g.data.poll).toEqual({ holdSec: 0, nextSec: 300 });
    expect(h.data.poll).toEqual({ holdSec: 55, nextSec: 1 });
  });
});
