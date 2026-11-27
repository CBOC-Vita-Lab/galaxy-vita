// Ranked matchmaking: a small queue with a skill window that widens the longer a player waits.
// Players poll (every 1.5-3 s); each poll tries to pair the caller. When two players match, the server
// creates a ranked room with both inside, and from then on the room endpoints are used exactly as for invites.

import { ApiError, b64url, randomBytes, sha256hex, strict } from './util.js';
import { createPairedRoom, roomView, loadRoom, touch } from './rooms.js';
import { atDailyCap, dailyCapError, getDaily, pairRatedToday } from './limits.js';

/** Allowed rating difference after waiting `waitMs`. */
export function windowFor(cfg, waitMs) {
  const q = cfg.queue;
  const steps = Math.floor(Math.max(0, waitMs) / q.stepMs);
  return Math.min(q.maxWindow, q.baseWindow + q.step * steps);
}

function queueView(c, row, extra = {}) {
  const wait = c.now - row.enqueued_ms;
  const silent = c.now - row.last_poll_ms;
  const v = {
    status: row.status, // queued | matched
    waitedMs: wait,
    window: windowFor(c.cfg, wait),
    expiresMs: row.last_poll_ms + c.cfg.queue.staleMs,
    // connection health as the server sees it: quiet after 5 s without a poll (send a ping/poll), lost after 15 s;
    // a lost ticket is skipped by the matchmaker but can still be resumed until it is dropped at 30 s
    link: silent >= c.cfg.rooms.lostMs ? 'lost' : silent >= c.cfg.rooms.quietMs ? 'quiet' : 'ok',
    ticketId: row.ticket_id,
    roomCode: row.room_code || null,
    ...extra,
  };
  return v;
}

function dropStale(c) {
  c.db.run(`DELETE FROM queue WHERE status = 'queued' AND last_poll_ms < ?`, c.now - c.cfg.queue.staleMs);
  c.db.run(`DELETE FROM queue WHERE status = 'matched' AND last_poll_ms < ?`, c.now - 120_000);
}

/** Would pairing these two break a daily cap (rated per pair / per account, match records per account)? */
function capped(c, a, b) {
  const m = c.cfg.matches;
  const caps = c.cfg.caps;
  if (m.ratedPerPairDay && pairRatedToday(c, a, b) >= m.ratedPerPairDay) return true;
  for (const pid of [a, b]) {
    if (atDailyCap(c, pid, 'matches', caps.matchesPerAccountDay) || atDailyCap(c, pid, 'rated', m.ratedPerAccountDay)) return true;
  }
  return false;
}

function tryPair(c, me) {
  const q = c.cfg.queue;
  const myWait = c.now - me.enqueued_ms;
  const myWin = windowFor(c.cfg, myWait);
  const cands = c.db.all(
    `SELECT * FROM queue WHERE status = 'queued' AND pid <> ? ORDER BY enqueued_ms LIMIT 200`, me.pid,
  );
  let best = null;
  let bestDiff = Infinity;
  for (const o of cands) {
    if (c.now - o.last_poll_ms >= c.cfg.rooms.lostMs) continue; // a ghost: not heard from for 15 s
    const oWait = c.now - o.enqueued_ms;
    const diff = Math.abs(o.rating - me.rating);
    if (diff > Math.min(myWin, windowFor(c.cfg, oWait))) continue; // both players must accept the gap
    if (o.region !== me.region && Math.min(myWait, oWait) < q.regionStrictMs) continue; // same region first
    if (diff < bestDiff) {
      if (capped(c, me.pid, o.pid)) continue; // a pair or player over today's cap is never paired
      best = o;
      bestDiff = diff;
    }
  }
  if (!best) return null;
  // the longer waiter hosts (slot 0)
  const [a, b] = best.enqueued_ms <= me.enqueued_ms ? [best, me] : [me, best];
  const code = createPairedRoom(c, a.pid, b.pid);
  c.db.run(`UPDATE queue SET status = 'matched', room_code = ?, last_poll_ms = ? WHERE pid IN (?, ?)`, code, c.now, a.pid, b.pid);
  return code;
}

