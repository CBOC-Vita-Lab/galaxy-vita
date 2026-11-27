// Invite rooms: signaling for direct (hole-punched) play. A room links exactly two players (GAME=msc; Galaxy's rooms of up to
// four are rooms4.js, which reuses the helpers exported here).
// The ranked matchmaker creates rooms of kind "ranked" through createPairedRoom(); everything after
// that (endpoint exchange, started, results) is identical to unranked.

import { ApiError, b64encode, b64url, randomBytes, randomInt, parseEndpoint, b64decode, dottedIp, strict, noBody } from './util.js';
import { bumpDaily, requireDaily, dailyCapError, atDailyCap } from './limits.js';

export const CODE_ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789'; // no I L O 0 1
const CODE_LEN = 6;
export const LIVE = ['open', 'joined', 'ready', 'started'];

export function normalizeCode(raw, len = CODE_LEN) {
  if (typeof raw !== 'string') throw new ApiError(400, 'bad_code', 'code must be a string');
  const code = raw.toUpperCase().replace(/[\s-]/g, '');
  if (code.length !== len || ![...code].every((ch) => CODE_ALPHABET.includes(ch))) {
    throw new ApiError(400, 'bad_code', `codes are ${len} characters from ${CODE_ALPHABET}`);
  }
  return code;
}

export function newCode(c) {
  for (let attempt = 0; attempt < 20; attempt++) {
    let s = '';
    for (let i = 0; i < CODE_LEN; i++) s += CODE_ALPHABET[randomInt(CODE_ALPHABET.length)];
    if (!c.db.get(`SELECT 1 AS x FROM rooms WHERE code = ?`, s)) return s;
  }
  throw new ApiError(503, 'busy', 'could not allocate a room code, try again');
}

