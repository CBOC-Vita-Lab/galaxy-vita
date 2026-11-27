// The shipped per-star floors (src/core/smg_floors.js): table sanity, the keys match the client's galaxy list, and a time under a
// star's floor is refused (too_fast): it never reaches a board.
import { describe, it, expect } from 'vitest';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { makeSmg, smgSignUp } from '../smg-helpers.js';
import {
  DEFAULT_STAR_FLOORS, BEST_SECONDS, GALAXIES, FLOOR_FACTOR, GLOBAL_FLOOR_FRAMES, NO_DATA_GALAXIES, buildFloors,
} from '../../src/core/smg_floors.js';

const body = (o) => ({ galaxy: 0, star: 3, frames: 3600, players: 1, build: '3.0.0', ...o });

describe('smg star floors: table', () => {
  it('has well-formed keys and values, never below the global floor or above the cap', () => {
    const keys = Object.keys(DEFAULT_STAR_FLOORS);
    expect(keys.length).toBeGreaterThan(40);
    for (const k of keys) {
      const [g, s] = k.split(':').map(Number);
      expect(g).toBeGreaterThanOrEqual(0);
      expect(g).toBeLessThan(GALAXIES.length);
      expect(s).toBeGreaterThanOrEqual(1);
      expect(s).toBeLessThanOrEqual(8);
      expect(DEFAULT_STAR_FLOORS[k]).toBeGreaterThan(GLOBAL_FLOOR_FRAMES);
      expect(DEFAULT_STAR_FLOORS[k]).toBeLessThan(2 * 3600 * 60);
    }
  });

  it('every floor is FLOOR_FACTOR of the best known time (in 60 fps frames), and no source row is lost', () => {
    const seen = new Set();
    for (const [g, s, sec] of BEST_SECONDS) {
      expect(seen.has(`${g}:${s}`)).toBe(false); // one source row per star
      seen.add(`${g}:${s}`);
      const want = Math.max(GLOBAL_FLOOR_FRAMES, Math.round(sec * 60 * FLOOR_FACTOR));
      expect(DEFAULT_STAR_FLOORS[`${g}:${s}`] ?? GLOBAL_FLOOR_FRAMES).toBe(want);
      expect(want).toBeLessThanOrEqual(Math.round(sec * 60 * 0.85)); // never stricter than the brief's 85 %
    }
    expect(buildFloors(0.85)['1:1']).toBe(Math.round(62 * 60 * 0.85));
  });

  it('the galaxy index order is the client list (MpMessages.cpp kGalaxies)', () => {
    const f = fileURLToPath(new URL('../../../decomp/src/Game/MP/MpMessages.cpp', import.meta.url));
    if (!existsSync(f)) return; // server checked out alone
    const src = readFileSync(f, 'utf8');
    const block = src.slice(src.indexOf('kGalaxies[] = {'), src.indexOf('constexpr int kGalaxyCount'));
    const stages = [...block.matchAll(/"([A-Za-z0-9]+Galaxy)"/g)].map((m) => m[1]);
    expect(stages).toEqual(GALAXIES.map((g) => g[0]));
  });

  it('lists the galaxies without any data (they keep the global floor)', () => {
    expect(NO_DATA_GALAXIES.length).toBeGreaterThan(0);
    for (const g of NO_DATA_GALAXIES) expect(Object.keys(DEFAULT_STAR_FLOORS).some((k) => k.startsWith(`${g}:`))).toBe(false);
  });
});

describe('smg star floors: enforcement', () => {
  it('a time under the star floor is refused and never shown; the floor itself is accepted', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const floor = DEFAULT_STAR_FLOORS['0:3']; // Good Egg, King Kaliente's Battle Fleet
    expect(floor).toBeGreaterThan(GLOBAL_FLOOR_FRAMES);
    const fast = await s.call('POST', '/v1/times', body({ frames: 601 }), a.token); // a "first 10 s" submission
    expect(fast.status).toBe(400);
    expect(fast.data.error).toMatchObject({ code: 'bad_time', reason: 'too_fast', field: 'frames' });
    expect((await s.call('POST', '/v1/times', body({ frames: floor - 1 }), a.token)).data.error.reason).toBe('too_fast');
    const board = await s.call('POST', '/v1/times/get', { galaxy: 0, star: 3 }, a.token);
    expect(board.data.rows).toEqual([]);
    expect((await s.raw('GET', '/v1/public/times?galaxy=0&star=3')).data.rows).toEqual([]);
    expect((await s.call('POST', '/v1/times', body({ frames: floor }), a.token)).data).toMatchObject({ ok: true, rank: 1 });
    s.advance(61_000); // the public page is memoised for 60 s
    expect((await s.raw('GET', '/v1/public/times?galaxy=0&star=3')).data.rows[0].frames).toBe(floor);
  });

  it('a star without data keeps the global 10 s floor; SMG_STAR_FLOORS overrides one star', async () => {
    const s = makeSmg({ SMG_STAR_FLOORS: '{"0:3":1000}' });
    const a = await smgSignUp(s, 'Ana');
    expect((await s.call('POST', '/v1/times', body({ galaxy: 5, frames: 599 }), a.token)).data.error.reason).toBe('too_fast'); // Sweet Sweet: no data
    expect((await s.call('POST', '/v1/times', body({ galaxy: 5, frames: 600 }), a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/times', body({ frames: 999 }), a.token)).data.error.reason).toBe('too_fast'); // 0:3 now 1000, below the table's
    expect((await s.call('POST', '/v1/times', body({ frames: 1000 }), a.token)).status).toBe(200);
  });
});
