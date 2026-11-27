// Finding 7: TURN budget poisoning. Usage reports are untrusted; they only count against a live credential, are clamped to
// what that credential could carry, and are limited per player per day. Plus the few-accounts exhaustion scenario.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, pairInRoom } from '../helpers.js';
import { loadConfig } from '../../src/core/config.js';
import { reportCapBytes } from '../../src/core/turn.js';

const mb = (s) => {
  const r = s.store.sql.exec(`SELECT value FROM counters WHERE key LIKE 'turn:mb:%'`).toArray()[0];
  return r ? r.value : 0;
};
const usage = (s, p, bytes) => s.call('POST', '/v1/turn/usage', { bytes }, p.token);
const mint = (s, p, code) => s.call('POST', '/v1/turn', { room: code }, p.token);
async function room(s, tag) {
  const a = await signUp(s, `Host${tag}`);
  const b = await signUp(s, `Gst${tag}`);
  const { code } = await pairInRoom(s, a, b);
  return { a, b, code };
}

describe('usage reports (finding 7)', () => {
  it('defaults: 128 kbit/s over a 1 h credential is the clamp (57.6 MB), 12 reports a day, 6 credentials a day', () => {
    const t = loadConfig({}).turn;
    expect(reportCapBytes(t)).toBe(57_600_000);
    expect(t.maxReportsPerPlayerDay).toBe(12);
    expect(t.maxPerPlayerDay).toBe(6);
    expect(t.newAccountDaily).toBe(60);
  });

  it('is refused without credentials minted by the reporter (403 turn_not_allowed) and changes no counter', async () => {
    const s = makeServer();
    const { a, b, code } = await room(s, '1');
    const r = await usage(s, a, 5 * 1048576);
    expect(r.status).toBe(403);
    expect(r.data.error.code).toBe('turn_not_allowed');
    await mint(s, a, code);
    expect((await usage(s, b, 5 * 1048576)).status).toBe(403); // b never minted: a's credential is not b's
    expect(mb(s)).toBe(0);
    expect((await usage(s, a, 5 * 1048576)).status).toBe(200);
    expect(mb(s)).toBe(5);
  });

  it('is only accepted within the credential TTL (+ the short grace for the report after the match)', async () => {
    const s = makeServer({ TURN_TTL_SEC: '3600', TURN_REPORT_GRACE_SEC: '300' });
    const { a, code } = await room(s, '1');
    await mint(s, a, code);
    s.advance(3600_000 + 299_000);
    expect((await usage(s, a, 1048576)).status).toBe(200);
    s.advance(2_000);
    const late = await usage(s, a, 1048576);
    expect(late.status).toBe(403);
    expect(late.data.error.code).toBe('turn_not_allowed');
  });

  it('is clamped to what one credential can carry at 128 kbit/s, and a credential cannot be reported on twice', async () => {
    const s = makeServer();
    const { a, code } = await room(s, '1');
    await mint(s, a, code);
    const huge = await usage(s, a, 2 ** 39); // 512 GB claimed
    expect(huge.data).toMatchObject({ ok: true, countedBytes: 57_600_000, clamped: true });
    expect(mb(s)).toBe(55); // ceil(57.6e6 / 1048576)
    const again = await usage(s, a, 2 ** 39);
    expect(again.data).toMatchObject({ countedBytes: 0, clamped: true }); // that credential is used up
    expect(mb(s)).toBe(55);
    await mint(s, a, code); // a second credential carries a second allowance
    expect((await usage(s, a, 10_000_000)).data).toMatchObject({ countedBytes: 10_000_000, clamped: false });
  });

  it('is capped per player per UTC day (429 rate_limited / daily_cap), and resets the next day', async () => {
    const s = makeServer({ TURN_MAX_REPORTS_PER_PLAYER_DAY: '3' });
    const { a, code } = await room(s, '1');
    await mint(s, a, code);
    for (let i = 0; i < 3; i++) expect((await usage(s, a, 1000)).status).toBe(200);
    const r = await usage(s, a, 1000);
    expect(r.status).toBe(429);
    expect(r.data.error).toMatchObject({ code: 'rate_limited', reason: 'daily_cap', scope: 'turn_reports' });
    expect(r.data.error.retryAfterSec).toBeGreaterThan(0);
    s.advance(24 * 3600_000);
    const fresh = await room(s, '2');
    await mint(s, fresh.a, fresh.code);
    expect((await usage(s, fresh.a, 1000)).status).toBe(200);
    expect((await usage(s, a, 1000)).status).toBe(403); // a's old credential has expired by now
  });

  it('a refused mint (provider failure) leaves nothing to report against', async () => {
    let fail = true;
    const provider = { mint: async () => { if (fail) throw new Error('down'); return { servers: [{ urls: ['stun:x:1'] }] }; } };
    const s = makeServer({}, { deps: { turnProvider: provider } });
    const { a, code } = await room(s, '1');
    expect((await mint(s, a, code)).status).toBe(502);
    expect((await usage(s, a, 1000)).status).toBe(403);
    fail = false;
    expect((await mint(s, a, code)).status).toBe(200);
    expect((await usage(s, a, 1000)).status).toBe(200);
  });
});

describe('few-accounts exhaustion (finding 7)', () => {
  it('poisoning is bounded: N accounts can add at most N x (credentials per day) x (clamp), not their claimed numbers', async () => {
    const s = makeServer({ TURN_MAX_CREDS_PER_PLAYER_DAY: '2', TURN_MAX_REPORTS_PER_PLAYER_DAY: '50' });
    let total = 0;
    for (let i = 0; i < 4; i++) {
      const r = await room(s, `p${i}`);
      for (const who of [r.a, r.b]) {
        for (let k = 0; k < 4; k++) {
          await mint(s, who, r.code); // only 2 of 4 succeed (per-player cap)
          const u = await usage(s, who, 2 ** 39);
          if (u.status === 200) total += u.data.countedBytes;
        }
      }
    }
    // 8 accounts x 2 credentials x 57.6 MB, however many times and however big they claimed
    expect(total).toBe(8 * 2 * 57_600_000);
    expect(mb(s)).toBeLessThanOrEqual(8 * 2 * 55);
  });

  it('fresh accounts share a slice of the daily credentials: a swarm of new accounts cannot starve established players', async () => {
    const s = makeServer({ TURN_MAX_CREDS_PER_DAY: '10', TURN_MAX_CREDS_PER_PLAYER_DAY: '3', TURN_NEW_ACCOUNT_DAILY: '4', TURN_NEW_ACCOUNT_HOURS: '24' });
    // an established pair exists before the swarm (older than 24 h by the time the swarm arrives)
    const old1 = await signUp(s, 'Oldie1');
    const old2 = await signUp(s, 'Oldie2');
    s.advance(25 * 3600_000);
    const swarm = [];
    for (let i = 0; i < 4; i++) swarm.push(await room(s, `s${i}`));
    let ok = 0;
    let busy = 0;
    for (const r of swarm) {
      for (const who of [r.a, r.b]) {
        for (let k = 0; k < 3; k++) {
          const m = await mint(s, who, r.code);
          if (m.status === 200) ok++;
          else if (m.data.error.code === 'turn_budget') busy++;
        }
      }
    }
    expect(ok).toBe(4); // the fresh slice
    expect(busy).toBeGreaterThan(0);
    const { code } = await pairInRoom(s, old1, old2);
    expect((await mint(s, old1, code)).status).toBe(200); // the global budget (10) still has room for established players
    expect((await mint(s, old2, code)).status).toBe(200);
  });
});
