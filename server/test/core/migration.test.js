// Deploy safety (1.3): the live Durable Object holds data made by the deployed schema (test/fixtures/schema-e46c3e3.js, verbatim from git).
// The new code must migrate it in place, once, additively, keep every row and column value, and be safe to start again.
import { describe, it, expect } from 'vitest';
import { openNodeSql } from '../../src/sqlite-node.js';
import { createApp } from '../../src/core/app.js';
import { MIGRATIONS as OLD_MIGRATIONS, migrate as oldMigrate } from '../fixtures/schema-e46c3e3.js';
import { MIGRATIONS } from '../../src/core/schema.js';
import { HMAC_HEX, T0 } from '../helpers.js';

const env = { RANK_HMAC_KEY_HEX: HMAC_HEX, ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '0' };

function tables(sql) {
  return sql.exec(`SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' AND name <> '_migrations' ORDER BY name`).toArray().map((r) => r.name);
}
function columns(sql, t) {
  return sql.exec(`PRAGMA table_info(${t})`).toArray();
}
function dump(sql, t, cols) {
  const list = cols.map((c) => `"${c}"`).join(',');
  try { return sql.exec(`SELECT ${list} FROM ${t} ORDER BY rowid`).toArray(); }
  catch { return sql.exec(`SELECT ${list} FROM ${t} ORDER BY ${list}`).toArray(); } // a WITHOUT ROWID table (addr_day, sec34)
}

/** A database exactly as the deployed service makes it, with rows in every table. */
function oldDatabase() {
  const store = openNodeSql();
  oldMigrate(store.sql, T0);
  const sql = store.sql;
  for (const t of tables(sql)) {
    const cols = columns(sql, t);
    for (let i = 0; i < 3; i++) {
      const vals = cols.map((c) => {
        if (/INT/i.test(c.type)) return c.pk ? 100001 + i : 1000 + i * 7 + c.cid;
        return `${t}-${c.name}-${i}`;
      });
      sql.exec(`INSERT INTO ${t} (${cols.map((c) => `"${c.name}"`).join(',')}) VALUES (${cols.map(() => '?').join(',')})`, ...vals);
    }
  }
  // real-looking history for the backfills: two players, a verified ranked match 3-1, an unranked started match, a void one
  sql.exec(`INSERT INTO profiles (pid, code, name, name_key, token_hash, created_ms, last_seen_ms, wins, losses) VALUES
    (200001, 'AAAAAAAA', 'Alpha', 'alpha', 'h1', 1, 1, 1, 0), (200002, 'BBBBBBBB', 'Bravo', 'bravo', 'h2', 1, 1, 0, 1),
    (200003, 'CCCCCCCC', 'Casual', 'casual', 'h3', 1, 1, 0, 0), (200004, 'DDDDDDDD', 'Ghost', 'ghost', 'h4', 1, 1, 0, 0)`);
  sql.exec(`INSERT INTO matches (match_id, room_code, kind, p0, p1, created_ms, started_ms, resolved_ms, status, score0, score1) VALUES
    ('m-ranked-1', 'R1', 'ranked', 200001, 200002, 10, 20, ?, 'verified', 3, 1),
    ('m-casual-1', 'R2', 'unranked', 200003, 200001, 10, 20, 30, 'started', NULL, NULL),
    ('m-void-1', 'R3', 'unranked', 200004, 200003, 10, NULL, 30, 'void', NULL, NULL)`, T0 - 5000);
  return store;
}

describe('migration from the deployed schema', () => {
  it('keeps every row and value, adds only, backfills from history, and can start again', () => {
    const store = oldDatabase();
    const sql = store.sql;
    expect(sql.exec('SELECT MAX(id) AS v FROM _migrations').toArray()[0].v).toBe(OLD_MIGRATIONS.at(-1).id);
    const before = {};
    for (const t of tables(sql)) {
      const cols = columns(sql, t).map((c) => c.name);
      before[t] = { cols, rows: dump(sql, t, cols) };
    }

    createApp({ sql, tx: store.tx, env, now: () => T0 }); // what the Durable Object does at its next start
    expect(sql.exec('SELECT MAX(id) AS v FROM _migrations').toArray()[0].v).toBe(MIGRATIONS.at(-1).id);
    for (const [t, { cols, rows }] of Object.entries(before)) {
      expect(columns(sql, t).map((c) => c.name).slice(0, cols.length)).toEqual(cols); // old columns first, unchanged
      expect(dump(sql, t, cols)).toEqual(rows); // every row, every value
    }
    expect(tables(sql)).toContain('season_scores');

    const p = (pid) => sql.exec('SELECT matches_played, last_played_ms, points_total FROM profiles WHERE pid = ?', pid).toArray()[0];
    expect(p(200001)).toEqual({ matches_played: 2, last_played_ms: T0 - 5000, points_total: 13 });
    expect(p(200002)).toEqual({ matches_played: 1, last_played_ms: T0 - 5000, points_total: 2 });
    expect(p(200003)).toEqual({ matches_played: 1, last_played_ms: 0, points_total: 0 }); // unranked only: played, never ranked
    expect(p(200004)).toEqual({ matches_played: 0, last_played_ms: 0, points_total: 0 }); // only a void match: never played

    // a second start (or a rerun after a crash) changes nothing
    const after = {};
    for (const t of tables(sql)) after[t] = dump(sql, t, columns(sql, t).map((c) => c.name));
    createApp({ sql, tx: store.tx, env, now: () => T0 + 1 });
    for (const t of tables(sql)) expect(dump(sql, t, columns(sql, t).map((c) => c.name))).toEqual(after[t]);
    expect(sql.exec('SELECT COUNT(*) AS n FROM _migrations').toArray()[0].n).toBe(MIGRATIONS.length);
  });

  it('a migration interrupted halfway leaves the old schema and is applied again in full', () => {
    const store = oldDatabase();
    const sql = store.sql;
    // simulate a crash after the first step of migration 3 outside a transaction (the column exists, the migration is not recorded)
    sql.exec('ALTER TABLE profiles ADD COLUMN matches_played INTEGER NOT NULL DEFAULT 0');
    createApp({ sql, tx: store.tx, env, now: () => T0 });
    expect(sql.exec('SELECT MAX(id) AS v FROM _migrations').toArray()[0].v).toBe(MIGRATIONS.at(-1).id);
    expect(sql.exec('SELECT matches_played FROM profiles WHERE pid = 200001').toArray()[0].matches_played).toBe(2);
    // and a failing step rolls the whole migration back
    const s2 = oldDatabase();
    const bad = { ...s2.sql, exec: (q, ...b) => { if (/CREATE TABLE IF NOT EXISTS season_scores/.test(q)) throw new Error('disk full'); return s2.sql.exec(q, ...b); } };
    expect(() => createApp({ sql: bad, tx: s2.tx, env, now: () => T0 })).toThrow('disk full');
    expect(columns(s2.sql, 'profiles').map((c) => c.name)).not.toContain('points_total');
    expect(s2.sql.exec('SELECT MAX(id) AS v FROM _migrations').toArray()[0].v).toBe(OLD_MIGRATIONS.at(-1).id);
  });
});
