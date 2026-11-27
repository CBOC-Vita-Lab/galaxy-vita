// svcfix3 (review S4): a full account table must never lock out a new player while it is full of idle or never-used accounts.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp } from '../smg-helpers.js';

const DAY = 86400_000;
const MIN = 60_000;
const FARM = { REGISTER_IP_BURST: '1000', REGISTER_IP_PER_HOUR: '1000', REGISTER_NET_PER_DAY: '0', REGISTER_MAX_PER_DAY: '100000', ADDR_DAILY_OPEN: '0', PRUNE_UNUSED_DAYS: '0' };
const status = async (s, a) => (await s.call('GET', '/v1/me', undefined, a.token)).status;

describe('S4: sign-up when the table is full', () => {
  it('a table full of fresh silent accounts (all under the 24 h line) still takes a new player once they are 30 minutes old; never before', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '12' });
    const used = await smgSignUp(s, 'Used');
    const farm = [];
    for (let i = 0; i < 11; i++) farm.push(await smgSignUp(s, `Farm${i}`));
    s.advance(10 * MIN);
    await s.call('GET', '/v1/me', undefined, used.token); // a real call after 5 minutes: not silent
    expect((await s.call('POST', '/v1/register', { name: 'TooSoon' })).data.error.code).toBe('server_full');
    s.advance(25 * MIN);
    expect((await s.call('POST', '/v1/register', { name: 'Newcomer' })).status).toBe(200);
    expect(await status(s, used)).toBe(200);
    expect(await status(s, farm[0])).toBe(401); // the oldest silent one made room
    expect(await status(s, farm[10])).toBe(200);
  });

  it('a silent account with a friend or a star time is never taken, whatever its age', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '3' });
    const a = await smgSignUp(s, 'Aaa');
    const b = await smgSignUp(s, 'Bbb');
    await smgSignUp(s, 'Ccc');
    await s.call('POST', '/v1/friends', { code: b.code }, a.token); // a has a friend row, b is the target
    s.advance(2 * 3600_000);
    expect((await s.call('POST', '/v1/register', { name: 'New1' })).status).toBe(200); // Ccc made room
    expect(await status(s, a)).toBe(200);
    expect(await status(s, b)).toBe(200);
    expect((await s.call('POST', '/v1/register', { name: 'New2' })).data.error.code).toBe('server_full'); // New1 is fresh, a and b hold a friend row
  });

  it('a full table of used accounts with no friend and no time, unseen 14 days, still takes a new player; accounts with a friend wait for the 60-day rule', async () => {
    const s = makeSmg({ ...FARM, MAX_PLAYERS: '4' });
    const users = [];
    for (let i = 0; i < 4; i++) users.push(await smgSignUp(s, `User${i}`));
    s.advance(10 * MIN);
    for (const u of users) await s.call('GET', '/v1/me', undefined, u.token); // all used
    await s.call('POST', '/v1/friends', { code: users[1].code }, users[0].token);
    await s.call('POST', '/v1/friends', { code: users[0].code }, users[1].token);
    s.advance(13 * DAY);
    expect((await s.call('POST', '/v1/register', { name: 'TooEarly' })).data.error.code).toBe('server_full');
    s.advance(2 * DAY);
    expect((await s.call('POST', '/v1/register', { name: 'Fresh1' })).status).toBe(200); // users[2] (friendless, unseen 15 d) goes
    expect((await s.call('POST', '/v1/register', { name: 'Fresh2' })).status).toBe(200); // users[3] goes
    expect(await status(s, users[0])).toBe(200);
    expect(await status(s, users[1])).toBe(200);
    expect((await s.call('POST', '/v1/register', { name: 'Fresh3' })).data.error.code).toBe('server_full'); // only friends and fresh accounts are left
    s.advance(50 * DAY); // 65 days unseen: the existing 60-day rule
    expect((await s.call('POST', '/v1/register', { name: 'Fresh4' })).status).toBe(200);
  });
});