/** Virtual IPs: 10.(64..127).x.1 for the host, .2 for the guest. Unique among live rooms. */
function allocVips(c) {
  const seqRow = c.db.get(`SELECT value FROM settings WHERE key = 'vip_seq'`);
  let seq = seqRow ? Number(seqRow.value) : randomInt(16384);
  for (let tries = 0; tries < 16384; tries++) {
    seq = (seq + 1) & 0x3fff;
    const base = ((10 << 24) | ((64 + (seq >> 8)) << 16) | ((seq & 255) << 8)) >>> 0;
    const host = (base | 1) >>> 0;
    const taken = c.db.get(
      `SELECT 1 AS x FROM rooms WHERE vip_host = ? AND state IN ('joined','ready','started')`, host,
    );
    if (!taken) {
      c.db.run(`INSERT INTO settings (key, value) VALUES ('vip_seq', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, String(seq));
      return { host, guest: (base | 2) >>> 0 };
    }
  }
  throw new ApiError(503, 'busy', 'no virtual addresses free');
}

export function newMatchId() {
  return b64url(randomBytes(9));
}

function insertMatch(c, code, kind, p0, p1) {
  const id = newMatchId();
  c.db.run(
    `INSERT INTO matches (match_id, room_code, kind, p0, p1, created_ms, status) VALUES (?, ?, ?, ?, ?, ?, 'pending')`,
    id, code, kind, p0, p1, c.now,
  );
  bumpDaily(c, p0, 'matches');
  bumpDaily(c, p1, 'matches');
  return id;
}

function bump(c, code, extra = '', ...binds) {
  c.db.run(`UPDATE rooms SET version = version + 1${extra} WHERE code = ?`, ...binds, code);
}

export function closeRoom(c, r, reason, state = 'closed') {
  c.db.run(
    `UPDATE rooms SET state = ?, closed_reason = ?, expires_ms = ?, version = version + 1 WHERE code = ?`,
    state, reason, c.now, r.code,
  );
  if (r.match_id) {
    c.db.run(`UPDATE matches SET status = 'void', resolved_ms = ? WHERE match_id = ? AND status = 'pending'`, c.now, r.match_id);
  }
}

/** Lazily applies expiry and the ranked join-ack deadline; returns the fresh row. */
export function loadRoom(c, code) {
  let r = c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
  if (!r) return r;
  if (LIVE.includes(r.state) && c.now > r.expires_ms) {
    closeRoom(c, r, 'expired', 'expired');
    return c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
  }
  if (r.kind === 'ranked' && r.state === 'joined' && !(r.host_ack && r.join_ack) && c.now > r.created_ms + c.cfg.rooms.ackMs) {
    ackTimeout(c, r);
    r = c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
  }
  return r;
}

/**
 * A ranked pairing nobody confirmed in time (ackMs): the room ends, the match is void, the player who did confirm
 * goes back into the queue with their original waiting time, the one who did not is dropped from it.
 */
function ackTimeout(c, r) {
  closeRoom(c, r, 'ack_timeout');
  const members = [[r.host_pid, r.host_ack], [r.join_pid, r.join_ack]];
  for (const [pid, acked] of members) {
    if (acked) c.db.run(`UPDATE queue SET status = 'queued', room_code = NULL, last_poll_ms = ? WHERE pid = ?`, c.now, pid);
    else c.db.run(`DELETE FROM queue WHERE pid = ?`, pid);
  }
}

/** Any call by a member proves they are present; the first one also counts as the join acknowledgement. */
export function touch(c, r, pid) {
  const slot = slotOf(r, pid);
  if (slot < 0) return;
  c.presence.set(`${r.code}:${slot}`, c.now);
  const col = slot === 0 ? 'host_ack' : 'join_ack';
  if (!r[col]) c.db.run(`UPDATE rooms SET ${col} = 1 WHERE code = ?`, r.code);
}

/** How long a member's report "I exchange direct heartbeats with slot N" counts as evidence that N is alive (svcfix2). */
export const DIRECT_TTL_MS = 150_000;

/**
 * When the service last had a sign of life of `slot`: its own call (presence) or, in a STARTED room, another member's recent report
 * that it exchanges direct heartbeats with it (`c.direct`, memory only; see rooms4.noteDirect). Once every peer of a started room is
 * linked directly the clients stop polling except once a minute (the host) / every few minutes (guests): the peers' own heartbeats are
 * the proof of life, the service just needs to hear about them.
 */
export function seenAt(c, r, slot) {
  const own = c.presence.get(`${r.code}:${slot}`);
  if (r.state !== 'started' || !c.direct || !c.direct.size) return own;
  let best = own;
  for (let o = 0; o < 4; o++) {
    if (o === slot) continue;
    const rep = c.direct.get(`${r.code}:${o}`);
    if (rep && c.now - rep.at < DIRECT_TTL_MS && ((rep.mask >> slot) & 1) && (best === undefined || rep.at > best)) best = rep.at;
  }
  return best;
}

export function linkState(c, r, slot) {
  const seen = seenAt(c, r, slot);
  if (seen === undefined) return 'ok';
  const silent = c.now - seen;
  return silent >= c.cfg.rooms.lostMs ? 'lost' : silent >= c.cfg.rooms.quietMs ? 'quiet' : 'ok';
}

export function pairKey(a, b) {
  return a < b ? [a, b] : [b, a];
}

export function pairFailures(c, a, b) {
  const [lo, hi] = pairKey(a, b);
  const row = c.db.get(`SELECT fails, oks, updated_ms FROM pair_stats WHERE lo_pid = ? AND hi_pid = ?`, lo, hi);
  if (!row || c.now - row.updated_ms > c.cfg.rooms.pairFailKeepMs) return 0;
  return row.fails - row.oks;
}

/** Should these two skip hole punching and go straight to the relay? */
function steerFor(c, r) {
  if (r.join_pid == null) return { relay: false, reason: null };
  const hp = c.db.get(`SELECT relay_only FROM profiles WHERE pid = ?`, r.host_pid);
  const jp = c.db.get(`SELECT relay_only FROM profiles WHERE pid = ?`, r.join_pid);
  if ((hp && hp.relay_only) || (jp && jp.relay_only)) return { relay: true, reason: 'relay_only' };
  if (r.host_nat === 'symmetric' || r.join_nat === 'symmetric') return { relay: true, reason: 'nat' };
  if (pairFailures(c, r.host_pid, r.join_pid) >= c.cfg.rooms.pairFailLimit) return { relay: true, reason: 'pair_failures' };
  return { relay: false, reason: null };
}

export function nameOf(c, pid) {
  const p = c.db.get(`SELECT name FROM profiles WHERE pid = ?`, pid);
  return p ? p.name : null;
}

export function slotOf(r, pid) {
  if (r.host_pid === pid) return 0;
  if (r.join_pid === pid) return 1;
  return -1;
}

export function relayOnlyPid(c, pid) {
  const p = c.db.get(`SELECT relay_only FROM profiles WHERE pid = ?`, pid);
  return !!(p && p.relay_only);
}

export function roomView(c, r, pid) {
  const slot = slotOf(r, pid);
  const mine = slot === 0 ? 'host' : 'join';
  const theirs = slot === 0 ? 'join' : 'host';
  const peerPid = slot === 0 ? r.join_pid : r.host_pid;
  const paired = r.join_pid != null;
  const view = {
    code: r.code,
    kind: r.kind,
    state: r.state,
    version: r.version,
    role: slot === 0 ? 'host' : 'guest',
    slot,
    createdMs: r.created_ms,
    expiresMs: r.expires_ms,
    closedReason: r.closed_reason || null,
    matchId: r.match_id || null,
    you: { pid, name: nameOf(c, pid), vip: paired ? dottedIp(slot === 0 ? r.vip_host : r.vip_join) : null },
    peer: null,
    session: null,
    started: { you: !!r[`${mine}_started`], peer: !!r[`${theirs}_started`] },
  };
  if (paired && peerPid != null) {
    const bothPosted = r.host_ep_ms && r.join_ep_ms;
    let endpoint = null;
    if (bothPosted) {
      // Direct addresses are only shared between two players who both allow it: a relay-only player never
      // receives, and never exposes, a raw address. Relay addresses (TURN allocations) are always shared.
      const direct = !relayOnlyPid(c, pid) && !relayOnlyPid(c, peerPid);
      const age = c.now - r[`${theirs}_ep_ms`];
      endpoint = {
        ...(direct ? { public: r[`${theirs}_public`], local: r[`${theirs}_local`], candidates: r[`${theirs}_cands`] ? JSON.parse(r[`${theirs}_cands`]) : [] } : {}),
        relay: r[`${theirs}_relay`] || null,
        nonce: r[`${theirs}_nonce`],
        nat: r[`${theirs}_nat`] || null,
        ageMs: age,
        // NAT mappings die after ~30 s without traffic: an older address must be refreshed (STUN again, re-post)
        stale: direct && age > c.cfg.rooms.natLifeMs && !r[`${theirs}_punch`],
      };
    }
    view.peer = {
      pid: peerPid,
      name: nameOf(c, peerPid),
      slot: 1 - slot,
      vip: dottedIp(slot === 0 ? r.vip_join : r.vip_host),
      link: linkState(c, r, 1 - slot),
      endpoint,
    };
    if (r.state !== 'closed' && r.state !== 'expired') {
      view.session = {
        key: r.key_b64,
        cookie: r.cookie,
        punchAtMs: bothPosted ? r.punch_at_ms : null,
        punch: c.cfg.rooms.punch,
        stun: c.cfg.stunServers,
        natLifeSec: c.cfg.rooms.natLifeMs / 1000,
        steer: steerFor(c, r),
      };
    }
  }
  return view;
}

/** Close the player's other unstarted rooms so a player is never in two lobbies. */
function leaveOthers(c, pid, exceptCode) {
  const rows = c.db.all(
    `SELECT * FROM rooms WHERE (host_pid = ? OR join_pid = ?) AND code <> ? AND state IN ('open','joined','ready')`,
    pid, pid, exceptCode || '',
  );
  for (const r of rows) {
    if (r.kind === 'ranked') continue; // ranked rooms end through their own rules
    if (r.host_pid === pid) closeRoom(c, r, 'replaced');
    else resetGuest(c, r);
  }
}

function resetGuest(c, r) {
  c.db.run(
    `UPDATE rooms SET state = 'open', join_pid = NULL, key_b64 = NULL, cookie = NULL, vip_host = NULL, vip_join = NULL,
       join_public = NULL, join_local = NULL, join_nonce = NULL, join_nat = NULL, join_ep_ms = NULL, join_relay = NULL, join_cands = NULL,
       join_ack = 0, join_punch = NULL, host_punch = NULL, punch_at_ms = NULL,
       host_started = 0, join_started = 0, expires_ms = ?, version = version + 1
     WHERE code = ?`,
    c.now + c.cfg.rooms.openTtlMs, r.code,
  );
  if (r.match_id) {
    c.db.run(`DELETE FROM matches WHERE match_id = ? AND status = 'pending'`, r.match_id);
    c.db.run(`UPDATE rooms SET match_id = NULL WHERE code = ?`, r.code);
  }
}

export function createRoom(c, p) {
  return c.tx(() => {
    const live = c.db.get(
      `SELECT code FROM rooms WHERE kind = 'ranked' AND (host_pid = ? OR join_pid = ?) AND state IN ('joined','ready','started')`,
      p.pid, p.pid,
    );
    if (live) throw new ApiError(409, 'in_ranked_match', 'finish or leave your ranked match first');
    // daily caps (stored): every room and match record costs rows on the free tier
    requireDaily(c, p.pid, 'rooms', c.cfg.caps.roomsPerAccountDay, 'rooms');
    requireDaily(c, p.pid, 'matches', c.cfg.caps.matchesPerAccountDay, 'matches');
    leaveOthers(c, p.pid, null);
    const code = newCode(c);
    c.db.run(
      `INSERT INTO rooms (code, kind, state, host_pid, created_ms, expires_ms) VALUES (?, 'unranked', 'open', ?, ?, ?)`,
      code, p.pid, c.now, c.now + c.cfg.rooms.openTtlMs,
    );
    bumpDaily(c, p.pid, 'rooms');
    return roomView(c, loadRoom(c, code), p.pid);
  });
}

export function joinRoom(c, p, body) {
  strict(body, ['code']);
  const code = normalizeCode(body.code);
  return c.tx(() => {
    const r = loadRoom(c, code);
    if (!r || r.kind !== 'unranked') throw new ApiError(404, 'room_not_found', 'no such room');
    if (r.host_pid === p.pid) throw new ApiError(409, 'own_room', 'that is your own room');
    if (r.join_pid === p.pid) return roomView(c, r, p.pid); // idempotent
    if (r.state === 'expired') throw new ApiError(410, 'room_expired', 'that room has expired');
    if (r.state === 'closed') throw new ApiError(410, 'room_closed', 'that room is closed');
    if (r.join_pid != null) throw new ApiError(409, 'room_full', 'that room is full');
    const live = c.db.get(
      `SELECT code FROM rooms WHERE kind = 'ranked' AND (host_pid = ? OR join_pid = ?) AND state IN ('joined','ready','started')`,
      p.pid, p.pid,
    );
    if (live) throw new ApiError(409, 'in_ranked_match', 'finish or leave your ranked match first');
    requireDaily(c, p.pid, 'matches', c.cfg.caps.matchesPerAccountDay, 'matches');
    if (atDailyCap(c, r.host_pid, 'matches', c.cfg.caps.matchesPerAccountDay)) throw dailyCapError(c, 'host_matches');
    leaveOthers(c, p.pid, code);
    const vips = allocVips(c);
    const matchId = insertMatch(c, code, 'unranked', r.host_pid, p.pid);
    c.db.run(
      `UPDATE rooms SET join_ack = 1, state = 'joined', join_pid = ?, key_b64 = ?, cookie = ?, vip_host = ?, vip_join = ?,
         expires_ms = ?, match_id = ?, version = version + 1 WHERE code = ?`,
      p.pid, b64encode(randomBytes(32)), 1 + randomInt(0xfffffffe), vips.host, vips.guest,
      c.now + c.cfg.rooms.joinedTtlMs, matchId, code,
    );
    return roomView(c, loadRoom(c, code), p.pid);
  });
}

/** Used by the matchmaker: a room with both players already inside. Slot 0 is `a`. */
export function createPairedRoom(c, a, b) {
  const code = newCode(c);
  const vips = allocVips(c);
  const matchId = insertMatch(c, code, 'ranked', a, b);
  c.db.run(
    `INSERT INTO rooms (code, kind, state, host_pid, join_pid, created_ms, expires_ms, key_b64, cookie, vip_host, vip_join, match_id)
     VALUES (?, 'ranked', 'joined', ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    code, a, b, c.now, c.now + c.cfg.rooms.joinedTtlMs, b64encode(randomBytes(32)), 1 + randomInt(0xfffffffe),
    vips.host, vips.guest, matchId,
  );
  return code;
}

function memberRoom(c, p, rawCode) {
  const code = normalizeCode(rawCode);
  const r = loadRoom(c, code);
  if (!r) throw new ApiError(404, 'room_not_found', 'no such room');
  if (slotOf(r, p.pid) < 0) throw new ApiError(403, 'not_member', 'you are not in that room');
  touch(c, r, p.pid);
  return c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
}

export function getRoom(c, p, rawCode, sinceVersion) {
  return c.tx(() => {
    const r = memberRoom(c, p, rawCode);
    if (sinceVersion !== undefined && sinceVersion === r.version) {
      return { code: r.code, state: r.state, version: r.version, changed: false };
    }
    return { ...roomView(c, r, p.pid), changed: true };
  });
}

export const NAT_TYPES = ['unknown', 'open', 'cone', 'restricted', 'symmetric'];
export const CAND_TYPES = ['host', 'srflx', 'observed', 'relay'];

export function checkEndpoint(label, v, { allowPrivate = true } = {}) {
  const ep = parseEndpoint(v);
  const bad = (m) => new ApiError(400, 'bad_endpoint', `${label}: ${m}`, { field: label });
  if (!ep) throw bad('expected "a.b.c.d:port"');
  const [a, b] = ep.ip;
  if (a === 0 || a === 127 || a >= 224 || (a === 169 && b === 254)) throw bad('address not usable');
  if (!allowPrivate && (a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 100 && b >= 64 && b <= 127))) throw bad('private address');
  return v;
}

export function postEndpoint(c, p, rawCode, body) {
  strict(body, ['public', 'local', 'relay', 'nonce', 'nat', 'candidates']);
  const relayOnly = relayOnlyPid(c, p.pid);
  const nonce = b64decode(body.nonce);
  if (!nonce || nonce.length < 8 || nonce.length > 32) throw new ApiError(400, 'bad_nonce', 'nonce must be 8..32 bytes, base64', { field: 'nonce' });
  let pub = null;
  let local = null;
  let relay = null;
  let cands = [];
  if (body.relay !== undefined) relay = checkEndpoint('relay', body.relay, { allowPrivate: false });
  if (relayOnly) {
    // privacy: a relay-only account must not hand over any direct address, even by mistake
    if (body.public !== undefined || body.local !== undefined || body.candidates !== undefined) {
      throw new ApiError(400, 'relay_only_account', 'relay-only accounts send only the relayed address', { field: 'public' });
    }
    if (!relay) throw new ApiError(400, 'bad_endpoint', 'relay: required for relay-only accounts', { field: 'relay' });
  } else {
    // `public` is what STUN saw. CGNAT / double NAT can make it private-looking, so only reject the obviously unusable.
    pub = checkEndpoint('public', body.public);
    local = checkEndpoint('local', body.local);
    if (body.candidates !== undefined) {
      if (!Array.isArray(body.candidates) || body.candidates.length > 6) throw new ApiError(400, 'bad_endpoint', 'candidates: at most 6', { field: 'candidates' });
      cands = body.candidates.map((cd) => {
        strict(cd, ['type', 'addr']);
        if (!CAND_TYPES.includes(cd.type)) throw new ApiError(400, 'bad_endpoint', 'candidates: bad type', { field: 'candidates' });
        return { type: cd.type, addr: checkEndpoint('candidates', cd.addr) };
      });
    }
  }
  const nat = body.nat === undefined || body.nat === null ? null : body.nat;
  if (nat !== null && !NAT_TYPES.includes(nat)) throw new ApiError(400, 'bad_request', `nat must be one of ${NAT_TYPES.join(', ')}`, { field: 'nat' });
  return c.tx(() => {
    const r = memberRoom(c, p, rawCode);
    if (r.state === 'closed' || r.state === 'expired') throw new ApiError(410, `room_${r.state}`, `that room is ${r.state}`);
    const me = slotOf(r, p.pid) === 0 ? 'host' : 'join';
    c.db.run(
      `UPDATE rooms SET ${me}_public = ?, ${me}_local = ?, ${me}_nonce = ?, ${me}_nat = ?, ${me}_ep_ms = ?, ${me}_relay = ?, ${me}_cands = ?,
         ${me}_punch = NULL, version = version + 1 WHERE code = ?`,
      pub, local, body.nonce, nat, c.now, relay, cands.length ? JSON.stringify(cands) : null, r.code,
    );
    const r2 = c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code);
    if (r2.join_pid != null && r2.host_ep_ms && r2.join_ep_ms) {
      // Both sides know both addresses now: tell them when to start punching (server clock, ms).
      const punchAt = Math.max(r2.host_ep_ms, r2.join_ep_ms) + c.cfg.rooms.punchDelayMs;
      c.db.run(
        `UPDATE rooms SET punch_at_ms = ?, state = CASE WHEN state = 'joined' THEN 'ready' ELSE state END WHERE code = ?`,
        punchAt, r.code,
      );
    }
    return roomView(c, c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code), p.pid);
  });
}

