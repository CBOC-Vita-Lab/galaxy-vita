// Galaxy 3.0 beta diagnostics: POST /v1/report (masking, shapes, limits, caps, TTL, account purge, the D1 store), the admin routes, the Workers Logs line shapes.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';
import { makeD1 } from '../stubs/d1.js';
import { maskText, flatObject, chunkUtf8 } from '../../src/core/reports.js';
import { edgeCheck } from '../../src/core/edge.js';
import { REPORT_BODY_MAX } from '../../src/core/reports.js';

const ADMIN = 'test-admin-token-0123456789';
const DAY = 86400_000;
const body = (o = {}) => ({ v: 1, kind: 'user', where: 'settings', log: 'line 1\nline 2', ...o });
let lines;
let spy;
beforeEach(() => { lines = []; spy = vi.spyOn(console, 'log').mockImplementation((x) => { lines.push(String(x)); }); });
afterEach(() => spy.mockRestore());
const logged = () => lines.map((l) => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);

async function setup(env = {}, n = 1) {
  const s = makeSmg(env);
  const ps = [];
  for (let i = 0; i < n; i++) ps.push(await smgSignUp(s, `Player${String.fromCharCode(65 + i)}`));
  const report = (p, b = body(), headers = {}) => s.call('POST', '/v1/report', b, p.token, headers);
  const admin = (path) => s.raw('GET', path, undefined, ADMIN);
  return { s, ps, a: ps[0], report, admin };
}

describe('masking', () => {
  it('IPv4 addresses (also with a port) become [ip]; a version string of three parts and a number with a dot stay', () => {
    expect(maskText('peer 203.0.113.7:41100 and 10.0.0.1')).toBe('peer [ip]:41100 and [ip]');
    expect(maskText('app 3.0.0 ratio 1.5 build 3.0.0.12')).toBe('app 3.0.0 ratio 1.5 build [ip]');
    expect(maskText('999.1.1.1 stays')).toBe('999.1.1.1 stays');
  });
  it('IPv6 addresses in every form become [ip]: full, compressed, loopback, mapped, bracketed with a port', () => {
    for (const a of ['2001:db8:85a3:0:0:8a2e:370:7334', '2001:db8::1', '::1', 'fe80::1ff:fe23:4567:890a', '::ffff:192.0.2.128', '1:2:3:4:5:6:7:8', 'ff02::']) {
      expect(maskText(`from ${a} ok`), a).toBe('from [ip] ok');
    }
    expect(maskText('[2001:db8::1]:8080')).toBe('[[ip]]:8080');
  });
  it('times, C++ scopes and short colon runs are not addresses', () => {
    for (const t of ['12:34:56', 'std::string', 'Foo::Bar::baz', 'ratio 1:2', 'at 23:59:59.123', 'a:b:c', 'key:value:other']) expect(maskText(t), t).toBe(t);
  });
  it('4+ digits after profile/pid/account/uid/player are hidden (any case, with or without = or :); short, glued and plural forms stay', () => {
    expect(maskText('login ok: profile 100003')).toBe('login ok: profile [id]');
    for (const w of ['profile', 'pid', 'account', 'uid', 'player', 'PID', 'Player', 'UID']) {
      for (const sep of [' ', '=', ':', ' = ', ': ']) expect(maskText(`x ${w}${sep}4711 y`), w + sep).toBe(`x ${w}${sep}[id] y`);
    }
    expect(maskText('uid=1234567890123 and pid 99999')).toBe('uid=[id] and pid [id]');
    for (const t of ['player 12', 'profile 123', 'players 12345', 'pid4711', 'xpid 4711', 'account number 12345']) expect(maskText(t), t).toBe(t);
    const once = maskText('profile 100003 pid=4711');
    expect(maskText(once)).toBe(once);
  });
  it('e-mail addresses become [email]; 32 or more hex digits become [hex]; 31 stay', () => {
    expect(maskText('mail me: a.b+c@example.co.uk now')).toBe('mail me: [email] now');
    expect(maskText(`id ${'ab'.repeat(16)} and ${'f'.repeat(40)}`)).toBe('id [hex] and [hex]');
    expect(maskText('f'.repeat(31))).toBe('f'.repeat(31));
  });
  it('control characters go (except newline and tab); masking twice changes nothing more', () => {
    expect(maskText('a\u0000b\u0007c\u001bd\re\nf\tg\u007fh\u0085i')).toBe('abcde\nf\tghi'.replace('abcde', 'abcd' + 'e'));
    const once = maskText('x 1.2.3.4 y a@b.cc z ::1');
    expect(maskText(once)).toBe(once);
  });
  it('flatObject keeps short strings and finite numbers, masks string values, drops nested values, booleans, null, bad keys and everything past 40 keys', () => {
    const big = Object.fromEntries(Array.from({ length: 50 }, (_, i) => [`k${i}`, i]));
    expect(flatObject({ a: 'x 1.2.3.4', b: 2, c: { d: 1 }, e: [1], f: true, g: null, 'bad key!': 1, h: NaN, i: 'y '.repeat(50) })).toEqual({ a: 'x [ip]', b: 2, i: 'y '.repeat(32) });
    expect(Object.keys(flatObject(big))).toHaveLength(40);
    expect(flatObject(null)).toBeNull();
    expect(flatObject({ a: { b: 1 } })).toBeNull();
    expect(() => flatObject('text')).toThrow();
    expect(() => flatObject([1])).toThrow();
  });
  it('chunkUtf8 never splits a character and never exceeds the size', () => {
    const text = 'é€𝄞a'.repeat(3000);
    const parts = chunkUtf8(text, 1000);
    expect(parts.join('')).toBe(text);
    for (const p of parts) expect(new TextEncoder().encode(p).length).toBeLessThanOrEqual(1000);
    expect(chunkUtf8('')).toEqual(['']);
  });
});

