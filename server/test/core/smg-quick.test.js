// GAME=smg quick match and the public room list.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';

describe('smg quick match', () => {
  it('creates a public room, then fills the oldest one of the same mode and version', async () => {
    const s = makeSmg();
    const p = [];
    for (const n of ['Ana', 'Ben', 'Cy', 'Dee', 'Eve', 'Fay']) p.push(await smgSignUp(s, n));
    const first = await s.call('POST', '/v1/quick', { mode: 'coop' }, p[0].token);
    expect(first.data).toMatchObject({ quick: 'created', visibility: 'public', mode: 'coop', maxMembers: 4, slot: 0 });
    s.advance(1000);
    const race = await s.call('POST', '/v1/quick', { mode: 'race' }, p[1].token); // other mode: own room
    expect(race.data.quick).toBe('created');
    const other = await s.call('POST', '/v1/quick', { mode: 'coop' }, p[2].token, { 'x-msc-app': '3.0.1' }); // other version: own room
    expect(other.data.quick).toBe('created');
    for (let i = 3; i < 6; i++) {
      const j = await s.call('POST', '/v1/quick', { mode: 'coop' }, p[i].token);
      expect(j.data).toMatchObject({ code: first.data.code, quick: 'joined' });
    }
    // full now (4): the next one makes a new room
    const g = await smgSignUp(s, 'Gil');
    expect((await s.call('POST', '/v1/quick', { mode: 'coop' }, g.token)).data.quick).toBe('created');
  });

  it('skips rooms whose host is gone and code-only rooms; validates the body', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const c = await smgSignUp(s, 'Cy');
    await s.call('POST', '/v1/rooms', { mode: 'coop' }, c.token); // visibility code
    const r = await s.call('POST', '/v1/quick', { mode: 'coop' }, a.token);
    s.advance(20_000); // a never polls again: lost, and alone in the room -> closed
    const j = await s.call('POST', '/v1/quick', { mode: 'coop' }, b.token);
    expect(j.data.quick).toBe('created');
    expect(j.data.code).not.toBe(r.data.code);
    expect((await s.call('POST', '/v1/quick', { mode: 'pvp' }, b.token)).data.error.code).toBe('bad_mode');
    expect((await s.call('POST', '/v1/quick', {}, b.token)).data.error.code).toBe('bad_mode');
    expect((await s.call('POST', '/v1/quick', { mode: 'coop', x: 1 }, b.token)).data.error.code).toBe('unknown_field');
  });

  it('10 quick matches per 10 minutes', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    const a = await smgSignUp(s, 'Ana');
    for (let i = 0; i < 10; i++) expect((await s.call('POST', '/v1/quick', { mode: 'coop' }, a.token)).status).toBe(200);
    const no = await s.call('POST', '/v1/quick', { mode: 'coop' }, a.token);
    expect(no.status).toBe(429);
  });

  it('GET /v1/rooms/public lists names only (no pid), joinable rooms of my version, filtered by mode', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const b = await smgSignUp(s, 'Ben');
    const c = await smgSignUp(s, 'Cy');
    await s.call('POST', '/v1/rooms', { mode: 'race', visibility: 'public', settings: { galaxy: 3, scenario: 1 } }, a.token);
    await s.call('POST', '/v1/rooms', { mode: 'coop', visibility: 'public' }, b.token);
    const all = await s.call('GET', '/v1/rooms/public', undefined, c.token);
    expect(all.data.rooms).toHaveLength(2);
    const race = await s.call('GET', '/v1/rooms/public?mode=race', undefined, c.token);
    expect(race.data.rooms).toEqual([{ code: expect.any(String), mode: 'race', state: 'open', players: 1, maxMembers: 4, host: 'Ana', names: ['Ana'], galaxy: 3, scenario: 1 }]);
    expect(JSON.stringify(all.data)).not.toMatch(/pid|203\.0|vip/);
    expect((await s.call('GET', '/v1/rooms/public?mode=x', undefined, c.token)).status).toBe(400);
    expect((await s.call('GET', '/v1/rooms/public', undefined, c.token, { 'x-msc-app': '3.1.0' })).data.rooms).toEqual([]);
  });
});
