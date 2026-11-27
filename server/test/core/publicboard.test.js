// The public web leaderboard: GET /v1/public/leaderboard and the /leaderboard page.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, makeBlob } from '../helpers.js';
import { edgeCheck } from '../../src/core/edge.js';
import { PAGE_JS, PAGE_BODY, PAGE_CSS, pageHtml } from '../../src/core/leaderboard_page.js';

const URL_ = '/v1/public/leaderboard';
const get = (s, q = '', headers = {}) => s.call('GET', `${URL_}${q}`, undefined, undefined, headers);

/** Insert score rows directly (the credit rule would clamp puts). rows: [{name, score, wins, losses, category, updated, endian}] */
async function seed(s, rows, { category = 0 } = {}) {
  const out = [];
  for (const r of rows) {
    const p = await signUp(s, r.name);
    s.store.sql.exec(
      'INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (?,?,?,?,?,?)',
      r.category ?? category, p.pid, r.region ?? 2, r.score, makeBlob(p.name, { wins: r.wins ?? 0, losses: r.losses ?? 0, endian: r.endian }),
      r.updated ?? s.clock.now - 1000,
    );
    out.push(p);
  }
  return out;
}

const keysDeep = (v, acc = new Set()) => {
  if (Array.isArray(v)) v.forEach((x) => keysDeep(x, acc));
  else if (v && typeof v === 'object') for (const [k, x] of Object.entries(v)) { acc.add(k); keysDeep(x, acc); }
  return acc;
};

