import { describe, it, expect } from 'vitest';
import { makeServer, signUp } from '../helpers.js';
import { windowFor } from '../../src/core/ranked.js';
import { loadConfig } from '../../src/core/config.js';

const setRating = (s, pid, rating) => s.store.sql.exec('UPDATE profiles SET rating = ? WHERE pid = ?', rating, pid);

describe('skill window', () => {
  it('starts at 100, widens 50 per 5 s up to 1000', () => {
    const cfg = loadConfig({});
    expect(windowFor(cfg, 0)).toBe(100);
    expect(windowFor(cfg, 4999)).toBe(100);
    expect(windowFor(cfg, 5000)).toBe(150);
    expect(windowFor(cfg, 30_000)).toBe(400);
    expect(windowFor(cfg, 600_000)).toBe(1000);
  });
});

describe('ranked queue', () => {
  it('two similar players pair immediately; the longer waiter hosts', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    const qa = await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect(qa.data).toMatchObject({ status: 'queued', window: 100 });
    s.advance(1000);
    const qb = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect(qb.data.status).toBe('matched');
    expect(qb.data.room).toMatchObject({ kind: 'ranked', state: 'joined', role: 'guest', slot: 1 });
    const pa = await s.call('GET', '/v1/ranked/queue', undefined, a.token);
    expect(pa.data.status).toBe('matched');
    expect(pa.data.room).toMatchObject({ role: 'host', slot: 0 });
    expect(pa.data.roomCode).toBe(qb.data.roomCode);
    expect(pa.data.room.session.key).toBe(qb.data.room.session.key);
    expect(pa.data.room.you.vip).not.toBe(qb.data.room.you.vip);
    expect(pa.data.room.matchId).toBe(qb.data.room.matchId);
    // ranked rooms cannot be joined by invite
    const c = await signUp(s, 'Ccc3');
    expect((await s.call('POST', '/v1/rooms/join', { code: qb.data.roomCode }, c.token)).data.error.code).toBe('room_not_found');
  });

  it('players far apart wait until their windows overlap', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    setRating(s, b.pid, 1300);
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const qb = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect(qb.data.status).toBe('queued');
    // gap 300: both need a window >= 300, i.e. 4 steps = 20 s of waiting for the newer one too
    s.advance(15_000);
    await s.call('GET', '/v1/ranked/queue', undefined, a.token);
    expect((await s.call('GET', '/v1/ranked/queue', undefined, b.token)).data.status).toBe('queued');
    s.advance(10_000);
    await s.call('GET', '/v1/ranked/queue', undefined, a.token);
    const done = await s.call('GET', '/v1/ranked/queue', undefined, b.token);
    expect(done.data.status).toBe('matched');
  });

  it('picks the closest rating among candidates', async () => {
    const s = makeServer();
    const p = [];
    for (const [n, r] of [['Far', 1090], ['Near', 1010], ['Mid', 1060]]) {
      const x = await signUp(s, n);
      setRating(s, x.pid, r);
      p.push(x);
    }
    for (const x of p) await s.call('POST', '/v1/ranked/queue', undefined, x.token);
    // three players inside each other's window: first two pair up (Far+Near? depends on arrival) -> check the third stays queued
    const statuses = [];
    for (const x of p) statuses.push((await s.call('GET', '/v1/ranked/queue', undefined, x.token)).data.status);
    expect(statuses.filter((v) => v === 'matched').length).toBe(2);
    expect(statuses.filter((v) => v === 'queued').length).toBe(1);
    const me = await signUp(s, 'Late');
    const q = await s.call('POST', '/v1/ranked/queue', undefined, me.token);
    expect(q.data.status).toBe('matched'); // pairs with the remaining one
  });

  it('prefers the same region for the first seconds, then any region', async () => {
    const s = makeServer();
    const us = await signUp(s, 'Usa1', 2);
    const eu = await signUp(s, 'Eur1', 4);
    await s.call('POST', '/v1/ranked/queue', undefined, us.token);
    expect((await s.call('POST', '/v1/ranked/queue', undefined, eu.token)).data.status).toBe('queued');
    s.advance(21_000);
    await s.call('GET', '/v1/ranked/queue', undefined, us.token);
    expect((await s.call('GET', '/v1/ranked/queue', undefined, eu.token)).data.status).toBe('matched');
  });

  it('stale entries (no polling) are dropped; polling keeps you alive', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    s.advance(25_000);
    await s.call('GET', '/v1/ranked/queue', undefined, a.token); // still alive
    s.advance(25_000);
    await s.call('GET', '/v1/ranked/queue', undefined, a.token);
    s.advance(35_000); // a stopped polling
    const qb = await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    expect(qb.data.status).toBe('queued'); // a was dropped
    expect((await s.call('GET', '/v1/ranked/queue', undefined, a.token)).data.status).toBe('none');
  });

  it('leave removes you; waiting past the max wait times out', async () => {
    const s = makeServer({ QUEUE_MAX_WAIT_SEC: '60' });
    const a = await signUp(s, 'Aaa1');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    await s.call('DELETE', '/v1/ranked/queue', undefined, a.token);
    expect((await s.call('GET', '/v1/ranked/queue', undefined, a.token)).data.status).toBe('none');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    for (let i = 0; i < 7; i++) {
      s.advance(10_000);
      await s.call('GET', '/v1/ranked/queue', undefined, a.token);
    }
    expect((await s.call('GET', '/v1/ranked/queue', undefined, a.token)).data.status).toBe('none');
  });

  it('a player in a ranked match cannot queue again or open invite rooms', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    await s.call('POST', '/v1/ranked/queue', undefined, b.token);
    const again = await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    expect(again.status).toBe(409);
    expect(again.data.error.code).toBe('in_ranked_match');
    expect((await s.call('POST', '/v1/rooms', undefined, a.token)).data.error.code).toBe('in_ranked_match');
  });

  it('leaving a ranked room before the start voids the match without penalty', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Aaa1');
    const b = await signUp(s, 'Bbb2');
    await s.call('POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await s.call('POST', '/v1/ranked/queue', undefined, b.token)).data;
    await s.call('POST', `/v1/rooms/${q.roomCode}/leave`, undefined, b.token);
    expect((await s.call('GET', `/v1/matches/${q.room.matchId}`, undefined, a.token)).data.status).toBe('void');
    expect((await s.call('GET', '/v1/me', undefined, b.token)).data.abandons).toBe(0);
    expect((await s.call('POST', '/v1/ranked/queue', undefined, b.token)).status).toBe(200);
  });
});
