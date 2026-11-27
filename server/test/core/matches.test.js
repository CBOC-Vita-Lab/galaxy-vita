import { describe, it, expect } from 'vitest';
import { makeServer, signUp, pairInRoom, startMatch } from '../helpers.js';
import { calcPoints, eloExpected } from '../../src/core/matches.js';

async function rankedMatch(s, a, b) {
  await s.call('POST', '/v1/ranked/queue', undefined, a.token);
  const q = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
  const code = q.roomCode;
  // a hosts (waited longer)
  await startMatch(s, a, b, code);
  return { code, matchId: q.room.matchId };
}
const res = (s, id, token, o) => s.call('POST', `/v1/matches/${id}/result`, { result: 0, score0: 3, score1: 1, checksum: 'deadbeefcafe0001', ...o }, token);
const allowance = (s, pid, cat = 0) => (s.store.sql.exec('SELECT amount FROM allowance WHERE pid = ? AND category = ?', pid, cat).toArray()[0] || { amount: 0 }).amount;
const me = async (s, p) => (await s.call('GET', '/v1/me', undefined, p.token)).data;

describe('points formula mirrors CalculateNetworkResultPoints', () => {
  it('win/loss/tie, goal cap 10, result codes', () => {
    expect(calcPoints(0, 3, 1)).toEqual({ points: 13, won: true });
    expect(calcPoints(0, 1, 3)).toEqual({ points: 2, won: false });
    expect(calcPoints(0, 2, 2)).toEqual({ points: 3, won: false });
    expect(calcPoints(0, 15, 0)).toEqual({ points: 20, won: true });
    expect(calcPoints(4, 5, 0)).toEqual({ points: 5, won: false });
    expect(calcPoints(3, 5, 0)).toEqual({ points: 5, won: false });
    expect(calcPoints(2, 5, 0)).toEqual({ points: 0, won: false });
  });
  it('elo is symmetric', () => {
    expect(eloExpected(1000, 1000)).toBe(0.5);
    expect(eloExpected(1200, 1000) + eloExpected(1000, 1200)).toBeCloseTo(1, 10);
  });
});

describe('ranked result agreement', () => {
  it('both report the same score and checksum -> verified, points credited, rating moves', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    const r1 = await res(s, matchId, a.token);
    expect(r1.status).toBe(200);
    expect(r1.data).toMatchObject({ status: 'awaiting_peer', final: false, submitted: { you: true, peer: false } });
    expect(r1.data.resultDeadlineMs).toBe(s.clock.now + 120_000);
    const r2 = await res(s, matchId, b.token);
    expect(r2.data).toMatchObject({ status: 'verified', final: true, slot: 1, scores: { slot0: 3, slot1: 1 } });
    expect(r2.data.credit).toEqual({ points: 2, won: false });
    const g = (await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token)).data;
    expect(g.credit).toEqual({ points: 13, won: true });
    expect(allowance(s, a.pid, 0)).toBe(13 + 5);
    expect(allowance(s, b.pid, 1)).toBe(2 + 5);
    const pa = await me(s, a);
    const pb = await me(s, b);
    expect([pa.wins, pa.losses, pb.wins, pb.losses]).toEqual([1, 0, 0, 1]);
    expect(pa.rating).toBe(1012);
    expect(pb.rating).toBe(988);
  });

  it('checksums are case-insensitive, scores must match exactly', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token, { checksum: 'DEADBEEFCAFE0001' });
    expect((await res(s, matchId, b.token)).data.status).toBe('verified');
  });

  it('different score -> disputed: nobody gains points, restore credit only, rating untouched', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token, { score0: 9, score1: 0 });
    const r = await res(s, matchId, b.token);
    expect(r.data).toMatchObject({ status: 'disputed', final: true, scores: null, credit: null });
    expect(allowance(s, a.pid)).toBe(5);
    expect(allowance(s, b.pid)).toBe(5);
    expect((await me(s, a)).rating).toBe(1000);
  });

  it('different checksum -> disputed', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token);
    expect((await res(s, matchId, b.token, { checksum: 'deadbeefcafe0002' })).data.status).toBe('disputed');
  });

  it('a desync report (result 2) or two connection losses -> no_contest', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    let m = await rankedMatch(s, a, b);
    await res(s, m.matchId, a.token, { result: 2 });
    expect((await res(s, m.matchId, b.token, { result: 2 })).data.status).toBe('no_contest');
    expect(allowance(s, a.pid)).toBe(5);

    const c = await signUp(s, 'Ccc3');
    const d = await signUp(s, 'Ddd4');
    m = await rankedMatch(s, c, d);
    await res(s, m.matchId, c.token, { result: 4 });
    expect((await res(s, m.matchId, d.token, { result: 3 })).data.status).toBe('no_contest');
  });

  it('result 0 against a connection-loss report is not verified', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token, { result: 0 });
    expect((await res(s, matchId, b.token, { result: 4 })).data.status).toBe('no_contest');
  });
});

