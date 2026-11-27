// A whole ranked session as the game would drive it, including the game's own "-5 at start, restore at end" scoring.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, makeBlob, startMatch } from '../helpers.js';

const putScore = (s, p, category, score) => s.call('POST', '/v1/rank/put', { category, region: 2, score, data: makeBlob(p.name) }, p.token);
const near = (s, p, category = 0) => s.call('POST', '/v1/rank/get', { mode: 2, category, region: 2, sort: 1, since: 1, limit: 5 }, p.token);
const result = (s, id, p, o) => s.call('POST', `/v1/matches/${id}/result`, { result: 0, score0: 4, score1: 2, checksum: '0123456789abcdef', ...o }, p.token);
const mine = (r, p) => r.data.rows.find((x) => x.pid === p.pid);

describe('ranked session', () => {
  it('queue -> match -> play -> agreed result -> honest scores are stored, inflated ones are clamped', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Anna');
    const b = await signUp(s, 'Bert');
    // first thing a client does: read its own rows (new player -> synthesized score-0 row, order >= 1)
    const n0 = await near(s, a);
    expect(mine(n0, a)).toMatchObject({ score: 0 });
    expect(mine(n0, a).order).toBeGreaterThanOrEqual(1);

    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    expect(q.status).toBe('matched');
    await startMatch(s, a, b, q.roomCode);
    const id = q.room.matchId;

    // match start: the game takes 5 points off (never below 0)
    expect((await putScore(s, a, 0, 0)).data.stored).toBe(0);
    // match end: both report the same final result
    await result(s, id, a);
    expect((await result(s, id, b)).data.status).toBe('verified');
    // Anna won 4-2: 10 + 4 = 14 (+ restore 5 = credit 19)
    expect((await putScore(s, a, 0, 14)).data).toEqual({ ok: true, stored: 14, clamped: false });
    // Bert lost: 1 + 2 = 3
    expect((await putScore(s, b, 0, 3)).data).toEqual({ ok: true, stored: 3, clamped: false });
    // a cheat attempt: claim 500 -> clamped to what is left (5)
    expect((await putScore(s, a, 0, 500)).data).toEqual({ ok: true, stored: 19, clamped: true });
    // and the daily board (category 1) is credited separately
    expect((await putScore(s, a, 1, 14)).data.stored).toBe(14);

    const n1 = await near(s, a);
    expect(n1.data.rows.map((r) => [r.pid, r.score, r.order])).toEqual([[a.pid, 19, 1], [b.pid, 3, 2]]);
    const profile = (await s.call('GET', '/v1/me', undefined, a.token)).data;
    expect(profile).toMatchObject({ wins: 1, rating: 1012 });
    // the room is over, both can queue again
    expect((await s.call('POST', '/v1/ranked/queue', undefined, a.token)).status).toBe(200);
  });

  it('the leaver keeps the penalty: no restore credit, so the -5 stands', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Anna');
    const b = await signUp(s, 'Bert');
    // both have 20 points from before
    s.store.sql.exec('INSERT INTO allowance (pid, category, amount) VALUES (?, 0, 20), (?, 0, 20)', a.pid, b.pid);
    await putScore(s, a, 0, 20);
    await putScore(s, b, 0, 20);
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    await startMatch(s, a, b, q.roomCode);
    await putScore(s, a, 0, 15); // -5 at match start
    await putScore(s, b, 0, 15);
    // Bert pulls the plug; Anna reports a connection loss and waits out the window
    await result(s, q.room.matchId, a, { result: 4 });
    s.advance(125_000);
    await s.call('GET', `/v1/matches/${q.room.matchId}`, undefined, a.token);
    // Bert's client (still running) tries to restore its points anyway
    expect((await putScore(s, b, 0, 20)).data).toEqual({ ok: true, stored: 15, clamped: true });
    // Anna gets the forfeit win: 10 + restore 5
    expect((await putScore(s, a, 0, 35)).data).toEqual({ ok: true, stored: 30, clamped: true });
  });

  it('disputed results restore the penalty and give nothing else', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Anna');
    const b = await signUp(s, 'Bert');
    s.store.sql.exec('INSERT INTO allowance (pid, category, amount) VALUES (?, 0, 20), (?, 0, 20)', a.pid, b.pid);
    await putScore(s, a, 0, 20);
    await putScore(s, b, 0, 20);
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    await startMatch(s, a, b, q.roomCode);
    await putScore(s, a, 0, 15);
    await result(s, q.room.matchId, a, { score0: 10, score1: 0 });
    expect((await result(s, q.room.matchId, b, { score0: 0, score1: 10 })).data.status).toBe('disputed');
    expect((await putScore(s, a, 0, 30)).data).toEqual({ ok: true, stored: 20, clamped: true });
  });
});
