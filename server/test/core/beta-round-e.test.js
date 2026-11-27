// Round e (package k): token and address masking, summary metering, D1 timeouts, report storage details, the turn check's new-account gate.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { makeSmg, smgSignUp, roomOf } from '../smg-helpers.js';
import { maskText, flatObject, REPORT_BODY_MAX, REPORT_MAX_LOG_BYTES } from '../../src/core/reports.js';
import { GRAPHQL_URL } from '../../src/core/turn-usage.js';

const ADMIN = 'test-admin-token-0123456789';
const DAY = 86400_000;
let lines; let spy;
beforeEach(() => { lines = []; spy = vi.spyOn(console, 'log').mockImplementation((x) => { lines.push(String(x)); }); });
afterEach(() => spy.mockRestore());
const body = (o = {}) => ({ v: 1, kind: 'user', where: 'settings', log: 'line', ...o });

describe('masking: tokens and more addresses', () => {
  it('the request\'s own bearer token goes, wherever it appears (log, note, values, keys); other tokens by shape', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Tok');
    expect(a.token).toMatch(/^[A-Za-z0-9_-]{43}$/);
    const r = await s.call('POST', '/v1/report', body({ note: `tok ${a.token}`, log: `Authorization: Bearer ${a.token}\nx ${a.token} y\nBearer abc.def/ghi\nbearer   zzz`, check: { t: a.token }, stats: { u: `Bearer ${a.token}` } }), a.token);
    expect(r.status).toBe(201);
    const row = s.d1.rows('SELECT * FROM reports')[0];
    const all = JSON.stringify(row) + lines.join('\n');
    expect(all).not.toContain(a.token);
    expect(row.log).toBe('Authorization: Bearer [REDACTED]\nx [REDACTED] y\nBearer [REDACTED]\nBearer [REDACTED]');
    expect(row.note).toBe('tok [REDACTED]');
  });
  it('a 43-character token that is NOT the request\'s own, and any 40+ run of [A-Za-z0-9_-], becomes [token]; a 40+ hex run stays [hex]; shorter runs stay', () => {
    const other = 'Ab_-'.repeat(11).slice(0, 43);
    expect(maskText(`x ${other} y`)).toBe('x [token] y');
    expect(maskText(`${'a1'.repeat(25)}`)).toBe('[hex]');
    expect(maskText('f'.repeat(32))).toBe('[hex]');
    expect(maskText('g'.repeat(39))).toBe('g'.repeat(39));
    expect(maskText('g'.repeat(40))).toBe('[token]');
    expect(maskText(`${'a'.repeat(32)}Zq-_ab` + 'x'.repeat(8))).toBe('[token]'); // a token that STARTS with 32 hex digits is masked whole
    expect(maskText('Bearer abc')).toBe('Bearer [REDACTED]');
    expect(maskText('xBearer abc')).toBe('xBearer abc'); // only the word
  });
  it('MAC addresses (colon, dash, dotted) are masked; a time or a short hex run is not', () => {
    for (const m of ['aa:bb:cc:dd:ee:ff', 'AA-BB-CC-DD-EE-FF', 'aabb.ccdd.eeff', '00:1A:2b:3C:4d:5E']) expect(maskText(`mac ${m} ok`), m).toBe('mac [mac] ok');
    expect(maskText('at 12:34:56 and 12:34:56:78')).toBe('at 12:34:56 and 12:34:56:78');
  });
  it('IPv4 glued to letters or underscores is masked; plain cases keep working', () => {
    expect(maskText('ip_1.2.3.4 x192.168.1.1 1.2.3.4b host.10.0.0.1 a=203.0.113.7:41100')).toBe('ip_[ip] x[ip] [ip]b host.[ip] a=[ip]:41100');
    expect(maskText('203.0.113.7 and 10.0.0.1')).toBe('[ip] and [ip]');
    expect(maskText('app 3.0.0 ratio 1.5 999.1.1.1')).toBe('app 3.0.0 ratio 1.5 999.1.1.1');
  });
  it('IPv6 followed by a letter is masked; the earlier forms keep working', () => {
    expect(maskText('a 2001:db8::1x b')).toBe('a [ip]x b');
    expect(maskText('a 2001:db8::7g')).toBe('a [ip]g');
    for (const a of ['2001:db8:85a3:0:0:8a2e:370:7334', '2001:db8::1', '::1', '::ffff:1.2.3.4', 'fe80::1ff:fe23:4567:890a']) expect(maskText(`from ${a} ok`), a).toBe('from [ip] ok');
    expect(maskText('[2001:db8::1]:8080')).toBe('[[ip]]:8080');
    expect(maskText('12:34:56 std::string')).toBe('12:34:56 std::string');
    expect(maskText('a@b.co and ' + 'ab'.repeat(16))).toBe('[email] and [hex]');
  });
  it('masking is idempotent', () => {
    const once = maskText('Bearer x a@b.cc aa:bb:cc:dd:ee:ff 1.2.3.4 ::1 ' + 'z'.repeat(45));
    expect(maskText(once)).toBe(once);
  });
  it('every rule is linear: 48 KB adversarial inputs take milliseconds', () => {
    const inputs = ['a'.repeat(48000), 'a.'.repeat(24000), 'a@'.repeat(24000), '1:'.repeat(24000), 'a:'.repeat(24000), '1.'.repeat(24000), ':'.repeat(48000), '0'.repeat(48000), 'a-'.repeat(24000), 'Bearer '.repeat(6000), '1.2.3.'.repeat(8000), 'aa:'.repeat(16000), 'ab.'.repeat(16000)];
    for (const i of inputs) { const t = Date.now(); maskText(i); expect(Date.now() - t, i.slice(0, 12)).toBeLessThan(400); }
  });
  it('flat object KEYS are masked as well; a key that masks to the same text merges', () => {
    const o = flatObject({ '10.0.0.5': 1, 'aa-bb-cc-dd-ee-ff': 2, ['ab'.repeat(16)]: 3, 'a b': 4, ok: 5 });
    expect(Object.keys(o).sort()).toEqual(['_hex_', '_ip_', '_mac_', 'ok']);
    expect(JSON.stringify(o)).not.toMatch(/10\.0\.0\.5|aa-bb|abab/);
    expect(flatObject({ '10.0.0.5': 1, '10.0.0.6': 2 })).toEqual({ _ip_: 2 }); // two keys that mask alike merge (the later value)
  });
});

