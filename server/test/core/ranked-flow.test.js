// 1.3 ranked: the exact request sequence of the game's ranked client (native/platform/sdk/dwc/np_dwc_match.cpp + the signaling's "@CODE"
// room entry + the result queue + the DWC ranking client), against the service: queue -> pairing -> room entry by GET -> endpoints ->
// started -> both results -> verified -> score puts (credit) -> the boards show both. Then the three ways it goes wrong.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, makeBlob } from '../helpers.js';

const APP = { 'x-msc-proto': '1', 'x-msc-app': '1.3.0', 'x-msc-build': 'a865ff18' };
const DAY = 86400_000;

async function pairByQueue(s, a, b) {
  const qa = await s.call('POST', '/v1/ranked/queue', {}, a.token, APP);
  expect(qa.status).toBe(200);
  expect(qa.data.status).toBe('queued');
  expect(qa.data.ticket.id).toBeTruthy();
  const qb = await s.call('POST', '/v1/ranked/queue', {}, b.token, APP);
  expect(qb.data.status).toBe('matched');
  const pa = await s.call('GET', '/v1/ranked/queue', undefined, a.token, APP); // the waiting console's next poll
  expect(pa.data.status).toBe('matched');
  const views = { a: pa.data, b: qb.data };
  for (const v of Object.values(views)) {
    expect(v.roomCode).toBe(qb.data.roomCode);
    expect(v.room.matchId).toBe(qb.data.room.matchId);
  }
  expect([views.a.room.slot, views.b.room.slot].sort()).toEqual([0, 1]);
  expect(views.a.room.slot).toBe(0); // the player who waited longer is slot 0 (AID 0, the session's host)
  return { code: qb.data.roomCode, matchId: qb.data.room.matchId };
}

