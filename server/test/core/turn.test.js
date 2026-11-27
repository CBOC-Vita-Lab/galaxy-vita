import { describe, it, expect } from 'vitest';
import { makeServer, signUp, pairInRoom } from '../helpers.js';
import { udpOnly, cloudflareProvider, createProvider } from '../../src/core/turn.js';
import { loadConfig } from '../../src/core/config.js';
import { createHmac } from 'node:crypto';

async function roomWith(s, tag = '') {
  const a = await signUp(s, `Hosty${tag}`);
  const b = await signUp(s, `Guesty${tag}`);
  const { code } = await pairInRoom(s, a, b);
  return { a, b, code };
}

describe('turn credentials', () => {
  it('are minted for members of an active room, short-lived, UDP only', async () => {
    const s = makeServer();
    const { a, code } = await roomWith(s);
    const r = await s.call('POST', '/v1/turn', { room: code }, a.token);
    expect(r.status).toBe(200);
    expect(r.data.ttlSec).toBe(3600);
    expect(r.data.expiresMs).toBe(s.clock.now + 3600_000);
    const all = r.data.servers.flatMap((x) => x.urls);
    expect(all.some((u) => u.startsWith('stun:'))).toBe(true);
    expect(all.some((u) => /transport=udp/.test(u))).toBe(true);
    expect(all.some((u) => /transport=tcp|turns:/.test(u))).toBe(false);
    const withCred = r.data.servers.find((x) => x.username);
    expect(withCred.username).toBeTruthy();
    expect(withCred.credential).toBeTruthy();
  });

  it('refuses strangers, open rooms, closed rooms and bad rooms', async () => {
    const s = makeServer();
    const { a, code } = await roomWith(s);
    const c = await signUp(s, 'Third');
    expect((await s.call('POST', '/v1/turn', { room: code }, c.token)).data.error.code).toBe('turn_not_allowed');
    expect((await s.call('POST', '/v1/turn', { room: 'ZZZZZZ' }, a.token)).data.error.code).toBe('turn_not_allowed');
    expect((await s.call('POST', '/v1/turn', {}, a.token)).data.error.code).toBe('turn_not_allowed');
    const solo = await signUp(s, 'Solo');
    const open = (await s.call('POST', '/v1/rooms', undefined, solo.token)).data.code;
    expect((await s.call('POST', '/v1/turn', { room: open }, solo.token)).data.error.code).toBe('turn_not_allowed');
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).data.error.code).toBe('turn_not_allowed');
  });

  it('daily global budget and per-player cap trip with the right errors, then reset next day', async () => {
    const s = makeServer({ TURN_MAX_CREDS_PER_DAY: '3', TURN_MAX_CREDS_PER_PLAYER_DAY: '2' });
    const { a, b, code } = await roomWith(s);
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).status).toBe(200);
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).status).toBe(200);
    const p = await s.call('POST', '/v1/turn', { room: code }, a.token);
    expect(p.status).toBe(429);
    expect(p.data.error.code).toBe('turn_player_limit');
    expect((await s.call('POST', '/v1/turn', { room: code }, b.token)).status).toBe(200);
    const g = await s.call('POST', '/v1/turn', { room: code }, b.token);
    expect(g.status).toBe(503);
    expect(g.data.error.code).toBe('turn_budget');
    s.advance(24 * 3600_000);
    const fresh = await roomWith(s, '2');
    expect((await s.call('POST', '/v1/turn', { room: fresh.code }, fresh.b.token)).status).toBe(200);
  });

  it('monthly byte budget (reported usage) trips the breaker; reports are capped', async () => {
    // TURN_MAX_KBPS is raised so the per-credential clamp (see turn-abuse.test.js) does not hide the per-report MB cap
    const s = makeServer({ TURN_MONTHLY_BUDGET_MB: '250', TURN_MAX_REPORT_MB: '200', TURN_MAX_KBPS: '100000' });
    const { a, code } = await roomWith(s);
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).status).toBe(200); // usage is only accepted after a mint
    expect((await s.call('POST', '/v1/turn/usage', { bytes: 2 ** 39 }, a.token)).status).toBe(200); // capped to 200 MB
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).status).toBe(200);
    await s.call('POST', '/v1/turn/usage', { bytes: 100 * 1048576 }, a.token);
    const r = await s.call('POST', '/v1/turn', { room: code }, a.token);
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('turn_budget');
    expect((await s.call('POST', '/v1/turn/usage', { bytes: -1 }, a.token)).status).toBe(400);
    expect((await s.call('POST', '/v1/turn/usage', { bytes: 1.5 }, a.token)).status).toBe(400);
  });

  it('kill switch and missing provider', async () => {
    const s = makeServer({ KILL_SWITCH: 'turn' });
    const { a, code } = await roomWith(s);
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).data.error.code).toBe('service_disabled');
    expect((await s.call('GET', '/v1/info')).data.features.turn).toBe(false);
    const none = makeServer({ TURN_PROVIDER: '' });
    const r = await roomWith(none);
    expect((await none.call('POST', '/v1/turn', { room: r.code }, r.a.token)).data.error.code).toBe('turn_unavailable');
    expect((await none.call('GET', '/v1/info')).data.features.turn).toBe(false);
  });

  it('a failing provider returns 502 and gives the reservation back', async () => {
    let fail = true;
    const provider = { mint: async () => { if (fail) throw new Error('upstream down'); return { servers: [{ urls: ['stun:x:1'] }] }; } };
    const s = makeServer({ TURN_MAX_CREDS_PER_PLAYER_DAY: '1' }, { deps: { turnProvider: provider } });
    const { a, code } = await roomWith(s);
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).status).toBe(502);
    fail = false;
    expect((await s.call('POST', '/v1/turn', { room: code }, a.token)).status).toBe(200); // not counted against the cap
  });
});