/** A member reports how hole punching went; failures per pair steer later matches of the same two players to the relay. */
export function reportPunch(c, p, rawCode, body) {
  strict(body, ['result', 'path', 'ms']);
  if (body.result !== 'ok' && body.result !== 'fail') throw new ApiError(400, 'bad_request', 'result must be "ok" or "fail"', { field: 'result' });
  if (!['direct', 'lan', 'relay'].includes(body.path)) throw new ApiError(400, 'bad_request', 'path must be direct, lan or relay', { field: 'path' });
  if (body.ms !== undefined && !(Number.isInteger(body.ms) && body.ms >= 0 && body.ms <= 120000)) throw new ApiError(400, 'bad_request', 'ms out of range', { field: 'ms' });
  const failed = body.result === 'fail' || body.path === 'relay';
  return c.tx(() => {
    const r = memberRoom(c, p, rawCode);
    if (r.join_pid == null) throw new ApiError(409, 'room_not_ready', 'no partner yet');
    const slot = slotOf(r, p.pid);
    const me = slot === 0 ? 'host' : 'join';
    const other = slot === 0 ? 'join' : 'host';
    const verdict = failed ? 'fail' : 'ok';
    const prevMine = r[`${me}_punch`];
    const prevOther = r[`${other}_punch`];
    // one outcome per room and pair: "fail" from either side wins over "ok"
    const before = prevMine || prevOther ? (prevMine === 'fail' || prevOther === 'fail' ? 'fail' : 'ok') : null;
    const after = verdict === 'fail' || prevOther === 'fail' ? 'fail' : 'ok';
    c.db.run(`UPDATE rooms SET ${me}_punch = ?, version = version + 1 WHERE code = ?`, verdict, r.code);
    if (before !== after) {
      const [lo, hi] = pairKey(r.host_pid, r.join_pid);
      c.db.run(`INSERT INTO pair_stats (lo_pid, hi_pid, updated_ms) VALUES (?, ?, ?) ON CONFLICT(lo_pid, hi_pid) DO NOTHING`, lo, hi, c.now);
      const dFail = (after === 'fail' ? 1 : 0) - (before === 'fail' ? 1 : 0);
      const dOk = (after === 'ok' ? 1 : 0) - (before === 'ok' ? 1 : 0);
      c.db.run(`UPDATE pair_stats SET fails = fails + ?, oks = oks + ?, updated_ms = ? WHERE lo_pid = ? AND hi_pid = ?`, dFail, dOk, c.now, lo, hi);
    }
    return roomView(c, c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code), p.pid);
  });
}

