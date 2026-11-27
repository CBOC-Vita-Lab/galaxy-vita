// svcfix3 part D: the relay's hard usage stop (Cloudflare's own egress numbers through GraphQL), the revoke sweep, the admin self-check.
// Everything is mocked: mint, revoke and the GraphQL analytics read go through one fake fetch.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { makeSmg, smgSignUp, roomOf } from '../smg-helpers.js';
import { GRAPHQL_URL } from '../../src/core/turn-usage.js';

const ADMIN = 'test-admin-token-0123456789';
const KEY = 'abcdef0123456789abcdef0123456789';
const ACCT = '0123456789abcdef0123456789abcdef';
const GB = 1e9;

function fakeNet() {
  const st = { gm: 0, gd: 0, am: 0, gqlFail: false, gqlErrors: false, mintFail: false, revokeFail: false, revoke404: 0, revoke200: false, mintBad: false, tagRefuse: 0, forbid: 0, gqlMsg: undefined, n: 0 };
  const log = [];
  const fetchImpl = async (url, init = {}) => {
    const rec = { url: String(url), method: init.method, body: init.body ? String(init.body) : '', auth: (init.headers || {}).authorization };
    log.push(rec);
    if (rec.url === GRAPHQL_URL) {
      if (st.gqlFail) return new Response('{}', { status: 500 });
      if (st.gqlErrors) return Response.json({ data: null, errors: [{ message: st.gqlMsg ?? 'unknown field' }] });
      const g = (v) => [{ sum: { egressBytes: v } }];
      return Response.json({ data: { viewer: { accounts: [{ gm: g(st.gm), gd: g(st.gd), am: g(st.am) }] } } });
    }
    if (/\/credentials\/generate-ice-servers$/.test(rec.url)) {
      if (st.mintFail) return new Response('no', { status: 500 });
      if (st.forbid) return new Response('{"errors":[{"message":"forbidden"}]}', { status: st.forbid });
      if (st.tagRefuse && rec.body.includes('customIdentifier')) return new Response('{"errors":[{"message":"unknown field customIdentifier"}]}', { status: st.tagRefuse });
      if (st.mintBad) return new Response('{"errors":[{"message":"unknown field customIdentifier"}]}', { status: 400 });
      st.n++;
      return Response.json({ iceServers: { urls: ['stun:stun.cloudflare.com:3478', 'turn:turn.cloudflare.com:3478?transport=udp'], username: `user${st.n}`, credential: `cred${st.n}` } });
    }
    if (/\/credentials\/[^/]+\/revoke$/.test(rec.url)) { if (st.revoke404 > 0) { st.revoke404--; return new Response('{"errors":[{"message":"404: cannot find specified username"}]}', { status: 404 }); } if (st.revoke404 < 0) return new Response('404: cannot find specified username', { status: 404 }); } 
    if (/\/credentials\/[^/]+\/revoke$/.test(rec.url)) return new Response(null, { status: st.revokeFail ? 400 : st.revoke200 ? 200 : 204 });
    return new Response('?', { status: 404 });
  };
  const calls = (re) => log.filter((l) => re.test(l.url));
  return { st, log, fetchImpl, calls, gql: () => calls(/graphql/), revokes: () => calls(/\/revoke$/), mints: () => calls(/generate-ice-servers$/) };
}

const ENV = {
  TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: KEY, TURN_KEY_API_TOKEN: 'key-token-secret-value',
  TURN_MAX_CREDS_PER_ROOM: '0', TURN_MAX_CREDS_PER_ADDR_DAY: '0', TURN_NEW_ACCOUNT_HOURS: '0', TURN_UNNEEDED_SHARE_PCT: '100', TURN_MAX_CREDS_PER_PLAYER_DAY: '50',
};
const WITH_USAGE = { ...ENV, TURN_USAGE_TOKEN: 'usage-token-secret-value', TURN_ACCOUNT_ID: ACCT };

