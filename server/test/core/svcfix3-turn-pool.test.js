// svcfix3 (review S5): a pool of aged accounts must not use up the day's relay credentials.
import { describe, it, expect } from 'vitest';
import { makeSmg, smgSignUp, roomOf, ep } from '../smg-helpers.js';

const mint = (s, p, code, ip) => s.call('POST', '/v1/turn', { room: code }, p.token, ip ? { ip } : {});

describe('S5: TURN credentials for Galaxy rooms', () => {
  it('defaults: 8 per room, 24 per address a day, half of the day for credentials without a visible reason', async () => {
    const s = makeSmg();
    expect(s.app.cfg.turn.maxPerRoom).toBe(8);
    expect(s.app.cfg.turn.maxPerAddrDay).toBe(24);
    expect(s.app.cfg.turn.unneededSharePct).toBe(50);
  });

  it('one room cannot mint more than TURN_MAX_CREDS_PER_ROOM credentials however many times its members ask', async () => {
    const s = makeSmg({ TURN_MAX_CREDS_PER_ROOM: '3', TURN_MAX_CREDS_PER_PLAYER_DAY: '12' });
    const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb');
    const code = await roomOf(s, [a, b]);
    expect((await mint(s, a, code)).status).toBe(200);
    expect((await mint(s, b, code)).status).toBe(200);
    expect((await mint(s, a, code)).status).toBe(200);
    const r = await mint(s, b, code);
    expect(r.status).toBe(429);
    expect(r.data.error.code).toBe('turn_room_limit');
  });

  it('one source address is capped per day (the cap resets next day); another address is not affected', async () => {
    const s = makeSmg({ TURN_MAX_CREDS_PER_ADDR_DAY: '2', TURN_MAX_CREDS_PER_ROOM: '0' });
    const mk = async (tag, ip) => {
      const a = await smgSignUp(s, `H${tag}`); const b = await smgSignUp(s, `G${tag}`);
      const made = await s.call('POST', '/v1/rooms', {}, a.token, { ip });
      await s.call('POST', '/v1/rooms/join', { code: made.data.code }, b.token, { ip });
      return { a, b, code: made.data.code };
    };
    const r1 = await mk('1', '203.0.113.7');
    expect((await mint(s, r1.a, r1.code)).status).toBe(200);
    expect((await mint(s, r1.b, r1.code)).status).toBe(200);
    const no = await mint(s, r1.a, r1.code);
    expect(no.status).toBe(429);
    const r2 = await mk('2', '203.0.113.8');
    expect((await mint(s, r2.a, r2.code)).status).toBe(200);
    s.advance(24 * 3600_000);
    const r3 = await mk('3', '203.0.113.7');
    expect((await mint(s, r3.a, r3.code)).status).toBe(200);
  });

  it('credentials without a visible reason use only half of the day; a relay-only pair can still get the other half', async () => {
    const s = makeSmg({ TURN_MAX_CREDS_PER_DAY: '4', TURN_UNNEEDED_SHARE_PCT: '50', TURN_MAX_CREDS_PER_ADDR_DAY: '0', TURN_MAX_CREDS_PER_ROOM: '0', TURN_NEW_ACCOUNT_HOURS: '0' });
    const pool = [];
    for (let i = 0; i < 3; i++) {
      const a = await smgSignUp(s, `Pa${i}`); const b = await smgSignUp(s, `Pb${i}`);
      pool.push({ a, b, code: await roomOf(s, [a, b]) });
    }
    expect((await mint(s, pool[0].a, pool[0].code)).status).toBe(200);
    expect((await mint(s, pool[1].a, pool[1].code)).status).toBe(200);
    const busy = await mint(s, pool[2].a, pool[2].code);
    expect(busy.status).toBe(503);
    expect(busy.data.error.code).toBe('turn_budget');
    // a player whose connection needs the relay (relay-only account) is served from the rest of the day
    const x = await smgSignUp(s, 'RelayX'); const y = await smgSignUp(s, 'RelayY');
    expect((await s.call('PUT', '/v1/me', { relayOnly: true }, x.token)).status).toBe(200);
    const code = await roomOf(s, [x, y]);
    expect((await mint(s, x, code)).status).toBe(200);
    expect((await mint(s, y, code)).status).toBe(200);
    expect((await mint(s, x, code)).data.error.code).toBe('turn_budget'); // the whole day (4) is now used
  });
});

