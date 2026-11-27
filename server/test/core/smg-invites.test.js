// code5: far-away invites (invites.js): a 5-character code the host registers and a friend resolves to the host's SEALED blob. The service
// never reads the blob (48 base64url characters: the host's console seals it, platform/src/mp/mp_invite.cpp).
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';

const BLOB = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV'.slice(0, 48); // 48 characters of base64url
const BLOB2 = 'ZYXWVUTSRQPONMLKJIHGFEDCBAzyxwvutsrqponmlkjihgfe'.slice(0, 48);
const ips = { host: '198.51.100.10', g1: '198.51.100.11', g2: '198.51.100.12', g3: '198.51.100.13', g4: '198.51.100.14', st: '198.51.100.50' };
const reg = (s, p, code = 'KCBC4', blob = BLOB, ip = ips.host) => s.call('POST', '/v1/invites', { code, blob }, p.token, { ip });
const res = (s, p, code, ip) => s.call('POST', '/v1/invites/resolve', { code }, p.token, { ip: ip || ips.g1 });
const err = (r) => r.data && r.data.error && r.data.error.code;

describe('invites: the round trip', () => {
  it('a host registers a 5-character code, a friend resolves it to the same sealed blob, case and separators forgiven', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const g = await smgSignUp(s, 'Gus');
    const r = await reg(s, host);
    expect(r.status).toBe(200);
    expect(r.data).toMatchObject({ code: 'KCBC4', ttlSec: 600, keepSec: 300 });
    expect(r.data.expiresMs).toBe(s.clock.now + 600_000);
    for (const typed of ['KCBC4', 'kcbc4', 'KC BC4', 'kc-bc4', ' K C B C 4 ']) {
      const v = await res(s, g, typed);
      expect(v.status, typed).toBe(200);
      expect(v.data.blob).toBe(BLOB);
    }
  });

  it('refuses codes and blobs of the wrong shape, unknown fields, and look-alike characters (never sent by the game: it checks first)', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    for (const code of ['KCBC', 'KCBC45', 'KCBCO', 'KCBC0', 'KCBCI', 'KCBCL', 'KCBC1', 'KC!C4', 5, null, undefined]) {
      const r = await s.call('POST', '/v1/invites', { code, blob: BLOB }, host.token);
      expect(r.status, String(code)).toBe(400);
    }
    for (const blob of ['', 'short', BLOB + 'A', BLOB.slice(1) + '=', BLOB.slice(1) + '+', 7, undefined]) {
      expect((await s.call('POST', '/v1/invites', { code: 'KCBC4', blob }, host.token)).status, String(blob)).toBe(400);
    }
    expect((await s.call('POST', '/v1/invites', { code: 'KCBC4', blob: BLOB, extra: 1 }, host.token)).status).toBe(400);
    expect((await s.call('POST', '/v1/invites/resolve', { code: 'KCBC4', x: 1 }, host.token)).status).toBe(400);
    expect((await s.call('POST', '/v1/invites/resolve', { code: 'KCBCO' }, host.token)).status).toBe(400);
  });

  it('the host cannot resolve its own code; a friend of another version is told so; the service cannot be asked without an account', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const g = await smgSignUp(s, 'Gus');
    await reg(s, host);
    expect(err(await res(s, host, 'KCBC4', ips.host))).toBe('own_invite');
    const old = await s.call('POST', '/v1/invites/resolve', { code: 'KCBC4' }, g.token, { ip: ips.g1, 'x-msc-app': '3.0.1' });
    expect(old.status).toBe(409);
    expect(err(old)).toBe('version_mismatch');
    expect((await s.call('POST', '/v1/invites/resolve', { code: 'KCBC4' }, undefined, { ip: ips.g1 })).status).toBe(401);
  });

  it('the sealed text is stored as given and appears in no answer but the resolve (and not in the admin export)', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const r = await reg(s, host);
    expect(JSON.stringify(r.data)).not.toContain(BLOB);
    const ex = await s.raw('GET', '/v1/admin/export?table=invites', undefined, 'test-admin-token-0123456789');
    expect(JSON.stringify(ex.data)).not.toContain(BLOB);
    const stats = await s.raw('GET', '/v1/admin/stats', undefined, 'test-admin-token-0123456789');
    expect(JSON.stringify(stats.data)).not.toContain(BLOB);
  });
});