describe('POST /v1/report', () => {
  it('stores a masked report and answers 201 {id} of 8 characters; the row holds exactly the masked text', async () => {
    const t = await setup();
    const r = await t.report(t.a, body({ kind: 'after_error', where: 'post_session', note: 'froze at 203.0.113.9', check: { nat: 'symmetric', rtt: 41, deep: { x: 1 } }, stats: { rx: 12.5, who: 'a@b.com' },
      log: 'peer 198.51.100.4:41100 mail a@b.com id ' + 'ab'.repeat(20) + ' v6 2001:db8::7 \u0001ok' }), { 'x-msc-app': '3.0.1' });
    expect(r.status).toBe(201);
    expect(r.data.id).toMatch(/^[A-Za-z0-9_-]{8}$/);
    const rows = t.s.d1.rows('SELECT * FROM reports');
    expect(rows).toHaveLength(1);
    expect(rows[0]).toMatchObject({ id: r.data.id, pid: t.a.pid, kind: 'after_error', where_: 'post_session', app: '3.0.1', note: 'froze at [ip]' });
    expect(rows[0].log).toBe('peer [ip]:41100 mail [email] id [hex] v6 [ip] ok');
    expect(JSON.parse(rows[0].check_json)).toEqual({ nat: 'symmetric', rtt: 41 });
    expect(JSON.parse(rows[0].stats_json)).toEqual({ rx: 12.5, who: '[email]' });
  });
  it('bad shapes are 400 and nothing is stored or counted', async () => {
    const t = await setup();
    const bad = [
      undefined, '[]', '"x"', body({ v: 2 }), body({ v: undefined }), body({ kind: 'other' }), body({ where: 'lobby' }), body({ note: 5 }), body({ note: 'x'.repeat(201) }),
      body({ log: 5 }), body({ log: undefined }), body({ check: 'text' }), body({ stats: [1] }), body({ extra: 1 }), 'not json',
    ];
    for (const b of bad) {
      const r = await t.s.call('POST', '/v1/report', b, t.a.token);
      expect(r.status, JSON.stringify(b)).toBe(400);
    }
    expect(t.s.d1.calls).toBe(0); // validation comes before any storage
    expect((await t.report(t.a)).status).toBe(201); // the limit of 3 was not used by the junk
  });
  it('a note of exactly 200 characters, a log of exactly 48,000 bytes, check and stats null or absent are accepted; 48,001 bytes is 413', async () => {
    const t = await setup({ REPORT_MAX_PER_PLAYER_DAY: '9' });
    expect((await t.report(t.a, body({ note: 'n'.repeat(200), check: null, stats: null, log: 'a'.repeat(48000) }))).status).toBe(201);
    const r = await t.report(t.a, body({ log: 'a'.repeat(48001) }));
    expect(r.status).toBe(413);
    expect((await t.report(t.a, body({ log: 'é'.repeat(24001) }))).status).toBe(413); // bytes, not characters
    expect((await t.report(t.a, body({ log: 'é'.repeat(24000) }))).status).toBe(201);
    expect(t.s.d1.rows('SELECT COUNT(*) AS n FROM reports')[0].n).toBe(2);
  });
  it('the Worker refuses a body over the report cap before the object, and keeps the 4,096 cap on every other route', () => {
    const mk = (path, len) => new Request(`https://x.invalid${path}`, { method: 'POST', headers: { 'content-length': String(len), authorization: `Bearer ${'A'.repeat(43)}`, 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' }, body: 'x'.repeat(0) });
    const env = { SERVICE_GAME: 'smg', ADMIN_TOKEN: ADMIN };
    expect(edgeCheck(mk('/v1/report', REPORT_BODY_MAX), env)).toBeNull();
    expect(edgeCheck(mk('/v1/report', REPORT_BODY_MAX + 1), env)?.status).toBe(413);
    expect(edgeCheck(mk('/v1/friends', 4097), env)?.status).toBe(413);
  });
  it('3 reports a player a day (the 4th is 429 with Retry-After), a new UTC day opens it again', async () => {
    const t = await setup();
    for (let i = 0; i < 3; i++) expect((await t.report(t.a)).status).toBe(201);
    const r = await t.report(t.a);
    expect(r.status).toBe(429);
    expect(r.headers.get('retry-after')).toMatch(/^\d+$/);
    t.s.advance(DAY);
    expect((await t.report(t.a)).status).toBe(201);
  });
  it('30 reports an address a day, across accounts; another address is not affected', async () => {
    const t = await setup({}, 11);
    let n = 0;
    for (const p of t.ps.slice(0, 10)) for (let i = 0; i < 3; i++) { expect((await t.report(p, body(), { ip: '203.0.113.50' })).status).toBe(201); n++; }
    expect(n).toBe(30);
    const r = await t.report(t.ps[10], body(), { ip: '203.0.113.50' });
    expect(r.status).toBe(429);
    expect((await t.report(t.ps[10], body(), { ip: '203.0.113.51' })).status).toBe(201);
  });
  it('the day has its own small share of the request budget: past it, 503 report_budget (rooms are not touched)', async () => {
    const t = await setup({ DAILY_REQUEST_BUDGET: '100', REPORT_SHARE_PCT: '3' }, 3); // 3 % of 100 = 3 a day
    for (const p of t.ps) expect((await t.report(p)).status).toBe(201);
    const t2 = await setup({ DAILY_REQUEST_BUDGET: '100', REPORT_SHARE_PCT: '3' }, 4);
    for (const p of t2.ps.slice(0, 3)) await t2.report(p);
    const r = await t2.report(t2.ps[3]);
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('report_budget');
    expect(r.headers.get('retry-after')).toBe('3600');
    expect(t2.s.d1.rows('SELECT COUNT(*) AS n FROM reports')[0].n).toBe(3);
    // the share is per UTC day
    t2.s.advance(DAY);
    expect((await t2.report(t2.ps[3])).status).toBe(201);
  });
  it('the kill switch "report" answers 503 service_disabled; "all" too; nothing else is switched off', async () => {
    const t = await setup();
    await t.s.raw('POST', '/v1/admin/switches', { kill: ['report'] }, ADMIN);
    const r = await t.report(t.a);
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('service_disabled');
    expect((await t.s.call('GET', '/v1/me', undefined, t.a.token)).status).toBe(200);
    await t.s.raw('POST', '/v1/admin/switches', { kill: [] }, ADMIN);
    expect((await t.report(t.a)).status).toBe(201);
  });
  it('a failing D1 is 503 report_storage and gives the limits back (the same player can send once it works again)', async () => {
    const t = await setup();
    t.s.d1.failing = true;
    for (let i = 0; i < 5; i++) {
      const r = await t.report(t.a);
      expect(r.status).toBe(503);
      expect(r.data.error.code).toBe('report_storage');
    }
    t.s.d1.failing = false;
    for (let i = 0; i < 3; i++) expect((await t.report(t.a)).status).toBe(201);
    expect(lines.filter((l) => l.includes('"evt":"report"'))).toHaveLength(3); // a refused report is not logged
  });
  it('without the D1 binding the service says so (503 report_unavailable), everything else works', async () => {
    const t = await setup({ REPORTS_DB: null });
    const r = await t.report(t.a);
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('report_unavailable');
    expect((await t.s.raw('GET', '/v1/admin/reports', undefined, ADMIN)).status).toBe(503);
    expect((await t.s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).status).toBe(200);
  });
  it('tables are created on first use (none before the first report)', async () => {
    const t = await setup();
    expect(t.s.d1.rows(`SELECT name FROM sqlite_master WHERE type = 'table'`)).toEqual([]);
    await t.report(t.a);
    const names = t.s.d1.rows(`SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'`).map((r) => r.name).sort();
    expect(names).toEqual(['reports', 'summaries', 'summary_days']);
  });
});

describe('caps and expiry', () => {
  it('at most REPORT_MAX_ROWS rows: the oldest are dropped first', async () => {
    const t = await setup({ REPORT_MAX_ROWS: '3', REPORT_MAX_PER_PLAYER_DAY: '99' });
    const ids = [];
    for (let i = 0; i < 5; i++) { ids.push((await t.report(t.a, body({ log: `r${i}` }))).data.id); t.s.advance(1000); }
    const left = t.s.d1.rows('SELECT id, log FROM reports ORDER BY created').map((r) => r.log);
    expect(left).toEqual(['r2', 'r3', 'r4']);
    expect((await t.admin('/v1/admin/stats')).data.reports.stored).toBe(3);
  });
  it('at most REPORT_MAX_MB bytes: the oldest are dropped until the new one fits', async () => {
    const t = await setup({ REPORT_MAX_MB: '1', REPORT_MAX_PER_PLAYER_DAY: '999', REPORT_MAX_PER_ADDR_DAY: '999' });
    for (let i = 0; i < 40; i++) { await t.report(t.a, body({ log: `${i % 10} ` + 'ab '.repeat(15600) })); t.s.advance(1000); }
    const rows = t.s.d1.rows('SELECT SUM(bytes) AS b, COUNT(*) AS n FROM reports')[0];
    expect(rows.b).toBeLessThanOrEqual(1048576);
    expect(rows.n).toBeGreaterThan(15);
    expect(rows.n).toBeLessThan(40);
    const newest = t.s.d1.rows('SELECT log FROM reports ORDER BY created DESC LIMIT 1')[0].log;
    expect(newest[0]).toBe('9'); // the newest report is the one kept
    const stat = (await t.admin('/v1/admin/stats')).data.reports;
    expect(stat.stored).toBe(rows.n);
    expect(stat.bytes).toBe(rows.b);
  });
  it('rows older than 30 days go with the next report, and the hourly alarm sweep recounts', async () => {
    const t = await setup({ REPORT_MAX_PER_PLAYER_DAY: '99' });
    await t.report(t.a, body({ log: 'old' }));
    t.s.advance(29 * DAY);
    await t.report(t.a, body({ log: 'mid' }));
    expect(t.s.d1.rows('SELECT log FROM reports').map((r) => r.log).sort()).toEqual(['mid', 'old']);
    t.s.advance(2 * DAY); // 'old' is 31 days old, 'mid' 2
    await t.report(t.a, body({ log: 'new' }));
    expect(t.s.d1.rows('SELECT log FROM reports').map((r) => r.log).sort()).toEqual(['mid', 'new']);
    t.s.advance(29 * DAY); // 'mid' is 31 days old; no report arrives, the alarm's tick does it
    await t.s.app.turnTick();
    expect(t.s.d1.rows('SELECT log FROM reports').map((r) => r.log)).toEqual(['new']);
    expect((await t.admin('/v1/admin/stats')).data.reports.stored).toBe(1);
  });
});

describe('account deletion purges the reports', () => {
  it('DELETE /v1/me removes that player\'s reports from D1 and no one else\'s', async () => {
    const t = await setup({}, 2);
    await t.report(t.ps[0]); await t.report(t.ps[0]); await t.report(t.ps[1]);
    expect((await t.s.call('DELETE', '/v1/me', undefined, t.ps[0].token)).status).toBe(200);
    const rows = t.s.d1.rows('SELECT pid FROM reports');
    expect(rows).toEqual([{ pid: t.ps[1].pid }]);
    expect((await t.admin('/v1/admin/stats')).data.reports.stored).toBe(1);
  });
  it('a D1 outage at that moment loses nothing: the purge is remembered and done by the next alarm tick', async () => {
    const t = await setup();
    await t.report(t.a);
    t.s.d1.failing = true;
    expect((await t.s.call('DELETE', '/v1/me', undefined, t.a.token)).status).toBe(200); // the account is gone either way
    t.s.d1.failing = false;
    expect(t.s.d1.rows('SELECT * FROM reports')).toHaveLength(1);
    await t.s.app.turnTick();
    expect(t.s.d1.rows('SELECT * FROM reports')).toHaveLength(0);
    await t.s.app.turnTick(); // the queue is empty: nothing more happens
    expect(t.s.store.sql.exec(`SELECT COUNT(*) AS n FROM counters WHERE key LIKE 'purge:%'`).toArray()[0].n).toBe(0);
  });
});

describe('admin routes', () => {
  it('list: newest first, since filter, id / created / kind / where / app / note / log size, at most 100; no log text, no player id', async () => {
    const t = await setup({ REPORT_MAX_PER_PLAYER_DAY: '999', REPORT_MAX_PER_ADDR_DAY: '999', DAILY_REQUEST_BUDGET: '1000000' });
    const ids = [];
    for (let i = 0; i < 105; i++) { ids.push((await t.report(t.a, body({ log: 'é'.repeat(i + 1), note: `n${i}` }), { 'x-msc-app': '3.0.0' })).data.id); t.s.advance(1000); }
    const all = (await t.admin('/v1/admin/reports')).data.reports;
    expect(all).toHaveLength(100);
    expect(all[0]).toEqual({ id: ids[104], created: expect.any(Number), kind: 'user', where: 'settings', app: '3.0.0', note: 'n104', bytes: 334 }); // bytes: the stored size (log + note + objects + 120)
    expect(all[1].id).toBe(ids[103]);
    expect(JSON.stringify(all)).not.toMatch(/"log":|pid/);
    const since = all[4].created;
    const part = (await t.admin(`/v1/admin/reports?since=${since}`)).data.reports;
    expect(part.map((r) => r.id)).toEqual(ids.slice(100).reverse());
    expect((await t.admin('/v1/admin/reports?since=abc')).status).toBe(400);
  });
  it('get: the whole row, the player id as its last 4 digits only; unknown id is 404; both need the admin token', async () => {
    const t = await setup();
    const id = (await t.report(t.a, body({ check: { a: 1 }, stats: { b: 'x' }, log: 'hello' }))).data.id;
    const row = (await t.admin(`/v1/admin/reports/${id}`)).data;
    expect(row).toMatchObject({ id, pid: String(t.a.pid).slice(-4), kind: 'user', where: 'settings', check: { a: 1 }, stats: { b: 'x' }, log: 'hello', note: '' });
    expect(String(row.pid)).toHaveLength(4);
    expect(JSON.stringify(row)).not.toContain(String(t.a.pid));
    expect((await t.admin('/v1/admin/reports/nosuchid')).status).toBe(404);
    for (const path of ['/v1/admin/reports', `/v1/admin/reports/${id}`, '/v1/admin/summaries']) {
      expect((await t.s.raw('GET', path)).status).toBe(401);
      expect((await t.s.call('GET', path, undefined, t.a.token)).status).toBe(401); // a player token is not the admin token
    }
  });
  it('admin stats show the stored reports, today\'s count against the share, and the D1 binding', async () => {
    const t = await setup();
    await t.report(t.a);
    const st = (await t.admin('/v1/admin/stats')).data.reports;
    expect(st).toMatchObject({ stored: 1, today: 1, d1: true, maxRows: 2000, maxBytes: 64 * 1048576 });
    expect(st.shareToday).toBe(900); // 2 % of 45,000
  });
});

describe('Workers Logs lines', () => {
  it('one {evt:"report"} line with exactly id, kind, where, app, note, check, stats, log_bytes, then {evt:"report_log"} chunks of at most 16 KB (seq from 1, of = count)', async () => {
    const t = await setup();
    const text = ('é' + ' ab'.repeat(33) + '\n').repeat(300); // 30,600 bytes
    const r = await t.report(t.a, body({ kind: 'after_error', where: 'mp_menu', note: 'n 1.2.3.4', check: { x: 1 }, stats: { y: 'z' }, log: text }), { 'x-msc-app': '3.0.0' });
    expect(r.status).toBe(201);
    const ls = logged();
    const head = ls.filter((l) => l.evt === 'report');
    expect(head).toHaveLength(1);
    expect(head[0]).toEqual({ evt: 'report', id: r.data.id, kind: 'after_error', where: 'mp_menu', app: '3.0.0', note: 'n [ip]', check: { x: 1 }, stats: { y: 'z' }, log_bytes: 30600 });
    const chunks = ls.filter((l) => l.evt === 'report_log');
    expect(chunks).toHaveLength(2);
    expect(chunks.map((c) => c.seq)).toEqual([1, 2]);
    for (const c of chunks) {
      expect(Object.keys(c).sort()).toEqual(['evt', 'id', 'of', 'seq', 'text']);
      expect(c.id).toBe(r.data.id);
      expect(c.of).toBe(2);
      expect(new TextEncoder().encode(c.text).length).toBeLessThanOrEqual(16384);
    }
    expect(chunks.map((c) => c.text).join('')).toBe(text);
  });
  it('a small report is one chunk; check and stats are null when absent', async () => {
    const t = await setup();
    const r = await t.report(t.a, body({ log: '' }));
    const ls = logged();
    expect(ls.find((l) => l.evt === 'report')).toMatchObject({ id: r.data.id, note: '', check: null, stats: null, log_bytes: 0 });
    expect(ls.filter((l) => l.evt === 'report_log')).toEqual([{ evt: 'report_log', id: r.data.id, seq: 1, of: 1, text: '' }]);
  });
  it('no line holds a token, a player id, a name or an address', async () => {
    const t = await setup();
    await t.report(t.a, body({ note: 'from 203.0.113.9', log: 'peer 198.51.100.4 mail a@b.com' }), { ip: '192.0.2.77' });
    await t.s.call('POST', '/v1/rooms/ABCDE/leave', { summary: { transport: 'udp', dur_s: 5 } }, t.a.token);
    const all = lines.join('\n');
    for (const secret of [t.a.token, String(t.a.pid), 'PlayerA', '192.0.2.77', '203.0.113.9', '198.51.100.4', 'a@b.com', t.a.code]) expect(all, secret).not.toContain(secret);
  });
});