async function link(s, a, b, code) {
  // the client's room entry for a ranked room: GET (a ranked room cannot be joined by code)
  for (const p of [a, b]) expect((await s.call('GET', `/v1/rooms/${code}`, undefined, p.token, APP)).status).toBe(200);
  expect((await s.call('POST', '/v1/rooms/join', { code }, b.token, APP)).status).toBe(404);
  const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.${n}.10:41000`, nonce: 'AAAAAAAAAAA=', nat: 'cone' });
  await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token, APP);
  await s.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token, APP);
  await s.call('POST', `/v1/rooms/${code}/started`, {}, a.token, APP);
  const st = await s.call('POST', `/v1/rooms/${code}/started`, {}, b.token, APP);
  expect(st.data.state).toBe('started');
}

const result = (s, p, matchId, o = {}) => s.call('POST', `/v1/matches/${matchId}/result`,
  { result: 0, score0: 2, score1: 1, checksum: '00000d2a1234abcd0000beef', ...o }, p.token, { ...APP, 'x-msc-attempt': '1', 'x-msc-mode': 'ranked' });
const put = (s, p, score, wins, losses, category = 0) => s.call('POST', '/v1/rank/put',
  { category, region: 2, score, data: makeBlob(p.name, { wins, losses }) }, p.token, APP);

describe('ranked: the whole game client sequence on the service', () => {
  it('queue -> match -> both report -> verified -> puts -> both rows on the game board and the web board', async () => {
    const s = makeServer({ MIN_MATCH_SEC: '60' });
    const a = await signUp(s, 'RankA1');
    const b = await signUp(s, 'RankB2');
    const { code, matchId } = await pairByQueue(s, a, b);
    await link(s, a, b, code);
    s.advance(190_000); // a 3-minute match
    const ra = await result(s, a, matchId);
    expect(ra.data.status).toBe('awaiting_peer');
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token, APP)).data.final).toBe(false); // the client holds its puts
    const rb = await result(s, b, matchId);
    expect(rb.data).toMatchObject({ status: 'verified', final: true, credit: { points: 2, won: false } });
    expect((await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token, APP)).data.credit).toEqual({ points: 12, won: true });
    // the game's puts after the match is final: 10 + 2 goals and 1 + 1 goal (plus the 5 the game took at the start comes back as credit)
    expect((await put(s, a, 12, 1, 0)).data).toMatchObject({ stored: 12, clamped: false });
    expect((await put(s, b, 2, 0, 1)).data).toMatchObject({ stored: 2, clamped: false });
    expect((await put(s, a, 12, 1, 0, 1)).data.stored).toBe(12); // the daily board
    // the game's NEAR read shows its own row (order >= 1) and the opponent
    const near = await s.call('POST', '/v1/rank/get', { mode: 2, category: 0, region: 255, sort: 1, since: 60, limit: 10 }, b.token, APP);
    expect(near.data.rows.map((r) => [r.order, r.pid, r.score])).toEqual([[1, a.pid, 12], [2, b.pid, 2]]);
    // the web board
    s.advance(61_000);
    const web = (await s.call('GET', '/v1/public/leaderboard?board=season')).data;
    expect(web.rows.map((r) => [r.rank, r.name, r.wins, r.losses, r.points])).toEqual([[1, 'RankA1', 1, 0, 12], [2, 'RankB2', 0, 1, 2]]);
    expect(web.rows.every((r) => r.lastPlayedMs > 0)).toBe(true);
    // the room ended with the match; both may queue again
    expect((await s.call('GET', `/v1/rooms/${code}`, undefined, a.token, APP)).data.state).toBe('closed');
    expect((await s.call('POST', '/v1/ranked/queue', {}, a.token, APP)).data.status).toBe('queued');
  });

  it('a duplicate submit (the retry queue after a suspend) is the same answer; a different one is refused', async () => {
    const s = makeServer();
    const a = await signUp(s, 'DupA1');
    const b = await signUp(s, 'DupB2');
    const { code, matchId } = await pairByQueue(s, a, b);
    await link(s, a, b, code);
    s.advance(190_000);
    await result(s, a, matchId);
    const again = await result(s, a, matchId, { attempt: 2 });
    expect(again.status).toBe(200);
    expect(again.data.duplicate).toBe(true);
    expect((await result(s, a, matchId, { score0: 3 })).status).toBe(409);
    expect((await result(s, b, matchId)).data.status).toBe('verified');
    expect((await result(s, b, matchId)).data.duplicate).toBe(true); // after the settle too
  });

  it('mismatched scores or digests: disputed, nobody gains, the start penalty comes back', async () => {
    const s = makeServer();
    const a = await signUp(s, 'DispA1');
    const b = await signUp(s, 'DispB2');
    const { code, matchId } = await pairByQueue(s, a, b);
    await link(s, a, b, code);
    s.advance(190_000);
    await result(s, a, matchId);
    const r = await result(s, b, matchId, { checksum: '00000d2a1234abcd0000beee' }); // a desync nobody noticed in the match
    expect(r.data.status).toBe('disputed');
    expect((await put(s, a, 12, 1, 0)).data).toMatchObject({ stored: 5, clamped: true }); // only the 5-point restore
    s.advance(61_000);
    expect((await s.call('GET', '/v1/public/leaderboard?board=alltime')).data.rows).toEqual([]);
  });

  it('one side disconnects (never reports): after the window the reporter gets the forfeit win, the silent side the loss and a cooldown', async () => {
    const s = makeServer();
    const a = await signUp(s, 'StayA1');
    const b = await signUp(s, 'LeaveB2');
    const { code, matchId } = await pairByQueue(s, a, b);
    await link(s, a, b, code);
    s.advance(100_000);
    await result(s, a, matchId, { result: 4 }); // the game's "connection lost" result: the link went at minute 1
    s.advance(121_000); // the result window
    s.app.sweep();
    const m = (await s.call('GET', `/v1/matches/${matchId}`, undefined, a.token, APP)).data;
    expect(m).toMatchObject({ status: 'abandoned', leaverSlot: 1, credit: { points: 10, won: true } });
    const q = await s.call('POST', '/v1/ranked/queue', {}, b.token, APP);
    expect(q.status).toBe(429);
    expect(q.data.error.code).toBe('queue_cooldown');
    expect((await s.call('POST', '/v1/ranked/queue', {}, a.token, APP)).data.status).toBe('queued');
    // a forfeit is not a verified match: no all-time points, no last played
    s.advance(DAY);
    expect((await s.call('GET', '/v1/public/leaderboard?board=alltime')).data.rows).toEqual([]);
  });
});