describe('invites: life', () => {
  it('open 10 minutes; keep renews it from now, never past the longest life; a missed keep has a 60 s grace', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const g = await smgSignUp(s, 'Gus');
    await reg(s, host);
    s.advance(300_000);
    const k = await s.call('POST', '/v1/invites/KCBC4/keep', undefined, host.token, { ip: ips.host });
    expect(k.status).toBe(200);
    expect(k.data.expiresMs).toBe(s.clock.now + 600_000);
    s.advance(600_000 + 30_000); // 30 s late: inside the grace
    expect((await res(s, g, 'KCBC4')).status).toBe(200);
    s.advance(40_000); // 70 s late: over
    const late = await res(s, g, 'KCBC4');
    expect(late.status).toBe(410);
    expect(err(late)).toBe('invite_expired');
    // the longest life (60 min from the start) bounds every keep
    const s2 = makeSmg();
    const h2 = await smgSignUp(s2, 'Host');
    const t0 = s2.clock.now;
    await reg(s2, h2);
    let last;
    for (let i = 0; i < 12; i++) { s2.advance(300_000); last = await s2.call('POST', '/v1/invites/KCBC4/keep', undefined, h2.token, { ip: ips.host }); }
    expect(last.status).toBe(200);
    expect(last.data.expiresMs).toBe(t0 + 3600_000);
  });

  it('only the host may keep a code; a code that is over cannot be kept', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const g = await smgSignUp(s, 'Gus');
    await reg(s, host);
    expect((await s.call('POST', '/v1/invites/KCBC4/keep', undefined, g.token, { ip: ips.g1 })).status).toBe(404);
    s.advance(700_000);
    const k = await s.call('POST', '/v1/invites/KCBC4/keep', undefined, host.token, { ip: ips.host });
    expect(k.status).toBe(410);
  });

  it('the host closes it: dead at once ("closed"), idempotent, a stranger cannot close it; a dead row is forgotten after a few minutes', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const g = await smgSignUp(s, 'Gus');
    await reg(s, host);
    expect((await s.call('DELETE', '/v1/invites/KCBC4', undefined, g.token, { ip: ips.g1 })).status).toBe(200);
    expect((await res(s, g, 'KCBC4')).status).toBe(200); // (the stranger's delete did nothing)
    expect((await s.call('POST', '/v1/invites/KCBC4/close', undefined, host.token, { ip: ips.host })).status).toBe(200); // (np_https speaks GET and POST only)
    expect((await s.call('DELETE', '/v1/invites/KCBC4', undefined, host.token, { ip: ips.host })).status).toBe(200);
    const dead = await res(s, g, 'KCBC4');
    expect(dead.status).toBe(410);
    expect(err(dead)).toBe('invite_closed');
    s.advance(301_000);
    s.app.sweep();
    expect(err(await res(s, g, 'KCBC4'))).toBe('invite_not_found');
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM invites`).toArray()[0].n).toBe(0);
  });

  it('one live invite per account: a new one closes the old; a dead code is free to take again; a live one is not', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const other = await smgSignUp(s, 'Other');
    const g = await smgSignUp(s, 'Gus');
    await reg(s, host, 'KCBC4');
    expect((await reg(s, host, 'WME6M', BLOB2)).status).toBe(200);
    expect(err(await res(s, g, 'KCBC4'))).toBe('invite_closed');
    expect((await res(s, g, 'WME6M')).data.blob).toBe(BLOB2);
    const taken = await reg(s, other, 'WME6M', BLOB, ips.st);
    expect(taken.status).toBe(409);
    expect(err(taken)).toBe('invite_taken');
    expect((await reg(s, other, 'KCBC4', BLOB, ips.st)).status).toBe(200); // the old one was dead
    expect((await res(s, g, 'KCBC4', ips.g2)).data.blob).toBe(BLOB);
  });
});

describe('invites: who may take a code', () => {
  it('three different accounts, then "full": the fourth and every stranger after the friends are in learn nothing; a friend may retry', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const g = [];
    for (const n of ['A', 'B', 'C', 'D']) g.push(await smgSignUp(s, `Guest${n}`));
    await reg(s, host);
    for (let i = 0; i < 3; i++) expect((await res(s, g[i], 'KCBC4', ips[`g${i + 1}`])).status).toBe(200);
    expect((await res(s, g[0], 'KCBC4', ips.g1)).status).toBe(200); // a retry of a friend
    const full = await res(s, g[3], 'KCBC4', ips.g4);
    expect(full.status).toBe(409);
    expect(err(full)).toBe('invite_full');
    expect(JSON.stringify(full.data)).not.toContain(BLOB);
  });

  it('a code is answered at most 12 times in all (retries included)', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    const g = await smgSignUp(s, 'Gus');
    await reg(s, host);
    let n = 0;
    for (; n < 20; n++) { if ((await res(s, g, 'KCBC4', ips.g1)).status !== 200) break; }
    expect(n).toBe(12);
  });
});

describe('invites: guessing costs the address, one allowance for every kind of code', () => {
  it('wrong 5-character codes: ten, then a wait that every account behind the address shares; an honest typo elsewhere is unaffected', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1000' });
    const a = await smgSignUp(s, 'Guesser');
    const b = await smgSignUp(s, 'Guesser2');
    let blocked = 0;
    for (let i = 0; i < 14; i++) {
      const r = await res(s, i % 2 ? a : b, 'AAAA' + 'BCDEFGH'[i % 7], ips.st);
      if (r.status === 429) blocked++;
    }
    expect(blocked).toBeGreaterThanOrEqual(3);
    expect((await res(s, a, 'AAAAB', '198.51.100.99')).status).toBe(404);
  });

  it('room codes and invite codes draw on the SAME daily allowance (40) of an address', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1000', WRONG_CODES_PER_DAY: '12' });
    const a = await smgSignUp(s, 'Guesser');
    let hits = 0;
    const log = [];
    for (let i = 0; i < 30; i++) {
      s.advance(601_000); // (the ten-minute allowance is not what this test is about)
      const r = i % 2
        ? await s.call('POST', '/v1/rooms/join', { code: 'BBBBB' + 'CDEFGHJ'[i % 7] }, a.token, { ip: ips.st })
        : await res(s, a, 'BBBB' + 'CDEFGHJ'[i % 7], ips.st);
      log.push(r.status);
      if (r.status === 429) hits++;
    }
    // 12 wrong codes in all (rooms + invites), the rest refused for the day
    expect(log.slice(0, 12).every((x) => x === 404)).toBe(true);
    expect(hits).toBeGreaterThanOrEqual(30 - 14);
    s.advance(86400_000);
    expect((await res(s, a, 'BBBBC', ips.st)).status).toBe(404);
  });

  it('probing the host side is charged the same: registering at a LIVE code of somebody else is a wrong guess', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1000', WRONG_CODES_PER_DAY: '5' });
    const host = await smgSignUp(s, 'Host');
    const spy = await smgSignUp(s, 'Spy');
    await reg(s, host, 'KCBC4');
    const answers = [];
    for (let i = 0; i < 8; i++) {
      answers.push((await reg(s, spy, 'KCBC4', BLOB, ips.st)).status);
    }
    expect(answers[0]).toBe(409);
    expect(answers).toContain(429);
    // an honest host who collides once (a 1 in 28 million chance) just picks another: one charge only
    const s2 = makeSmg({ RATE_LIMIT_SCALE: '1000' });
    const h1 = await smgSignUp(s2, 'One');
    const h2 = await smgSignUp(s2, 'Two');
    await reg(s2, h1, 'KCBC4');
    expect((await reg(s2, h2, 'KCBC4', BLOB, ips.g1)).status).toBe(409);
    expect((await reg(s2, h2, 'WME6M', BLOB, ips.g1)).status).toBe(200);
  });
});

describe('invites: limits, switches, the budget', () => {
  it('registering is limited per account (10 in 10 minutes) and counts as a room of the day', async () => {
    const s = makeSmg({ RATE_LIMIT_SCALE: '1', ROOMS_PER_ACCOUNT_DAY: '3' });
    const host = await smgSignUp(s, 'Host');
    for (let i = 0; i < 3; i++) expect((await reg(s, host, 'KCBC' + '4567'[i])).status).toBe(200);
    const r = await reg(s, host, 'KCBC8');
    expect(r.status).toBe(429);
    expect(r.data.error.reason).toBe('daily_cap');
  });

  it('the rooms switch turns invites off (503 service_disabled); closing still works', async () => {
    const s = makeSmg();
    const host = await smgSignUp(s, 'Host');
    await reg(s, host);
    await s.raw('POST', '/v1/admin/switches', { kill: ['rooms'] }, 'test-admin-token-0123456789');
    const r = await reg(s, host, 'WME6M');
    expect(r.status).toBe(503);
    expect(err(r)).toBe('service_disabled');
    expect((await res(s, host, 'KCBC4', ips.g1)).status).toBe(503);
  });

  it('too many live invites at once: busy (the free plan holds a few thousand rows, not millions)', async () => {
    const s = makeSmg({ INVITE_MAX_LIVE: '10' });
    const hosts = [];
    for (let i = 0; i < 11; i++) hosts.push(await smgSignUp(s, `Host${i}`));
    const alpha = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
    for (let i = 0; i < 10; i++) expect((await reg(s, hosts[i], `KCBC${alpha[i]}`, BLOB, `198.51.100.${100 + i}`)).status).toBe(200);
    const r = await reg(s, hosts[10], 'WME6M', BLOB, '198.51.100.120');
    expect(r.status).toBe(503);
    expect(err(r)).toBe('busy');
  });

  it('over budget: the host cannot register (503 over_budget) and the client falls back; a code issued before it still resolves in the reserve; closing is never refused', async () => {
    const s = makeSmg({ DAILY_REQUEST_BUDGET: '200', LIVE_RESERVE_PCT: '20' });
    const host = await smgSignUp(s, 'Host'); // 1
    const g = await smgSignUp(s, 'Gus'); // 2
    expect((await reg(s, host)).status).toBe(200); // 3
    let r;
    do { r = await s.call('GET', '/v1/info'); } while (r.status === 200); // to 80 %: new sessions stop
    expect(r.headers.get('x-msc-budget')).toBe('new-over');
    const late = await reg(s, host, 'WME6M');
    expect(late.status).toBe(503);
    expect(late.headers.get('x-msc-budget')).toBe('new-over');
    expect((await res(s, g, 'KCBC4')).status).toBe(200); // an invite issued before: part of a session being formed
    expect((await res(s, g, 'ZZZZZ')).status).toBe(503); // (an unknown code is an ordinary new request: refused, never answered "wrong")
    expect((await s.call('POST', '/v1/invites/KCBC4/close', undefined, host.token, { ip: ips.host })).status).toBe(200);
  });
});

describe('invites: storage', () => {
  it('migration 11 creates the table on an existing database and a second start changes nothing', async () => {
    const s = makeSmg();
    const cols = s.store.sql.exec(`PRAGMA table_info(invites)`).toArray().map((c) => c.name);
    expect(cols).toEqual(['code', 'pid', 'blob', 'app', 'created_ms', 'expires_ms', 'state', 'closed_ms', 'reason', 'resolves', 'resolvers']);
    const { createApp } = await import('../../src/core/app.js');
    createApp({ sql: s.store.sql, tx: s.store.tx, env: s.env, now: () => s.clock.now });
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM _migrations WHERE id = 11`).toArray()[0].n).toBe(1);
  });

  it('the sweep expires and forgets: 961 abandoned invites leave no rows behind', async () => {
    const s = makeSmg({ INVITE_MAX_LIVE: '5000' });
    const insert = (code, pid, exp) => s.store.sql.exec(`INSERT INTO invites (code, pid, blob, app, created_ms, expires_ms) VALUES (?, ?, 'x', '3.0.0', ?, ?)`, code, pid, s.clock.now, exp);
    const alpha = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
    for (let i = 0; i < 961; i++) insert(alpha[i % 31] + alpha[(i / 31 | 0) % 31] + 'AAA', 1000 + i, s.clock.now + 1000);
    s.advance(2 * 60_000);
    s.app.sweep();
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM invites WHERE state = 'open'`).toArray()[0].n).toBe(0);
    s.advance(10 * 60_000);
    s.app.sweep();
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM invites`).toArray()[0].n).toBe(0);
  });
});