export async function joinQueue(c, p) {
  const ticketId = b64url(randomBytes(9));
  const ticketToken = b64url(randomBytes(16));
  const ticketHash = await sha256hex(ticketToken);
  return c.tx(() => {
    dropStale(c);
    if (p.cooldown_until_ms > c.now) {
      throw new ApiError(429, 'queue_cooldown', 'you left a ranked match; wait before queuing again', {
        retryAfterSec: Math.ceil((p.cooldown_until_ms - c.now) / 1000),
      });
    }
    const live = c.db.get(
      `SELECT code FROM rooms WHERE (host_pid = ? OR join_pid = ?) AND kind = 'ranked' AND state IN ('joined','ready','started')`,
      p.pid, p.pid,
    );
    if (live) throw new ApiError(409, 'in_ranked_match', 'you already have a ranked match', { roomCode: live.code });
    // tell a player who is over today's cap instead of letting them wait for a pairing that cannot happen
    const today = getDaily(c, p.pid);
    if ((c.cfg.matches.ratedPerAccountDay && today.rated >= c.cfg.matches.ratedPerAccountDay)
      || (c.cfg.caps.matchesPerAccountDay && today.matches >= c.cfg.caps.matchesPerAccountDay)) {
      throw dailyCapError(c, 'ranked');
    }
    c.db.run(`DELETE FROM queue WHERE pid = ?`, p.pid);
    c.db.run(
      `INSERT INTO queue (pid, region, rating, enqueued_ms, last_poll_ms, status, ticket_id, ticket_hash) VALUES (?, ?, ?, ?, ?, 'queued', ?, ?)`,
      p.pid, p.region, p.rating, c.now, c.now, ticketId, ticketHash,
    );
    // the ticket secret is shown once, here; it lets the same player resume this exact queue entry after a dropped link
    return { ...pollQueue(c, p, true), ticket: { id: ticketId, token: ticketToken } };
  });
}

/** Resume a queue entry after the client lost its connection (app restart, suspend, Wi-Fi blip). */
export async function resumeQueue(c, p, body) {
  strict(body, ['ticketId', 'ticketToken']);
  if (typeof body.ticketId !== 'string' || typeof body.ticketToken !== 'string') throw new ApiError(400, 'bad_request', 'ticketId and ticketToken required');
  const hash = await sha256hex(body.ticketToken);
  return c.tx(() => {
    dropStale(c);
    const row = c.db.get(`SELECT * FROM queue WHERE pid = ?`, p.pid);
    if (!row || row.ticket_id !== body.ticketId || row.ticket_hash !== hash) {
      throw new ApiError(410, 'ticket_expired', 'that queue ticket is gone; join the queue again');
    }
    return pollQueue(c, p, true);
  });
}

export function pollQueue(c, p, inner = false) {
  const run = () => {
    if (!inner) dropStale(c);
    const row = c.db.get(`SELECT * FROM queue WHERE pid = ?`, p.pid);
    if (!row) return { status: 'none' };
    if (row.status === 'queued') {
      if (c.now - row.enqueued_ms > c.cfg.queue.maxWaitMs) {
        c.db.run(`DELETE FROM queue WHERE pid = ?`, p.pid);
        return { status: 'timeout' };
      }
      c.db.run(`UPDATE queue SET last_poll_ms = ? WHERE pid = ?`, c.now, p.pid);
      tryPair(c, { ...row, last_poll_ms: c.now });
    }
    const cur = c.db.get(`SELECT * FROM queue WHERE pid = ?`, p.pid);
    if (cur.status === 'matched') {
      const room = loadRoom(c, cur.room_code); // may discover that the pairing was not confirmed in time
      const after = c.db.get(`SELECT * FROM queue WHERE pid = ?`, p.pid);
      if (!after) return { status: 'none', reason: 'ack_timeout' };
      if (after.status === 'queued') return queueView(c, after, { requeued: true });
      touch(c, room, p.pid); // receiving the pairing is the join acknowledgement
      return queueView(c, after, { room: roomView(c, c.db.get(`SELECT * FROM rooms WHERE code = ?`, room.code), p.pid) });
    }
    return queueView(c, cur);
  };
  return inner ? run() : c.tx(run);
}

export function leaveQueue(c, p) {
  c.db.run(`DELETE FROM queue WHERE pid = ?`, p.pid);
  return { ok: true };
}

export function sweepQueue(c) {
  dropStale(c);
}