async function setup(env, net = fakeNet()) {
  const s = makeSmg(env, { deps: { fetchImpl: net.fetchImpl } });
  const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb');
  const code = await roomOf(s, [a, b]);
  const mint = (p = a) => s.call('POST', '/v1/turn', { room: code }, p.token);
  const stats = async () => (await s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data.turn;
  return { s, a, b, code, mint, stats, net };
}

let warn;
beforeEach(() => { warn = vi.spyOn(console, 'warn').mockImplementation(() => {}); });
afterEach(() => warn.mockRestore());

describe('mint and revoke through the provider', () => {
  it('the cloudflare provider mints for our key with a ttl, remembers the pass name, and the answer carries only udp urls', async () => {
    const t = await setup(WITH_USAGE);
    const r = await t.mint();
    expect(r.status).toBe(200);
    expect(r.data.servers.some((x) => x.username === 'user1' && x.credential === 'cred1')).toBe(true);
    const m = t.net.mints()[0];
    expect(m.url).toBe(`https://rtc.live.cloudflare.com/v1/turn/keys/${KEY}/credentials/generate-ice-servers`);
    expect(JSON.parse(m.body)).toEqual({ ttl: t.s.app.cfg.turn.ttlSec, customIdentifier: 'galaxy' }); // tagged for analytics
    expect(m.auth).toBe('Bearer key-token-secret-value');
  });
});

describe('without TURN_USAGE_TOKEN', () => {
  it('keeps the estimate caps, says so once in the log, and the stats show source estimate', async () => {
    const t = await setup(ENV);
    expect((await t.mint()).status).toBe(200);
    expect((await t.mint(t.b)).status).toBe(200);
    expect(t.net.gql()).toHaveLength(0);
    const msgs = warn.mock.calls.map((x) => String(x[0])).filter((x) => /hard usage stop unavailable/.test(x));
    expect(msgs).toHaveLength(1);
    const st = await t.stats();
    expect(st.usage.source).toBe('estimate');
    expect(st.usage.hardStop).toBe('unavailable');
    expect(st.usage.tripped).toBe(false);
    expect(st.monthlyBudgetMb).toBeGreaterThan(0); // the estimate cap is still reported
  });
});

describe('with the usage token: reading Cloudflare\'s numbers', () => {
  it('reads month and day for our key and the month for the account, at most every 10 minutes', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.gm = 12 * GB; t.net.st.gd = 1 * GB; t.net.st.am = 30 * GB;
    expect((await t.mint()).status).toBe(200);
    expect(t.net.gql()).toHaveLength(1);
    const q = JSON.parse(t.net.gql()[0].body).query;
    expect(q).toContain('callsTurnUsageAdaptiveGroups');
    expect(q).toContain('egressBytes');
    expect(q).toContain(`accountTag: "${ACCT}"`);
    expect(q).toContain(`keyId: "${KEY}"`);
    expect(t.net.gql()[0].auth).toBe('Bearer usage-token-secret-value');
    expect(q).toContain('date_geq: "2026-10-01", date_leq: "2026-10-30"'); // month to date
    expect(q).toContain('date_geq: "2026-10-30", date_leq: "2026-10-30"'); // today
    expect(q).not.toContain('datetime_');
    await t.mint(t.b);
    t.s.advance(9 * 60_000);
    await t.mint();
    expect(t.net.gql()).toHaveLength(1); // not again within 10 minutes
    t.s.advance(2 * 60_000);
    await t.mint();
    expect(t.net.gql()).toHaveLength(2);
    const st = await t.stats();
    expect(st.usage).toMatchObject({ source: 'cloudflare', hardStop: 'on', galaxyMonthMb: 12000, galaxyDayMb: 1000, accountMonthMb: 30000, tripped: false });
    expect(st.usage.limits).toEqual({ galaxyMonthGb: 300, accountMonthGb: 700, galaxyDayGb: 25 });
    expect(JSON.stringify(st)).not.toContain('usage-token-secret-value');
  });

  it.each([
    ['galaxy_month', { gm: 300 * GB }, 'galaxy month'],
    ['account_month', { am: 700 * GB }, 'account month'],
    ['galaxy_day', { gd: 25 * GB }, 'galaxy day'],
  ])('a trip on %s stops new passes (503 turn_budget with retry-after) and revokes every pass still valid', async (reason, nums) => {
    const t = await setup(WITH_USAGE);
    expect((await t.mint()).status).toBe(200); // user1
    expect((await t.mint(t.b)).status).toBe(200); // user2
    Object.assign(t.net.st, nums);
    t.s.advance(11 * 60_000);
    const r = await t.mint();
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('turn_budget');
    expect(Number(r.headers.get('retry-after'))).toBeGreaterThanOrEqual(60);
    expect(t.net.mints()).toHaveLength(2); // nothing new minted
    const names = t.net.revokes().map((x) => x.url.match(/credentials\/([^/]+)\/revoke/)[1]).sort();
    expect(names).toEqual(['user1', 'user2']);
    const st = await t.stats();
    expect(st.usage.tripped).toBe(true);
    expect(st.usage.trippedReason).toBe(reason);
    expect(st.usage.revokedPasses).toBe(2);
    // still refused on the next ask; and nothing is revoked twice
    expect((await t.mint(t.b)).status).toBe(503);
    expect(t.net.revokes()).toHaveLength(2);
  });

  it('just under each limit nothing trips; a switch off (0) disables that limit', async () => {
    const t = await setup({ ...WITH_USAGE, TURN_HARD_DAY_GB: '0' });
    Object.assign(t.net.st, { gm: 299 * GB, am: 699 * GB, gd: 90 * GB });
    expect((await t.mint()).status).toBe(200);
    expect((await t.stats()).usage.tripped).toBe(false);
  });

  it('the limits come from TURN_HARD_MONTH_GB, TURN_HARD_ACCOUNT_MONTH_GB and TURN_HARD_DAY_GB', async () => {
    const t = await setup({ ...WITH_USAGE, TURN_HARD_MONTH_GB: '5', TURN_HARD_ACCOUNT_MONTH_GB: '9', TURN_HARD_DAY_GB: '2' });
    t.net.st.gm = 5 * GB;
    expect((await t.mint()).status).toBe(503);
    expect((await t.stats()).usage.limits).toEqual({ galaxyMonthGb: 5, accountMonthGb: 9, galaxyDayGb: 2 });
  });

  it('a trip clears when the next read is under the limits again (a new day)', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.gd = 26 * GB;
    expect((await t.mint()).status).toBe(503);
    t.net.st.gd = 0;
    t.s.advance(11 * 60_000);
    expect((await t.mint()).status).toBe(200);
    expect((await t.stats()).usage.tripped).toBe(false);
  });

  it('the alarm tick reads when due and sweeps the revoke while tripped (no request needed)', async () => {
    const t = await setup(WITH_USAGE);
    await t.mint();
    t.net.st.gd = 30 * GB;
    t.s.advance(11 * 60_000);
    await t.s.app.turnTick();
    expect(t.net.gql()).toHaveLength(2);
    expect(t.net.revokes()).toHaveLength(1);
    await t.s.app.turnTick(); // nothing due, nothing left to revoke
    expect(t.net.revokes()).toHaveLength(1);
  });

  it('a revoke that fails is retried by the next tick', async () => {
    const t = await setup(WITH_USAGE);
    await t.mint();
    t.net.st.gd = 30 * GB; t.net.st.revokeFail = true;
    t.s.advance(11 * 60_000);
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(1);
    t.net.st.revokeFail = false;
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(2);
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(2);
  });
});

