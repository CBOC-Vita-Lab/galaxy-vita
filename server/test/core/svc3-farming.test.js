// svc3 (review S6): sign-up farming. Many silent accounts (registered, never used) must not fill the account table for a year.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';

const DAY = 86400_000;
const HOUR = 3600_000;
// per-address caps are not what these tests are about
const FARM = { REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000', REGISTER_NET_PER_DAY: '0', REGISTER_MAX_PER_DAY: '100000', ADDR_DAILY_OPEN: '0' };

describe('silent accounts', () => {
  it('a full service makes room by removing a silent account (registered, never used, a day old) instead of "server_full" for a year', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '40' });
    for (let i = 0; i < 40; i++) await smgSignUp(s, `Farm${i}`);
    expect((await s.call('POST', '/v1/register', { name: 'TooSoon' })).data.error.code).toBe('server_full'); // all fresh: nobody is evicted
    s.advance(25 * HOUR);
    const r = await s.call('POST', '/v1/register', { name: 'Newcomer' });
    expect(r.status).toBe(200);
  });

  it('a console that used its account (a call more than 5 minutes after sign-up), has a friend or holds a time is never counted as silent', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '6' });
    const used = await smgSignUp(s, 'Used');
    const pal = await smgSignUp(s, 'Pal');
    const lonely = await smgSignUp(s, 'Lonely');
    for (let i = 0; i < 3; i++) await smgSignUp(s, `Farm${i}`);
    s.advance(6 * 60_000);
    await s.call('GET', '/v1/me', undefined, used.token);        // a real call after the first 5 minutes: last_seen moves
    await s.call('POST', '/v1/friends', { code: lonely.code }, pal.token); // pal and lonely have a friend row
    s.advance(2 * DAY);
    s.app.sweep();
    s.advance(HOUR + 1); s.app.sweep();                           // (the hourly scan)
    const alive = async (a) => (await s.call('GET', '/v1/me', undefined, a.token)).status;
    expect(await alive(used)).toBe(200);
    expect(await alive(pal)).toBe(200);
    expect(await alive(lonely)).toBe(200);
  });

  it('the hourly sweep removes silent accounts older than PRUNE_UNUSED_DAYS (3), a few hundred at a time', async () => {
    const s = makeSmg({ ...FARM });
    const farm = [];
    for (let i = 0; i < 30; i++) farm.push(await smgSignUp(s, `Farm${i}`));
    const keeper = await smgSignUp(s, 'Keeper');
    s.advance(2 * DAY); await s.call('GET', '/v1/me', undefined, keeper.token); // keeper is seen on day 2
    s.advance(2 * DAY); s.app.sweep(); // day 4: the farm is 4 days old and was never seen again
    expect((await s.call('GET', '/v1/me', undefined, farm[0].token)).status).toBe(401);
    expect((await s.call('GET', '/v1/me', undefined, farm[29].token)).status).toBe(401);
    expect((await s.call('GET', '/v1/me', undefined, keeper.token)).status).toBe(200);
  });
});

describe('the sign-up gets harder as the table fills', () => {
  it('from half full the daily sign-up cap halves, from 80 % it is a quarter', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '100', REGISTER_MAX_PER_DAY: '40', PRUNE_UNUSED_DAYS: '0', REGISTER_EVICT_UNUSED_HOURS: '0' });
    const perDay = [];
    for (let day = 0; day < 6; day++) {
      let today = 0;
      for (let i = 0; i < 60; i++) { const r = await s.call('POST', '/v1/register', { name: `D${day}x${i}` }); if (r.status === 200) today++; else break; }
      perDay.push(today);
      s.advance(DAY);
    }
    // 40 while under half (day 1); day 2 starts at 40 and the cap drops to 20 at 50 accounts; ... a quarter (10) from 80; the table is full on day 5
    expect(perDay).toEqual([40, 20, 20, 10, 10, 0]);
  });

  it('the proof of work is one bit harder from half full and two from 80 %, never above 22 (the client\'s limit)', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '10', REGISTER_POW_BITS: '6', REGISTER_POW_MAX_BITS: '6', REGISTER_POW_FREE: '1000', PRUNE_UNUSED_DAYS: '0' });
    const bits = async () => (await s.call('GET', '/v1/register/challenge', undefined, undefined, { ip: '203.0.113.200' })).data.bits;
    expect(await bits()).toBe(6);
    let seq = 0;
    const direct = (n) => { // fill the table without going through the sign-up (which wants the work)
      for (let i = 0; i < n; i++) {
        const k = String(++seq).padStart(6, '0');
        s.store.sql.exec(`INSERT INTO profiles (code, name, name_key, token_hash, created_ms, last_seen_ms, name_changed_ms, region, relay_only) VALUES (?, 'X', ?, ?, ?, ?, ?, 2, 0)`,
          `Z${k}`, `x#${k}`, `h${k}`, s.clock.now, s.clock.now, s.clock.now).toArray();
      }
    };
    direct(5);
    s.advance(11 * 60_000); // the table size is looked at again after 10 minutes
    expect(await bits()).toBe(7);
    direct(3);
    s.advance(11 * 60_000);
    expect(await bits()).toBe(8);
  });
});

describe('rows read: the account count is not a full scan per sign-up', () => {
  it('60 sign-ups run COUNT(*) over the profiles at most a handful of times', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '50000' });
    let counts = 0;
    const exec = s.store.sql.exec.bind(s.store.sql);
    s.store.sql.exec = (q, ...b) => { if (/COUNT\(\*\) AS n FROM profiles/.test(q)) counts++; return exec(q, ...b); };
    for (let i = 0; i < 60; i++) await smgSignUp(s, `Cnt${i}`);
    expect(counts).toBeLessThanOrEqual(3);
  });
});
