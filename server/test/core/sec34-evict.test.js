// sec34 (VitAdhoc review finding 1): the per-address daily ceilings lived in the object's memory only. A Durable Object that is evicted
// between requests (what the platform does to an idle object) started every address again at zero. These tests build a FRESH app over the
// SAME storage for every request (what an eviction does) and show that the caps still trip; and that the write cost stays small.
import { describe, it, expect } from 'vitest';
import { openNodeSql } from '../../src/sqlite-node.js';
import { createApp } from '../../src/core/app.js';
import { createBudget } from '../../src/core/budget.js';
import { dayIndex, secondsToMidnight } from '../../src/core/limits.js';

const T0 = Date.UTC(2026, 10, 20, 12, 0, 0);
const baseEnv = { GAME: 'smg', ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REGISTER_POW_BITS: '0', RATE_LIMIT_SCALE: '1', ADDR_DAILY_OPEN: '50', DAILY_REQUEST_BUDGET: '100000', DAILY_OBJECT_BUDGET: '100000' };

function rig(env = {}) {
  const store = openNodeSql();
  const clock = { now: T0 };
  const e = { ...baseEnv, ...env };
  const mk = () => createApp({ sql: store.sql, tx: store.tx, env: e, now: () => clock.now });
  const writes = { n: 0 };
  const counting = { exec: (q, ...b) => { if (/^\s*(INSERT|UPDATE|DELETE|REPLACE)/i.test(q)) writes.n++; return store.sql.exec(q, ...b); } };
  const mkCounting = () => createApp({ sql: counting, tx: store.tx, env: e, now: () => clock.now });
  const hit = (app, ip, path = '/v1/info') => app.handle(new Request(`https://t.invalid${path}`, { headers: { 'cf-connecting-ip': ip, 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' } }));
  return { store, clock, mk, mkCounting, hit, writes };
}

describe('per-address caps survive eviction of the object', () => {
  it('resident object: the 51st open request of one address in a day is 429 daily_cap (control)', async () => {
    const r = rig();
    const app = r.mk();
    const codes = [];
    for (let i = 0; i < 60; i++) codes.push((await r.hit(app, '198.51.100.9')).status);
    expect(codes.slice(0, 50).every((c) => c === 200)).toBe(true);
    expect(codes.slice(50).every((c) => c === 429)).toBe(true);
  });
  it('a fresh app over the same storage for EVERY request: the cap still trips (it never did before the fix)', async () => {
    const r = rig();
    const codes = [];
    for (let i = 0; i < 200; i++) codes.push((await r.hit(r.mk(), '198.51.100.9')).status);
    const ok = codes.filter((c) => c === 200).length;
    expect(ok).toBe(50); // the first count after every restart is written at once: nothing is lost
    expect(codes.filter((c) => c === 429).length).toBeGreaterThan(100);
    expect(codes[199]).toBe(429);
  });
  it('another address is not affected, and the next UTC day starts from zero', async () => {
    const r = rig();
    for (let i = 0; i < 200; i++) await r.hit(r.mk(), '198.51.100.9');
    expect((await r.hit(r.mk(), '198.51.100.10')).status).toBe(200);
    r.clock.now += 86400_000;
    expect((await r.hit(r.mk(), '198.51.100.9')).status).toBe(200);
  });
  it('wrong-token (bad auth) counts also survive: the failed-authentication ceiling trips across fresh apps', async () => {
    const r = rig({ ADDR_BAD_DAY: '30', ADDR_DAILY_PLAYER: '100000' });
    const codes = [];
    for (let i = 0; i < 120; i++) {
      const res = await r.mk().handle(new Request('https://t.invalid/v1/me', { headers: { 'cf-connecting-ip': '198.51.100.20', authorization: `Bearer ${'A'.repeat(43)}`, 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' } }));
      codes.push(res.status);
    }
    expect(codes.filter((c) => c === 429).length).toBeGreaterThan(50);
  });
  it('the stored rows hold only a hash of the address, never the address', async () => {
    const r = rig();
    for (let i = 0; i < 45; i++) await r.hit(r.mk(), '198.51.100.77');
    const rows = r.store.raw.prepare(`SELECT * FROM addr_day`).all();
    expect(rows.length).toBeGreaterThan(0);
    expect(JSON.stringify(rows)).not.toContain('198.51.100.77');
    const all = JSON.stringify([r.store.raw.prepare(`SELECT * FROM settings`).all()]);
    expect(all).not.toContain('198.51.100.77');
  });
  it('write budget: a resident object writes one row per 20 counted requests of an address (plus the first of an open-class key), not one per request', async () => {
    const r = rig();
    const app = r.mk();
    const before = r.store.raw.prepare(`SELECT COUNT(*) AS n FROM addr_day`).get().n;
    let upserts = 0;
    const origExec = r.store.sql.exec;
    r.store.sql.exec = (q, ...b) => { if (/INTO addr_day/i.test(q)) upserts++; return origExec(q, ...b); };
    for (let i = 0; i < 40; i++) await r.hit(app, '198.51.100.31');
    r.store.sql.exec = origExec;
    expect(before).toBe(0);
    expect(upserts).toBe(3); // 40 requests -> writes at the 1st, the 20th and the 40th
  });
  it('rows of earlier days are deleted when the day rolls', async () => {
    const r = rig();
    for (let i = 0; i < 45; i++) await r.hit(r.mk(), '198.51.100.40');
    expect(r.store.raw.prepare(`SELECT COUNT(*) AS n FROM addr_day`).get().n).toBeGreaterThan(0);
    r.clock.now += 86400_000;
    await r.hit(r.mk(), '198.51.100.41');
    const days = r.store.raw.prepare(`SELECT DISTINCT day FROM addr_day`).all().map((x) => x.day);
    expect(days.every((d) => d === dayIndex(r.clock.now))).toBe(true);
  });
});

describe('fix34: rows read / written per request (point reads, step writes, MAX upsert, old days)', () => {
  // Counts the rows of every addr_day statement: rows READ = rows a SELECT on addr_day returned (a point read is at most 1), rows WRITTEN = one per INSERT/DELETE that changed a row.
  function meter(r) {
    const m = { read: 0, written: 0, bulk: 0 };
    const orig = r.store.sql.exec;
    r.store.sql.exec = (q, ...b) => {
      const cur = orig(q, ...b);
      if (/FROM addr_day/i.test(q) && /^\s*SELECT/i.test(q)) { const n = cur.toArray().length; m.read += n; if (!/WHERE day = \? AND cls = \? AND k = \?/i.test(q)) m.bulk++; }
      if (/INTO addr_day/i.test(q)) m.written++;
      return cur;
    };
    return m;
  }
  const poll = (app, ip) => app.handle(new Request('https://t.invalid/v1/info', { headers: { 'cf-connecting-ip': ip, 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' } }));
  it('no statement ever reads a day\'s rows in bulk, however many rows are stored', async () => {
    const r = rig();
    r.mk();
    for (let i = 0; i < 300; i++) r.store.raw.prepare(`INSERT INTO addr_day (day, cls, k, n) VALUES (?, 'open', ?, 3)`).run(dayIndex(T0), `k${i}`);
    const m = meter(r);
    for (let i = 0; i < 10; i++) await r.hit(r.mk(), '198.51.100.50');
    expect(m.bulk).toBe(0);
    expect(m.read).toBeLessThanOrEqual(10); // at most one row per request (the key's own)
  });
  it('MEASURED, open-class request (the worst case: first-written class), 400 other rows stored: resident vs evicted every request', async () => {
    const rr = rig({ ADDR_DAILY_OPEN: '100000' }), re = rig({ ADDR_DAILY_OPEN: '100000' });
    rr.mk(); re.mk();
    for (const r of [rr, re]) for (let i = 0; i < 400; i++) r.store.raw.prepare(`INSERT INTO addr_day (day, cls, k, n) VALUES (?, 'open', ?, 3)`).run(dayIndex(T0), `k${i}`);
    const mr = meter(rr), me = meter(re);
    const app = rr.mk();
    for (let i = 0; i < 200; i++) await rr.hit(app, '198.51.100.60');
    for (let i = 0; i < 200; i++) await re.hit(re.mk(), '198.51.100.60');
    console.log(`rows per open request: resident read ${mr.read / 200} written ${mr.written / 200}; evicted-every-request read ${me.read / 200} written ${me.written / 200}`);
    expect(mr.read / 200).toBeLessThan(0.02);
    expect(mr.written / 200).toBeLessThan(0.1);
    expect(me.read / 200).toBeLessThanOrEqual(1);
    expect(me.written / 200).toBeLessThanOrEqual(1);
  });
  it('MEASURED, authenticated poll (live class + account): evicted every request writes (almost) nothing, reads at most 2 point rows', async () => {
    const r = rig({ ADDR_DAILY_OPEN: '100000' });
    const m = meter(r);
    // drive the budget object itself with the live class and a pid key, as app.js does for a poll
    const mkDb = () => ({ get: (q, ...b) => r.store.sql.exec(q, ...b).toArray()[0], all: (q, ...b) => r.store.sql.exec(q, ...b).toArray(), run: (q, ...b) => { r.store.sql.exec(q, ...b); } });
    const cal = { dayIndex, secondsToMidnight };
    const cfg = { dailyBudget: 0, liveReservePct: 0, abuse: { openPerDay: 100000, playerPerDay: 100000, livePerDay: 100000, pidPerDay: 100000, badPerDay: 100000 }, reserve: {} };
    r.store.sql.exec(`CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)`);
    r.store.sql.exec(`CREATE TABLE IF NOT EXISTS addr_day (day INTEGER NOT NULL, cls TEXT NOT NULL, k TEXT NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (day, cls, k)) WITHOUT ROWID`);
    for (let i = 0; i < 400; i++) r.store.raw.prepare(`INSERT OR IGNORE INTO addr_day (day, cls, k, n) VALUES (?, 'live', ?, 3)`).run(dayIndex(T0), `k${i}`);
    for (let i = 0; i < 200; i++) { const b = createBudget(mkDb(), cfg, 1, cal); expect(b.gateAddress('live', 'poll-key', T0)).toBeNull(); expect(b.gatePid(7, T0)).toBeNull(); }
    console.log(`rows per live poll (class + account), evicted every request: read ${m.read / 200} written ${m.written / 200}`);
    expect(m.written / 200).toBe(0); // step-only classes: an evicted-every-request key never reaches a multiple of 20, so nothing is written (the price: such a key is not counted)
    expect(m.read / 200).toBeLessThanOrEqual(2);
  });
  it('the upsert keeps the larger count (a stale writer cannot lower it)', () => {
    const r = rig();
    r.store.sql.exec(`CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)`);
    r.store.sql.exec(`CREATE TABLE IF NOT EXISTS addr_day (day INTEGER NOT NULL, cls TEXT NOT NULL, k TEXT NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (day, cls, k)) WITHOUT ROWID`);
    const d = dayIndex(T0);
    r.store.raw.prepare(`INSERT INTO addr_day (day, cls, k, n) VALUES (?, 'open', 'x', 990)`).run(d);
    r.store.raw.prepare(`INSERT INTO addr_day (day, cls, k, n) VALUES (?, 'open', 'x', 1) ON CONFLICT(day, cls, k) DO UPDATE SET n = MAX(n, excluded.n)`).run(d);
    expect(r.store.raw.prepare(`SELECT n FROM addr_day WHERE k = 'x'`).get().n).toBe(990);
  });
  it('a count that reaches its cap is written at once even between steps (the cap survives an eviction)', async () => {
    const r = rig({ ADDR_DAILY_LIVE: '7' });
    const cal = { dayIndex, secondsToMidnight };
    const mkDb = () => ({ get: (q, ...b) => r.store.sql.exec(q, ...b).toArray()[0], all: (q, ...b) => r.store.sql.exec(q, ...b).toArray(), run: (q, ...b) => { r.store.sql.exec(q, ...b); } });
    r.store.sql.exec(`CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)`);
    r.store.sql.exec(`CREATE TABLE IF NOT EXISTS addr_day (day INTEGER NOT NULL, cls TEXT NOT NULL, k TEXT NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (day, cls, k)) WITHOUT ROWID`);
    const cfg = { dailyBudget: 0, liveReservePct: 0, abuse: { openPerDay: 7, playerPerDay: 7, livePerDay: 7, pidPerDay: 7, badPerDay: 7 }, reserve: {} };
    const b1 = createBudget(mkDb(), cfg, 1, cal);
    for (let i = 0; i < 7; i++) expect(b1.gateAddress('live', 'k', T0)).toBeNull();
    const b2 = createBudget(mkDb(), cfg, 1, cal); // evicted: new object
    expect(b2.gateAddress('live', 'k', T0)).not.toBeNull();
  });
  it('the admin export never carries the address-hash salt (list count, pages), and the salt exists', async () => {
    const r = rig();
    const app = r.mk();
    await r.hit(app, '198.51.100.70');
    expect(r.store.raw.prepare(`SELECT COUNT(*) AS n FROM settings WHERE key = 'addr_salt'`).get().n).toBe(1);
    const get = (q) => app.handle(new Request(`https://t.invalid/v1/admin/export${q}`, { headers: { authorization: 'Bearer test-admin-token-0123456789', 'x-msc-game': 'smg' } }));
    const list = await (await get('')).json();
    const total = r.store.raw.prepare(`SELECT COUNT(*) AS n FROM settings WHERE key <> 'addr_salt'`).get().n;
    expect(list.tables.find((t) => t.table === 'settings').rows).toBe(total);
    const page = await (await get('?table=settings&after=0&limit=500')).json();
    expect(JSON.stringify(page)).not.toContain('addr_salt');
    expect(page.rows.length).toBe(total);
  });
});

describe('budget unit: persistence of per-key counters', () => {
  const cal = { dayIndex, secondsToMidnight };
  const cfg = { dailyBudget: 0, liveReservePct: 0, abuse: { openPerDay: 100, playerPerDay: 100, livePerDay: 100, pidPerDay: 100, badPerDay: 100 }, reserve: {} };
  function mkDb() {
    const s = openNodeSql();
    s.sql.exec(`CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)`);
    s.sql.exec(`CREATE TABLE addr_day (day INTEGER NOT NULL, cls TEXT NOT NULL, k TEXT NOT NULL, n INTEGER NOT NULL, PRIMARY KEY (day, cls, k)) WITHOUT ROWID`);
    return { get: (q, ...b) => s.sql.exec(q, ...b).toArray()[0], all: (q, ...b) => s.sql.exec(q, ...b).toArray(), run: (q, ...b) => { s.sql.exec(q, ...b); }, raw: s.raw };
  }
  it('a counter flushed at a multiple of the step is read back by a new budget object', () => {
    const db = mkDb();
    const b1 = createBudget(db, cfg, 1, cal);
    for (let i = 0; i < 65; i++) b1.gateAddress('open', 'k1', T0);
    const b2 = createBudget(db, cfg, 1, cal);
    let admitted = 0;
    for (let i = 0; i < 100; i++) if (!b2.gateAddress('open', 'k1', T0)) admitted++;
    expect(admitted).toBeGreaterThanOrEqual(100 - 65);
    expect(admitted).toBeLessThanOrEqual(100 - 60); // the writes were at 1, 20, 40, 60: at most 5 uncounted
  });
});

describe('IPv6: the daily ceilings count a /48, not a /64 (finding 4)', () => {
  it('a holder of a /48 who uses a different /64 for every request is stopped at the same cap as one address', async () => {
    const r = rig();
    const codes = [];
    for (let i = 0; i < 120; i++) codes.push((await r.hit(r.mk(), `2001:db8:abcd:${(i + 1).toString(16)}::1`)).status);
    expect(codes.filter((c) => c === 200).length).toBe(50);
    expect(codes[119]).toBe(429);
  });
  it('a different /48 is a different allowance; IPv4 stays per address (not pooled into a /24)', async () => {
    const r = rig();
    for (let i = 0; i < 120; i++) await r.hit(r.mk(), `2001:db8:abcd:${(i + 1).toString(16)}::1`);
    expect((await r.hit(r.mk(), '2001:db8:abce:1::1')).status).toBe(200);
    for (let i = 0; i < 60; i++) await r.hit(r.mk(), '198.51.100.5');
    expect((await r.hit(r.mk(), '198.51.100.6')).status).toBe(200);
    expect((await r.hit(r.mk(), '198.51.100.5')).status).toBe(429);
  });
  it('the short-window limits still use the /64 (ipKey is unchanged)', async () => {
    const { ipKey, dailyAddrKey } = await import('../../src/core/util.js');
    expect(ipKey('2001:db8:abcd:1::5')).toBe('v6:2001:0db8:abcd:0001');
    expect(dailyAddrKey(ipKey('2001:db8:abcd:1::5'))).toBe('v6:2001:0db8:abcd');
    expect(dailyAddrKey(ipKey('2001:db8:abcd:ffff:1:2:3:4'))).toBe(dailyAddrKey(ipKey('2001:db8:abcd:1::5')));
    expect(dailyAddrKey('203.0.113.9')).toBe('203.0.113.9');
  });
});
