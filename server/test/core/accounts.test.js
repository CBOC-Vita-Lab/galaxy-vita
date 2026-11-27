import { describe, it, expect } from 'vitest';
import { makeServer, signUp } from '../helpers.js';
import { foldKey, validateName } from '../../src/core/names.js';

describe('registration', () => {
  it('registers once: token + profile id > 0, name echoed', async () => {
    const s = makeServer();
    const r = await s.call('POST', '/v1/register', { name: 'Sally', region: 4 });
    expect(r.status).toBe(200);
    expect(r.data.pid).toBeGreaterThan(100000);
    expect(r.data.token).toMatch(/^[A-Za-z0-9_-]{43}$/);
    expect(r.data.name).toBe('Sally');
    expect(r.data.region).toBe(4);
  });

  it('tokens are random and only a hash is stored', async () => {
    const s = makeServer();
    const a = await signUp(s, 'AlphaOne');
    const b = await signUp(s, 'BetaTwo');
    expect(a.token).not.toBe(b.token);
    expect(a.pid).not.toBe(b.pid);
    const rows = s.store.sql.exec('SELECT token_hash FROM profiles').toArray();
    for (const row of rows) {
      expect(row.token_hash).toMatch(/^[0-9a-f]{64}$/);
      expect([a.token, b.token]).not.toContain(row.token_hash);
    }
  });

  it('GET /v1/me with the token returns the profile; wrong or missing token is 401', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    const me = await s.call('GET', '/v1/me', undefined, a.token);
    expect(me.status).toBe(200);
    expect(me.data).toMatchObject({ pid: a.pid, name: 'Alpha', rating: 1000, wins: 0, losses: 0 });
    expect((await s.call('GET', '/v1/me')).status).toBe(401);
    expect((await s.call('GET', '/v1/me', undefined, 'x'.repeat(43))).status).toBe(401);
    expect((await s.call('GET', '/v1/me', undefined, 'short')).status).toBe(401);
    const r = await s.call('GET', '/v1/me');
    expect(r.headers.get('www-authenticate')).toBe('Bearer');
  });

  it('rejects bad regions and non-object bodies', async () => {
    const s = makeServer();
    expect((await s.call('POST', '/v1/register', { name: 'Okay', region: 3 })).data.error.code).toBe('bad_region');
    expect((await s.call('POST', '/v1/register', [1])).data.error.code).toBe('bad_request');
    expect((await s.call('POST', '/v1/register', 'null')).status).toBe(400);
  });
});

describe('name validation', () => {
  const reason = (n) => { try { validateName(n); return 'ok'; } catch (e) { return e.extra.reason; } };

  it('length 2..10', () => {
    expect(reason('A')).toBe('too_short');
    expect(reason(' A ')).toBe('too_short'); // trimmed
    expect(reason('AB')).toBe('ok');
    expect(reason('abcdefghij')).toBe('ok');
    expect(reason('abcdefghijk')).toBe('too_long');
  });
  it('charset ASCII letters digits space . _ -', () => {
    expect(reason('Sally_99')).toBe('ok');
    expect(reason('a.b-c d')).toBe('ok');
    expect(reason('Sally!')).toBe('charset');
    expect(reason('Kýle')).toBe('charset');
    expect(reason('名前だよ')).toBe('charset');
    expect(reason('a\tb')).toBe('charset');
    expect(reason('<b>hi</b>')).toBe('charset');
  });
  it('edges and spacing', () => {
    expect(reason('-Sally')).toBe('edges');
    expect(reason('Sally.')).toBe('edges');
    expect(reason('A  B')).toBe('spaces');
    expect(reason('a-_.b')).toBe('ok');
    expect(reason('1.')).toBe('edges');
  });
  it('reserved names and the basic profanity filter (incl. leet spellings)', () => {
    expect(reason('Admin')).toBe('reserved');
    expect(reason('4dm1n')).toBe('reserved');
    expect(reason('Nintendo')).toBe('reserved');
    expect(reason('fuckface')).toBe('filtered');
    expect(reason('Sh1tLord')).toBe('filtered');
    expect(reason('xX_n1gg_Xx')).toBe('filtered');
    expect(reason('H1tler')).toBe('filtered');
    expect(reason('ass')).toBe('filtered');
    expect(reason('Glass')).toBe('ok'); // short words only match whole names
    expect(reason('Dickens')).toBe('ok'); // S10: the short words match as whole words only (it was a documented limitation)
    expect(reason('Mario')).toBe('ok');
  });
  it('extra blocklist from config', async () => {
    const s = makeServer({ NAME_BLOCKLIST_EXTRA: 'banana,pumpkin' });
    const r = await s.call('POST', '/v1/register', { name: 'MrBanana' });
    expect(r.status).toBe(400);
    expect(r.data.error.reason).toBe('filtered');
    expect((await s.call('POST', '/v1/register', { name: 'MrApple' })).status).toBe(200);
  });
  it('non-strings', () => {
    expect(reason(null)).toBe('type');
    expect(reason(12)).toBe('type');
  });
  it('fold key collapses look-alikes', () => {
    expect(foldKey('Sally')).toBe(foldKey('SaIly'));
    expect(foldKey('Sally')).toBe(foldKey('sa11y'));
    expect(foldKey('S a-l.ly')).toBe(foldKey('sally'));
    expect(foldKey('P4ul')).toBe(foldKey('paul'));
  });
});

