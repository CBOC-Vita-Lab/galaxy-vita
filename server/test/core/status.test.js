// Status that lasts (1.3, the owner's idle-player requirements): pruning never removes anyone who played, last played on the web board,
// past seasons archived, an all-time board.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp, startMatch, pairInRoom, makeBlob } from '../helpers.js';
import { season } from '../../src/core/calendar.js';

const DAY = 86400_000;
const YEAR = 365 * DAY;
const adm = { authorization: 'Bearer test-admin-token-0123456789' };
const res = (s, id, token, o) => s.call('POST', `/v1/matches/${id}/result`, { result: 0, score0: 3, score1: 1, checksum: 'deadbeefcafe0001', ...o }, token);
const exists = (s, pid) => s.store.sql.exec('SELECT COUNT(*) AS n FROM profiles WHERE pid = ?', pid).toArray()[0].n === 1;
const board = async (s, q = '') => (await s.call('GET', `/v1/public/leaderboard${q}`)).data;

async function rankedMatch(s, a, b, o = {}) {
  await s.call('POST', '/v1/ranked/queue', undefined, a.token);
  const q = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
  await startMatch(s, a, b, q.data.roomCode);
  s.advance(200_000);
  await res(s, q.data.room.matchId, a.token, o);
  return res(s, q.data.room.matchId, b.token, o);
}

describe('pruning only ever removes accounts that never played', () => {
  it('an account with any started match, any score row or any archived season is kept however long it is idle', async () => {
    const s = makeServer();
    const never = await signUp(s, 'Never1');
    const a = await signUp(s, 'Unranked1');
    const b = await signUp(s, 'Unranked2');
    const scorer = await signUp(s, 'ScoreOnly');
    const archived = await signUp(s, 'ArchOnly');
    const { code } = await pairInRoom(s, a, b);
    await startMatch(s, a, b, code); // an unranked match: no win, no loss, no score
    s.store.sql.exec(`INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (1, ?, 2, 0, '', ?)`, scorer.pid, s.clock.now);
    s.store.sql.exec(`INSERT INTO season_scores (season_start_ms, pid, score, data_b64, updated_ms) VALUES (1, ?, 5, '', 1)`, archived.pid);
    for (let i = 0; i < 10; i++) { s.advance(YEAR); s.app.sweep(); } // ten years away; the match records themselves expired after 90 days
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM matches').toArray()[0].n).toBe(0);
    expect(exists(s, never.pid)).toBe(false);
    for (const p of [a, b, scorer, archived]) expect(exists(s, p.pid)).toBe(true);
    // their names and rows are untouched
    expect(s.store.sql.exec('SELECT name FROM profiles WHERE pid = ?', a.pid).toArray()[0].name).toBe('Unranked1');
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM scores WHERE pid = ?', scorer.pid).toArray()[0].n).toBe(1);
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM season_scores WHERE pid = ?', archived.pid).toArray()[0].n).toBe(1);
    expect((await s.call('GET', '/v1/me', undefined, a.token)).status).toBe(200); // the token still works after ten years
  });

  it('a ranked player with wins and points is kept; a never-played account goes after 180 days, not before', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Ranked1');
    const b = await signUp(s, 'Ranked2');
    await rankedMatch(s, a, b);
    const idle = await signUp(s, 'IdleOne');
    s.advance(179 * DAY); s.app.sweep();
    expect(exists(s, idle.pid)).toBe(true);
    s.advance(2 * DAY); s.app.sweep();
    expect(exists(s, idle.pid)).toBe(false);
    s.advance(20 * YEAR); s.app.sweep();
    expect(exists(s, a.pid) && exists(s, b.pid)).toBe(true);
  });
});