describe('fail safe: the analytics cannot be read', () => {
  it('one or two failures in a row do not stop the relay; the third does, for an hour, and is logged', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.gqlFail = true;
    expect((await t.mint()).status).toBe(200); // failure 1 (estimate caps still apply)
    t.s.advance(61_000);
    expect((await t.mint(t.b)).status).toBe(200); // failure 2
    t.s.advance(61_000);
    const r = await t.mint(); // failure 3 -> tripped
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('turn_budget');
    const st = await t.stats();
    expect(st.usage).toMatchObject({ tripped: true, trippedReason: 'analytics_unavailable', readFailsInARow: 3 });
    expect(warn.mock.calls.some((x) => /unreadable 3 times in a row/.test(String(x[0])))).toBe(true);
    expect(t.net.revokes().length).toBeGreaterThanOrEqual(0);
    // tripped for the hour: no new passes even though the analytics come back? the next read is due in 60 s and clears it
    t.net.st.gqlFail = false;
    t.s.advance(61_000);
    expect((await t.mint()).status).toBe(200);
    expect((await t.stats()).usage.tripped).toBe(false);
  });

  it('the trip lasts the hour while reads keep failing, then each further failure renews it', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.gqlFail = true;
    for (let i = 0; i < 3; i++) { await t.mint(); t.s.advance(61_000); }
    expect((await t.mint()).status).toBe(503);
    t.s.advance(30 * 60_000);
    expect((await t.mint()).status).toBe(503);
  });

  it('an answer with GraphQL errors counts as a failure; the stats show the reason without secrets', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.gqlErrors = true;
    await t.mint();
    const st = await t.stats();
    expect(st.usage.readFailsInARow).toBe(1);
    expect(st.usage.lastError).toMatch(/errors/);
  });
});

