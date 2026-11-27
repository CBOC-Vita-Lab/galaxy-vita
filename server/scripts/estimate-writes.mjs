#!/usr/bin/env node
// Free-tier budget estimate: replays one typical ranked match (and one invite match) through the real application on an
// in-memory SQLite database and counts HTTP requests and "rows written" the way Cloudflare bills them
// (https://developers.cloudflare.com/d1/platform/pricing/ : every INSERT / UPDATE / DELETE counts the table row, and one more
// row for each index the write touches; the Durable Objects page says SQLite storage uses the same rule).
//
//   node scripts/estimate-writes.mjs
//
// The index rule is applied statement by statement: INSERT / DELETE = 1 + number of indexes on the table (primary keys that are
// not the rowid count as an index), UPDATE = 1 + number of indexes whose column is in the SET list. ON CONFLICT upserts are
// counted as inserts (an upper bound). It is an estimate, not a bill: compare it with the dashboard after the first week.

import { makeServer, signUp, makeBlob } from '../test/helpers.js';

const srv = makeServer({ RATE_LIMIT_SCALE: '1000', MIN_MATCH_SEC: '0', QUEUE_BASE_WINDOW: '1000' });
const tally = { requests: 0, rows: 0, byTable: {} };
const real = srv.store.sql.exec.bind(srv.store.sql);
const idxCache = new Map();
function indexCols(table) {
  if (!idxCache.has(table)) {
    const cols = [];
    for (const ix of real(`PRAGMA index_list(${table})`).toArray()) {
      if (ix.origin === 'pk' && real(`PRAGMA table_info(${table})`).toArray().filter((c) => c.pk).length === 1
        && /INTEGER/i.test(real(`PRAGMA table_info(${table})`).toArray().find((c) => c.pk).type)) continue; // rowid alias
      cols.push(real(`PRAGMA index_info(${ix.name})`).toArray().map((c) => c.name));
    }
    idxCache.set(table, cols);
  }
  return idxCache.get(table);
}
let counting = false;
srv.store.sql.exec = (q, ...b) => {
  const cur = real(q, ...b);
  const m = /^\s*(INSERT(?: OR \w+)? INTO|UPDATE|DELETE FROM)\s+(\w+)/i.exec(q);
  if (counting && m && !q.includes('PRAGMA')) {
    const rows = real('SELECT changes() AS n').toArray()[0].n;
    if (rows > 0) {
      const table = m[2];
      const idx = indexCols(table);
      let per = 1;
      if (/^UPDATE/i.test(m[1])) {
        const set = /\bSET\b([\s\S]*?)(\bWHERE\b|$)/i.exec(q)?.[1] || '';
        per += idx.filter((cols) => cols.some((c) => new RegExp(`\\b${c}\\s*=`).test(set))).length;
      } else per += idx.length;
      tally.rows += rows * per;
      tally.byTable[table] = (tally.byTable[table] || 0) + rows * per;
    }
  }
  return cur;
};
const call = async (...a) => { if (counting) tally.requests++; return srv.call(...a); };
const adv = (ms) => srv.advance(ms);

async function snapshot(label, fn) {
  tally.requests = 0; tally.rows = 0; tally.byTable = {};
  counting = true;
  await fn();
  counting = false;
  console.log(`${label}: ${tally.requests} requests, ${tally.rows} rows written`);
  console.log('  by table:', Object.entries(tally.byTable).sort((a, b) => b[1] - a[1]).map(([t, n]) => `${t} ${n}`).join(', '));
  return { ...tally };
}

const a = await signUp(srv, 'Estim1');
const b = await signUp(srv, 'Estim2');
const poll = async (n, fn) => { for (let i = 0; i < n; i++) { await fn(); adv(2000); } };
const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAECAwQFBgc=', nat: 'cone' });

console.log('Typical ranked match, both players, 10 s of queueing, 3 min of play, ~5 requests of margin per client:\n');
const ranked = await snapshot('ranked match', async () => {
  await call('GET', '/v1/me', undefined, a.token);
  await call('GET', '/v1/me', undefined, b.token);
  await call('POST', '/v1/ranked/queue', undefined, a.token);
  await poll(4, () => call('GET', '/v1/ranked/queue', undefined, a.token)); // a waits ~8 s
  const q = await call('POST', '/v1/ranked/queue', undefined, b.token);
  const code = q.data.roomCode;
  const matchId = q.data.room.matchId;
  await call('GET', '/v1/ranked/queue', undefined, a.token);
  await call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
  await call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
  await poll(3, async () => { await call('GET', `/v1/rooms/${code}`, undefined, a.token); await call('GET', `/v1/rooms/${code}`, undefined, b.token); });
  await call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', ms: 800 }, a.token);
  await call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', ms: 900 }, b.token);
  await call('POST', `/v1/rooms/${code}/started`, undefined, a.token);
  await call('POST', `/v1/rooms/${code}/started`, undefined, b.token);
  adv(180_000);
  const res = { result: 0, score0: 3, score1: 1, checksum: 'deadbeefcafe0001', build: 'a1b2c3d4e5f60718', mode: 'ranked' };
  await call('POST', `/v1/matches/${matchId}/result`, res, a.token);
  await call('POST', `/v1/matches/${matchId}/result`, res, b.token);
  await call('GET', `/v1/matches/${matchId}`, undefined, a.token);
  await call('GET', `/v1/matches/${matchId}`, undefined, b.token);
  for (const [p, name] of [[a, 'Estim1'], [b, 'Estim2']]) {
    for (const category of [0, 1]) await call('POST', '/v1/rank/put', { category, region: 2, score: 0, data: makeBlob(name) }, p.token);
    for (const mode of [1, 2]) await call('POST', '/v1/rank/get', { mode, category: 0, limit: 5, sort: 1 }, p.token);
  }
  await call('GET', '/v1/me', undefined, a.token);
});
// the alarm: one invocation a minute while a room / queue / match is pending (about 6 minutes from queue to settled), one row each for setAlarm
const alarms = 6;
console.log(`+ housekeeping alarm: ~${alarms} invocations (requests) and ~${alarms} setAlarm rows\n`);

console.log('Invite (unranked) match:');
const invite = await snapshot('invite match', async () => {
  const room = await call('POST', '/v1/rooms', undefined, a.token);
  const code = room.data.code;
  await call('POST', '/v1/rooms/join', { code }, b.token);
  await call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
  await call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
  await poll(3, async () => { await call('GET', `/v1/rooms/${code}`, undefined, a.token); await call('GET', `/v1/rooms/${code}`, undefined, b.token); });
  await call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', ms: 800 }, a.token);
  await call('POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', ms: 900 }, b.token);
  await call('POST', `/v1/rooms/${code}/started`, undefined, a.token);
  await call('POST', `/v1/rooms/${code}/started`, undefined, b.token);
  await call('POST', `/v1/rooms/${code}/leave`, undefined, a.token);
});

const reg = await snapshot('registration', async () => { await srv.call('POST', '/v1/register', { name: 'Newbie1' }); tally.requests++; });

const perDayRows = 100_000;
const perDayReq = 100_000;
const rows = ranked.rows + alarms;
const reqs = ranked.requests + alarms;
console.log(`\nRanked match: ${reqs} requests and ~${rows} rows written (alarm included).`);
console.log(`Free tier (100,000 requests and 100,000 rows written a day, Durable Objects with SQLite): about ${Math.floor(perDayReq / reqs)} ranked matches a day by requests, about ${Math.floor(perDayRows / rows)} by rows written.`);
console.log(`Invite match: ${invite.requests} requests, ~${invite.rows} rows written. Registration: ~${reg.rows} rows written.`);
