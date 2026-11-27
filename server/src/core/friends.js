// Galaxy (GAME=smg) friends and presence. A friendship is two directed rows (each player adds the other by public code);
// only MUTUAL friends see each other's presence and room. Presence = any authenticated call in the last SMG_ONLINE_SEC (60 s),
// kept in memory only (c.online: pid -> last call ms), never stored.

import { ApiError, strict } from './util.js';
import { normalizeCode } from './rooms.js';
import { fmtCode } from './accounts.js';
import { friendRoom, joinable, isMutual } from './rooms4.js';

/** Stored incoming friend requests per player (requests nobody answered); a stranger cannot grow this table without bound. */
export const INCOMING_MAX = 100;

function target(c, raw) {
  if (typeof raw !== 'string') throw new ApiError(400, 'bad_code', 'code must be a string', { field: 'code' });
  const code = normalizeCode(raw.replace(/-/g, ''), 8);
  const f = c.db.get(`SELECT pid, code, name FROM profiles WHERE code = ? AND banned = 0`, code);
  if (!f) throw new ApiError(404, 'player_not_found', 'no such player');
  return f;
}

/** POST /v1/friends {"code":"XXXX-XXXX"}: add (idempotent). */
export function addFriend(c, p, body) {
  strict(body, ['code']);
  const f = target(c, body.code);
  if (f.pid === p.pid) throw new ApiError(400, 'own_code', 'that is your own code', { field: 'code' });
  return c.tx(() => {
    if (isBlocked(c, p.pid, f.pid)) throw new ApiError(409, 'blocked', 'you blocked that player; unblock them first', { field: 'code' });
    if (isBlocked(c, f.pid, p.pid)) {
      // They blocked you: nothing is stored and nothing is revealed (the answer looks like any request that has not been answered yet).
      return { ok: true, added: true, friend: { code: fmtCode(f.code), name: f.name, mutual: false } };
    }
    const have = c.db.get(`SELECT 1 AS x FROM friends WHERE pid = ? AND friend_pid = ?`, p.pid, f.pid);
    if (!have) {
      const n = c.db.get(`SELECT COUNT(*) AS n FROM friends WHERE pid = ?`, p.pid).n;
      if (n >= c.cfg.smg.friendsMax) throw new ApiError(409, 'friends_full', `at most ${c.cfg.smg.friendsMax} friends`, { max: c.cfg.smg.friendsMax });
      c.db.run(`INSERT INTO friends (pid, friend_pid, created_ms) VALUES (?, ?, ?)`, p.pid, f.pid, c.now);
      // the target's stored incoming requests (added by others, not added back) are capped: the oldest beyond the cap are dropped
      c.db.run(
        `DELETE FROM friends WHERE rowid IN (
           SELECT f.rowid FROM friends f WHERE f.friend_pid = ?1
              AND NOT EXISTS (SELECT 1 FROM friends m WHERE m.pid = f.friend_pid AND m.friend_pid = f.pid)
            ORDER BY f.created_ms DESC, f.rowid DESC LIMIT -1 OFFSET ?2)`,
        f.pid, INCOMING_MAX,
      );
    }
    return { ok: true, added: !have, friend: { code: fmtCode(f.code), name: f.name, mutual: isMutual(c, p.pid, f.pid) } };
  });
}

/** DELETE /v1/friends/{code} and POST /v1/friends/remove {"code"}: remove your side (the friendship stops being mutual). */
export function removeFriend(c, p, rawCode) {
  const f = target(c, rawCode);
  return c.tx(() => {
    const had = c.db.get(`SELECT 1 AS x FROM friends WHERE pid = ? AND friend_pid = ?`, p.pid, f.pid);
    c.db.run(`DELETE FROM friends WHERE pid = ? AND friend_pid = ?`, p.pid, f.pid);
    return { ok: true, removed: !!had };
  });
}

/** Whether `pid` has blocked `other`. */
export function isBlocked(c, pid, other) {
  return !!c.db.get(`SELECT 1 AS x FROM friend_blocks WHERE pid = ? AND blocked_pid = ?`, pid, other);
}

/**
 * POST /v1/friends/dismiss {"code"}: drop a request someone sent you (an entry of "incoming": they added you, you did not add them).
 * Idempotent; a mutual friend is not touched (that is /remove). It does not stop them from sending a new request (use block for that).
 */
export function dismissIncoming(c, p, body) {
  strict(body, ['code']);
  const f = target(c, body.code);
  if (f.pid === p.pid) throw new ApiError(400, 'own_code', 'that is your own code', { field: 'code' });
  return c.tx(() => {
    if (c.db.get(`SELECT 1 AS x FROM friends WHERE pid = ? AND friend_pid = ?`, p.pid, f.pid)) return { ok: true, dismissed: false };
    const had = c.db.get(`SELECT 1 AS x FROM friends WHERE pid = ? AND friend_pid = ?`, f.pid, p.pid);
    c.db.run(`DELETE FROM friends WHERE pid = ? AND friend_pid = ?`, f.pid, p.pid);
    return { ok: true, dismissed: !!had };
  });
}