describe('disconnect rule', () => {
  it('only one player reports within the window -> the silent one is the leaver', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token, { result: 4, score0: 2, score1: 0 });
    s.advance(60_000);
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token)).data.status).toBe('awaiting_peer');
    s.advance(61_000);
    const g = (await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token)).data;
    expect(g).toMatchObject({ status: 'abandoned', final: true, leaverSlot: 1 });
    expect(g.credit).toEqual({ points: 10, won: true }); // flat forfeit win
    const gb = (await s.call('GET', `/v1/matches/${matchId}`, undefined, b.token)).data;
    expect(gb.credit).toEqual({ points: 0, won: false });
    expect(allowance(s, a.pid)).toBe(15);
    expect(allowance(s, b.pid)).toBe(0); // the leaver gets no restore: the 5 point penalty stands
    const pb = await me(s, b);
    expect(pb).toMatchObject({ abandons: 1, losses: 1, rating: 988 });
    expect(pb.cooldownUntilMs).toBe(s.clock.now + 60_000);
  });

  it('the leaver cannot report after the window; the cooldown blocks queueing and grows with repeats', async () => {
    const s = makeServer({ ABANDON_COOLDOWN_SEC: '60' });
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    let m = await rankedMatch(s, a, b);
    await res(s, m.matchId, a.token, { result: 0 });
    s.advance(125_000);
    const late = await res(s, m.matchId, b.token);
    expect(late.status).toBe(409);
    expect(late.data.error.code).toBe('match_closed');
    expect((await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data.error.code).toBe('queue_cooldown');
    s.advance(61_000); // cooldown (60 s) is over now
    // second abandon within 24h -> 120 s cooldown
    await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, a.token)).data;
    expect(q.status).toBe('matched');
    await startMatch(s, b, a, q.roomCode);
    m = { matchId: q.room.matchId };
    // b hosted now (slot 0); a reports, b stays silent again
    await res(s, m.matchId, a.token, { result: 4 });
    s.advance(125_000);
    await s.call('GET', `/v1/matches/${m.matchId}`, undefined, a.token);
    const blocked = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect(blocked.status).toBe(429);
    expect(blocked.data.error.code).toBe('queue_cooldown');
    expect(blocked.data.error.retryAfterSec).toBeGreaterThan(100);
    expect((await me(s, b)).abandons).toBe(2);
    s.advance(130_000);
    expect((await s.call('POST', '/v1/ranked/queue', undefined, b.token)).status).toBe(200);
  });

  it('a lone desync report is not blamed on anyone', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token, { result: 2 });
    s.advance(125_000);
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, b.token)).data.status).toBe('no_contest');
    expect((await me(s, b)).abandons).toBe(0);
  });

  it('sweep (alarm) settles overdue matches even if nobody asks', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token, { result: 0 });
    s.advance(130_000);
    s.app.sweep();
    expect(s.store.sql.exec('SELECT status FROM matches WHERE match_id = ?', matchId).one().status).toBe('abandoned');
  });

  it('a match nobody reports on is no_contest after the maximum duration; never-started is void', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    s.advance(91 * 60_000);
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token)).data.status).toBe('no_contest');
    const c = await signUp(s, 'Ccc3');
    const d = await signUp(s, 'Ddd4');
    await s.call('POST', '/v1/ranked/queue', undefined, c.token);
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, d.token)).data;
    s.advance(6 * 60_000);
    expect((await s.call('GET', `/v1/matches/${q.room.matchId}`, undefined, c.token)).data.status).toBe('void');
  });
});

describe('result submission rules', () => {
  it('validates input', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    const post = (o) => res(s, matchId, a.token, o);
    expect((await post({ result: 1 })).data.error.code).toBe('bad_result');
    expect((await post({ result: '0' })).data.error.code).toBe('bad_result');
    expect((await post({ score0: -1 })).data.error.code).toBe('bad_score');
    expect((await post({ score1: 100 })).data.error.code).toBe('bad_score');
    expect((await post({ score0: 1.5 })).data.error.code).toBe('bad_score');
    expect((await post({ checksum: 'xyz' })).data.error.code).toBe('bad_checksum');
    expect((await post({ checksum: 'abcdef' })).data.error.code).toBe('bad_checksum');
    expect((await post({ checksum: 'a'.repeat(65) })).data.error.code).toBe('bad_checksum');
    expect((await res(s, 'short', a.token)).data.error.code).toBe('bad_match');
  });

  it('resubmitting the same result is idempotent, a different one is refused', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token);
    expect((await res(s, matchId, a.token)).status).toBe(200);
    const diff = await res(s, matchId, a.token, { score0: 4 });
    expect(diff.status).toBe(409);
    expect(diff.data.error.code).toBe('already_submitted');
  });

  it('strangers cannot read or write a match; unknown ids 404; unstarted matches refuse results', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const c = await signUp(s, 'Ccc3');
    const { matchId } = await rankedMatch(s, a, b);
    expect((await res(s, matchId, c.token)).status).toBe(404);
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, c.token)).status).toBe(404);
    expect((await s.call('GET', `/v1/matches/abcdefghijkl`, undefined, a.token)).status).toBe(404);
    const d = await signUp(s, 'Ddd4');
    const e = await signUp(s, 'Eee5');
    const r = await pairInRoom(s, d, e);
    const early = await res(s, r.matchId, d.token);
    expect(early.status).toBe(409);
    expect(early.data.error.code).toBe('match_not_started');
  });

  it('unranked matches are recorded and agreed but never touch rating, stats or credit', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { code, matchId } = await pairInRoom(s, a, b);
    await startMatch(s, a, b, code);
    await res(s, matchId, a.token);
    const r = await res(s, matchId, b.token);
    expect(r.data).toMatchObject({ kind: 'unranked', status: 'verified' });
    expect(allowance(s, a.pid)).toBe(0);
    expect(await me(s, a)).toMatchObject({ wins: 0, rating: 1000 });
  });

  it('history lists the caller\'s matches', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    const h = await s.call('GET', '/v1/me/matches?limit=5', undefined, a.token);
    expect(h.data.matches.map((m) => m.matchId)).toEqual([matchId]);
  });

  it('ties are draws; goal points are capped', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const { matchId } = await rankedMatch(s, a, b);
    await res(s, matchId, a.token, { score0: 2, score1: 2 });
    await res(s, matchId, b.token, { score0: 2, score1: 2 });
    expect(await me(s, a)).toMatchObject({ draws: 1, wins: 0, losses: 0, rating: 1000 });
  });
});