describe('report storage details', () => {
  async function srv(env = {}) { const s = makeSmg(env); const a = await smgSignUp(s, 'Rep'); return { s, a, post: (b, t = a) => s.call('POST', '/v1/report', b, t.token) }; }
  it('a heavily escaped but legal 48,000-byte log fits the body cap: control characters (6 bytes each as \\u00XX) and quotes; one byte over the log cap is still 413', async () => {
    const { post, s } = await srv({ REPORT_MAX_PER_PLAYER_DAY: '9' });
    expect(REPORT_BODY_MAX).toBeGreaterThanOrEqual(REPORT_MAX_LOG_BYTES * 6 + 1024);
    const ctl = JSON.stringify(body({ log: '\u0001'.repeat(REPORT_MAX_LOG_BYTES) }));
    expect(ctl.length).toBeGreaterThan(288000);
    expect((await post(JSON.parse(ctl))).status).toBe(201);
    expect((await post(body({ log: '"'.repeat(REPORT_MAX_LOG_BYTES) }))).status).toBe(201);
    expect((await post(body({ log: '\u0001'.repeat(REPORT_MAX_LOG_BYTES + 1) }))).status).toBe(413);
    expect(s.d1.rows('SELECT COUNT(*) AS n FROM reports')[0].n).toBe(2);
  });
  it('the raw body cap of the object follows the same number (a body over it is 413 before parsing)', async () => {
    const { s, a } = await srv();
    const big = JSON.stringify(body({ log: 'x'.repeat(REPORT_BODY_MAX) }));
    const r = await s.raw('POST', '/v1/report', big, a.token, { 'content-length': String(big.length) });
    expect(r.status).toBe(413);
  });
  it('bytes comes BEFORE log in the table definition', async () => {
    const { post, s } = await srv();
    await post(body());
    const cols = s.d1.rows(`PRAGMA table_info(reports)`).map((c) => c.name);
    expect(cols.indexOf('bytes')).toBeGreaterThan(-1);
    expect(cols.indexOf('bytes')).toBeLessThan(cols.indexOf('log'));
  });
  it('at the row cap the running totals stay exact with NO recount per report (the hourly sweep recounts)', async () => {
    const { post, s, a } = await srv({ REPORT_MAX_ROWS: '3', REPORT_MAX_PER_PLAYER_DAY: '99', REPORT_MAX_PER_ADDR_DAY: '99' });
    for (let i = 0; i < 8; i++) { await post(body({ log: `entry ${i} ` + 'ab '.repeat(i + 1) })); s.advance(1000); }
    const rows = s.d1.rows('SELECT COUNT(*) AS n, SUM(bytes) AS b FROM reports')[0];
    expect(rows.n).toBe(3);
    const st = (await s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data.reports;
    expect(st.stored).toBe(3);
    expect(st.bytes).toBe(rows.b);
    expect(s.d1.sqls.some((q) => /COUNT\(\*\) AS n, COALESCE\(SUM/.test(q))).toBe(false);
    await s.app.turnTick(); s.advance(3600_000 + 1); await s.app.turnTick();
    expect(s.d1.sqls.some((q) => /COUNT\(\*\) AS n, COALESCE\(SUM/.test(q))).toBe(true); // the sweep does recount
    expect((await s.raw('GET', '/v1/admin/stats', undefined, ADMIN)).data.reports.bytes).toBe(rows.b);
  });
  it('the admin list reads the stored size from the bytes column (no LENGTH over the text)', async () => {
    const { post, s } = await srv();
    await post(body({ log: 'hello' }));
    const r = await s.raw('GET', '/v1/admin/reports', undefined, ADMIN);
    expect(r.data.reports[0].bytes).toBe(5 + 0 + 0 + 0 + 120);
    expect(s.d1.sqls.some((q) => /LENGTH\(CAST\(log/.test(q))).toBe(false);
  });
  it('the purge queue is unbounded-safe: 1,500 queued players all go, in batches over several ticks, and the queue ends empty', async () => {
    const { s } = await srv();
    await s.d1.prepare(`CREATE TABLE IF NOT EXISTS reports (id TEXT PRIMARY KEY, pid INTEGER NOT NULL, created INTEGER NOT NULL, kind TEXT NOT NULL, where_ TEXT NOT NULL, app TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', check_json TEXT NOT NULL DEFAULT '', stats_json TEXT NOT NULL DEFAULT '', bytes INTEGER NOT NULL DEFAULT 0, log TEXT NOT NULL DEFAULT '')`).run();
    for (let i = 0; i < 1500; i++) {
      s.store.sql.exec(`INSERT INTO counters (key, value, updated_ms) VALUES (?, 1, ?)`, `purge:${900000 + i}`, s.clock.now);
      s.d1.raw.prepare(`INSERT INTO reports (id, pid, created, kind, where_, bytes) VALUES (?, ?, ?, 'user', 'settings', 10)`).run(`r${i}`, 900000 + i, s.clock.now);
    }
    for (let k = 0; k < 10; k++) await s.app.turnTick();
    expect(s.d1.rows('SELECT COUNT(*) AS n FROM reports')[0].n).toBe(0);
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM counters WHERE key LIKE 'purge:%'`).toArray()[0].n).toBe(0);
  });
  it('a stalled D1 turns a report into 503 report_storage after the time limit and gives the limits back', async () => {
    const { post, s } = await srv({ REPORT_D1_TIMEOUT_MS: '50' });
    s.d1.hang = true;
    const r = await post(body());
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('report_storage');
    s.d1.hang = false;
    for (let i = 0; i < 3; i++) expect((await post(body())).status).toBe(201);
  });
});

describe('session summaries are metered', () => {
  const SUM = { transport: 'udp', ended: 'leave', dur_s: 10 };
  async function srv(env = {}) {
    const s = makeSmg(env);
    const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb');
    const room = () => roomOf(s, [a, b]);
    return { s, a, b, room };
  }
  const nSum = (s) => (s.d1.sqls.length ? s.d1.rows('SELECT COUNT(*) AS n FROM summaries')[0].n : 0);
  it('leaving a closed or expired room again records NOTHING: no D1 call, no log line, the leave still answers ok', async () => {
    const { s, b, room } = await srv();
    const code = await room();
    expect((await s.call('POST', `/v1/rooms/${code}/leave`, { summary: SUM }, b.token)).status).toBe(200);
    expect(nSum(s)).toBe(1);
    // close the room completely: the host leaves, then the room is closed; a second leave by a former member is refused or ok-without-removal, never a write
    const calls = s.d1.calls; lines.length = 0;
    for (let i = 0; i < 5; i++) await s.call('POST', `/v1/rooms/${code}/leave`, { summary: SUM }, b.token);
    expect(s.d1.calls).toBe(calls);
    expect(lines.filter((l) => l.includes('"evt":"summary"'))).toHaveLength(0);
  });
  it('an expired room: leave answers ok; the member (its row is still there) gets ONE summary (round f), a repeat records nothing', async () => {
    const { s, a, b, room } = await srv();
    const code = await room();
    s.advance(3 * 3600_000);
    s.store.sql.exec(`UPDATE rooms SET state = 'expired' WHERE code = ?`, code);
    for (let i = 0; i < 5; i++) { const r = await s.call('POST', `/v1/rooms/${code}/leave`, { summary: SUM }, b.token); expect(r.status).toBe(200); expect(r.data.state).toBe('expired'); }
    expect(nSum(s)).toBe(1);
  });
  it('one summary per player and room, even when the player joins and leaves again', async () => {
    const { s, a, b } = await srv();
    const made = await s.call('POST', '/v1/rooms', { mode: 'coop', visibility: 'public' }, a.token);
    const code = made.data.code;
    for (let i = 0; i < 3; i++) {
      const j = await s.call('POST', '/v1/rooms/join', { code }, b.token);
      expect(j.status).toBe(200);
      expect((await s.call('POST', `/v1/rooms/${code}/leave`, { summary: SUM }, b.token)).status).toBe(200);
    }
    expect(nSum(s)).toBe(1);
  });
  it('SUMMARY_MAX_PER_DAY in all, SUMMARY_MAX_PER_PLAYER_DAY a player: past them the leave works and nothing is stored or logged; a new UTC day opens again', async () => {
    const { s, a, b } = await srv({ SUMMARY_MAX_PER_DAY: '3', SUMMARY_MAX_PER_PLAYER_DAY: '2' });
    const leaveNew = async (p) => { const m = await s.call('POST', '/v1/rooms', { mode: 'coop', visibility: 'public' }, a.token); await s.call('POST', '/v1/rooms/join', { code: m.data.code }, p.token); return s.call('POST', `/v1/rooms/${m.data.code}/leave`, { summary: SUM }, p.token); };
    for (let i = 0; i < 4; i++) expect((await leaveNew(b)).status).toBe(200);
    expect(nSum(s)).toBe(2); // the player's 2
    for (let i = 0; i < 3; i++) expect((await leaveNew(a)).status).toBeLessThan(500);
    expect(nSum(s)).toBeLessThanOrEqual(3);
    s.advance(DAY);
    expect((await leaveNew(b)).status).toBe(200);
    expect(nSum(s)).toBeGreaterThanOrEqual(3);
  });
  it('a stalled D1 never holds a leave (it answers ok after SUMMARY_TIMEOUT_MS), and the next summary is stored once D1 is back', async () => {
    const { s, b, room } = await srv({ SUMMARY_TIMEOUT_MS: '50' });
    const code = await room();
    s.d1.hang = true;
    const t0 = Date.now();
    const r = await s.call('POST', `/v1/rooms/${code}/leave`, { summary: SUM }, b.token);
    expect(r.status).toBe(200);
    expect(Date.now() - t0).toBeLessThan(1500);
    s.d1.hang = false;
    const code2 = await room();
    expect((await s.call('POST', `/v1/rooms/${code2}/leave`, { summary: SUM }, b.token)).status).toBe(200);
    expect(nSum(s)).toBe(1);
  });
  it('"ended" takes letters and underscores only: error:10.0.0.5 and digit forms are not accepted', async () => {
    const { clampSummary } = await import('../../src/core/summaries.js');
    expect(clampSummary({ transport: 'udp', ended: 'error:10.0.0.5' }).ended).toBeUndefined();
    expect(clampSummary({ transport: 'udp', ended: 'error:e1' }).ended).toBeUndefined();
    expect(clampSummary({ transport: 'udp', ended: 'error:rx_timeout' }).ended).toBe('error:rx_timeout');
  });
});

describe('/v1/turn/check: the lower daily cap and the new-account gate', () => {
  const KEY = 'abcdef0123456789abcdef0123456789';
  const ENV = { TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: KEY, TURN_KEY_API_TOKEN: 'key-token-secret-value', TURN_USAGE_TOKEN: 'usage-token-secret-value', TURN_ACCOUNT_ID: '0123456789abcdef0123456789abcdef' };
  const fetchImpl = async (url) => {
    const u = String(url);
    if (u === GRAPHQL_URL) { const g = (v) => [{ sum: { egressBytes: v } }]; return Response.json({ data: { viewer: { accounts: [{ gm: g(0), gd: g(0), am: g(0) }] } } }); }
    if (/generate-ice-servers$/.test(u)) return Response.json({ iceServers: { urls: ['turn:turn.cloudflare.com:3478?transport=udp'], username: 'u1', credential: 'c1' } });
    return new Response(null, { status: 204 });
  };
  it('TURN_CHECK_MAX_PER_DAY defaults to 60', () => {
    const s = makeSmg({ ...ENV });
    expect(s.app.cfg.turn.checkMaxPerDay).toBe(60);
  });
  it('a fresh account (younger than TURN_NEW_ACCOUNT_HOURS) draws on the new-account slice: once it is used up the check is 503 turn_budget; an older account still passes; a give-back restores the slice', async () => {
    const s = makeSmg({ ...ENV, TURN_NEW_ACCOUNT_HOURS: '24', TURN_NEW_ACCOUNT_DAILY: '1' }, { deps: { fetchImpl } });
    const a = await smgSignUp(s, 'New1'); const b = await smgSignUp(s, 'New2');
    expect((await s.call('POST', '/v1/turn/check', undefined, a.token)).status).toBe(200);
    const r = await s.call('POST', '/v1/turn/check', undefined, b.token);
    expect(r.status).toBe(503);
    expect(r.data.error.code).toBe('turn_budget');
    s.advance(25 * 3600_000); // b is older than 24 h now (the day counter has moved too)
    expect((await s.call('POST', '/v1/turn/check', undefined, b.token)).status).toBe(200);
  });
  it('the slice counter is shared with normal passes: a check by a fresh account counts as one of the new-account passes', async () => {
    const s = makeSmg({ ...ENV, TURN_NEW_ACCOUNT_HOURS: '24', TURN_NEW_ACCOUNT_DAILY: '5' }, { deps: { fetchImpl } });
    const a = await smgSignUp(s, 'New3');
    await s.call('POST', '/v1/turn/check', undefined, a.token);
    const row = s.store.sql.exec(`SELECT value FROM counters WHERE key LIKE 'turn:creds:%:new'`).toArray()[0];
    expect(row.value).toBe(1);
  });
});
