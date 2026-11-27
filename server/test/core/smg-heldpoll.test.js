// Held polls (request budget): GET /v1/rooms/:code?v=<version>&wait=<s> answers when the room changes or when the time is up.
// Real (short) timers: the application awaits them; the test clock only moves the service's own notion of time.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, ep, roomOf } from '../smg-helpers.js';

describe('held polls', () => {
  it('nothing changes: the answer comes after `wait` seconds, unchanged; without wait it comes at once (old clients)', async () => {
    const s = makeSmg();
    const h = await smgSignUp(s, 'Host');
    const code = (await s.call('POST', '/v1/rooms', {}, h.token)).data.code;
    const v = (await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data.version;
    let t0 = Date.now();
    const quick = await s.call('GET', `/v1/rooms/${code}?v=${v}`, undefined, h.token);
    expect(quick.data.changed).toBe(false);
    expect(Date.now() - t0).toBeLessThan(500);
    t0 = Date.now();
    const held = await s.call('GET', `/v1/rooms/${code}?v=${v}&wait=1`, undefined, h.token);
    expect(held.data).toMatchObject({ changed: false, version: v });
    expect(Date.now() - t0).toBeGreaterThanOrEqual(950);
    expect(s.app.waiting()).toBe(0);
  });

  it('a guest joining wakes the waiting host at once with the new view', async () => {
    const s = makeSmg();
    const h = await smgSignUp(s, 'Host');
    const g = await smgSignUp(s, 'Gus');
    const code = (await s.call('POST', '/v1/rooms', {}, h.token)).data.code;
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), h.token);
    const v = (await s.call('GET', `/v1/rooms/${code}`, undefined, h.token)).data.version;
    const t0 = Date.now();
    const pending = s.call('GET', `/v1/rooms/${code}?v=${v}&wait=10`, undefined, h.token);
    await new Promise((r) => setTimeout(r, 50));
    expect(s.app.waiting()).toBe(1);
    await s.call('POST', '/v1/rooms/join', { code }, g.token);
    const woke = await pending;
    expect(Date.now() - t0).toBeLessThan(2000);
    expect(woke.data.changed).toBe(true);
    expect(woke.data.members).toHaveLength(2);
    // the guest's endpoint wakes it again; another member's plain poll does not (no version change)
    const v2 = woke.data.version;
    const p2 = s.call('GET', `/v1/rooms/${code}?v=${v2}&wait=2`, undefined, h.token);
    await new Promise((r) => setTimeout(r, 50));
    await s.call('GET', `/v1/rooms/${code}`, undefined, g.token);
    await new Promise((r) => setTimeout(r, 200));
    expect(s.app.waiting()).toBe(1); // still holding
    await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), g.token);
    expect((await p2).data.changed).toBe(true);
  });

  it('wait is bounded (1..20 s; anything else answers at once) and the number of held polls is capped', async () => {
    const s = makeSmg({}, { deps: { maxWaiters: 2 } });
    const ps = [];
    for (let i = 0; i < 3; i++) ps.push(await smgSignUp(s, `P${i}`));
    const codes = [];
    for (const p of ps) codes.push((await s.call('POST', '/v1/rooms', {}, p.token)).data.code);
    for (const bad of ['0', '21', 'x', '1e9', '-5']) {
      const t0 = Date.now();
      await s.call('GET', `/v1/rooms/${codes[0]}?v=1&wait=${bad}`, undefined, ps[0].token);
      expect(Date.now() - t0).toBeLessThan(500);
    }
    const a = s.call('GET', `/v1/rooms/${codes[0]}?v=1&wait=2`, undefined, ps[0].token);
    const b = s.call('GET', `/v1/rooms/${codes[1]}?v=1&wait=2`, undefined, ps[1].token);
    await new Promise((r) => setTimeout(r, 50));
    const t0 = Date.now();
    await s.call('GET', `/v1/rooms/${codes[2]}?v=1&wait=2`, undefined, ps[2].token); // the third: answered at once
    expect(Date.now() - t0).toBeLessThan(500);
    await Promise.all([a, b]);
    expect(s.app.waiting()).toBe(0);
  });

  it('a held poll keeps its member present (presence is noted again when it answers)', async () => {
    const s = makeSmg();
    const [h, g] = [await smgSignUp(s, 'Host'), await smgSignUp(s, 'Gus')];
    const code = await roomOf(s, [h, g]);
    const v = (await s.call('GET', `/v1/rooms/${code}`, undefined, g.token)).data.version;
    s.advance(11_000);
    await s.call('GET', `/v1/rooms/${code}?v=${v}&wait=1`, undefined, h.token);
    s.advance(10_000); // 21 s after the guest's last call, 10 s after the host's held poll ended
    const view = (await s.call('GET', `/v1/rooms/${code}`, undefined, g.token)).data;
    expect(view.members.find((m) => m.slot === 0).link).toBe('ok');
  });
});
