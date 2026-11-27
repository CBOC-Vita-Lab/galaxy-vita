// svcfix2 item 8 (independent review): a stranger who guessed (or was told) a room code got the host's PUBLIC address as soon as he posted
// his own endpoint. Now a stranger who joins a code room sees no address until the host's console accepts him (a mutual friend is
// accepted at once), and wrong codes are limited per ADDRESS (accounts are cheap).
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';

const epb = (ip) => ({ public: `${ip}:41100`, local: `192.168.1.${ip.split('.')[3]}:41100`, nonce: 'AAAAAAAAAAA=' });
const ips = { host: '198.51.100.10', g1: '198.51.100.11', st: '198.51.100.13' };
const pub = (v) => v.data.members.map((x) => x.endpoint && x.endpoint.public);

async function room(s) {
  const host = await smgSignUp(s, 'Host');
  const m = await s.call('POST', '/v1/rooms', {}, host.token, { ip: ips.host });
  const code = m.data.code;
  await s.call('POST', `/v1/rooms/${code}/endpoint`, epb(ips.host), host.token, { ip: ips.host });
  return { host, code };
}

describe('room code privacy', () => {
  it('a stranger with only the code sees no address; the host accepts him and then both do', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    const { host, code } = await room(s);
    const st = await smgSignUp(s, 'Stranger');
    expect((await s.call('POST', '/v1/rooms/join', { code }, st.token, { ip: ips.st })).status).toBe(200);
    await s.call('POST', `/v1/rooms/${code}/endpoint`, epb(ips.st), st.token, { ip: ips.st });
    const v = await s.call('GET', `/v1/rooms/${code}`, undefined, st.token, { ip: ips.st });
    expect(pub(v).filter(Boolean)).toEqual([]);
    const hv = await s.call('GET', `/v1/rooms/${code}`, undefined, host.token, { ip: ips.host });
    expect(pub(hv).filter(Boolean)).toEqual([]); // the host does not get his either until he accepts
    expect(hv.data.members.find((x) => x.name === 'Stranger').accepted).toBe(false);
    // only the host may accept
    expect((await s.call('POST', `/v1/rooms/${code}/accept`, { slot: 1 }, st.token, { ip: ips.st })).status).toBe(403);
    expect((await s.call('POST', `/v1/rooms/${code}/accept`, { slot: 1 }, host.token, { ip: ips.host })).status).toBe(200);
    const v2 = await s.call('GET', `/v1/rooms/${code}`, undefined, st.token, { ip: ips.st });
    expect(pub(v2).filter(Boolean).length).toBeGreaterThan(0);
  });
  it('a mutual friend is accepted at once; a stranger who leaves and comes back is still waiting', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    const { host, code } = await room(s);
    const g1 = await smgSignUp(s, 'Friendly');
    await s.call('POST', '/v1/friends', { code: g1.code }, host.token, { ip: ips.host });
    await s.call('POST', '/v1/friends', { code: host.code }, g1.token, { ip: ips.g1 });
    await s.call('POST', '/v1/rooms/join', { code }, g1.token, { ip: ips.g1 });
    await s.call('POST', `/v1/rooms/${code}/endpoint`, epb(ips.g1), g1.token, { ip: ips.g1 });
    const v = await s.call('GET', `/v1/rooms/${code}`, undefined, g1.token, { ip: ips.g1 });
    expect(pub(v).filter(Boolean).length).toBeGreaterThan(0);
    const st = await smgSignUp(s, 'Stranger');
    await s.call('POST', '/v1/rooms/join', { code }, st.token, { ip: ips.st });
    await s.call('POST', `/v1/rooms/${code}/leave`, undefined, st.token, { ip: ips.st });
    await s.call('POST', '/v1/rooms/join', { code }, st.token, { ip: ips.st });
    const hv = await s.call('GET', `/v1/rooms/${code}`, undefined, host.token, { ip: ips.host });
    expect(hv.data.members.find((x) => x.name === 'Stranger').accepted).toBe(false);
  });
  it('a public room is unchanged (the host chose it)', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1' });
    const host = await smgSignUp(s, 'Host');
    const m = await s.call('POST', '/v1/rooms', { visibility: 'public' }, host.token, { ip: ips.host });
    const st = await smgSignUp(s, 'Stranger');
    await s.call('POST', '/v1/rooms/join', { code: m.data.code }, st.token, { ip: ips.st });
    const hv = await s.call('GET', `/v1/rooms/${m.data.code}`, undefined, host.token, { ip: ips.host });
    expect(hv.data.members.find((x) => x.name === 'Stranger').accepted).toBe(true);
  });
  it('wrong codes cost the ADDRESS: ten, then a wait that every account behind it shares', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1000' });
    const a = await smgSignUp(s, 'Guesser');
    const b = await smgSignUp(s, 'Guesser2');
    let blocked = 0;
    for (let i = 0; i < 14; i++) {
      const r = await s.call('POST', '/v1/rooms/join', { code: 'AAAAA' + 'BCDEFGH'[i % 7] }, (i % 2 ? a : b).token, { ip: ips.st });
      if (r.status === 429) blocked++;
    }
    expect(blocked).toBeGreaterThanOrEqual(3);
    // an honest typo from another address is not affected
    expect((await s.call('POST', '/v1/rooms/join', { code: 'AAAAAB' }, a.token, { ip: '198.51.100.99' })).status).toBe(404);
  });
});
