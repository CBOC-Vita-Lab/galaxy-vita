// svcfix2 item 5 (independent review): proof-of-work escalation bypass. Nine challenges fetched first were all issued at the base
// difficulty, and registering all nine cost 9 x base. The difficulty is now bound to the state at REDEEM time.
import { describe, it, expect } from 'vitest';
import { makeSmg } from '../smg-helpers.js';
import { solve } from '../../src/core/pow.js';

const BASE = 8; // cheap in a test; the production value is 15
const ip = '203.0.113.200';
const get = async (s, ipx = ip) => (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: ipx })).data;
const reg = async (s, name, ch, ipx = ip) => s.call('POST', '/v1/register', { name, pow: { challenge: ch.challenge, nonce: await solve(ch.challenge, ch.bits) } }, undefined, { ip: ipx });

describe('proof of work: difficulty bound at redeem time', () => {
  it('nine challenges prefetched at the base price: the fourth sign-up is refused with a harder challenge, and the old one is not spent', async () => {
    const s = makeSmg({ REGISTER_POW_BITS: String(BASE), RATE_LIMIT_SCALE: '1000', REGISTER_IP_BURST: '100', REGISTER_IP_PER_HOUR: '1000' });
    const chs = [];
    for (let i = 0; i < 9; i++) chs.push(await get(s));
    expect(chs.map((c) => c.bits)).toEqual(Array(9).fill(BASE)); // the bypass: all cheap
    const ok = [];
    let firstRefusal = null;
    for (let i = 0; i < 9; i++) {
      const r = await reg(s, `Pre${i}`, chs[i]);
      ok.push(r.status);
      if (r.status !== 200 && !firstRefusal) firstRefusal = { i, r };
    }
    expect(ok.slice(0, 3)).toEqual([200, 200, 200]); // REGISTER_POW_FREE sign-ups a day are at the base price
    expect(firstRefusal.i).toBe(3);
    expect(firstRefusal.r.status).toBe(428);
    expect(firstRefusal.r.data.error.code).toBe('pow_required');
    expect(firstRefusal.r.data.error.bits).toBeGreaterThan(BASE);
    expect(ok.slice(3)).toEqual(Array(6).fill(428)); // none of the stockpile works once the price has gone up
    // the honest road: take the new price and the sign-up goes through; each further one costs a bit more
    const bitsSeen = [];
    for (let i = 3; i < 6; i++) { const c = await get(s); bitsSeen.push(c.bits); expect((await reg(s, `Late${i}`, c)).status).toBe(200); }
    expect(bitsSeen).toEqual([BASE + 1, BASE + 2, BASE + 3]);
  });
  it('an honest console that fetches and redeems one challenge at a time is not disturbed', async () => {
    const s = makeSmg({ REGISTER_POW_BITS: String(BASE) });
    const c = await get(s, '198.51.100.7');
    expect(c.bits).toBe(BASE);
    expect((await reg(s, 'Honest1', c, '198.51.100.7')).status).toBe(200);
  });
  it('the old (cheap) challenge is refused once the price has gone up', async () => {
    const s = makeSmg({ REGISTER_POW_BITS: String(BASE), RATE_LIMIT_SCALE: '1000', REGISTER_IP_BURST: '100', REGISTER_IP_PER_HOUR: '1000' });
    const early = await get(s);
    for (let i = 0; i < 4; i++) { const c = await get(s); await reg(s, `Fill${i}`, c); }
    expect((await reg(s, 'EarlyBird', early)).status).toBe(428);
  });
});