/**
 * POST /v1/friends/block {"code"}: both directions of the friendship go, and from now on their requests are dropped silently and
 * no presence or room is shared. Idempotent; at most SMG_BLOCKS_MAX (200) blocked players.
 */
export function blockPlayer(c, p, body) {
  strict(body, ['code']);
  const f = target(c, body.code);
  if (f.pid === p.pid) throw new ApiError(400, 'own_code', 'that is your own code', { field: 'code' });
  return c.tx(() => {
    const have = isBlocked(c, p.pid, f.pid);
    if (!have) {
      const n = c.db.get(`SELECT COUNT(*) AS n FROM friend_blocks WHERE pid = ?`, p.pid).n;
      if (n >= c.cfg.smg.blocksMax) throw new ApiError(409, 'blocks_full', `at most ${c.cfg.smg.blocksMax} blocked players`, { max: c.cfg.smg.blocksMax });
      c.db.run(`INSERT INTO friend_blocks (pid, blocked_pid, created_ms) VALUES (?, ?, ?)`, p.pid, f.pid, c.now);
    }
    c.db.run(`DELETE FROM friends WHERE (pid = ? AND friend_pid = ?) OR (pid = ? AND friend_pid = ?)`, p.pid, f.pid, f.pid, p.pid);
    return { ok: true, blocked: !have };
  });
}

/** POST /v1/friends/unblock {"code"}: idempotent. Does not restore the friendship. */
export function unblockPlayer(c, p, body) {
  strict(body, ['code']);
  const f = target(c, body.code);
  return c.tx(() => {
    const had = isBlocked(c, p.pid, f.pid);
    c.db.run(`DELETE FROM friend_blocks WHERE pid = ? AND blocked_pid = ?`, p.pid, f.pid);
    return { ok: true, unblocked: !!had };
  });
}

/** GET /v1/friends: the players you added (presence for mutual ones) and who added you without being added back. */
export function listFriends(c, p, ver) {
  const rows = c.db.all(
    `SELECT f.friend_pid AS pid, pr.code, pr.name,
            EXISTS (SELECT 1 FROM friends r WHERE r.pid = f.friend_pid AND r.friend_pid = f.pid) AS mutual
       FROM friends f JOIN profiles pr ON pr.pid = f.friend_pid
      WHERE f.pid = ? AND pr.banned = 0
      ORDER BY pr.name COLLATE NOCASE, pr.pid`,
    p.pid,
  );
  const friends = rows.map((r) => {
    const out = { code: fmtCode(r.code), name: r.name, mutual: !!r.mutual, online: false, room: null };
    if (!r.mutual) return out;
    const seen = c.online.get(r.pid);
    out.online = seen !== undefined && c.now - seen < c.cfg.smg.onlineMs;
    const room = out.online ? friendRoom(c, r.pid) : null;
    if (room && room.visibility !== 'code') out.room = { code: room.code, joinable: joinable(c, room, ver, p.pid), mode: room.mode };
    return out;
  });
  const incoming = c.db.all(
    `SELECT pr.code, pr.name FROM friends f JOIN profiles pr ON pr.pid = f.pid
      WHERE f.friend_pid = ? AND pr.banned = 0 AND NOT EXISTS (SELECT 1 FROM friends m WHERE m.pid = f.friend_pid AND m.friend_pid = f.pid)
      ORDER BY f.created_ms DESC LIMIT 50`,
    p.pid,
  ).map((r) => ({ code: fmtCode(r.code), name: r.name }));
  const blocked = c.db.all(
    `SELECT pr.code, pr.name FROM friend_blocks b JOIN profiles pr ON pr.pid = b.blocked_pid WHERE b.pid = ? ORDER BY b.created_ms DESC LIMIT 50`,
    p.pid,
  ).map((r) => ({ code: fmtCode(r.code), name: r.name }));
  const incomingTotal = c.db.get(
    `SELECT COUNT(*) AS n FROM friends f JOIN profiles pr ON pr.pid = f.pid
      WHERE f.friend_pid = ? AND pr.banned = 0 AND NOT EXISTS (SELECT 1 FROM friends m WHERE m.pid = f.friend_pid AND m.friend_pid = f.pid)`,
    p.pid,
  ).n;
  return { friends, incoming, incomingTotal, blocked, max: c.cfg.smg.friendsMax, onlineSec: c.cfg.smg.onlineMs / 1000 };
}