describe('name uniqueness', () => {
  it('same name, different case, spacing or look-alikes is taken', async () => {
    const s = makeServer();
    await signUp(s, 'Sally');
    for (const dup of ['sally', 'SALLY', 'SaIly', 'Sa11y', 'S.a.l.l.y', 's a l l y']) {
      const r = await s.call('POST', '/v1/register', { name: dup });
      expect(r.status, dup).toBe(409);
      expect(r.data.error.code).toBe('name_taken');
    }
    expect((await s.call('POST', '/v1/register', { name: 'Sally2' })).status).toBe(200);
  });

  it('rename: allowed once per cooldown, unique, keeps token', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    await signUp(s, 'Taken');
    const clash = await s.call('PUT', '/v1/me', { name: 'taken' }, a.token);
    // inside the 24h cooldown the cooldown check fires first
    expect(clash.status).toBe(429);
    expect(clash.data.error.code).toBe('rename_too_soon');
    s.advance(25 * 3600_000);
    expect((await s.call('PUT', '/v1/me', { name: 'taken' }, a.token)).data.error.code).toBe('name_taken');
    const ok = await s.call('PUT', '/v1/me', { name: 'Omega' }, a.token);
    expect(ok.status).toBe(200);
    expect(ok.data.name).toBe('Omega');
    // the old name is free again
    expect((await s.call('POST', '/v1/register', { name: 'Alpha' })).status).toBe(200);
    // same name with different case by the owner is a no-op on the cooldown rules (allowed after cooldown)
    s.advance(25 * 3600_000);
    expect((await s.call('PUT', '/v1/me', { name: 'OMEGA' }, a.token)).status).toBe(200);
  });

  it('region can change without touching the name cooldown', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha', 2);
    const r = await s.call('PUT', '/v1/me', { name: 'Alpha', region: 4 }, a.token);
    expect(r.status).toBe(200);
    expect(r.data.region).toBe(4);
  });
});

describe('account deletion and bans', () => {
  it('DELETE /v1/me removes the profile, scores and queue entry, frees the name, invalidates the token', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect((await s.call('DELETE', '/v1/me', undefined, a.token)).status).toBe(200);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(401);
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM queue').one().n).toBe(0);
    expect((await s.call('POST', '/v1/register', { name: 'Alpha' })).status).toBe(200);
  });

  it('banned accounts get 403', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Alpha');
    const adm = { authorization: 'Bearer test-admin-token-0123456789' };
    expect((await s.call('POST', '/v1/admin/ban', { pid: a.pid, reason: 'test' }, undefined, adm)).status).toBe(200);
    const r = await s.call('GET', '/v1/me', undefined, a.token);
    expect(r.status).toBe(403);
    expect(r.data.error.code).toBe('banned');
  });

  it('player cap closes registration', async () => {
    const s = makeServer({ MAX_PLAYERS: '2' });
    await signUp(s, 'Aaa1');
    await signUp(s, 'Bbb2');
    const r = await s.call('POST', '/v1/register', { name: 'Ccc3' });
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('server_full');
  });
});