describe('provider plumbing', () => {
  it('udpOnly drops tcp and tls urls and keeps credentials only where present', () => {
    const out = udpOnly([
      { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
      { urls: ['turn:t:3478?transport=udp', 'turn:t:3478?transport=tcp', 'turns:t:5349?transport=tcp'], username: 'u', credential: 'c' },
      { urls: ['turns:t:443?transport=tcp'], username: 'u', credential: 'c' },
    ]);
    expect(out).toEqual([
      { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
      { urls: ['turn:t:3478?transport=udp'], username: 'u', credential: 'c' },
    ]);
  });

  it('cloudflare provider calls the key endpoint with the secret and never returns it', async () => {
    const calls = [];
    const fakeFetch = async (url, init) => {
      calls.push({ url, init });
      return new Response(JSON.stringify({ iceServers: [{ urls: ['stun:stun.cloudflare.com:3478'] }, { urls: ['turn:turn.cloudflare.com:3478?transport=udp'], username: 'U', credential: 'C' }] }));
    };
    const cfg = loadConfig({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid123', TURN_KEY_API_TOKEN: 'sekret-token' }).turn;
    const s = makeServer({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid123', TURN_KEY_API_TOKEN: 'sekret-token' }, { deps: { fetchImpl: fakeFetch } });
    const { a, code } = await roomWith(s);
    const r = await s.call('POST', '/v1/turn', { room: code }, a.token);
    expect(r.status).toBe(200);
    expect(calls).toHaveLength(1);
    expect(calls[0].url).toBe('https://rtc.live.cloudflare.com/v1/turn/keys/keyid123/credentials/generate-ice-servers');
    expect(calls[0].init.headers.authorization).toBe('Bearer sekret-token');
    expect(JSON.parse(calls[0].init.body)).toEqual({ ttl: 3600, customIdentifier: 'galaxy' });
    expect(JSON.stringify(r.data)).not.toContain('sekret-token');
    expect(JSON.stringify(r.data)).not.toContain('keyid123');
    expect(r.data.servers[1]).toEqual({ urls: ['turn:turn.cloudflare.com:3478?transport=udp'], username: 'U', credential: 'C' });
    // object-shaped answers (the older "generate" call) are accepted too
    const p = cloudflareProvider(cfg, async () => new Response(JSON.stringify({ iceServers: { urls: ['turn:x:1?transport=udp'], username: 'a', credential: 'b' } })));
    expect((await p.mint(60)).servers).toHaveLength(1);
    // upstream error -> 502 turn_upstream
    const bad = cloudflareProvider(cfg, async () => new Response('no', { status: 403 }));
    await expect(bad.mint(60)).rejects.toMatchObject({ status: 502, code: 'turn_upstream' });
  });

  it('createProvider needs full credentials for cloudflare', () => {
    expect(createProvider(loadConfig({ TURN_PROVIDER: 'cloudflare' }).turn)).toBeNull();
    expect(createProvider(loadConfig({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'k', TURN_KEY_API_TOKEN: 't' }).turn)).not.toBeNull();
    expect(createProvider(loadConfig({ TURN_PROVIDER: 'stub' }).turn)).not.toBeNull();
    expect(createProvider(loadConfig({}).turn)).toBeNull();
  });

  it('the stub signs REST-scheme credentials for a local TURN server when given URLs and a secret', async () => {
    const s = makeServer({ TURN_PROVIDER: 'stub', TURN_STUB_URLS: 'turn:198.51.100.9:3478?transport=udp, turn:198.51.100.9:53?transport=udp', TURN_STUB_SECRET: 'sekrit' });
    const { a, code } = await roomWith(s, 'S');
    const r = await s.call('POST', '/v1/turn', { room: code }, a.token);
    expect(r.status).toBe(200);
    const t = r.data.servers.find((x) => x.username);
    expect(t.urls).toEqual(['turn:198.51.100.9:3478?transport=udp', 'turn:198.51.100.9:53?transport=udp']);
    expect(t.username).toMatch(/^\d+:stub-/);
    expect(Math.abs(Number(t.username.split(':')[0]) - (Math.floor(Date.now() / 1000) + 3600))).toBeLessThan(5);   // (real clock, like the stub always was)
    expect(t.credential).toBe(createHmac('sha1', 'sekrit').update(t.username).digest('base64'));
  });
});
