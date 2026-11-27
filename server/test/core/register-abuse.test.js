// Finding 8: registration and quota exhaustion.
import { describe, it, expect } from 'vitest';
import { makeServer } from '../helpers.js';
import { ipKey } from '../../src/core/util.js';
import { solve } from '../../src/core/pow.js';
import { loadConfig } from '../../src/core/config.js';

const reg = (s, body, ip) => s.call('POST', '/v1/register', body, undefined, ip ? { ip } : {});

describe('IPv6 is keyed by /64', () => {
  it('ipKey', () => {
    expect(ipKey('203.0.113.7')).toBe('203.0.113.7');
    expect(ipKey('2001:db8:1:2:3:4:5:6')).toBe('v6:2001:0db8:0001:0002');
    expect(ipKey('2001:db8:1:2::9')).toBe(ipKey('2001:DB8:1:2:ffff:ffff:ffff:ffff'));
    expect(ipKey('2001:db8:1:3::9')).not.toBe(ipKey('2001:db8:1:2::9'));
    expect(ipKey('::ffff:198.51.100.4')).toBe('198.51.100.4');
    expect(ipKey('fe80::1%eth0')).toBe(ipKey('fe80::2'));
    expect(ipKey('unknown')).toBe('unknown');
    expect(ipKey('nonsense:::zz')).toMatch(/^v6:/); // never throws, never an empty key
  });
  it('one /64 shares one registration bucket however many addresses it uses; another /64 does not', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1', REGISTER_IP_BURST: '3' });
    for (let i = 0; i < 3; i++) expect((await reg(s, { name: `Six${i}x` }, `2001:db8:aaaa:1::${i + 1}`)).status).toBe(200);
    const r = await reg(s, { name: 'SixZz' }, '2001:db8:aaaa:1:dead:beef:0:99');
    expect(r.status).toBe(429);
    expect((await reg(s, { name: 'SixYy' }, '2001:db8:aaaa:2::1')).status).toBe(200);
  });
  it('an IPv4-mapped IPv6 address is the same client as the IPv4 address', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1', REGISTER_IP_BURST: '3' });
    for (let i = 0; i < 3; i++) expect((await reg(s, { name: `Mapped${i}` }, '198.51.100.4')).status).toBe(200);
    expect((await reg(s, { name: 'MappedZ' }, '::ffff:198.51.100.4')).status).toBe(429);
  });
});

describe('per-address limit tolerates a shared NAT: a burst, then a slow rate', () => {
  it('8 devices behind one address register at once; after the burst only 3 an hour', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1' });
    for (let i = 0; i < 10; i++) expect((await reg(s, { name: `Venue${i}x` }, '203.0.113.50')).status).toBe(200);
    const r = await reg(s, { name: 'VenueZz' }, '203.0.113.50');
    expect(r.status).toBe(429);
    expect(r.data.error.code).toBe('rate_limited');
    expect(r.data.error.retryAfterSec).toBeLessThanOrEqual(1200);
    s.advance(21 * 60_000);
    expect((await reg(s, { name: 'VenueYy' }, '203.0.113.50')).status).toBe(200);
    expect((await reg(s, { name: 'VenueXx' }, '203.0.113.50')).status).toBe(429); // one token per 20 minutes
    s.advance(3600_000);
    for (let i = 0; i < 3; i++) expect((await reg(s, { name: `Later${i}x` }, '203.0.113.50')).status).toBe(200);
  });
  it('is configurable', () => {
    const c = loadConfig({ REGISTER_IP_BURST: '20', REGISTER_IP_PER_HOUR: '6' }).register;
    expect([c.ipBurst, c.ipPerHour, c.powBits]).toEqual([20, 6, 0]);
  });
});

describe('invalid requests do not charge the global registration bucket', () => {
  it('50 garbage bodies from 50 addresses leave the whole daily budget intact', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1', REGISTER_MAX_PER_DAY: '5' });
    const bad = ['not json', '{"name":"x"}', '{"name":"Fine Name","extra":1}', '{"name":"Ok Name","region":99}', '[]', '{"name":"Ok Name","relayOnly":"yes"}'];
    let n = 0;
    for (let i = 0; i < 50; i++) {
      const r = await s.call('POST', '/v1/register', bad[i % bad.length], undefined, { ip: `203.0.${i}.9` });
      expect(r.status).toBe(400);
      n++;
    }
    expect(n).toBe(50);
    for (let i = 0; i < 5; i++) expect((await reg(s, { name: `Real${i}x` }, `198.51.${i}.1`)).status).toBe(200);
    expect((await reg(s, { name: 'RealZz' }, '198.51.99.1')).status).toBe(429); // the budget is 5 real registrations
  });
  it('a taken name is refused before any bucket that counts successes is charged', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1', REGISTER_MAX_PER_DAY: '2' });
    await reg(s, { name: 'Taken1' }, '198.51.100.1');
    for (let i = 0; i < 20; i++) expect((await reg(s, { name: 'taken1' }, `198.51.101.${i}`)).data.error.code).toBe('name_taken');
    expect((await reg(s, { name: 'Free1' }, '198.51.102.1')).status).toBe(200); // second of two
    expect((await reg(s, { name: 'Free2' }, '198.51.102.2')).status).toBe(429);
  });
  it('one address sending garbage is throttled by its own cheap bucket (30 an hour), not by the registration buckets', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1' });
    const codes = [];
    for (let i = 0; i < 33; i++) codes.push((await s.call('POST', '/v1/register', '{bad', undefined, { ip: '203.0.113.200' })).status);
    expect(codes.slice(0, 30).every((x) => x === 400)).toBe(true);
    expect(codes.slice(30)).toEqual([429, 429, 429]);
    expect((await reg(s, { name: 'StillOk1' }, '203.0.113.200')).status).toBe(200); // a good request from there still has its own tokens
  });
});

