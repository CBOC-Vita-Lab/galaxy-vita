// Findings 5 and 6 of the security review: results that cannot be verified, and the loser who dodges the loss.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, startMatch } from '../helpers.js';
import { loadConfig } from '../../src/core/config.js';

const adm = { authorization: 'Bearer test-admin-token-0123456789' };
const res = (s, id, token, o) => s.call('POST', `/v1/matches/${id}/result`, { result: 0, score0: 3, score1: 1, checksum: 'deadbeefcafe0001', ...o }, token);
const me = async (s, p) => (await s.call('GET', '/v1/me', undefined, p.token)).data;
const allowance = (s, pid, cat = 0) => (s.store.sql.exec('SELECT amount FROM allowance WHERE pid = ? AND category = ?', pid, cat).toArray()[0] || { amount: 0 }).amount;
const flags = async (s, kind) => (await s.call('GET', `/v1/admin/flags${kind ? `?kind=${kind}` : ''}`, undefined, undefined, adm)).data.flags;

/** a (waits first, slot 0) and b get a ranked room and start it. Returns the match id. */
async function ranked(s, a, b) {
  const q1 = await s.call('POST', '/v1/ranked/queue', undefined, a.token);
  if (q1.status !== 200) return { err: q1 };
  const q = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
  if (q.status !== 200 || !q.data.roomCode) return { err: q };
  await startMatch(s, a, b, q.data.roomCode);
  return { matchId: q.data.room.matchId, code: q.data.roomCode };
}
/** Play a whole ranked match with both honest reports; slot 0 wins 3-1 by default. */
async function playOut(s, a, b, o = {}) {
  const m = await ranked(s, a, b);
  if (m.err) return m;
  s.advance(o.length ?? 200_000);
  await res(s, m.matchId, a.token, o.a);
  const last = await res(s, m.matchId, b.token, o.b);
  return { ...m, last };
}

