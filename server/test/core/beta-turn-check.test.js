// POST /v1/turn/check: the console's relay self-test credential (300 s, tag "check"), with its own small limits inside the daily credential allowance.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';
import { GRAPHQL_URL } from '../../src/core/turn-usage.js';

const ADMIN = 'test-admin-token-0123456789';
const KEY = 'abcdef0123456789abcdef0123456789';
const ACCT = '0123456789abcdef0123456789abcdef';
const GB = 1e9;

function fakeNet() {
  const st = { gd: 0, mintFail: false, n: 0 };
  const log = [];
  const fetchImpl = async (url, init = {}) => {
    const rec = { url: String(url), body: init.body ? String(init.body) : '' };
    log.push(rec);
    if (rec.url === GRAPHQL_URL) { const g = (v) => [{ sum: { egressBytes: v } }]; return Response.json({ data: { viewer: { accounts: [{ gm: g(0), gd: g(st.gd), am: g(0) }] } } }); }
    if (/generate-ice-servers$/.test(rec.url)) {
      if (st.mintFail) return new Response('no', { status: 500 });
      st.n++;
      return Response.json({ iceServers: { urls: ['stun:stun.cloudflare.com:3478', 'turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:3478?transport=tcp'], username: `user${st.n}`, credential: `cred${st.n}` } });
    }
    if (/\/revoke$/.test(rec.url)) return new Response(null, { status: 204 });
    return new Response('?', { status: 404 });
  };
  const calls = (re) => log.filter((l) => re.test(l.url));
  return { st, fetchImpl, mints: () => calls(/generate-ice-servers$/), revokes: () => calls(/\/revoke$/) };
}
const ENV = { TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: KEY, TURN_KEY_API_TOKEN: 'key-token-secret-value', TURN_USAGE_TOKEN: 'usage-token-secret-value', TURN_ACCOUNT_ID: ACCT, TURN_NEW_ACCOUNT_HOURS: '0' };

async function setup(env = {}, n = 1) {
  const net = fakeNet();
  const s = makeSmg({ ...ENV, ...env }, { deps: { fetchImpl: net.fetchImpl } });
  const ps = [];
  for (let i = 0; i < n; i++) ps.push(await smgSignUp(s, `P${String.fromCharCode(97 + i)}${i}`));
  const check = (p = ps[0], headers = {}) => s.call('POST', '/v1/turn/check', undefined, p.token, headers);
  const stats = async () => (await s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data.turn;
  return { s, net, ps, check, stats };
}
let warn;
beforeEach(() => { warn = vi.spyOn(console, 'warn').mockImplementation(() => {}); });
afterEach(() => warn.mockRestore());

describe('POST /v1/turn/check', () => {
  it('needs no room: a 300 s credential tagged "check", udp urls only; the mint asks for ttl 300', async () => {
    const t = await setup();
    const r = await t.check();
    expect(r.status).toBe(200);
    expect(r.data.ttlSec).toBe(300);
    expect(r.data.expiresMs).toBe(t.s.clock.now + 300_000);
    expect(r.data.servers.every((x) => (Array.isArray(x.urls) ? x.urls : [x.urls]).every((u) => !/transport=tcp/.test(u)))).toBe(true);
    expect(r.data.servers.some((x) => x.username === 'user1')).toBe(true);
    expect(JSON.parse(t.net.mints()[0].body)).toEqual({ ttl: 300, customIdentifier: 'check' });
  });
  it('needs a player token (401) and takes no body', async () => {
    const t = await setup();
    expect((await t.s.call('POST', '/v1/turn/check')).status).toBe(401);
    expect((await t.s.call('POST', '/v1/turn/check', { room: 'ABCDEF' }, t.ps[0].token)).status).toBe(400);
  });
  it('2 a player a day (the third is 429 turn_player_limit), a new UTC day opens it again', async () => {
    const t = await setup({}, 1);
    expect((await t.check()).status).toBe(200);
    expect((await t.check()).status).toBe(200);
    const r = await t.check();
    expect(r.status).toBe(429);
    expect(r.data.error.code).toBe('turn_player_limit');
    expect(t.net.mints()).toHaveLength(2);
    t.s.advance(86400_000);
    expect((await t.check()).status).toBe(200);
  });
  it('6 an address a day across players; another address is not affected', async () => {
    const t = await setup({}, 4);
    for (const p of t.ps.slice(0, 3)) for (let i = 0; i < 2; i++) expect((await t.check(p, { ip: '203.0.113.9' })).status).toBe(200);
    const r = await t.check(t.ps[3], { ip: '203.0.113.9' });
    expect(r.status).toBe(429);
    expect((await t.check(t.ps[3], { ip: '203.0.113.10' })).status).toBe(200);
  });
  it('200 a day in all (503 turn_budget), and each pass counts inside TURN_MAX_CREDS_PER_DAY', async () => {
    const t = await setup({ TURN_CHECK_MAX_PER_DAY: '3', TURN_CHECK_PER_PLAYER_DAY: '9', TURN_CHECK_PER_ADDR_DAY: '99' }, 2);
    for (let i = 0; i < 3; i++) expect((await t.check(t.ps[i % 2])).status).toBe(200);
    const r = await t.check();
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('turn_budget');
    const st = await t.stats();
    expect(st.checkPassesToday).toBe(3);
    expect(st.checkMaxPerDay).toBe(3);
    const t2 = await setup({ TURN_MAX_CREDS_PER_DAY: '2', TURN_CHECK_PER_PLAYER_DAY: '9' });
    expect((await t2.check()).status).toBe(200);
    expect((await t2.check()).status).toBe(200);
    expect((await t2.check()).status).toBe(503); // the day's credential allowance is shared
  });
  it('a failing mint gives the reservation back', async () => {
    const t = await setup();
    t.net.st.mintFail = true;
    expect((await t.check()).status).toBeGreaterThanOrEqual(500);
    expect((await t.stats()).checkPassesToday).toBe(0);
    t.net.st.mintFail = false;
    expect((await t.check()).status).toBe(200);
    expect((await t.check()).status).toBe(200); // both of the player's allowance still there
  });
  it('the hard stop applies: a trip is 503 turn_budget and revokes the check passes still valid', async () => {
    const t = await setup();
    expect((await t.check()).status).toBe(200);
    t.net.st.gd = 25 * GB;
    t.s.advance(11 * 60_000);
    const r = await t.check();
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('turn_budget');
    expect(t.net.revokes().map((x) => x.url.match(/credentials\/([^/]+)\/revoke/)[1])).toEqual(['user1']);
  });
  it('the kill switch "turn" stops it, nothing else is touched', async () => {
    const t = await setup();
    await t.s.raw('POST', '/v1/admin/switches', { kill: ['turn'] }, ADMIN);
    const r = await t.check();
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('service_disabled');
    expect(t.net.mints()).toHaveLength(0);
  });
  it('without a relay configured: 503 turn_unavailable', async () => {
    const s = makeSmg({ TURN_PROVIDER: '' });
    const p = await smgSignUp(s, 'Solo');
    const r = await s.call('POST', '/v1/turn/check', undefined, p.token);
    expect(r.status).toBe(503);
  });
});
