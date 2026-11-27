// The schema of the deployed service (server/src/core/schema.js at e46c3e3), kept verbatim for the migration test. Do not edit.
// Storage schema. Each migration runs once, in order, inside the Durable Object's SQLite database.
// Add new migrations at the END; never edit a shipped one.

export const MIGRATIONS = [
  {
    id: 1,
    name: 'initial',
    sql: [
      `CREATE TABLE profiles (
        pid INTEGER PRIMARY KEY AUTOINCREMENT,
        code TEXT NOT NULL UNIQUE,
        relay_only INTEGER NOT NULL DEFAULT 0,
        name TEXT NOT NULL,
        name_key TEXT NOT NULL UNIQUE,
        token_hash TEXT NOT NULL UNIQUE,
        created_ms INTEGER NOT NULL,
        last_seen_ms INTEGER NOT NULL,
        name_changed_ms INTEGER NOT NULL DEFAULT 0,
        region INTEGER NOT NULL DEFAULT 2,
        rating INTEGER NOT NULL DEFAULT 1000,
        wins INTEGER NOT NULL DEFAULT 0,
        losses INTEGER NOT NULL DEFAULT 0,
        draws INTEGER NOT NULL DEFAULT 0,
        abandons INTEGER NOT NULL DEFAULT 0,
        abandon_streak INTEGER NOT NULL DEFAULT 0,
        last_abandon_ms INTEGER NOT NULL DEFAULT 0,
        cooldown_until_ms INTEGER NOT NULL DEFAULT 0,
        banned INTEGER NOT NULL DEFAULT 0,
        ban_reason TEXT NOT NULL DEFAULT ''
      )`,
      // profile ids start above 100000 (AUTOINCREMENT remembers the largest id ever used)
      `INSERT INTO profiles (pid, code, name, name_key, token_hash, created_ms, last_seen_ms) VALUES (100000, '', '', '', '', 0, 0)`,
      `DELETE FROM profiles WHERE pid = 100000`,
      `CREATE TABLE rooms (
        code TEXT PRIMARY KEY,
        kind TEXT NOT NULL,
        state TEXT NOT NULL,
        host_pid INTEGER NOT NULL,
        join_pid INTEGER,
        created_ms INTEGER NOT NULL,
        expires_ms INTEGER NOT NULL,
        version INTEGER NOT NULL DEFAULT 1,
        key_b64 TEXT,
        cookie INTEGER,
        vip_host INTEGER,
        vip_join INTEGER,
        host_public TEXT, host_local TEXT, host_nonce TEXT, host_nat TEXT, host_ep_ms INTEGER, host_relay TEXT, host_cands TEXT,
        join_public TEXT, join_local TEXT, join_nonce TEXT, join_nat TEXT, join_ep_ms INTEGER, join_relay TEXT, join_cands TEXT,
        host_ack INTEGER NOT NULL DEFAULT 0,
        join_ack INTEGER NOT NULL DEFAULT 0,
        host_punch TEXT,
        join_punch TEXT,
        punch_at_ms INTEGER,
        host_started INTEGER NOT NULL DEFAULT 0,
        join_started INTEGER NOT NULL DEFAULT 0,
        match_id TEXT,
        closed_reason TEXT
      )`,
      `CREATE INDEX rooms_host ON rooms(host_pid)`,
      `CREATE INDEX rooms_join ON rooms(join_pid)`,
      `CREATE INDEX rooms_expires ON rooms(expires_ms)`,
      `CREATE TABLE queue (
        pid INTEGER PRIMARY KEY,
        region INTEGER NOT NULL,
        rating INTEGER NOT NULL,
        enqueued_ms INTEGER NOT NULL,
        last_poll_ms INTEGER NOT NULL,
        status TEXT NOT NULL,
        room_code TEXT,
        ticket_id TEXT,
        ticket_hash TEXT
      )`,
      `CREATE TABLE matches (
        match_id TEXT PRIMARY KEY,
        room_code TEXT NOT NULL,
        kind TEXT NOT NULL,
        p0 INTEGER NOT NULL,
        p1 INTEGER NOT NULL,
        created_ms INTEGER NOT NULL,
        started_ms INTEGER,
        first_result_ms INTEGER,
        status TEXT NOT NULL,
        leaver_slot INTEGER,
        score0 INTEGER,
        score1 INTEGER,
        resolved_ms INTEGER
      )`,
      `CREATE INDEX matches_p0 ON matches(p0)`,
      `CREATE INDEX matches_p1 ON matches(p1)`,
      `CREATE INDEX matches_status ON matches(status)`,
      `CREATE TABLE results (
        match_id TEXT NOT NULL,
        slot INTEGER NOT NULL,
        pid INTEGER NOT NULL,
        result INTEGER NOT NULL,
        score0 INTEGER NOT NULL,
        score1 INTEGER NOT NULL,
        checksum TEXT NOT NULL,
        submitted_ms INTEGER NOT NULL,
        attempt INTEGER NOT NULL DEFAULT 1,
        build TEXT NOT NULL DEFAULT '',
        mode TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (match_id, slot)
      )`,
      `CREATE TABLE pair_stats (
        lo_pid INTEGER NOT NULL,
        hi_pid INTEGER NOT NULL,
        fails INTEGER NOT NULL DEFAULT 0,
        oks INTEGER NOT NULL DEFAULT 0,
        updated_ms INTEGER NOT NULL,
        PRIMARY KEY (lo_pid, hi_pid)
      )`,
      `CREATE TABLE scores (
        category INTEGER NOT NULL,
        pid INTEGER NOT NULL,
        region INTEGER NOT NULL,
        score INTEGER NOT NULL,
        data_b64 TEXT NOT NULL,
        updated_ms INTEGER NOT NULL,
        PRIMARY KEY (category, pid)
      )`,
      `CREATE INDEX scores_rank ON scores(category, score DESC, updated_ms, pid)`,
      `CREATE TABLE allowance (
        pid INTEGER NOT NULL,
        category INTEGER NOT NULL,
        amount INTEGER NOT NULL,
        PRIMARY KEY (pid, category)
      )`,
      `CREATE TABLE counters (
        key TEXT PRIMARY KEY,
        value INTEGER NOT NULL,
        updated_ms INTEGER NOT NULL
      )`,
      `CREATE TABLE settings (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL
      )`,
    ],
  },
  {
    // Result checks, caps and flags (security review round 1). Unshipped when written: extend until the first deploy.
    id: 2,
    name: 'abuse_controls',
    sql: [
      // why a settled match was not rated ('daily_cap'), who is blamed for a dispute / no-contest (bit 0 = slot 0, bit 1 = slot 1)
      // and the rating penalty each slot took for it
      `ALTER TABLE matches ADD COLUMN reason TEXT NOT NULL DEFAULT ''`,
      `ALTER TABLE matches ADD COLUMN blame INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE matches ADD COLUMN pen0 INTEGER NOT NULL DEFAULT 0`,
      `ALTER TABLE matches ADD COLUMN pen1 INTEGER NOT NULL DEFAULT 0`,
      // one row per player, overwritten when the UTC day changes: no growth, one row written per event
      `CREATE TABLE daily (
        pid INTEGER PRIMARY KEY,
        day INTEGER NOT NULL,
        rooms INTEGER NOT NULL DEFAULT 0,
        matches INTEGER NOT NULL DEFAULT 0,
        rated INTEGER NOT NULL DEFAULT 0,
        turn_reports INTEGER NOT NULL DEFAULT 0
      )`,
      `CREATE TABLE pair_daily (
        lo_pid INTEGER NOT NULL,
        hi_pid INTEGER NOT NULL,
        day INTEGER NOT NULL,
        rated INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (lo_pid, hi_pid)
      )`,
      // stored account flags for the admin (no automatic ban): kind = single_opponent | dodger
      `CREATE TABLE flags (
        pid INTEGER NOT NULL,
        kind TEXT NOT NULL,
        since_ms INTEGER NOT NULL,
        updated_ms INTEGER NOT NULL,
        hits INTEGER NOT NULL DEFAULT 1,
        detail TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (pid, kind)
      )`,
      // relay credentials minted per player: usage reports are only accepted against a recent mint
      `CREATE TABLE turn_mints (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        pid INTEGER NOT NULL,
        minted_ms INTEGER NOT NULL,
        used_bytes INTEGER NOT NULL DEFAULT 0
      )`,
      `CREATE INDEX turn_mints_pid ON turn_mints(pid, minted_ms)`,
    ],
  },
];

/** Tables the admin export/import knows about (all have rowid). */
export const EXPORT_TABLES = ['profiles', 'rooms', 'queue', 'matches', 'results', 'scores', 'allowance', 'pair_stats', 'counters', 'settings', 'flags'];

/**
 * @param {{exec: Function}} sql SqlStorage-like (exec(query, ...bindings) -> cursor with toArray())
 * @param {number} now
 * @returns {number} schema version afterwards
 */
export function migrate(sql, now) {
  sql.exec(`CREATE TABLE IF NOT EXISTS _migrations (id INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_ms INTEGER NOT NULL)`);
  const done = new Set(sql.exec(`SELECT id FROM _migrations`).toArray().map((r) => r.id));
  let last = 0;
  for (const m of MIGRATIONS) {
    last = m.id;
    if (done.has(m.id)) continue;
    for (const stmt of m.sql) sql.exec(stmt);
    sql.exec(`INSERT INTO _migrations (id, name, applied_ms) VALUES (?, ?, ?)`, m.id, m.name, now);
  }
  return last;
}