/** Both sides call this once their direct (or relayed) link is confirmed and the match is about to begin. */
export function markStarted(c, p, rawCode) {
  return c.tx(() => {
    const r = memberRoom(c, p, rawCode);
    if (r.state === 'closed' || r.state === 'expired') throw new ApiError(410, `room_${r.state}`, `that room is ${r.state}`);
    if (r.state === 'open' || r.state === 'joined') throw new ApiError(409, 'room_not_ready', 'exchange endpoints first');
    const me = slotOf(r, p.pid) === 0 ? 'host' : 'join';
    c.db.run(`UPDATE rooms SET ${me}_started = 1, version = version + 1 WHERE code = ?`, r.code);
    const r2 = c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code);
    if (r2.host_started && r2.join_started && r2.state !== 'started') {
      c.db.run(
        `UPDATE rooms SET state = 'started', expires_ms = ?, version = version + 1 WHERE code = ?`,
        c.now + c.cfg.rooms.startedTtlMs, r.code,
      );
      c.db.run(`UPDATE matches SET status = 'started', started_ms = ? WHERE match_id = ? AND status = 'pending'`, c.now, r2.match_id);
      // played at all (unranked too): an account that ever started a match is never pruned (accounts.pruneIdle)
      c.db.run(`UPDATE profiles SET matches_played = matches_played + 1 WHERE pid IN (?, ?)`, r2.host_pid, r2.join_pid);
    }
    return roomView(c, c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code), p.pid);
  });
}

/** Host: closes the room. Guest: leaves it (it reopens for another guest, unless the match already started). */
export function leaveRoom(c, p, rawCode) {
  return c.tx(() => {
    const r = memberRoom(c, p, rawCode);
    if (r.state === 'closed' || r.state === 'expired') return { ok: true, state: r.state };
    if (r.kind === 'ranked') c.db.run(`DELETE FROM queue WHERE room_code = ?`, r.code);
    if (r.kind === 'ranked' || r.state === 'started' || r.host_pid === p.pid) {
      // A ranked or started room ends for both; the result rules decide who gets what.
      closeRoom(c, r, r.host_pid === p.pid ? 'host_left' : 'guest_left');
      if (r.match_id && r.state !== 'started' && r.kind === 'ranked') {
        c.db.run(`UPDATE matches SET status = 'void', resolved_ms = ? WHERE match_id = ? AND status = 'pending'`, c.now, r.match_id);
      }
    } else {
      resetGuest(c, r);
    }
    return { ok: true };
  });
}

export function isLive(r) {
  return LIVE.includes(r.state);
}
