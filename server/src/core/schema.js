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
  {
    // Status that lasts (1.3): played-at-all counter (pruning never touches anyone who played), last verified ranked match, all-time
    // verified points, and the final standings of past weekly seasons. Additive only: the live data of migrations 1-2 is kept as it is.
    // Every step is safe to run again (addColumn checks the table first; the backfills recompute from matches; CREATE ... IF NOT EXISTS),
    // and the whole migration runs in one transaction (migrate()).
    id: 3,
    name: 'status_that_lasts',
    sql: [
      addColumn('profiles', 'matches_played', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('profiles', 'last_played_ms', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('profiles', 'points_total', 'INTEGER NOT NULL DEFAULT 0'),
      // backfill from the match records still kept (90 days) and from the counters that outlive them
      `UPDATE profiles SET matches_played = (SELECT COUNT(*) FROM matches m WHERE (m.p0 = profiles.pid OR m.p1 = profiles.pid) AND m.started_ms IS NOT NULL)
        WHERE matches_played = 0`,
      `UPDATE profiles SET matches_played = 1 WHERE matches_played = 0
        AND (wins + losses + draws + abandons > 0 OR pid IN (SELECT pid FROM scores))`,
      `UPDATE profiles SET last_played_ms = COALESCE((SELECT MAX(m.resolved_ms) FROM matches m
         WHERE m.kind = 'ranked' AND m.status = 'verified' AND (m.p0 = profiles.pid OR m.p1 = profiles.pid)), 0) WHERE last_played_ms = 0`,
      `UPDATE profiles SET points_total = COALESCE((SELECT SUM(
           CASE WHEN m.p0 = profiles.pid THEN (CASE WHEN m.score0 > m.score1 THEN 10 ELSE 1 END) + MIN(m.score0, 10)
                ELSE (CASE WHEN m.score1 > m.score0 THEN 10 ELSE 1 END) + MIN(m.score1, 10) END)
         FROM matches m WHERE m.kind = 'ranked' AND m.status = 'verified' AND (m.p0 = profiles.pid OR m.p1 = profiles.pid)), 0) WHERE points_total = 0`,
      // the all-time board reads the best rows through this index (a few rows read per page instead of every profile)
      `CREATE INDEX IF NOT EXISTS profiles_points ON profiles(points_total DESC, last_played_ms, pid)`,
      `CREATE TABLE IF NOT EXISTS season_scores (
        season_start_ms INTEGER NOT NULL,
        pid INTEGER NOT NULL,
        score INTEGER NOT NULL,
        data_b64 TEXT NOT NULL,
        updated_ms INTEGER NOT NULL,
        PRIMARY KEY (season_start_ms, pid)
      )`,
    ],
  },
  {
    // Galaxy 3.0 (GAME=smg, a second deployment of this code): rooms of up to 4, friends, star times. Additive only: Strikers rows
    // keep every value (the new rooms columns default to a 2-player Strikers room: game 'msc', max_members 2), and a Strikers
    // deployment never writes the new tables. Every step can run again (addColumn checks; IF NOT EXISTS).
    id: 4,
    name: 'galaxy_rooms_friends_times',
    sql: [
      addColumn('rooms', 'game', "TEXT NOT NULL DEFAULT 'msc'"),
      addColumn('rooms', 'max_members', 'INTEGER NOT NULL DEFAULT 2'),
      addColumn('rooms', 'mode', "TEXT NOT NULL DEFAULT ''"), // smg: coop | race
      addColumn('rooms', 'settings_json', "TEXT NOT NULL DEFAULT '{}'"),
      addColumn('rooms', 'visibility', "TEXT NOT NULL DEFAULT 'code'"), // code | friends | public
      addColumn('rooms', 'topology', "TEXT NOT NULL DEFAULT 'star'"),
      addColumn('rooms', 'app', "TEXT NOT NULL DEFAULT ''"), // smg: X-Msc-App (+ '+' X-Msc-Build) of the host: only the same version joins
      // smg host migration (DESIGN.md section 8): the host role moves to the lowest remaining slot; epoch +1 per move
      addColumn('rooms', 'host_slot', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('rooms', 'host_epoch', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('rooms', 'host_since_ms', 'INTEGER NOT NULL DEFAULT 0'),
      `CREATE INDEX IF NOT EXISTS rooms_quick ON rooms(game, visibility, mode, state, created_ms)`,
      // one row per member, the host included (slot 0); a guest who leaves loses the row, the room lives on
      `CREATE TABLE IF NOT EXISTS room_members (
        code TEXT NOT NULL,
        slot INTEGER NOT NULL,
        pid INTEGER NOT NULL,
        vip INTEGER NOT NULL,
        public TEXT, local TEXT, nonce TEXT, nat TEXT, ep_ms INTEGER, relay TEXT, cands TEXT,
        ack INTEGER NOT NULL DEFAULT 0,
        punch TEXT,
        started INTEGER NOT NULL DEFAULT 0,
        joined_ms INTEGER NOT NULL,
        PRIMARY KEY (code, slot)
      )`,
      `CREATE INDEX IF NOT EXISTS room_members_pid ON room_members(pid)`,
      // who left which slot when: the same player joining again within 15 s takes its old slot back (pruned by the sweep)
      `CREATE TABLE IF NOT EXISTS room_left (
        code TEXT NOT NULL,
        pid INTEGER NOT NULL,
        slot INTEGER NOT NULL,
        left_ms INTEGER NOT NULL,
        PRIMARY KEY (code, pid)
      )`,
      // directed: pid added friend_pid. Mutual = both rows exist; only mutual friends see presence and rooms.
      `CREATE TABLE IF NOT EXISTS friends (
        pid INTEGER NOT NULL,
        friend_pid INTEGER NOT NULL,
        created_ms INTEGER NOT NULL,
        PRIMARY KEY (pid, friend_pid)
      )`,
      `CREATE INDEX IF NOT EXISTS friends_rev ON friends(friend_pid)`,
      // best time per player, galaxy, star and category (category = players in the run, 1..4)
      `CREATE TABLE IF NOT EXISTS star_times (
        pid INTEGER NOT NULL,
        galaxy INTEGER NOT NULL,
        star INTEGER NOT NULL,
        category INTEGER NOT NULL,
        frames INTEGER NOT NULL,
        players INTEGER NOT NULL,
        verified INTEGER NOT NULL DEFAULT 0,
        flagged INTEGER NOT NULL DEFAULT 0,
        room TEXT,
        build TEXT NOT NULL DEFAULT '',
        splits TEXT,
        digest TEXT,
        updated_ms INTEGER NOT NULL,
        PRIMARY KEY (pid, galaxy, star, category)
      )`,
      `CREATE INDEX IF NOT EXISTS star_times_rank ON star_times(galaxy, star, category, frames, updated_ms, pid)`,
      // every submission for 2 days: the stored hourly cap, co-op / race verification, the self-improvement check
      `CREATE TABLE IF NOT EXISTS time_submits (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        pid INTEGER NOT NULL,
        galaxy INTEGER NOT NULL,
        star INTEGER NOT NULL,
        category INTEGER NOT NULL,
        frames INTEGER NOT NULL,
        room TEXT,
        verified INTEGER NOT NULL DEFAULT 0,
        submitted_ms INTEGER NOT NULL
      )`,
      `CREATE INDEX IF NOT EXISTS time_submits_pid ON time_submits(pid, submitted_ms)`,
      `CREATE INDEX IF NOT EXISTS time_submits_room ON time_submits(room, galaxy, star)`,
    ],
  },
  {
    // Galaxy 3.0: blocked players (friends.js). pid blocked blocked_pid: no further friend requests from them reach pid and no presence
    // is shared. Additive; a Strikers deployment never writes it.
    id: 5,
    name: 'galaxy_friend_blocks',
    sql: [
      `CREATE TABLE IF NOT EXISTS friend_blocks (
        pid INTEGER NOT NULL,
        blocked_pid INTEGER NOT NULL,
        created_ms INTEGER NOT NULL,
        PRIMARY KEY (pid, blocked_pid)
      )`,
      `CREATE INDEX IF NOT EXISTS friend_blocks_rev ON friend_blocks(blocked_pid)`,
    ],
  },
  {
    // Galaxy 3.0 hardening (security review): when a room started (a verified time cannot be longer than the room has been running),
    // where a member's calls come from (LAN endpoints go only to members behind the same public address; per-address caps on public
    // rooms), whether a member's endpoint claim did not match its address (steered to the relay), and an index so the idle-account
    // sweep does not scan every profile. Additive.
    id: 6,
    name: 'galaxy_hardening',
    sql: [
      addColumn('rooms', 'started_ms', 'INTEGER NOT NULL DEFAULT 0'),
      addColumn('room_members', 'src_ip', 'TEXT'),
      addColumn('room_members', 'forced_relay', 'INTEGER NOT NULL DEFAULT 0'),
      `CREATE INDEX IF NOT EXISTS profiles_last_seen ON profiles(last_seen_ms)`,
    ],
  },
  {
    // Galaxy 3.0 service review (Nov 11): accounts an admin keeps off the star-time boards (times.js: their times are refused and their
    // rows removed; the account still plays), and why a member left a room ('moved' = the same account joined another room from
    // somewhere else: a second console with a copied account file; rooms4.js answers account_elsewhere instead of not_member).
    // Additive: old builds and the previous code ignore both.
    id: 7,
    name: 'galaxy_service_review',
    sql: [
      `CREATE TABLE IF NOT EXISTS board_bans (
        pid INTEGER PRIMARY KEY,
        since_ms INTEGER NOT NULL,
        reason TEXT NOT NULL DEFAULT ''
      )`,
      addColumn('room_left', 'reason', "TEXT NOT NULL DEFAULT 'left'"),
    ],
  },
  {
    // Galaxy 3.0 outside review (S3): the board scans (total / before / rank in times.js) read every row of a star and category, hidden ones
    // included, because `verified` and `flagged` were not in the index. The new index carries both after pid, so the counts are answered from
    // the index alone, in board order; it replaces star_times_rank (same leading columns). Additive for old code: it only reads.
    id: 8,
    name: 'galaxy_board_index',
    sql: [
      `CREATE INDEX IF NOT EXISTS star_times_board ON star_times(galaxy, star, category, frames, updated_ms, pid, verified, flagged)`,
      `DROP INDEX IF EXISTS star_times_rank`,
    ],
  },
  {
    // Galaxy 3.0 outside review (S6): the most members a room ever held at once. A race or co-op time on the N-player board needs a room
    // that had N members (times.js); the live member list shrinks when people leave, this does not. 0 = a room from before this migration.
    id: 9,
    name: 'galaxy_room_peak',
    sql: [
      addColumn('rooms', 'peak_members', 'INTEGER NOT NULL DEFAULT 0'),
    ],
  },
  {
    // svcfix2 item 8 (privacy): a stranger who joins a room by CODE gets no address of the host (and the host none of his) until the
    // host's console accepts him (POST /v1/rooms/:code/accept) or he is the host's mutual friend. 1 = accepted (every existing row).
    id: 10,
    name: 'galaxy_join_accept',
    sql: [
      addColumn('room_members', 'accepted', 'INTEGER NOT NULL DEFAULT 1'),
      addColumn('room_left', 'accepted', 'INTEGER NOT NULL DEFAULT 1'),
    ],
  },
  {
    // code5: far-away invites (invites.js). One row per invite: the 5-character code, the host, the sealed blob (opaque to the service), the
    // version tag of the host (only the same version resolves), the life, and who resolved it. Short-lived; NOT in the admin export (it
    // holds sealed addresses). Additive: old builds never read it.
    id: 11,
    name: 'galaxy_invites',
    sql: [
      `CREATE TABLE IF NOT EXISTS invites (
        code TEXT PRIMARY KEY,
        pid INTEGER NOT NULL,
        blob TEXT NOT NULL,
        app TEXT NOT NULL DEFAULT '',
        created_ms INTEGER NOT NULL,
        expires_ms INTEGER NOT NULL,
        state TEXT NOT NULL DEFAULT 'open',
        closed_ms INTEGER NOT NULL DEFAULT 0,
        reason TEXT NOT NULL DEFAULT '',
        resolves INTEGER NOT NULL DEFAULT 0,
        resolvers TEXT NOT NULL DEFAULT ''
      )`,
      `CREATE INDEX IF NOT EXISTS invites_pid ON invites(pid, state)`,
    ],
  },
  {
    // Hard TURN usage stop: the username of every minted relay pass (to revoke the ones still valid when a usage limit trips) and whether it was revoked.
    // Additive: old builds never read the columns. Not in the admin export (turn_mints is not exported).
    id: 12,
    name: 'turn_pass_usernames',
    sql: [
      addColumn('turn_mints', 'username', "TEXT NOT NULL DEFAULT ''"),
      addColumn('turn_mints', 'revoked', 'INTEGER NOT NULL DEFAULT 0'),
    ],
  },
  {
    // sec34: the per-address / per-account daily request counters (budget.js) kept in storage so an evicted object does not reset them.
    // One row per (UTC day, class, key hash); written about once per 20 counted requests of a key; rows of earlier days are deleted when the day rolls.
    // WITHOUT ROWID: a write is one row written on the free plan, not two. Not in the admin export (counters of one day, keyed by hashes).
    id: 13,
    name: 'addr_day_counters',
    sql: [
      `CREATE TABLE IF NOT EXISTS addr_day (
        day INTEGER NOT NULL,
        cls TEXT NOT NULL,
        k TEXT NOT NULL,
        n INTEGER NOT NULL,
        PRIMARY KEY (day, cls, k)
      ) WITHOUT ROWID`,
    ],
  },
];

/** A migration step that adds a column only when the table does not have it yet (so a rerun cannot fail). */
function addColumn(table, column, decl) {
  return (sql) => {
    const cols = sql.exec(`PRAGMA table_info(${table})`).toArray().map((r) => r.name);
    if (!cols.includes(column)) sql.exec(`ALTER TABLE ${table} ADD COLUMN ${column} ${decl}`);
  };
}

/** Tables the admin export/import knows about (all have rowid). */
// room_members is left out on purpose: it holds live IP:ports of players (a backup is sensitive anyway, but this one need not carry addresses);
// rooms are short-lived and a restore does not need them.
export const EXPORT_TABLES = ['profiles', 'rooms', 'queue', 'matches', 'results', 'scores', 'allowance', 'pair_stats', 'counters', 'settings', 'flags',
  'season_scores', 'friends', 'star_times', 'friend_blocks', 'board_bans'];

/** Settings keys left out of the export (secrets: with the salt the per-address hashes of IPv4 could be brute-forced). */
export const EXPORT_HIDDEN_SETTINGS = ['addr_salt'];

/**
 * @param {{exec: Function}} sql SqlStorage-like (exec(query, ...bindings) -> cursor with toArray())
 * @param {number} now
 * @returns {number} schema version afterwards
 */
export function migrate(sql, now, tx = (fn) => fn()) {
  sql.exec(`CREATE TABLE IF NOT EXISTS _migrations (id INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_ms INTEGER NOT NULL)`);
  const done = new Set(sql.exec(`SELECT id FROM _migrations`).toArray().map((r) => r.id));
  let last = 0;
  for (const m of MIGRATIONS) {
    last = m.id;
    if (done.has(m.id)) continue;
    // one transaction per migration: it is applied completely or not at all (and then again at the next start)
    tx(() => {
      for (const stmt of m.sql) {
        if (typeof stmt === 'function') stmt(sql);
        else sql.exec(stmt);
      }
      sql.exec(`INSERT INTO _migrations (id, name, applied_ms) VALUES (?, ?, ?)`, m.id, m.name, now);
    });
  }
  return last;
}
