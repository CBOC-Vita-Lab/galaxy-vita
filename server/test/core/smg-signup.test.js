// GAME=smg sign-up (SMG-PROTOCOL.txt "Sign-up"): proof of work whose difficulty rises per address, a daily cap kept in storage (a
// restarted object does not hand out a second day's worth), a per-network cap, display names (duplicates allowed), quiet renames,
// eviction of the longest-unseen account when the service is full, and expiry of accounts unseen for a year.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';
import { createApp } from '../../src/core/app.js';
import { solve } from '../../src/core/pow.js';

const DAY = 86400_000;

async function registerWithPow(s, name, ip) {
  const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip })).data;
  const body = { name };
  if (ch.bits) body.pow = { challenge: ch.challenge, nonce: await solve(ch.challenge, ch.bits) };
  const r = await s.call('POST', '/v1/register', body, undefined, { ip });
  return { ...r, bits: ch.bits };
}

describe('smg sign-up: proof of work', () => {
  it('the first registrations of an address cost the base difficulty, then one more bit each, capped (small numbers: the JS solver is slow)', async () => {
    const s = makeSmg({ REGISTER_POW_BITS: '6', REGISTER_POW_FREE: '2', REGISTER_POW_MAX_BITS: '8' });
    const bits = [];
    for (let i = 0; i < 5; i++) {
      const r = await registerWithPow(s, `Venue${i}`, '203.0.113.7');
      expect(r.status).toBe(200);
      bits.push(r.bits);
    }
    expect(bits).toEqual([6, 6, 7, 8, 8]);
    // another address starts at the bottom; a day later the first one does too
    expect((await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.8' })).data.bits).toBe(6);
    s.advance(DAY + 1);
    expect((await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.7' })).data.bits).toBe(6);
  });

  it('the difficulty is bound to the challenge: editing the bits breaks the MAC; a challenge is used once', async () => {
    const s = makeSmg({ REGISTER_POW_BITS: '12' });
    const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.9' })).data;
    const cheap = ch.challenge.replace('.12.', '.1.');
    const r = await s.call('POST', '/v1/register', { name: 'Cheat', pow: { challenge: cheap, nonce: await solve(cheap, 1) } }, undefined, { ip: '203.0.113.9' });
    expect(r.data.error).toMatchObject({ code: 'bad_pow', reason: 'mac' });
    const nonce = await solve(ch.challenge, 12);
    expect((await s.call('POST', '/v1/register', { name: 'Fair', pow: { challenge: ch.challenge, nonce } }, undefined, { ip: '203.0.113.9' })).status).toBe(200);
    expect((await s.call('POST', '/v1/register', { name: 'Fair2', pow: { challenge: ch.challenge, nonce } }, undefined, { ip: '203.0.113.9' })).data.error.reason).toBe('used');
  });
});

describe('smg sign-up: caps', () => {
  it('the daily cap is stored: a restarted object (deploy, eviction) does not reset it; the next UTC day it opens again', async () => {
    const s = makeSmg({ REGISTER_MAX_PER_DAY: '3', REGISTER_IP_BURST: '50', REGISTER_NET_PER_DAY: '0' });
    for (let i = 0; i < 3; i++) await smgSignUp(s, `Day${i}`);
    const full = await s.call('POST', '/v1/register', { name: 'Late' });
    expect(full.status).toBe(429);
    expect(full.data.error).toMatchObject({ code: 'rate_limited', reason: 'register_daily' });
    expect(Number(full.headers.get('retry-after'))).toBeGreaterThan(0);
    // a fresh application over the same database = the Durable Object after a restart
    const again = createApp({ sql: s.store.sql, tx: s.store.tx, env: s.env, now: () => s.clock.now });
    const resp = await again.handle(new Request('https://test.invalid/v1/register', {
      method: 'POST', body: '{"name":"Late"}',
      headers: { 'content-type': 'application/json', 'x-msc-game': 'smg', 'x-msc-app': '3.0.0', 'cf-connecting-ip': '198.51.100.77' },
    }));
    expect(resp.status).toBe(429);
    s.advance(DAY);
    expect((await s.call('POST', '/v1/register', { name: 'Late' })).status).toBe(200);
  });

  it('per network (IPv4 /24, IPv6 /48) per day, beyond the per-address burst', async () => {
    const s = makeSmg({ REGISTER_NET_PER_DAY: '4', REGISTER_IP_BURST: '50', RATE_LIMIT_SCALE: '1' });
    let ok = 0;
    for (let i = 0; i < 6; i++) ok += (await s.call('POST', '/v1/register', { name: `Net${i}` }, undefined, { ip: `203.0.113.${10 + i}` })).status === 200 ? 1 : 0;
    expect(ok).toBe(4);
    expect((await s.call('POST', '/v1/register', { name: 'Other' }, undefined, { ip: '203.0.114.1' })).status).toBe(200);
    let v6 = 0;
    for (let i = 0; i < 6; i++) v6 += (await s.call('POST', '/v1/register', { name: `Six${i}` }, undefined, { ip: `2001:db8:5:${i}::1` })).status === 200 ? 1 : 0;
    expect(v6).toBe(4); // six different /64s of one /48
  });

  it('full: the account unseen longest (60 days or more) makes room; otherwise server_full with a retry hint', async () => {
    const s = makeSmg({ MAX_PLAYERS: '3', REGISTER_IP_BURST: '50', REGISTER_NET_PER_DAY: '0' });
    const old = await smgSignUp(s, 'Old');
    s.advance(30 * DAY);
    const mid = await smgSignUp(s, 'Mid');
    s.advance(6 * 60_000); // (a console that was used after its first 5 minutes: not a silent account, svc3-farming.test.js)
    await s.call('GET', '/v1/me', undefined, mid.token);
    const x = await smgSignUp(s, 'Third');
    await s.call('POST', '/v1/friends', { code: old.code }, x.token);
    const refused = await s.call('POST', '/v1/register', { name: 'Fourth' });
    expect(refused.status).toBe(503);
    expect(refused.data.error).toMatchObject({ code: 'server_full', retryAfterSec: 3600 });
    s.advance(31 * DAY); // Old unseen for 61 days, Mid 31
    await s.call('GET', '/v1/me', undefined, x.token);
    await s.call('GET', '/v1/me', undefined, mid.token); // (svcfix3: an account with no friend unseen 14 days would go before Old: Mid is seen again)
    const fourth = await s.call('POST', '/v1/register', { name: 'Fourth' });
    expect(fourth.status).toBe(200);
    expect((await s.call('GET', '/v1/me', undefined, old.token)).status).toBe(401); // gone, with its friend rows
    expect((await s.call('GET', '/v1/me', undefined, mid.token)).status).toBe(200);
    expect((await s.call('GET', '/v1/friends', undefined, x.token)).data.friends).toEqual([]);
  });

  it('an account unseen for a year expires with everything it owns; one seen meanwhile stays', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Gone');
    const b = await smgSignUp(s, 'Stays');
    await s.call('POST', '/v1/times', { galaxy: 1, star: 1, frames: 5000, players: 1, build: '3.0.0' }, a.token);
    for (let d = 0; d < 366; d += 100) { s.advance(100 * DAY); await s.call('GET', '/v1/me', undefined, b.token); }
    s.app.sweep();
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(401);
    expect((await s.call('GET', '/v1/me', undefined, b.token)).status).toBe(200);
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM star_times WHERE pid = ?`, a.pid).toArray()[0].n).toBe(0);
  });
});

describe('smg sign-up: display names', () => {
  it('two players may both be "Luigi"; renames are quiet (no cooldown, no uniqueness); the friend code tells them apart', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Luigi');
    const b = await smgSignUp(s, 'Luigi');
    expect(a.code).not.toBe(b.code);
    const c = await smgSignUp(s, 'Mario');
    expect((await s.call('PUT', '/v1/me', { name: 'Luigi' }, c.token)).data.name).toBe('Luigi');
    expect((await s.call('PUT', '/v1/me', { name: 'Mario' }, c.token)).data.name).toBe('Mario'); // at once again: no cooldown
    await s.call('POST', '/v1/friends', { code: a.code }, c.token);
    const list = (await s.call('GET', '/v1/friends', undefined, c.token)).data.friends;
    expect(list).toEqual([expect.objectContaining({ name: 'Luigi', code: a.code })]);
    expect((await s.call('GET', `/v1/players/${b.code}`, undefined, c.token)).data).toMatchObject({ name: 'Luigi', code: b.code });
  });
});

describe('smg sign-up: one account on two consoles (a copied account file)', () => {
  it('the console whose room the account left hears account_elsewhere (not not_member, which would make it rejoin and fight)', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Twin');
    const h1 = await smgSignUp(s, 'HostOne');
    const h2 = await smgSignUp(s, 'HostTwo');
    const r1 = (await s.call('POST', '/v1/rooms', {}, h1.token)).data.code;
    const r2 = (await s.call('POST', '/v1/rooms', {}, h2.token)).data.code;
    expect((await s.call('POST', '/v1/rooms/join', { code: r1 }, a.token)).status).toBe(200); // console 1
    expect((await s.call('POST', '/v1/rooms/join', { code: r2 }, a.token)).status).toBe(200); // console 2, same token
    const old = await s.call('GET', `/v1/rooms/${r1}`, undefined, a.token); // console 1 polls its room
    expect(old.status).toBe(409);
    expect(old.data.error.code).toBe('account_elsewhere');
    // leaving on purpose is still not_member afterwards
    await s.call('POST', `/v1/rooms/${r2}/leave`, undefined, a.token);
    expect((await s.call('GET', `/v1/rooms/${r2}`, undefined, a.token)).data.error.code).toBe('not_member');
  });
});