describe('GET /v1/public/leaderboard: shape and privacy', () => {
  it('has exactly the documented keys, for both boards', async () => {
    const s = makeServer();
    await seed(s, [{ name: 'Sam', score: 50, wins: 5, losses: 2 }, { name: 'Alex', score: 30, wins: 3, losses: 3 }]);
    await seed(s, [{ name: 'Robin', score: 12, wins: 1, losses: 0 }], { category: 1 });
    const season = await get(s);
    expect(season.status).toBe(200);
    expect(Object.keys(season.data)).toEqual(['board', 'season', 'updatedMs', 'rows']);
    expect(season.data.board).toBe('season');
    expect(Object.keys(season.data.season)).toEqual(['index', 'startMs', 'endMs']);
    expect(season.data.season.index).toBe(43); // the weekly season that began on 29 Oct (calendar.js)
    expect(season.data.season.startMs).toBe(Date.UTC(2026, 9, 29));
    expect(season.data.season.endMs).toBe(Date.UTC(2026, 10, 5));
    expect(season.data.updatedMs).toBe(s.clock.now);
    expect(season.data.rows).toEqual([
      { rank: 1, name: 'Sam', wins: 5, losses: 2, points: 50, lastPlayedMs: null },
      { rank: 2, name: 'Alex', wins: 3, losses: 3, points: 30, lastPlayedMs: null },
    ]);
    for (const row of season.data.rows) expect(Object.keys(row)).toEqual(['rank', 'name', 'wins', 'losses', 'points', 'lastPlayedMs']);
    const daily = await get(s, '?board=daily');
    expect(Object.keys(daily.data)).toEqual(['board', 'season', 'day', 'updatedMs', 'rows']);
    expect(daily.data.day).toEqual({ startMs: Date.UTC(2026, 9, 30), endMs: Date.UTC(2026, 9, 31) });
    expect(daily.data.rows).toEqual([{ rank: 1, name: 'Robin', wins: 1, losses: 0, points: 12, lastPlayedMs: null }]);
    expect(season.headers.get('content-type')).toContain('application/json');
    expect(season.headers.get('cache-control')).toBe('public, max-age=60');
  });

  it('never leaks a player code, pid, token, region or address; the only time of a player is lastPlayedMs, to the hour (1.3)', async () => {
    const s = makeServer();
    const ps = await seed(s, [{ name: 'Sam', score: 9, region: 4 }, { name: 'Alex', score: 8, region: 8 }]);
    const r = await get(s, '?limit=100', { ip: '203.0.113.77' });
    const text = JSON.stringify(r.data);
    expect([...keysDeep(r.data)].sort()).toEqual(
      ['board', 'endMs', 'index', 'lastPlayedMs', 'losses', 'name', 'points', 'rank', 'rows', 'season', 'startMs', 'updatedMs', 'wins'],
    );
    for (const p of ps) {
      expect(text).not.toContain(p.token);
      expect(text).not.toContain(String(p.pid));
      expect(text).not.toContain(String(p.code).replace(/-/g, ''));
      expect(text).not.toContain(p.code);
    }
    expect(text).not.toContain('203.0.113.77');
    expect(text).not.toMatch(/token|pid|code|region|lastupdate|updated_ms|data_b64|ip"/i);
    for (const h of ['set-cookie', 'x-msc-ip']) expect(r.headers.get(h)).toBeNull();
  });

  it('leaves out banned players (even if a score row survived) and players deleted from the profile table', async () => {
    const s = makeServer();
    const ps = await seed(s, [{ name: 'Sam', score: 90 }, { name: 'Alex', score: 80 }, { name: 'Robin', score: 70 }]);
    s.store.sql.exec('UPDATE profiles SET banned = 1 WHERE pid = ?', ps[0].pid); // e.g. restored from a backup with the score row intact
    s.store.sql.exec('DELETE FROM profiles WHERE pid = ?', ps[1].pid); // orphan score row
    const r = await get(s);
    expect(r.data.rows).toEqual([{ rank: 1, name: 'Robin', wins: 0, losses: 0, points: 70, lastPlayedMs: null }]);
  });

  it('a ban or an account deletion removes the player at once, even inside the 60 s memo', async () => {
    const s = makeServer();
    const ps = await seed(s, [{ name: 'Sam', score: 90 }, { name: 'Alex', score: 80 }, { name: 'Robin', score: 70 }]);
    expect((await get(s)).data.rows.length).toBe(3);
    await s.call('POST', '/v1/admin/ban', { pid: ps[0].pid, banned: true }, s.env.ADMIN_TOKEN);
    expect((await get(s)).data.rows.map((x) => x.name)).toEqual(['Alex', 'Robin']);
    expect((await s.call('DELETE', '/v1/me', undefined, ps[1].token)).status).toBe(200);
    expect((await get(s)).data.rows.map((x) => x.name)).toEqual(['Robin']);
  });

  it('only counts rows inside the current window: this season / this UTC day', async () => {
    const s = makeServer();
    const dayMs = 86400000;
    const seasonStart = Date.UTC(2026, 9, 29);
    await seed(s, [
      { name: 'Fresh', score: 10, updated: s.clock.now - 1000 },
      { name: 'LastSeas', score: 99, updated: seasonStart - 1 },
    ]);
    await seed(s, [
      { name: 'Today', score: 5, updated: Date.UTC(2026, 9, 30) },
      { name: 'Yesterday', score: 50, updated: Date.UTC(2026, 9, 30) - dayMs + 5000 },
    ], { category: 1 });
    expect((await get(s)).data.rows.map((r) => r.name)).toEqual(['Fresh']);
    expect((await get(s, '?board=daily')).data.rows.map((r) => r.name)).toEqual(['Today']);
  });

  it('reads the record from the stored blob in either byte order; an unreadable blob shows 0 - 0', async () => {
    const s = makeServer({ RANK_REQUIRE_HMAC: '0', RANK_CHECK_NAME: '0' });
    await seed(s, [
      { name: 'LittleEnd', score: 30, wins: 12, losses: 4, endian: 'le' },
      { name: 'BigEnd', score: 20, wins: 7, losses: 9, endian: 'be' },
    ]);
    const odd = await signUp(s, 'Odd');
    s.store.sql.exec('INSERT INTO scores VALUES (0, ?, 2, 10, ?, ?)', odd.pid, Buffer.alloc(40, 9).toString('base64'), s.clock.now - 5);
    const r = await get(s);
    expect(r.data.rows.map((x) => [x.name, x.wins, x.losses])).toEqual([['LittleEnd', 12, 4], ['BigEnd', 7, 9], ['Odd', 0, 0]]);
  });

  it('an empty board is an empty list, not an error', async () => {
    const s = makeServer();
    const r = await get(s);
    expect(r.status).toBe(200);
    expect(r.data.rows).toEqual([]);
  });
});

describe('ordering and ties are the ones rank/get gives the game', () => {
  it('same order, same competition ranks (1,2,2,4), same cut-off', async () => {
    const s = makeServer();
    const scores = [100, 90, 90, 80, 70, 70, 70, 60, 50, 50, 40, 30, 30, 20, 10];
    const ps = await seed(s, scores.map((score, i) => ({ name: `Plr${String(i).padStart(2, '0')}`, score, updated: s.clock.now - 5000 + (i % 4) * 100 })));
    const byPid = new Map(ps.map((p) => [p.pid, p.name]));
    const web = (await get(s, '?limit=100')).data.rows;
    expect(web.map((r) => r.rank)).toEqual([1, 2, 2, 4, 5, 5, 5, 8, 9, 9, 11, 12, 12, 14, 15]);
    // the game's TOPLIST (10 rows)
    const top = await s.call('POST', '/v1/rank/get', { mode: 1, category: 0, region: 2, sort: 1, since: 0, limit: 10 }, ps[0].token);
    expect(top.data.rows.map((r) => [r.order, byPid.get(r.pid), r.score])).toEqual(web.slice(0, 10).map((r) => [r.rank, r.name, r.points]));
    // NEAR windows around players deeper in the table
    for (const who of [ps[6], ps[11], ps[14]]) {
      const near = await s.call('POST', '/v1/rank/get', { mode: 2, category: 0, region: 2, sort: 1, since: 0, limit: 10 }, who.token);
      const mine = near.data.rows.map((r) => [r.order, byPid.get(r.pid), r.score]);
      const start = web.findIndex((r) => r.name === mine[0][1]);
      expect(mine).toEqual(web.slice(start, start + mine.length).map((r) => [r.rank, r.name, r.points]));
    }
    // the cut-off keeps the top of the table
    const cut = (await get(s, '?limit=5')).data.rows;
    expect(cut).toEqual(web.slice(0, 5));
  });

  it('ties go to the earlier update, then the lower pid, exactly like rank/get', async () => {
    const s = makeServer();
    const t = s.clock.now;
    const ps = await seed(s, [
      { name: 'Late', score: 40, updated: t - 100 },
      { name: 'Early', score: 40, updated: t - 900 },
      { name: 'SameA', score: 40, updated: t - 500 },
      { name: 'SameB', score: 40, updated: t - 500 },
    ]);
    const web = (await get(s)).data.rows;
    expect(web.map((r) => r.name)).toEqual(['Early', 'SameA', 'SameB', 'Late']);
    expect(new Set(web.map((r) => r.rank))).toEqual(new Set([1]));
    const top = await s.call('POST', '/v1/rank/get', { mode: 1, category: 0, region: 2, sort: 1, since: 0, limit: 4 }, ps[0].token);
    const names = new Map(ps.map((p) => [p.pid, p.name]));
    expect(top.data.rows.map((r) => names.get(r.pid))).toEqual(web.map((r) => r.name));
    expect(top.data.rows.map((r) => r.order)).toEqual([1, 1, 1, 1]);
  });
});

describe('parameters, memo, kill switch, rate limit', () => {
  it('validates board and limit', async () => {
    const s = makeServer();
    await seed(s, [{ name: 'Sam', score: 1 }, { name: 'Alex', score: 2 }, { name: 'Robin', score: 3 }]);
    for (const q of ['?board=weekly', '?board=', '?board=SEASON', '?limit=0', '?limit=101', '?limit=abc', '?limit=-1', '?limit=1.5', '?limit=']) {
      const r = await get(s, q);
      expect(r.status, q).toBe(400);
      expect(r.data.error.code).toBe('bad_param');
      expect(r.headers.get('cache-control')).toBe('no-store');
    }
    expect((await get(s, '?limit=2')).data.rows.length).toBe(2);
    expect((await get(s, '?limit=100&board=season&junk=1')).status).toBe(200);
    expect((await get(s)).data.rows.length).toBe(3); // default limit 50
  });

  it('memoises for 60 s: a new score appears after the memo expires, and the snapshot time is the memo time', async () => {
    const s = makeServer();
    const [a] = await seed(s, [{ name: 'Sam', score: 10 }]);
    const t0 = s.clock.now;
    expect((await get(s)).data.rows.length).toBe(1);
    await seed(s, [{ name: 'Alex', score: 20 }]);
    s.advance(30_000);
    const mid = await get(s);
    expect(mid.data.rows.length).toBe(1);
    expect(mid.data.updatedMs).toBe(t0);
    s.advance(31_000);
    const late = await get(s);
    expect(late.data.rows.map((r) => r.name)).toEqual(['Alex', 'Sam']);
    expect(late.data.updatedMs).toBe(s.clock.now);
    // the two boards and two limits are separate memo entries
    expect((await get(s, '?board=daily')).data.rows).toEqual([]);
    expect(a.pid).toBeGreaterThan(0);
  });

  it('does no database work on a memo hit', async () => {
    const s = makeServer();
    await seed(s, [{ name: 'Sam', score: 10 }]);
    await get(s);
    let reads = 0;
    const orig = s.store.sql.exec.bind(s.store.sql);
    s.store.sql.exec = (q, ...b) => { if (/FROM scores/.test(q)) reads++; return orig(q, ...b); };
    for (let i = 0; i < 20; i++) await get(s);
    expect(reads).toBe(0);
  });

  it('needs no token and no version header, even where X-Msc-App is mandatory', async () => {
    const s = makeServer({ REQUIRE_APP_VERSION: '1' });
    expect((await get(s)).status).toBe(200);
    expect((await s.call('GET', '/leaderboard')).status).toBe(200);
    const req = (path) => new Request(`https://x.invalid${path}`);
    expect(edgeCheck(req(URL_), { REQUIRE_APP_VERSION: '1' })).toBeNull();
    expect(edgeCheck(req('/leaderboard'), { REQUIRE_APP_VERSION: '1' })).toBeNull();
    expect(edgeCheck(new Request(`https://x.invalid${URL_}`, { method: 'POST' }), {})).toMatchObject({ status: 405 });
    expect(edgeCheck(new Request(`https://x.invalid${URL_}`, { headers: { 'x-msc-app': 'zz' } }), {})).toMatchObject({ status: 400 });
  });

  it('is switched off by the ranking and all kill switches (env and admin), other switches leave it alone', async () => {
    for (const kill of ['ranking', 'all', 'ranked,ranking']) {
      const s = makeServer({ KILL_SWITCH: kill, MAINTENANCE_MESSAGE: 'Back soon' });
      const r = await get(s);
      expect(r.status, kill).toBe(503);
      expect(r.data.error).toMatchObject({ code: 'service_disabled', message: 'Back soon' });
      expect(r.headers.get('cache-control')).toBe('no-store');
      expect((await s.call('GET', '/leaderboard')).status).toBe(200); // the page itself stays; it explains
    }
    expect((await get(makeServer({ KILL_SWITCH: 'ranked,turn,register,rooms' }))).status).toBe(200);
    const s = makeServer();
    await seed(s, [{ name: 'Sam', score: 10 }]);
    expect((await get(s)).status).toBe(200);
    await s.call('POST', '/v1/admin/switches', { kill: ['ranking'] }, s.env.ADMIN_TOKEN);
    expect((await get(s)).status).toBe(503); // even with a fresh memo
    await s.call('POST', '/v1/admin/switches', { kill: [] }, s.env.ADMIN_TOKEN);
    expect((await get(s)).status).toBe(200);
    expect((await s.call('GET', '/v1/info')).data.features.ranking).toBe(true);
  });

  it('is limited per address: 30 a minute, other addresses unaffected, refills with time', async () => {
    const s = makeServer({ RATE_LIMIT_SCALE: '1' });
    const ip = '203.0.113.9';
    for (let i = 0; i < 30; i++) expect((await get(s, '', { ip })).status, `request ${i + 1}`).toBe(200);
    const r = await get(s, '', { ip });
    expect(r.status).toBe(429);
    expect(r.data.error.code).toBe('rate_limited');
    expect(Number(r.headers.get('retry-after'))).toBeGreaterThan(0);
    expect((await get(s, '', { ip: '203.0.113.10' })).status).toBe(200);
    expect((await get(s, '', { ip: '2001:db8:1:2:aaaa::1' })).status).toBe(200);
    expect((await get(s, '', { ip: '2001:db8:1:2:bbbb::2' })).status).toBe(200); // same /64: shares a bucket, still within its cap
    s.advance(3000);
    expect((await get(s, '', { ip })).status).toBe(200);
    // the page has its own, wider bucket
    for (let i = 0; i < 60; i++) expect((await s.call('GET', '/leaderboard', undefined, undefined, { ip: '203.0.113.20' })).status).toBe(200);
    expect((await s.call('GET', '/leaderboard', undefined, undefined, { ip: '203.0.113.20' })).status).toBe(429);
  });
});

describe('GET /leaderboard (the page)', () => {
  it('is 200 text/html with a strict CSP, no-sniff, no framing and a short public cache', async () => {
    const s = makeServer();
    const r = await s.call('GET', '/leaderboard');
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toBe('text/html; charset=utf-8');
    expect(r.headers.get('cache-control')).toBe('public, max-age=300');
    expect(r.headers.get('x-content-type-options')).toBe('nosniff');
    expect(r.headers.get('x-frame-options')).toBe('DENY');
    expect(r.headers.get('referrer-policy')).toBe('no-referrer');
    const csp = r.headers.get('content-security-policy');
    const dirs = Object.fromEntries(csp.split(';').map((d) => d.trim().split(/\s+(.*)/s)).map(([k, v]) => [k, v ?? '']));
    expect(dirs['default-src']).toBe("'none'");
    expect(dirs['connect-src']).toBe("'self'");
    expect(dirs['base-uri']).toBe("'none'");
    expect(dirs['frame-ancestors']).toBe("'none'");
    expect(csp).not.toMatch(/unsafe-inline|unsafe-eval|\*|https?:/);
    expect(r.data).toBe(pageHtml());
  });

  it('the CSP hashes are those of the inline script and style, nothing else is inline', async () => {
    const s = makeServer();
    const r = await s.call('GET', '/leaderboard');
    const csp = r.headers.get('content-security-policy');
    const sha = async (t) => `'sha256-${Buffer.from(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(t))).toString('base64')}'`;
    const html = r.data;
    const scripts = [...html.matchAll(/<script([^>]*)>([\s\S]*?)<\/script>/g)];
    const styles = [...html.matchAll(/<style([^>]*)>([\s\S]*?)<\/style>/g)];
    expect(scripts.length).toBe(1);
    expect(styles.length).toBe(1);
    expect(scripts[0][1]).toBe('');
    expect(csp).toContain(`script-src ${await sha(scripts[0][2])}`);
    expect(csp).toContain(`style-src ${await sha(styles[0][2])}`);
    expect(html).not.toMatch(/\son[a-z]+\s*=/i); // no inline event handlers
    expect(html).not.toMatch(/\sstyle\s*=/i); // no style attributes (blocked by the CSP)
  });

  it('is self-contained: no external script, font, image, stylesheet or link, and no game art', () => {
    const html = pageHtml();
    expect(html).not.toMatch(/<script[^>]+src=/i);
    expect(html).not.toMatch(/<link[^>]+rel=["']?stylesheet/i);
    expect(html).not.toMatch(/<img|<iframe|<object|<embed|@import|@font-face|url\(\s*["']?(?!data:)/i);
    expect(html).not.toMatch(/https?:\/\//i);
    expect(html).toContain('<meta name="viewport" content="width=device-width,initial-scale=1">');
    expect(html).toContain('Mario Strikers Charged on PS Vita &mdash; fan project');
    for (const t of ['SEASON LEADERS', 'DAILY LEADERS', 'NO MATCHES PLAYED YET THIS SEASON', 'SEASON TIME REMAINING:']) expect(PAGE_JS + PAGE_BODY).toContain(t);
    for (const t of ['Rank', 'Name', 'Record', 'Pts']) expect(PAGE_BODY).toContain(`<span>${t}</span>`);
    expect(PAGE_CSS).toContain('clip-path');
  });

  it('only GET works, and the page is the same for everybody (nothing personal in it)', async () => {
    const s = makeServer();
    expect((await s.call('POST', '/leaderboard', {})).status).toBe(405);
    expect((await s.call('GET', '/leaderboard/extra')).status).toBe(404);
    const a = await s.call('GET', '/leaderboard', undefined, undefined, { ip: '198.51.100.7' });
    const b = await s.call('GET', '/leaderboard', undefined, undefined, { ip: '198.51.100.8', 'user-agent': 'x' });
    expect(a.data).toBe(b.data);
    expect(a.data).not.toMatch(/198\.51\.100/);
  });
});

describe('names are escaped: the page script only ever sets text', () => {
  // A tiny DOM that records what the script builds. Setting innerHTML / outerHTML / insertAdjacentHTML is an error here.
  function makeDom() {
    const byId = {};
    class El {
      constructor(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.listeners = {}; this._text = ''; this.className = ''; }
      set textContent(v) { this._text = String(v); this.children = []; }
      get textContent() { return this._text + this.children.map((c) => c.textContent).join(''); }
      set innerHTML(_) { throw new Error('innerHTML used'); }
      set outerHTML(_) { throw new Error('outerHTML used'); }
      insertAdjacentHTML() { throw new Error('insertAdjacentHTML used'); }
      appendChild(c) { this.children.push(c); return c; }
      replaceChildren(...c) { this.children = c; this._text = ''; }
      setAttribute(k, v) { this.attrs[k] = String(v); }
      addEventListener(t, f) { (this.listeners[t] ||= []).push(f); }
    }
    const doc = {
      title: '',
      hidden: false,
      listeners: {},
      createElement: (t) => new El(t),
      getElementById: (id) => (byId[id] ||= Object.assign(new El('div'), { id })),
      querySelectorAll: () => [new El('button'), new El('button')],
      addEventListener(t, f) { (this.listeners[t] ||= []).push(f); },
      write() { throw new Error('document.write used'); },
    };
    return { doc, byId };
  }
  const run = async (rows) => {
    const { doc, byId } = makeDom();
    const win = { location: { hash: '' }, history: { replaceState() {} }, addEventListener() {} };
    const fetchFn = async () => ({ ok: true, status: 200, json: async () => ({ board: 'season', season: { endMs: Date.now() + (2 * 3600 + 59 * 60 + 30) * 1000 }, rows }) });
    // eslint-disable-next-line no-new-func
    new Function('document', 'window', 'location', 'history', 'fetch', 'setInterval', PAGE_JS)(doc, win, win.location, win.history, fetchFn, () => 0);
    await new Promise((r) => setTimeout(r, 20));
    return { doc, byId };
  };

  it('hostile names land as text nodes of a span, never as markup', async () => {
    const hostile = ['<img src=x onerror=alert(1)>', '"><script>alert(1)</script>', "'; drop table--", '&lt;b&gt;', '<svg/onload=alert(1)>'];
    const { byId } = await run(hostile.map((name, i) => ({ rank: i + 1, name, wins: 1, losses: 2, points: 10 - i, lastPlayedMs: i ? Date.UTC(2026, 9, 29, 13) : '<b>x</b>' })));
    const rows = byId.rows.children;
    expect(rows.length).toBe(hostile.length);
    rows.forEach((li, i) => {
      expect(li.tag).toBe('li');
      expect(li.children.map((c) => c.tag)).toEqual(['span', 'span', 'span', 'span', 'span']);
      // last played (1.3): a date in the reader's format, or '-' for anything that is not a time (never markup)
      expect(li.children[3].textContent).toBe(i ? new Date(Date.UTC(2026, 9, 29, 13)).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }) : '-');
      expect(li.children[3].children).toEqual([]);
      expect(li.children[1].textContent).toBe(hostile[i]); // literal text
      expect(li.children[1].children).toEqual([]);
      expect(li.children[2].textContent).toBe('W: 1 - L: 2');
    });
  });

  it('shows the empty-state text when there are no rows, and the time remaining like the game hub', async () => {
    const { byId } = await run([]);
    expect(byId.rows.children[0].textContent).toBe('NO MATCHES PLAYED YET THIS SEASON');
    expect(byId.time.textContent).toMatch(/^SEASON TIME REMAINING: 0 D, 2 H, 59 M$/);
    expect(byId.title.textContent).toBe('SEASON LEADERS');
  });

  it('the source never uses an HTML-parsing API', () => {
    expect(PAGE_JS).not.toMatch(/innerHTML|outerHTML|insertAdjacentHTML|document\.write|eval\(|new Function|setTimeout\(\s*['"]/);
  });
});