// ---- svcfix3 review fixes: the room cap is per hour; the address counter is swept after 2 days
const HOUR = 3600_000;
const counterRows = (s, like) => s.store.sql.exec(`SELECT key, value FROM counters WHERE key LIKE ?`, like).toArray();
async function startedRoom4(s, players, ips) {
  const made = await s.call('POST', '/v1/rooms', { mode: 'coop' }, players[0].token, { ip: ips[0] });
  const code = made.data.code;
  for (let i = 1; i < players.length; i++) await s.call('POST', '/v1/rooms/join', { code }, players[i].token, { ip: ips[i] });
  for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/endpoint`, { ...ep(i + 1), public: `${ips[i]}:41100` }, players[i].token, { ip: ips[i] });
  for (let i = 0; i < players.length; i++) await s.call('POST', `/v1/rooms/${code}/started`, undefined, players[i].token, { ip: ips[i] });
  return code;
}
async function play(s, code, players, ips, ms) {
  for (let t = 0; t < ms; t += 15_000) {
    s.advance(Math.min(15_000, ms - t));
    for (let i = 0; i < players.length; i++) await s.call('GET', `/v1/rooms/${code}`, undefined, players[i].token, { ip: ips[i] });
  }
}

describe('S5 review fixes', () => {
  it('a 4-member room that renews its credentials every hour is never refused by the room cap (3 hours, 12 credentials)', async () => {
    const s = makeSmg();
    const ips = ['203.0.113.11', '203.0.113.12', '203.0.113.13', '203.0.113.14'];
    const ps = [];
    for (const n of ['Aaa', 'Bbb', 'Ccc', 'Ddd']) ps.push(await smgSignUp(s, n));
    const code = await startedRoom4(s, ps, ips);
    for (let hour = 0; hour < 3; hour++) {
      for (let i = 0; i < 4; i++) expect((await mint(s, ps[i], code, ips[i])).status, `hour ${hour} member ${i}`).toBe(200);
      await play(s, code, ps, ips, HOUR); // (a whole hour: the next mint falls in the next clock hour)
    }
    const keys = counterRows(s, `turn:room:${code}:%`);
    expect(keys.length).toBeGreaterThanOrEqual(3); // one counter per hour, each at most 4
    expect(keys.every((k) => k.value <= 4)).toBe(true);
  });

  it('the cap still holds inside one hour (8), and a counter of an old hour under the same room code is not inherited', async () => {
    const s = makeSmg({ TURN_MAX_CREDS_PER_PLAYER_DAY: '20' });
    const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb');
    const code = await roomOf(s, [a, b]);
    for (let i = 0; i < 4; i++) { expect((await mint(s, a, code)).status).toBe(200); expect((await mint(s, b, code)).status).toBe(200); }
    expect((await mint(s, a, code)).data.error.code).toBe('turn_room_limit');
    // the same code with a counter left over from an earlier hour (a reused code, a later day) counts for nothing now
    const s2 = makeSmg();
    const a2 = await smgSignUp(s2, 'Aaa'); const b2 = await smgSignUp(s2, 'Bbb');
    const code2 = await roomOf(s2, [a2, b2]);
    s2.store.sql.exec(`INSERT INTO counters (key, value, updated_ms) VALUES (?, 8, ?)`, `turn:room:${code2}:${Math.floor(s2.clock.now / HOUR) - 30}`, s2.clock.now);
    expect((await mint(s2, a2, code2)).status).toBe(200);
  });

  it('the per-address and per-room relay counters are swept after 2 days, not kept 40', async () => {
    const s = makeSmg();
    const a = await smgSignUp(s, 'Aaa'); const b = await smgSignUp(s, 'Bbb');
    const made = await s.call('POST', '/v1/rooms', {}, a.token, { ip: '203.0.113.9' });
    await s.call('POST', '/v1/rooms/join', { code: made.data.code }, b.token, { ip: '203.0.113.10' });
    expect((await mint(s, a, made.data.code)).status).toBe(200);
    expect(counterRows(s, 'turn:creds:%:a203.0.113.9').length).toBe(1);
    s.advance(1 * 86400_000); s.app.sweep();
    expect(counterRows(s, 'turn:creds:%:a%').length).toBe(1);
    s.advance(1.5 * 86400_000); s.app.sweep();
    expect(counterRows(s, 'turn:creds:%:a%')).toEqual([]);
    expect(counterRows(s, 'turn:room:%')).toEqual([]);
    expect(counterRows(s, 'turn:creds:%').length).toBeGreaterThan(0); // the day totals stay (the global budget counters)
  });
});