describe('last played, all-time board', () => {
  it('a verified ranked match sets last played (to the hour) and adds the game\'s points; an unverified one does not', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Winner1');
    const b = await signUp(s, 'Loser1');
    const c = await signUp(s, 'Other1');
    const r = await rankedMatch(s, a, b);
    expect(r.data.status).toBe('verified');
    const at = Math.floor(s.clock.now / 3600000) * 3600000;
    const all = await board(s, '?board=alltime');
    expect(all.rows).toEqual([
      { rank: 1, name: 'Winner1', wins: 1, losses: 0, points: 13, lastPlayedMs: at }, // 10 for the win + 3 goals
      { rank: 2, name: 'Loser1', wins: 0, losses: 1, points: 2, lastPlayedMs: at }, // 1 + 1 goal
    ]);
    // a disputed match changes neither
    s.advance(DAY);
    await s.call('POST', '/v1/ranked/queue', undefined, c.token);
    const q = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    await startMatch(s, c, b, q.data.roomCode);
    s.advance(200_000);
    await res(s, q.data.room.matchId, c.token);
    const d = await res(s, q.data.room.matchId, b.token, { score0: 1, score1: 3 });
    expect(d.data.status).toBe('disputed');
    s.advance(61_000);
    const all2 = await board(s, '?board=alltime');
    expect(all2.rows.map((x) => [x.name, x.points, x.lastPlayedMs])).toEqual([['Winner1', 13, at], ['Loser1', 2, at]]);
    // the season board shows the same last played next to the game's own row
    s.store.sql.exec(`INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (0, ?, 2, 13, ?, ?)`, a.pid, makeBlob('Winner1', { wins: 1 }), s.clock.now);
    const sea = await board(s);
    expect(sea.rows[0]).toEqual({ rank: 1, name: 'Winner1', wins: 1, losses: 0, points: 13, lastPlayedMs: at });
  });
});

describe('season archive', () => {
  const put = (s, p, score) => s.call('POST', '/v1/rank/put', { category: 0, region: 2, score, data: makeBlob(p.name, { wins: score }) }, p.token);

  it('the final standings of a season stay viewable after the next season starts, by both paths', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open' });
    const a = await signUp(s, 'Alpha1');
    const b = await signUp(s, 'Bravo1');
    const c = await signUp(s, 'Charlie1');
    const first = season(s.clock.now);
    await put(s, a, 40);
    await put(s, b, 25);
    await put(s, c, 0); // a reset row is not a standing
    s.clock.now = first.endMs + 1000; // the next season, before any sweep
    await put(s, a, 3); // overwrites a's row: archived first
    let list = await board(s, '?board=archive');
    expect(list.seasons).toEqual([{ startMs: first.startMs, endMs: first.endMs, index: first.index, players: 1 }]);
    s.app.sweep(); // the season change: everything else of the old season
    s.advance(61_000);
    list = await board(s, '?board=archive');
    expect(list.seasons[0].players).toBe(2);
    const old = await board(s, `?board=archive&season=${first.startMs}`);
    expect(old.archived).toEqual({ startMs: first.startMs, endMs: first.endMs, index: first.index });
    expect(old.rows.map((r) => [r.rank, r.name, r.points, r.wins])).toEqual([[1, 'Alpha1', 40, 40], [2, 'Bravo1', 25, 25]]);
    expect((await board(s)).rows.map((r) => [r.name, r.points])).toEqual([['Alpha1', 3]]); // the live season starts from 0
    // running the sweep again changes nothing; a put after the sweep does not add to the archive
    s.app.sweep();
    await put(s, b, 9);
    s.advance(61_000);
    expect((await board(s, `?board=archive&season=${first.startMs}`)).rows.length).toBe(2);
  });

  it('keeps the best ARCHIVE_TOP rows per season, drops a deleted account, hides banned players, refuses bad parameters', async () => {
    const s = makeServer({ RANK_PUT_POLICY: 'open', ARCHIVE_TOP: '10' });
    const ps = [];
    for (let i = 0; i < 12; i++) ps.push(await signUp(s, `Player${'ABCDEFGHJKMN'[i]}`)); // (no I / L: they fold together)
    const first = season(s.clock.now);
    for (let i = 0; i < 12; i++) await put(s, ps[i], 100 - i);
    s.clock.now = first.endMs + 1000;
    s.app.sweep();
    let rows = (await board(s, `?board=archive&season=${first.startMs}&limit=100`)).rows;
    expect(rows.length).toBe(10);
    expect(rows[9].name).toBe('PlayerK');
    await s.call('DELETE', '/v1/me', undefined, ps[0].token);
    await s.call('POST', '/v1/admin/ban', { pid: ps[1].pid, reason: 'x' }, undefined, adm);
    s.advance(61_000);
    rows = (await board(s, `?board=archive&season=${first.startMs}&limit=100`)).rows;
    expect(rows.map((r) => r.name)).not.toContain('PlayerA');
    expect(rows.map((r) => r.name)).not.toContain('PlayerB');
    expect(s.store.sql.exec('SELECT COUNT(*) AS n FROM season_scores WHERE pid = ?', ps[0].pid).toArray()[0].n).toBe(0);
    for (const q of ['?board=season&season=' + first.startMs, '?board=archive&season=123', '?board=archive&season=abc', '?board=nope']) {
      expect((await s.call('GET', `/v1/public/leaderboard${q}`)).status).toBe(400);
    }
  });
});