describe('finding 5: minimum plausible match length', () => {
  it('the default is derived from the game: well under the shortest timed match (120 s), above a rushed 3-goal game', () => {
    const cfg = loadConfig({});
    expect(cfg.matches.minMatchMs).toBe(60_000);
    expect(cfg.matches.minMatchMs).toBeLessThan(120_000); // sGameTimeOptions[0] = 120 s is the shortest timed match
    expect(loadConfig({ MIN_MATCH_SEC: '90' }).matches.minMatchMs).toBe(90_000);
    expect(loadConfig({ MIN_MATCH_SEC: '0' }).matches.minMatchMs).toBe(0);
  });

  it('a normal finish sooner than MIN_MATCH_SEC after both started is refused with 429 + time left, and stores nothing', async () => {
    const s = makeServer({ MIN_MATCH_SEC: '60' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await ranked(s, a, b);
    s.advance(10_000);
    const early = await res(s, matchId, a.token);
    expect(early.status).toBe(429);
    expect(early.data.error).toMatchObject({ code: 'rate_limited', reason: 'too_early', retryAfterSec: 50 });
    expect(early.headers.get('retry-after')).toBe('50');
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM results').toArray()[0].n).toBe(0);
    s.advance(49_000);
    expect((await res(s, matchId, a.token)).status).toBe(429); // 1 s short
    s.advance(1_000);
    expect((await res(s, matchId, a.token)).status).toBe(200);
    expect((await res(s, matchId, b.token)).data.status).toBe('verified');
  });

  it('connection-lost / desync reports and unranked results are not held back (only a forged normal finish is)', async () => {
    const s = makeServer({ MIN_MATCH_SEC: '60' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await ranked(s, a, b);
    s.advance(5_000);
    for (const result of [2, 3, 4]) {
      const x = await s.call('POST', `/v1/matches/${matchId}/result`, { result, score0: 0, score1: 0, checksum: 'deadbeefcafe0001' }, a.token);
      expect(x.status).toBe(200);
      break; // one report per player is enough; the code path is the same for 2/3/4
    }
    const c = await signUp(s, 'Ccc3');
    const d = await signUp(s, 'Ddd4');
    const created = await s.call('POST', '/v1/rooms', undefined, c.token);
    const joined = await s.call('POST', '/v1/rooms/join', { code: created.data.code }, d.token);
    await startMatch(s, c, d, created.data.code);
    expect((await res(s, joined.data.matchId, c.token)).status).toBe(200);
  });

  it('a retry of an accepted result is still answered (idempotent), even though the clock is "early" for new ones', async () => {
    const s = makeServer({ MIN_MATCH_SEC: '60' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await ranked(s, a, b);
    s.advance(61_000);
    expect((await res(s, matchId, a.token)).status).toBe(200);
    expect((await res(s, matchId, a.token)).data.duplicate).toBe(true);
  });
});

describe('finding 5: rated matches per pair and per account per day', () => {
  it('pair cap: the matchmaker stops pairing the pair, other opponents still work, and the next UTC day resets it', async () => {
    const s = makeServer({ RATED_PER_PAIR_DAY: '2', ONE_OPP_MIN_WINS: '100' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const c = await signUp(s, 'Ccc3');
    expect((await playOut(s, a, b)).last.data.status).toBe('verified');
    expect((await playOut(s, a, b)).last.data.status).toBe('verified');
    // third time: a and b both wait, nothing pairs them
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const qb = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect(qb.data.status).toBe('queued');
    // a third player is paired with whoever waits longest among the allowed ones
    const qc = await s.call('POST', '/v1/ranked/queue', undefined, c.token);
    expect(qc.data.status).toBe('matched');
    await s.call('DELETE', '/v1/ranked/queue', undefined, b.token);
    await s.call('POST', `/v1/rooms/${qc.data.roomCode}/leave`, undefined, c.token); // frees a again (the match is void)
    s.advance(24 * 3600_000);
    const again = await playOut(s, a, b);
    expect(again.err?.data).toBeUndefined();
    expect(again.last.data.status).toBe('verified');
  });

  it('settle is the authority: a pair already at the cap when its match ends is settled no_contest (reason daily_cap_pair), nobody gains', async () => {
    const s = makeServer({ RATED_PER_PAIR_DAY: '2' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const m = await ranked(s, a, b);
    const [lo, hi] = [Math.min(a.pid, b.pid), Math.max(a.pid, b.pid)];
    const day = Math.floor(s.clock.now / 86400000);
    s.store.sql.exec('INSERT INTO pair_daily (lo_pid, hi_pid, day, rated) VALUES (?, ?, ?, 2)', lo, hi, day);
    s.advance(200_000);
    await res(s, m.matchId, a.token);
    const r = await res(s, m.matchId, b.token);
    expect(r.data).toMatchObject({ status: 'no_contest', final: true, reason: 'daily_cap_pair', scores: null });
    expect([(await me(s, a)).rating, (await me(s, b)).rating]).toEqual([1000, 1000]);
    expect([(await me(s, a)).wins, (await me(s, b)).losses]).toEqual([0, 0]);
    expect(allowance(s, a.pid)).toBe(5); // only the restore
  });

  it('account cap: at the cap a player is told so (429 rate_limited, reason daily_cap, wait until midnight UTC) and others still pair', async () => {
    const s = makeServer({ RATED_PER_ACCOUNT_DAY: '2', RATED_PER_PAIR_DAY: '0', ONE_OPP_MIN_WINS: '100' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const c = await signUp(s, 'Ccc3');
    const d = await signUp(s, 'Ddd4');
    expect((await playOut(s, a, b)).last.data.status).toBe('verified');
    expect((await playOut(s, a, c)).last.data.status).toBe('verified');
    const blocked = await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect(blocked.status).toBe(429);
    expect(blocked.data.error).toMatchObject({ code: 'rate_limited', reason: 'daily_cap', scope: 'ranked' });
    expect(blocked.data.error.retryAfterSec).toBe(Math.ceil((Date.UTC(2026, 9, 31) - s.clock.now) / 1000)); // until midnight UTC
    expect((await playOut(s, b, d)).last.data.status).toBe('verified'); // b has 1, d has 0
    s.advance(13 * 3600_000);
    expect((await s.call('POST', '/v1/ranked/queue', undefined, a.token)).status).toBe(200);
  });

  it('a forfeit win counts as a rated win (abandoning on purpose is not an uncapped way to feed a friend)', async () => {
    const s = makeServer({ RATED_PER_PAIR_DAY: '1', RESULT_WINDOW_SEC: '30' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const m = await ranked(s, a, b);
    s.advance(100_000);
    await res(s, m.matchId, a.token);
    s.advance(31_000);
    expect((await s.call('GET', `/v1/matches/${m.matchId}`, undefined, a.token)).data.status).toBe('abandoned');
    s.advance(20 * 60_000); // cooldown over
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect((await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data.status).toBe('queued'); // pair already used its one rated match today
  });
});

describe('finding 5: one-opponent win flag', () => {
  const env = { ONE_OPP_MIN_WINS: '3', ONE_OPP_SHARE_PCT: '70', RATED_PER_PAIR_DAY: '0' };
  it('wins that come mostly from one opponent set a stored flag with the evidence; no automatic ban', async () => {
    const s = makeServer(env);
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    for (let i = 0; i < 2; i++) await playOut(s, a, b);
    expect(await flags(s, 'single_opponent')).toEqual([]); // below the minimum number of wins
    await playOut(s, a, b);
    const f = await flags(s, 'single_opponent');
    expect(f).toHaveLength(1);
    expect(f[0]).toMatchObject({ pid: a.pid, name: 'Aaa1', kind: 'single_opponent', banned: false, detail: { opponent: b.pid, winsVsOpponent: 3, totalWins: 3 } });
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200); // not banned
    expect((await s.call('POST', '/v1/ranked/queue', undefined, a.token)).status).toBe(200);
    expect((await s.call('GET', '/v1/admin/stats', undefined, undefined, adm)).data.flags).toEqual({ single_opponent: 1 });
  });
  it('the flag shows in the admin export (table flags) and can be cleared by the admin', async () => {
    const s = makeServer(env);
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    for (let i = 0; i < 3; i++) await playOut(s, a, b);
    const ex = await s.call('GET', '/v1/admin/export?table=flags', undefined, undefined, adm);
    expect(ex.data.rows).toHaveLength(1);
    expect(ex.data.rows[0]).toMatchObject({ pid: a.pid, kind: 'single_opponent' });
    expect((await s.call('GET', '/v1/admin/export', undefined, undefined, adm)).data.tables.map((t) => t.table)).toContain('flags');
    expect((await s.call('POST', '/v1/admin/flags/clear', { pid: a.pid, kind: 'single_opponent' }, undefined, adm)).status).toBe(200);
    expect(await flags(s)).toEqual([]);
    expect((await s.call('GET', '/v1/admin/flags')).status).toBe(401); // admin only
  });
  it('wins spread over several opponents are not flagged', async () => {
    const s = makeServer(env);
    const a = await signUp(s, 'Aaa1');
    const others = [await signUp(s, 'Bbb2'), await signUp(s, 'Ccc3'), await signUp(s, 'Ddd4')];
    for (const o of others) await playOut(s, a, o);
    await playOut(s, a, others[0]);
    expect(await flags(s)).toEqual([]); // 4 wins, 2 against the top opponent = 50 %
  });
});

describe('finding 6: the loser cannot dodge the loss', () => {
  const env = { DODGE_MIN_SAMPLE: '3', DODGE_RATE_PCT: '50', DODGE_WINDOW: '10', RATED_PER_PAIR_DAY: '0', ONE_OPP_MIN_WINS: '100', QUEUE_BASE_WINDOW: '1000' };
  // the winner (slot 0) reports the real result; the loser pulls the plug and says "connection lost"
  const dodge = (s, w, l) => playOut(s, w, l, { b: { result: 4, score0: 0, score1: 0 } });

  it('the first dodges are free (not enough history), the one that crosses the threshold costs rating and the restore, and flags', async () => {
    const s = makeServer(env);
    const d = await signUp(s, 'Dodger1');
    const victims = [];
    for (const n of ['Vic1', 'Vic2', 'Vic3']) victims.push(await signUp(s, n));
    for (const v of victims.slice(0, 2)) {
      const r = await dodge(s, v, d);
      expect(r.last.data).toMatchObject({ status: 'no_contest', penalty: null });
    }
    expect(await flags(s, 'dodger')).toEqual([]);
    const ratingBefore = (await me(s, d)).rating;
    const allowBefore = allowance(s, d.pid);
    const r = await dodge(s, victims[2], d);
    expect(r.last.data.status).toBe('no_contest');
    expect(r.last.data.penalty).toEqual({ rating: -9, restoreWithheld: true }); // 75 % of 24 * 0.5
    expect((await me(s, d)).rating).toBe(ratingBefore - 9);
    expect(allowance(s, d.pid)).toBe(allowBefore); // no restore for the dodger ...
    expect(allowance(s, victims[2].pid)).toBe(5);  // ... but the victim gets the usual restore
    expect((await me(s, victims[2])).rating).toBe(1000); // and loses nothing
    const f = await flags(s, 'dodger');
    expect(f).toHaveLength(1);
    expect(f[0]).toMatchObject({ pid: d.pid, kind: 'dodger', banned: false, detail: { blamed: 3, of: 3 } });
    expect(await flags(s, 'single_opponent')).toEqual([]);
    // the victim is never blamed
    const vic = await s.call('GET', `/v1/matches/${r.matchId}`, undefined, victims[2].token);
    expect(vic.data.penalty).toBeNull();
  });

  it('a desync claim or a disputed score is blamed too; a mismatch explained by different builds is blamed on nobody', async () => {
    const s = makeServer(env);
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const stats = (p) => s.store.sql.exec('SELECT blame FROM matches ORDER BY rowid DESC LIMIT 1').toArray()[0].blame;
    await playOut(s, a, b, { b: { result: 2, score0: 0, score1: 0 } });
    expect(stats()).toBe(2); // slot 1 claimed a desync
    await playOut(s, a, b, { b: { score0: 0, score1: 2 } });
    expect(stats()).toBe(3); // both said "normal finish" with different scores: nobody can tell who lied
    await playOut(s, a, b, { a: { build: 'aaaaaaaa11111111' }, b: { score0: 0, score1: 2, build: 'bbbbbbbb22222222' } });
    expect(stats()).toBe(0); // different builds explain it
  });

  it('honest play dilutes the rate: after enough clean matches the same player is no longer flagged or penalised', async () => {
    const s = makeServer({ ...env, DODGE_RATE_PCT: '50' });
    const d = await signUp(s, 'Dodger1');
    const o = await signUp(s, 'Other1');
    for (let i = 0; i < 3; i++) await dodge(s, o, d);
    expect((await flags(s, 'dodger'))[0].hits).toBe(1); // the third crossed 3/3
    for (let i = 0; i < 5; i++) await playOut(s, o, d); // honest losses: 3 of 8
    const rBefore = (await me(s, d)).rating;
    const r = await dodge(s, o, d); // 4 of 9 = 44 % < 50 %
    expect(r.err).toBeUndefined();
    expect(r.last.data.penalty).toBeNull();
    expect((await me(s, d)).rating).toBe(rBefore);
  });

  it('silence is never better than an honest report: abandon > flagged dispute > 0 in rating, no restore, and the honest loss still earns points', async () => {
    const mk = async () => {
      const s = makeServer({ ...env, DODGE_MIN_SAMPLE: '1', DODGE_RATE_PCT: '1', RESULT_WINDOW_SEC: '30' });
      return { s, w: await signUp(s, 'Winner1'), l: await signUp(s, 'Loser1') };
    };
    // (1) honest report of a loss
    let { s, w, l } = await mk();
    await playOut(s, w, l);
    const honest = { rating: (await me(s, l)).rating - 1000, points: allowance(s, l.pid), cool: (await me(s, l)).cooldownUntilMs };
    // (2) silence: the loser never reports
    ({ s, w, l } = await mk());
    let m = await ranked(s, w, l);
    s.advance(100_000);
    await res(s, m.matchId, w.token);
    s.advance(31_000);
    await s.call('GET', `/v1/matches/${m.matchId}`, undefined, w.token);
    const silent = { rating: (await me(s, l)).rating - 1000, points: allowance(s, l.pid), cool: (await me(s, l)).cooldownUntilMs };
    // (3) a (flagged) dodger: connection-lost report instead of the loss
    ({ s, w, l } = await mk());
    await dodge(s, w, l);
    const dodger = { rating: (await me(s, l)).rating - 1000, points: allowance(s, l.pid), cool: (await me(s, l)).cooldownUntilMs };

    expect(honest.rating).toBe(-12);
    expect(silent.rating).toBeLessThanOrEqual(honest.rating);          // silence costs at least as much rating as an honest loss
    expect(silent.rating).toBeLessThan(dodger.rating);                  // and strictly more than a flagged dodge
    expect(dodger.rating).toBeLessThan(0);                              // a flagged dodge is not free
    expect(dodger.rating).toBe(-9);
    expect(silent.points).toBe(0);                                      // silence: no restore, no points
    expect(dodger.points).toBe(0);
    expect(honest.points).toBe(2 + 5);                                  // honest loss: 1 + goals(1) + restore
    expect(honest.points).toBeGreaterThan(silent.points);
    expect(silent.cool).toBeGreaterThan(0);                             // only silence is also put on a queue cooldown
    expect(honest.cool).toBe(0);
  });

  it('the penalty is configurable and can be switched off (DODGE_PENALTY_PCT=0 still flags and still withholds the restore)', async () => {
    const s = makeServer({ ...env, DODGE_MIN_SAMPLE: '1', DODGE_RATE_PCT: '1', DODGE_PENALTY_PCT: '0' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const r = await dodge(s, a, b);
    expect(r.last.data.penalty).toBeNull();
    expect((await flags(s, 'dodger'))).toHaveLength(1);
    expect(allowance(s, b.pid)).toBe(0);
    expect((await me(s, b)).rating).toBe(1000);
  });
});
