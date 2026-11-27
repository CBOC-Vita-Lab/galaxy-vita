// GAME=smg at the edge and in the app: the game header, the route table (no ranked / ranking), version gate, info, the msc default.
import { describe, it, expect } from 'vitest';
import { edgeCheck, SMG_ROUTE_META, ROUTE_META } from '../../src/core/edge.js';
import { makeServer } from '../helpers.js';
import { makeSmg, smgSignUp } from '../smg-helpers.js';
import { loadConfig } from '../../src/core/config.js';

const TOKEN = 'a'.repeat(43);
const req = (method, path, headers = {}) => new Request(`https://x.invalid${path}`, { method, headers });
const SMG = { GAME: 'smg', REQUIRE_APP_VERSION: '0' };

describe('X-Msc-Game', () => {
  it('edge: absent = msc; wrong game = 400 game_mismatch; malformed = 400 bad_game; open routes accept no header', () => {
    const h = { authorization: `Bearer ${TOKEN}` };
    expect(edgeCheck(req('GET', '/v1/me', h), SMG)).toMatchObject({ status: 400, code: 'game_mismatch' });
    expect(edgeCheck(req('GET', '/v1/me', { ...h, 'x-msc-game': 'msc' }), SMG)).toMatchObject({ status: 400, code: 'game_mismatch' });
    expect(edgeCheck(req('GET', '/v1/me', { ...h, 'x-msc-game': 'smg' }), SMG)).toBeNull();
    expect(edgeCheck(req('GET', '/v1/me', { ...h, 'x-msc-game': 'SMG' }), SMG)).toMatchObject({ status: 400, code: 'bad_game' });
    expect(edgeCheck(req('GET', '/v1/info'), SMG)).toBeNull();
    expect(edgeCheck(req('GET', '/times'), SMG)).toBeNull();
    // Strikers deployment: unchanged without the header, refuses a Galaxy client
    expect(edgeCheck(req('GET', '/v1/me', h), {})).toBeNull();
    expect(edgeCheck(req('GET', '/v1/me', { ...h, 'x-msc-game': 'smg' }), {})).toMatchObject({ status: 400, code: 'game_mismatch' });
    // on Cloudflare GAME is the Durable Object binding: SERVICE_GAME carries the game
    expect(edgeCheck(req('GET', '/v1/me', h), { GAME: {}, SERVICE_GAME: 'smg' })).toMatchObject({ code: 'game_mismatch' });
  });

  it('app: the same answers', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    expect((await s.raw('GET', '/v1/me', undefined, a.token)).data.error.code).toBe('game_mismatch');
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200);
    const m = makeServer();
    expect((await m.call('POST', '/v1/register', { name: 'Zed' }, undefined, { 'x-msc-game': 'smg' })).data.error.code).toBe('game_mismatch');
  });
});

describe('smg route table', () => {
  it('edge and app tables are identical (no drift); ranked, ranking, matches and the Strikers board do not exist', async () => {
    const s = makeSmg();
    const key = (r) => r.join(' ');
    expect(s.app.routeMeta.map(key)).toEqual(SMG_ROUTE_META.map(key));
    for (const [m, p] of [['POST', '/v1/ranked/queue'], ['POST', '/v1/rank/put'], ['POST', '/v1/rank/get'], ['GET', '/v1/me/matches'], ['GET', '/leaderboard'], ['GET', '/v1/public/leaderboard']]) {
      expect(edgeCheck(req(m, p, { authorization: `Bearer ${TOKEN}`, 'x-msc-game': 'smg' }), SMG), `${m} ${p}`).toMatchObject({ status: 404 });
      expect((await s.call(m, p)).status, `${m} ${p}`).toBe(404);
    }
    // and the Strikers table has none of the Galaxy routes
    expect(ROUTE_META.some(([, p]) => p.startsWith('/v1/friends') || p.startsWith('/v1/times') || p === '/v1/quick')).toBe(false);
  });

  it('body cap 4096 at the edge', () => {
    expect(edgeCheck(req('POST', '/v1/register', { 'content-length': '4097', 'x-msc-game': 'smg' }), SMG)).toMatchObject({ status: 413 });
    expect(edgeCheck(req('POST', '/v1/register', { 'content-length': '4096', 'x-msc-game': 'smg' }), SMG)).toBeNull();
  });
});

describe('smg config and info', () => {
  it('version gate defaults per game (3.0.0), env wins; unknown GAME refuses to start', async () => {
    expect(loadConfig({ GAME: 'smg' })).toMatchObject({ game: 'smg', minApp: '3.0.0', latestApp: '3.0.0' });
    expect(loadConfig({})).toMatchObject({ game: 'msc', minApp: '', latestApp: '' });
    expect(loadConfig({ GAME: 'smg', MIN_APP_VERSION: '3.1.0' }).minApp).toBe('3.1.0');
    expect(() => loadConfig({ GAME: 'mk' })).toThrow(/GAME/);
    const s = makeSmg();
    const old = await s.call('POST', '/v1/register', { name: 'Old' }, undefined, { 'x-msc-app': '2.2.1' });
    expect(old.status).toBe(426);
    expect(old.data.error).toMatchObject({ code: 'update_required', minVersion: '3.0.0', latestVersion: '3.0.0' });
  });

  it('/v1/info says smg and lists the Galaxy features and limits', async () => {
    const s = makeSmg();
    const i = await s.call('GET', '/v1/info');
    expect(i.data).toMatchObject({ game: 'smg', features: { rooms: true, quick: true, friends: true, times: true, ranked: false, ranking: false }, limits: { maxMembers: 4, friendsMax: 50, timesPerHour: 30, timeFloorFrames: 600, timeCapFrames: 432000 }, link: { onlineSec: 60 } });
  });

  it('kill switches use the Galaxy names', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Ana');
    const adm = (body) => s.raw('POST', '/v1/admin/switches', body, 'test-admin-token-0123456789');
    expect((await adm({ kill: ['ranked'] })).status).toBe(400);
    expect((await adm({ kill: ['times'] })).status).toBe(200);
    expect((await s.call('POST', '/v1/times', { galaxy: 1, star: 1, frames: 900, players: 1, build: '3.0.0' }, a.token)).data.error.code).toBe('service_disabled');
  });

  it('the fake server runs GAME=smg', async () => {
    const { startFake } = await import('../../fake/server.mjs');
    const f = await startFake({ httpPort: 0, https: false, env: { GAME: 'smg', REGISTER_POW_BITS: '0' } });
    try {
      const r = await fetch(`${f.httpUrl}/v1/info`);
      expect((await r.json()).game).toBe('smg');
      const reg = await fetch(`${f.httpUrl}/v1/register`, { method: 'POST', headers: { 'content-type': 'application/json', 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' }, body: '{"name":"Fake"}' });
      expect(reg.status).toBe(200);
    } finally {
      await f.close();
    }
  });
});