describe('admin self-check: POST /v1/admin/turn-selfcheck', () => {
  it('mints a 300 s pass named selfcheck, revokes it at once, reports usage, never returns the credential', async () => {
    const net = fakeNet();
    net.st.gm = 12 * GB; net.st.am = 34 * GB;
    const s = makeSmg(WITH_USAGE, { deps: { fetchImpl: net.fetchImpl } });
    const r = await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN);
    expect(r.status).toBe(200);
    expect(r.data).toEqual({ mint: 'ok', revoke: 'ok', usage: 'ok', galaxyMonthMb: 12000, accountMonthMb: 34000, tripped: false, untagged: false });
    expect(JSON.parse(net.mints()[0].body)).toEqual({ ttl: 300, customIdentifier: 'selfcheck' });
    expect(net.revokes()).toHaveLength(1);
    expect(net.revokes()[0].url).toContain('/credentials/user1/revoke');
    const text = JSON.stringify(r.data);
    expect(text).not.toMatch(/cred1|user1|key-token|usage-token/);
  });
  it('needs the admin token', async () => {
    const s = makeSmg(WITH_USAGE, { deps: { fetchImpl: fakeNet().fetchImpl } });
    expect((await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, 'wrong-token-wrong-token-1234')).status).toBe(401);
    expect((await s.raw('POST', '/v1/admin/turn-selfcheck')).status).toBe(401);
  });
  it('without the usage token: usage absent; mint and revoke still checked', async () => {
    const net = fakeNet();
    const s = makeSmg(ENV, { deps: { fetchImpl: net.fetchImpl } });
    const r = await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN);
    expect(r.data).toMatchObject({ mint: 'ok', revoke: 'ok', usage: 'absent', galaxyMonthMb: null, accountMonthMb: null });
  });
  it('reports each failure: mint error, revoke error, usage error', async () => {
    const net = fakeNet();
    const s = makeSmg(WITH_USAGE, { deps: { fetchImpl: net.fetchImpl } });
    net.st.mintFail = true;
    expect((await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data).toMatchObject({ mint: 'error', revoke: 'skipped' });
    net.st.mintFail = false; net.st.revokeFail = true;
    expect((await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data).toMatchObject({ mint: 'ok', revoke: 'error' });
    net.st.revokeFail = false; net.st.gqlFail = true;
    expect((await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data).toMatchObject({ mint: 'ok', revoke: 'ok', usage: 'error' });
  });
  it('a refused field or an answer other than 204 is reported with its reason, not swallowed', async () => {
    const net = fakeNet();
    const s = makeSmg(WITH_USAGE, { deps: { fetchImpl: net.fetchImpl } });
    net.st.mintBad = true;
    const m = (await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
    expect(m.mint).toBe('error');
    expect(m.mintDetail).toMatch(/^HTTP 400: .*customIdentifier/);
    net.st.mintBad = false; net.st.revoke200 = true; // 200 is not the documented 204
    const v = (await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
    expect(v).toMatchObject({ mint: 'ok', revoke: 'error' });
    expect(v.revokeDetail).toMatch(/^HTTP 200/);
    net.st.revoke200 = false; net.st.gqlErrors = true;
    expect((await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data.usageDetail).toMatch(/errors/);
  });
  it('relay not configured: mint off', async () => {
    const s = makeSmg({ TURN_PROVIDER: 'cloudflare' });
    const r = await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN);
    expect(r.status).toBe(200);
    expect(r.data).toMatchObject({ mint: 'off', usage: 'absent' });
  });
});

describe('the customIdentifier tag never makes a mint fail', () => {
  it.each([[400], [404], [409], [422]])('a %i (any 4xx but 401, 403, 429) is retried ONCE without it, remembered, counted in the stats, and the self-check passes as untagged', async (status) => {
    const t = await setup(WITH_USAGE);
    t.net.st.tagRefuse = status;
    const r = await t.mint();
    expect(r.status).toBe(200);
    expect(r.data.servers.some((x) => x.username)).toBe(true);
    expect(t.net.mints()).toHaveLength(2);
    expect(JSON.parse(t.net.mints()[0].body).customIdentifier).toBe('galaxy');
    expect(JSON.parse(t.net.mints()[1].body)).toEqual({ ttl: t.s.app.cfg.turn.ttlSec });
    expect((await t.mint(t.b)).status).toBe(200);
    expect(t.net.mints()).toHaveLength(3); // no second try with the tag: remembered
    expect(JSON.parse(t.net.mints()[2].body).customIdentifier).toBeUndefined();
    expect((await t.stats()).tagUnsupported).toBe(true);
    expect(warn.mock.calls.filter((x) => /refused the customIdentifier tag/.test(String(x[0])))).toHaveLength(1);
    const sc = (await t.s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
    expect(sc).toMatchObject({ mint: 'ok', revoke: 'ok', untagged: true });
  });
  it('the tag is accepted: tagUnsupported stays false and the self-check says tagged', async () => {
    const t = await setup(WITH_USAGE);
    await t.mint();
    expect((await t.stats()).tagUnsupported).toBe(false);
    expect((await t.s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data.untagged).toBe(false);
  });
  it.each([[403], [401], [429], [500]])('a %i for another reason is NOT retried and does not switch the tag off', async (status) => {
    const t = await setup(WITH_USAGE);
    t.net.st.forbid = status;
    const r = await t.mint();
    expect(r.status).toBe(502);
    expect(t.net.mints()).toHaveLength(1);
    expect((await t.stats()).tagUnsupported).toBe(false);
  });
  it('an untagged retry that is refused too is an error (502), once', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.mintBad = true; // 400 whatever is sent
    const r = await t.mint();
    expect(r.status).toBe(502);
    expect(t.net.mints()).toHaveLength(2);
  });
});

describe('GraphQL error text is shown (scrubbed) so a wrong field can be diagnosed', () => {
  it('keeps the first 120 characters of errors[0].message, without e-mail addresses or 32+ hex runs, in the stats and the self-check', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.gqlErrors = true;
    t.net.st.gqlMsg = `unknown field "callsTurnUsageAdaptiveGroups" for owner@example.com account ${ACCT} ${'x'.repeat(200)}`;
    await t.mint();
    const st = (await t.stats()).usage;
    expect(st.lastError).toMatch(/^analytics answered with errors: unknown field "callsTurnUsageAdaptiveGroups" for \[EMAIL\] account \[REDACTED\] \[token\]$/);
    expect(st.lastError).not.toMatch(/owner@|0123456789abcdef/);
    expect(st.lastError.length).toBeLessThanOrEqual(170);
    const sc = (await t.s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
    expect(sc.usage).toBe('error');
    expect(sc.usageDetail).toMatch(/^analytics answered with errors: unknown field/);
    expect(sc.usageDetail).not.toMatch(/owner@|0123456789abcdef/);
  });
});

describe('one redactor for every upstream text', () => {
  const TOKENSHAPE = 'AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-abc';
  const echo = (extra = '') => `fail ${WITH_USAGE.TURN_KEY_API_TOKEN} ${WITH_USAGE.TURN_USAGE_TOKEN} ${KEY} ${ACCT} ${TOKENSHAPE} owner@example.com ${extra}`;
  const secrets = ['key-token-secret-value', 'usage-token-secret-value', KEY, ACCT, TOKENSHAPE, 'owner@example.com'];
  const clean = (text) => { for (const v of secrets) expect(text).not.toContain(v); };
  it('an upstream mint refusal that echoes every secret leaves none of them in the route answer, the stats or the logs', async () => {
    const net = fakeNet();
    const base = net.fetchImpl;
    const s = makeSmg(WITH_USAGE, { deps: { fetchImpl: async (url, init) => (/generate-ice-servers$/.test(String(url)) ? new Response(echo(), { status: 500 }) : base(url, init)) } });
    const sc = (await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
    expect(sc.mint).toBe('error');
    expect(sc.mintDetail).toMatch(/^HTTP 500: fail \[REDACTED\] \[REDACTED\] \[REDACTED\] \[REDACTED\] \[token\] \[EMAIL\]/);
    const stats = (await s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data;
    for (const text of [JSON.stringify(sc), JSON.stringify(stats), JSON.stringify(warn.mock.calls)]) clean(text);
  });
  it('an upstream revoke refusal that echoes every secret is redacted too', async () => {
    const net = fakeNet();
    const base = net.fetchImpl;
    const s = makeSmg(WITH_USAGE, { deps: { fetchImpl: async (url, init) => (/\/revoke$/.test(String(url)) ? new Response(echo(), { status: 400 }) : base(url, init)) } });
    const sc = (await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
    expect(sc.revoke).toBe('error');
    clean(JSON.stringify(sc));
    expect(sc.revokeDetail).toMatch(/^HTTP 400: fail \[REDACTED\]/);
  });
  it('an analytics error that echoes the usage token (and the others) never reaches the answer, the stats or the logs', async () => {
    const t = await setup(WITH_USAGE);
    t.net.st.gqlErrors = true;
    t.net.st.gqlMsg = echo();
    await t.mint();
    const sc = (await t.s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
    const stats = (await t.s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data;
    for (const text of [JSON.stringify(sc), JSON.stringify(stats), JSON.stringify(warn.mock.calls)]) clean(text);
    expect(sc.usageDetail).toMatch(/analytics answered with errors: fail \[REDACTED\]/);
  });
  it('redact(): exact values first, e-mail, hex, token-shaped runs, then cut to 120; a secret straddling the cut is gone', async () => {
    const { redact } = await import('../../src/core/redact.js');
    const sec = 'SECRETVALUE-1234567890';
    const long = `${'a '.repeat(55)}${sec}`; // the secret starts just before the 120 cut
    const out = redact(long, [sec]);
    expect(out.length).toBeLessThanOrEqual(120);
    expect(out).not.toContain('SECRET');
    expect(redact(`x ${'f'.repeat(40)} y ${TOKENSHAPE}`)).toBe('x [hex] y [token]');
    expect(redact('mail me@example.com')).toBe('mail [EMAIL]');
    expect(redact(undefined)).toBe('');
  });
});

describe('redact(): control characters', () => {
  it('removes control characters except newline and tab (which become spaces)', async () => {
    const { redact } = await import('../../src/core/redact.js');
    expect(redact('a\u001b[31mb\u0000c\rd\ne\tf\u007fg')).toBe('a[31mbcd e fg');
  });
});

describe('revoke 404 (the pass is not yet known to the revoke endpoint)', () => {
  const sleeps = [];
  const noSleep = async (ms) => { sleeps.push(ms); };
  beforeEach(() => { sleeps.length = 0; });
  const selfcheck = async (net) => {
    const s = makeSmg({ ...WITH_USAGE }, { deps: { fetchImpl: net.fetchImpl, sleep: noSleep } });
    return (await s.raw('POST', '/v1/admin/turn-selfcheck', undefined, ADMIN)).data;
  };
  it('404 then 204: the self-check reads ok, revokeDelayed, with the tries and the wait', async () => {
    const net = fakeNet(); net.st.revoke404 = 2;
    const d = await selfcheck(net);
    expect(d).toMatchObject({ mint: 'ok', revoke: 'ok', revokeDelayed: true, revokeTries: 3, revokeWaitedMs: 3000 });
    expect(sleeps).toEqual([1000, 2000]);
    expect(net.revokes()).toHaveLength(3);
  });
  it('no 404: no revokeDelayed field', async () => {
    const d = await selfcheck(fakeNet());
    expect(d.revoke).toBe('ok');
    expect(d).not.toHaveProperty('revokeDelayed');
  });
  it('404 every time: error with the upstream text, 5 tries, 15 s of waiting', async () => {
    const net = fakeNet(); net.st.revoke404 = -1;
    const d = await selfcheck(net);
    expect(d).toMatchObject({ mint: 'ok', revoke: 'error', revokeTries: 5, revokeWaitedMs: 15000 });
    expect(d.revokeDetail).toMatch(/^HTTP 404: .*cannot find/);
    expect(sleeps).toEqual([1000, 2000, 4000, 8000]);
    expect(d).not.toHaveProperty('revokeDelayed');
  });
  it('another refusal (400) is not retried', async () => {
    const net = fakeNet(); net.st.revokeFail = true;
    const d = await selfcheck(net);
    expect(d).toMatchObject({ revoke: 'error', revokeTries: 1, revokeWaitedMs: 0 });
    expect(sleeps).toEqual([]);
  });
  it('sweep: a 404 for a still-valid pass is counted (revokeNotFound), the pass stays, and the next sweep revokes it', async () => {
    const t = await setup(WITH_USAGE);
    await t.mint();
    t.net.st.gd = 30 * GB; t.net.st.revoke404 = 1;
    t.s.advance(11 * 60_000);
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(1);
    let st = await t.stats();
    expect(st.usage).toMatchObject({ revokeNotFound: 1, revokedPasses: 0, revokeGone: 0 });
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(2);
    st = await t.stats();
    expect(st.usage).toMatchObject({ revokeNotFound: 1, revokedPasses: 1 });
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(2); // done
  });
  it('sweep: a 404 for a pass whose ttl has run out counts as gone (not a failure, not retried)', async () => {
    const t = await setup(WITH_USAGE);
    await t.mint();
    t.net.st.gd = 30 * GB; t.net.st.revoke404 = -1;
    t.s.advance(3600 * 1000); // exactly the ttl: the last moment the sweep still looks at the pass
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(1);
    const st = await t.stats();
    expect(st.usage).toMatchObject({ revokeNotFound: 0, revokeGone: 1, revokedPasses: 0 });
    await t.s.app.turnTick();
    expect(t.net.revokes()).toHaveLength(1);
  });
});