describe('optional proof of work on register (REGISTER_POW_BITS, off by default)', () => {
  it('off: the challenge endpoint says bits 0 and register needs nothing', async () => {
    const s = makeServer();
    expect((await s.call('GET', '/v1/register/challenge')).data).toEqual({ bits: 0 });
    expect((await reg(s, { name: 'NoPow1' })).status).toBe(200);
    expect((await reg(s, { name: 'NoPow2', pow: { challenge: 'x', nonce: 'y' } })).status).toBe(200); // ignored when off
  });
  it('on: register without proof is 428 pow_required and carries a challenge; a solved challenge registers once', async () => {
    const s = makeServer({ REGISTER_POW_BITS: '10' });
    const need = await reg(s, { name: 'Pow1xx' }, '203.0.113.9');
    expect(need.status).toBe(428);
    expect(need.data.error).toMatchObject({ code: 'pow_required', bits: 10, algo: 'sha256' });
    const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.9' })).data;
    expect(ch).toMatchObject({ bits: 10, expiresMs: s.clock.now + 120_000 });
    const nonce = await solve(ch.challenge, 10);
    const ok = await reg(s, { name: 'Pow1xx', pow: { challenge: ch.challenge, nonce } }, '203.0.113.9');
    expect(ok.status).toBe(200);
    const again = await reg(s, { name: 'Pow2xx', pow: { challenge: ch.challenge, nonce } }, '203.0.113.9');
    expect(again.status).toBe(400);
    expect(again.data.error).toMatchObject({ code: 'bad_pow', reason: 'used' });
  });
  it('is bound to the address, expires, and must really have the zero bits', async () => {
    const s = makeServer({ REGISTER_POW_BITS: '10' });
    const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.9' })).data;
    const nonce = await solve(ch.challenge, 10);
    const other = await reg(s, { name: 'Pow3xx', pow: { challenge: ch.challenge, nonce } }, '203.0.113.10');
    expect(other.data.error).toMatchObject({ code: 'bad_pow', reason: 'mac' });
    const sameNet = await reg(s, { name: 'Pow3xx', pow: { challenge: ch.challenge, nonce: nonce + 'zzzz' } }, '203.0.113.9');
    expect(sameNet.data.error).toMatchObject({ code: 'bad_pow', reason: 'hash' });
    const forged = await reg(s, { name: 'Pow3xx', pow: { challenge: ch.challenge.replace(/.$/, (c) => (c === '0' ? '1' : '0')), nonce } }, '203.0.113.9');
    expect(forged.data.error).toMatchObject({ code: 'bad_pow', reason: 'mac' });
    s.advance(121_000);
    const late = await reg(s, { name: 'Pow3xx', pow: { challenge: ch.challenge, nonce } }, '203.0.113.9');
    expect(late.data.error).toMatchObject({ code: 'bad_pow', reason: 'expired' });
    expect((await reg(s, { name: 'Pow3xx', pow: 'x' }, '203.0.113.9')).data.error).toMatchObject({ code: 'bad_pow', reason: 'format' });
  });
  it('a failed proof charges only the cheap bad-request bucket, never the global one', async () => {
    const s = makeServer({ REGISTER_POW_BITS: '8', RATE_LIMIT_SCALE: '1', REGISTER_MAX_PER_DAY: '1' });
    for (let i = 0; i < 10; i++) expect((await reg(s, { name: 'Pow4xx' }, `203.0.${i}.1`)).status).toBe(428);
    const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.77' })).data;
    const nonce = await solve(ch.challenge, 8);
    expect((await reg(s, { name: 'Pow4xx', pow: { challenge: ch.challenge, nonce } }, '203.0.113.77')).status).toBe(200);
  });
  it('the challenge endpoint is rate limited per address and follows the register kill switch', async () => {
    const s = makeServer({ REGISTER_POW_BITS: '8', RATE_LIMIT_SCALE: '1' });
    const codes = [];
    for (let i = 0; i < 62; i++) codes.push((await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.5' })).status);
    expect(codes.filter((x) => x === 429)).toHaveLength(2);
    const k = makeServer({ REGISTER_POW_BITS: '8', KILL_SWITCH: 'register' });
    expect((await k.call('GET', '/v1/register/challenge')).data.error.code).toBe('service_disabled');
  });
  it('nothing else changes: a registered player still works with the token', async () => {
    const s = makeServer({ REGISTER_POW_BITS: '6' });
    const ch = (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.8' })).data;
    const r = await s.call('POST', '/v1/register', { name: 'Powok1', pow: { challenge: ch.challenge, nonce: await solve(ch.challenge, 6) } }, undefined, { ip: '203.0.113.8' });
    expect(r.status).toBe(200);
    expect((await s.call('GET', '/v1/me', undefined, r.data.token)).data.name).toBe('Powok1');
  });
});
