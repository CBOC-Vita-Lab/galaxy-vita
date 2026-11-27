// Galaxy (GAME=smg) rooms: up to 4 members, star topology through the host. Same signaling ideas as rooms.js (endpoint
// exchange, punch time, steer to the relay, presence by polling), but the members live in `room_members` (one row each, the host
// included) and every pair is (host, guest): a guest sees only the host's endpoint, the host sees each guest's. The view keeps the
// Strikers fields (`you`, `peer`, `session`, `started`) so a 2-member room still reads like a Strikers room, and adds `members`.
//
// Host migration (DESIGN.md section 8): slots never change. The host starts in slot 0 (`host_slot`); when the host leaves, or its
// link is lost (no call for LINK_LOST_SEC = 15 s), the lowest remaining slot becomes host, `host_epoch` + 1, and every guest's pair
// with the new host starts over (verdicts cleared, a fresh punchAtMs). The room closes only when its last member leaves. A player
// joining a room again within 15 s of leaving it takes its old slot back (when still free).
// SMG-PROTOCOL.txt documents every field.

import { ApiError, b64encode, randomBytes, randomInt, b64decode, dottedIp, strict, isInt } from './util.js';
import { bumpDaily, requireDaily } from './limits.js';
import { fmtCode } from './accounts.js';
import {
  CODE_ALPHABET, LIVE, normalizeCode, loadRoom, newCode, closeRoom, linkState, seenAt, pairKey, pairFailures, relayOnlyPid,
  checkEndpoint, NAT_TYPES, CAND_TYPES,
} from './rooms.js';

export const MODES = ['coop', 'race'];
export const VISIBILITY = ['code', 'friends', 'public'];
export const SETTINGS_MAX_BYTES = 512;
// Host-chosen session settings the service keeps for lists and quick match (the session itself carries MpSettings). Integers in
// [min, max] or booleans; nothing else is accepted.
const SETTING_RULES = {
  interaction: [0, 2], sharedKills: 'bool', rivals: [0, 2], raceWait: [0, 255], nameTags: [0, 2],
  dropIn: 'bool', sharedPurple: 'bool', sharedChips: 'bool', galaxy: [0, 63], scenario: [0, 8],
};
const DEFAULT_SETTINGS = { dropIn: true };

// ------------------------------------------------------------------ small helpers

export function checkSettings(raw) {
  if (raw === undefined) return { ...DEFAULT_SETTINGS };
  if (typeof raw !== 'object' || raw === null || Array.isArray(raw)) throw new ApiError(400, 'bad_settings', 'settings must be an object', { field: 'settings' });
  const out = {};
  for (const [k, v] of Object.entries(raw)) {
    const rule = SETTING_RULES[k];
    if (!rule) throw new ApiError(400, 'unknown_field', 'unexpected field', { field: `settings.${k.slice(0, 24)}` });
    if (rule === 'bool' ? typeof v !== 'boolean' : !isInt(v, rule[0], rule[1])) {
      throw new ApiError(400, 'bad_settings', `settings.${k} out of range`, { field: `settings.${k}` });
    }
    out[k] = v;
  }
  if (out.dropIn === undefined) out.dropIn = true;
  if (JSON.stringify(out).length > SETTINGS_MAX_BYTES) throw new ApiError(400, 'bad_settings', 'settings too large', { field: 'settings' });
  return out;
}

function parseSettings(json) {
  try {
    const o = JSON.parse(json || '{}');
    return o && typeof o === 'object' && !Array.isArray(o) ? o : {};
  } catch {
    return {};
  }
}

export function members(c, code) {
  return c.db.all(`SELECT * FROM room_members WHERE code = ? ORDER BY slot`, code);
}

/** The slot of `pid` in an smg room, or -1. */
export function memberSlot(c, code, pid) {
  const m = c.db.get(`SELECT slot FROM room_members WHERE code = ? AND pid = ?`, code, pid);
  return m ? m.slot : -1;
}

const isSmg = (r) => r && r.game === 'smg';

/** Version tag rooms are matched on: X-Msc-App, plus "+<build>" when the client sends X-Msc-Build. */
export function versionTag(rq) {
  return `${rq.app || ''}${rq.build ? `+${rq.build.toLowerCase()}` : ''}`;
}

// The punch verdicts of one (host, guest) pair live in the guest's row: "H/G", each part '-' | 'ok' | 'fail'.
function parsePunch(s) {
  const [h = '-', g = '-'] = String(s || '-/-').split('/');
  return { h, g };
}
const pairVerdict = ({ h, g }) => (h === '-' && g === '-' ? null : h === 'fail' || g === 'fail' ? 'fail' : 'ok');

/** Should this (host, guest) pair skip hole punching and use the relay? */
function steerPair(c, host, guest) {
  if (!host || !guest) return { relay: false, reason: null };
  if (relayOnlyPid(c, host.pid) || relayOnlyPid(c, guest.pid)) return { relay: true, reason: 'relay_only' };
  // an endpoint claim that did not match the address the member calls from (see postEndpoint): relay only, no addresses exchanged
  if (host.forced_relay || guest.forced_relay) return { relay: true, reason: 'endpoint_unverified' };
  if (host.nat === 'symmetric' || guest.nat === 'symmetric') return { relay: true, reason: 'nat' };
  if (pairFailures(c, host.pid, guest.pid) >= c.cfg.rooms.pairFailLimit) return { relay: true, reason: 'pair_failures' };
  return { relay: false, reason: null };
}

// both of a pair posted: punch at the later post (+ PUNCH_DELAY_MS); after a host change, never before the change
const punchAt = (c, r, a, b) => (a && b && a.ep_ms && b.ep_ms ? Math.max(a.ep_ms, b.ep_ms, r.host_since_ms || 0) + c.cfg.rooms.punchDelayMs : null);

/**
 * `other`'s endpoint as `viewer` may see it (Strikers shape), or null until both of the pair posted.
 * Addresses are handed out sparingly:
 *  - never to or from a member whose claim did not match its source address (forced_relay) or a relay-only account;
 *  - the LAN (`local`, and candidates of type "host") only when both members call from the same public address (behind one router);
 *  - in a PUBLIC room a guest gets the host's address only after the host has called since the guest joined (the host's console has seen
 *    the new member: a stranger who joins a public room and walks away from a dead or abandoned room learns nothing).
 */
function endpointFor(c, r, viewer, other, guestRow) {
  if (!viewer.ep_ms || !other.ep_ms) return null;
  let direct = !relayOnlyPid(c, viewer.pid) && !relayOnlyPid(c, other.pid) && !viewer.forced_relay && !other.forced_relay;
  // svcfix2 item 8: nobody learns an address of, or gives his to, a member the host has not accepted (a stranger who guessed a code)
  if (!viewer.accepted || !other.accepted) direct = false;
  if (direct && r.visibility === 'public' && viewer.slot !== r.host_slot && other.slot === r.host_slot) {
    const seen = c.presence.get(`${r.code}:${other.slot}`);
    if (seen !== undefined && seen < viewer.joined_ms) direct = false;
  }
  const sameRouter = viewer.src_ip === other.src_ip;
  const age = c.now - other.ep_ms;
  let cands = other.cands ? JSON.parse(other.cands) : [];
  if (!sameRouter) cands = cands.filter((cd) => cd.type !== 'host');
  return {
    ...(direct ? { public: other.public, local: sameRouter ? other.local : null, candidates: cands } : {}),
    relay: other.relay || null,
    nonce: other.nonce,
    nat: other.nat || null,
    ageMs: age,
    stale: direct && age > c.cfg.rooms.natLifeMs && pairVerdict(parsePunch(guestRow.punch)) === null,
  };
}

const NO_STEER = () => ({ relay: false, reason: null });
export const REJOIN_MS = 15_000;

/** Recompute open / joined / ready of an unstarted live room from its members. */
function settleState(c, code) {
  const r = c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
  if (!r || !LIVE.includes(r.state) || r.state === 'started') return;
  const ms = members(c, code);
  const state = ms.length < 2 ? 'open' : ms.every((m) => m.ep_ms) ? 'ready' : 'joined';
  if (state === r.state) return;
  const ttl = state === 'open' ? c.cfg.rooms.openTtlMs : c.cfg.rooms.joinedTtlMs;
  const grow = (r.state === 'open') !== (state === 'open'); // the TTL class changes only between "alone" and "with guests"
  c.db.run(
    `UPDATE rooms SET state = ?, expires_ms = CASE WHEN ? THEN ? ELSE expires_ms END, version = version + 1 WHERE code = ?`,
    state, grow ? 1 : 0, c.now + ttl, code,
  );
}

function bump(c, code) {
  c.db.run(`UPDATE rooms SET version = version + 1 WHERE code = ?`, code);
}

/** Virtual IPs: base 10.(64..127).x.0, member = base | (slot + 1). Unique among live rooms. */
function allocVipBase(c) {
  const seqRow = c.db.get(`SELECT value FROM settings WHERE key = 'vip_seq'`);
  let seq = seqRow ? Number(seqRow.value) : randomInt(16384);
  for (let tries = 0; tries < 16384; tries++) {
    seq = (seq + 1) & 0x3fff;
    const base = ((10 << 24) | ((64 + (seq >> 8)) << 16) | ((seq & 255) << 8)) >>> 0;
    const taken = c.db.get(`SELECT 1 AS x FROM rooms WHERE vip_host = ? AND state IN ('open','joined','ready','started')`, (base | 1) >>> 0);
    if (!taken) {
      c.db.run(`INSERT INTO settings (key, value) VALUES ('vip_seq', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, String(seq));
      return base;
    }
  }
  throw new ApiError(503, 'busy', 'no virtual addresses free');
}

/** Any call by a member proves presence (link) and acknowledges the join. */
function touch(c, code, m) {
  c.presence.set(`${code}:${m.slot}`, c.now);
  if (!m.ack) c.db.run(`UPDATE room_members SET ack = 1 WHERE code = ? AND slot = ?`, code, m.slot);
}

/** S2: a member's poll is being held: it is still here. Stamps presence only (no state change, no ack, no host check). */
export function holdPresence(c, pid, rawCode) {
  const code = normalizeCode(rawCode);
  const m = code && c.db.get(`SELECT slot FROM room_members WHERE code = ? AND pid = ?`, code, pid);
  if (m) c.presence.set(`${code}:${m.slot}`, c.now);
}

// ------------------------------------------------------------------ view

export function roomView(c, r, pid) {
  const ms = members(c, r.code);
  const me = ms.find((m) => m.pid === pid) || null;
  const slot = me ? me.slot : -1;
  const hs = r.host_slot;
  const host = ms.find((m) => m.slot === hs) || null;
  const live = r.state !== 'closed' && r.state !== 'expired';
  const rows = c.db.all(
    `SELECT pid, name, name_key, code FROM profiles WHERE pid IN (SELECT pid FROM room_members WHERE code = ?)`, r.code,
  );
  const names = new Map(rows.map((p) => [p.pid, p.name]));
  // the member's public player code (friend code: safe to show, grants nothing): what "Add Friend" / "Block" on a player in the room
  // use. Only members see a room view (403 not_member), so only co-members of a room joined through the service learn it.
  const codes = new Map(rows.map((p) => [p.pid, p.code ? fmtCode(p.code) : null]));
  // star: the pairs are (host, guest). For the viewer: host -> every guest; guest -> the host only.
  const pairWith = (m) => !!me && m.slot !== slot && (slot === hs || m.slot === hs);
  const guestOf = (m) => (slot === hs ? m : me); // the guest row of the viewer's pair with m
  // svcfix3 (review S10): display names are not unique. A member whose name (case and accents folded) another member of the room also
  // has gets `nameDup: true` and a `label`: the name plus the last four characters of the friend code ("Mario #4DXT"), so two "Mario"
  // can be told apart in the lobby. `label` equals `name` when nobody shares it.
  const folded = new Map(rows.map((p) => [p.pid, String(p.name_key || p.name || '').replace(/#[^#]*$/, '')]));
  const dupOf = (m) => ms.some((o) => o.pid !== m.pid && folded.get(o.pid) === folded.get(m.pid));
  const labelOf = (m) => {
    const nm = names.get(m.pid) ?? null;
    const cd = codes.get(m.pid);
    return nm !== null && cd && dupOf(m) ? `${nm} #${cd.replace(/-/g, '').slice(-4)}` : nm;
  };
  const list = ms.map((m) => {
    const paired = pairWith(m);
    return {
      slot: m.slot,
      pid: m.pid,
      name: names.get(m.pid) ?? null,
      label: labelOf(m),
      nameDup: dupOf(m),
      code: codes.get(m.pid) ?? null,
      vip: dottedIp(m.vip),
      accepted: !!m.accepted,
      link: m.slot === slot ? 'ok' : linkState(c, r, m.slot),
      endpoint: paired && live ? endpointFor(c, r, me, m, guestOf(m)) : null,
      punchAtMs: paired && live ? punchAt(c, r, me, m) : null,
      steer: paired ? steerPair(c, host, guestOf(m)) : NO_STEER(),
    };
  });
  const view = {
    code: r.code,
    game: 'smg',
    kind: r.kind,
    mode: r.mode,
    visibility: r.visibility,
    topology: r.topology,
    maxMembers: r.max_members,
    settings: parseSettings(r.settings_json),
    state: r.state,
    version: r.version,
    role: slot === hs ? 'host' : 'guest',
    slot,
    hostSlot: hs,
    hostEpoch: r.host_epoch,
    createdMs: r.created_ms,
    expiresMs: r.expires_ms,
    closedReason: r.closed_reason || null,
    matchId: null,
    you: { pid, name: names.get(pid) ?? null, vip: me ? dottedIp(me.vip) : null },
    peer: null,
    members: list,
    session: null,
    started: { you: !!(me && me.started), peer: false, room: r.state === 'started' },
  };
  // Legacy (Strikers client): `peer` is the other member of a 2-member room; `session` carries the viewer's pair with the host.
  if (me && ms.length === 2) {
    const other = list.find((m) => m.slot !== slot);
    view.peer = { pid: other.pid, name: other.name, slot: other.slot, vip: other.vip, link: other.link, endpoint: other.endpoint };
  }
  if (me) {
    if (slot === hs) view.started.peer = ms.some((m) => m.slot !== hs && m.started);
    else view.started.peer = !!(host && host.started);
  }
  if (me && live && ms.length >= 2) {
    // the pair the legacy session fields describe: a guest's pair with the host; the host's only pair when there is one guest
    const pairEntry = slot === hs ? (ms.length === 2 ? list.find((m) => m.slot !== hs) : null) : list.find((m) => m.slot === hs);
    view.session = {
      key: r.key_b64,
      cookie: r.cookie,
      punchAtMs: pairEntry ? pairEntry.punchAtMs : null,
      punch: c.cfg.rooms.punch,
      stun: c.cfg.stunServers,
      natLifeSec: c.cfg.rooms.natLifeMs / 1000,
      steer: pairEntry ? pairEntry.steer : NO_STEER(),
    };
  }
  return view;
}

// ------------------------------------------------------------------ membership changes

/**
 * Leave every other live smg room (one session at a time). why 'moved': the player's account is now in another room, which a console
 * still polling the old one hears as account_elsewhere (two consoles sharing one account file), not not_member (which makes it rejoin).
 */
function leaveOthers(c, pid, exceptCode, why = 'moved') {
  const rows = c.db.all(
    `SELECT r.*, m.slot AS my_slot FROM room_members m JOIN rooms r ON r.code = m.code
      WHERE m.pid = ? AND r.code <> ? AND r.state IN ('open','joined','ready','started')`,
    pid, exceptCode || '',
  );
  for (const r of rows) memberLeaves(c, r, r.my_slot, why);
}

/**
 * A member is gone (left, replaced by another room, or its link was lost). Its row goes; the last member closes the room; the host's
 * leaving moves the host role to the lowest remaining slot (epoch + 1) and restarts every guest's pair with the new host.
 */
function memberLeaves(c, r, slot, why) {
  const gone = c.db.get(`SELECT pid, accepted FROM room_members WHERE code = ? AND slot = ?`, r.code, slot);
  if (!gone) return;
  c.db.run(`DELETE FROM room_members WHERE code = ? AND slot = ?`, r.code, slot);
  c.db.run(
    `INSERT INTO room_left (code, pid, slot, left_ms, reason, accepted) VALUES (?, ?, ?, ?, ?, ?)
     ON CONFLICT(code, pid) DO UPDATE SET slot = excluded.slot, left_ms = excluded.left_ms, reason = excluded.reason, accepted = excluded.accepted`,
    r.code, gone.pid, slot, c.now, why === 'moved' ? 'moved' : 'left', gone.accepted,
  );
  c.presence.delete(`${r.code}:${slot}`);
  if (c.direct) c.direct.delete(`${r.code}:${slot}`);
  const rest = members(c, r.code);
  if (!rest.length) {
    closeRoom(c, r, why === 'lost' ? 'lost' : 'last_left');
    return;
  }
  if (slot === r.host_slot) {
    const next = rest[0]; // lowest remaining slot
    c.db.run(
      `UPDATE rooms SET host_slot = ?, host_pid = ?, host_epoch = host_epoch + 1, host_since_ms = ? WHERE code = ?`,
      next.slot, next.pid, c.now, r.code,
    );
    // new pairs: nobody has punched to the new host yet
    c.db.run(`UPDATE room_members SET punch = NULL WHERE code = ?`, r.code);
  }
  bump(c, r.code);
  settleState(c, r.code);
}

/** A guest silent this long (a crashed console) stops holding its slot. Longer than the host's 15 s: a guest on a loading screen may poll late. */
export const GUEST_GONE_MS = 60_000;

/**
 * Presence lives in memory only. After a Durable Object restart (every deploy) nobody has a timestamp, and linkState() reads "no
 * timestamp" as "ok", so a dead host (or a crashed guest) would never be noticed. Give every member of a live room a fresh stamp
 * the first time it is checked: a member that really is gone then ages out normally, one that is alive overwrites it on its next call.
 */
function seedPresence(c, r, ms) {
  for (const m of ms) {
    const key = `${r.code}:${m.slot}`;
    if (!c.presence.has(key)) c.presence.set(key, c.now);
  }
}

/** The host stopped calling for LINK_LOST_SEC (or a guest for GUEST_GONE_MS): treat it as gone (called on members' calls and by the sweep). */
export function checkHostLost(c, r) {
  if (!isSmg(r) || !LIVE.includes(r.state)) return false;
  seedPresence(c, r, members(c, r.code));
  // crashed guests first, so they cannot be picked as the next host
  for (const m of members(c, r.code)) {
    if (m.slot === r.host_slot) continue;
    const seen = seenAt(c, r, m.slot);
    if (seen !== undefined && c.now - seen >= GUEST_GONE_MS) memberLeaves(c, r, m.slot, 'lost');
  }
  if (linkState(c, r, r.host_slot) !== 'lost') return false;
  const cur = c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code);
  memberLeaves(c, cur, cur.host_slot, 'lost');
  return true;
}

/** Live public rooms hosted from one source address (a cap against one address flooding the public list; skipped when the address is unknown). */
function publicRoomsFrom(c, ipk) {
  return c.db.get(
    `SELECT COUNT(*) AS n FROM rooms r JOIN room_members m ON m.code = r.code AND m.slot = r.host_slot
      WHERE r.game = 'smg' AND r.visibility = 'public' AND r.state IN ('open','joined','ready','started') AND m.src_ip = ?`,
    ipk,
  ).n;
}

function newRoom(c, p, { mode, maxMembers, visibility, settings }, ver, ipk) {
  requireDaily(c, p.pid, 'rooms', c.cfg.caps.roomsPerAccountDay, 'rooms');
  leaveOthers(c, p.pid, null);
  if (visibility === 'public' && ipk && ipk !== 'unknown' && publicRoomsFrom(c, ipk) >= c.cfg.smg.publicRoomsPerIp) {
    throw new ApiError(429, 'too_many_rooms', 'too many public rooms from this network right now', { retryAfterSec: 300 }, { 'retry-after': '300' });
  }
  const code = newCode(c);
  const base = allocVipBase(c);
  c.db.run(
    `INSERT INTO rooms (code, kind, state, host_pid, created_ms, expires_ms, key_b64, cookie, vip_host,
       game, max_members, mode, settings_json, visibility, topology, app)
     VALUES (?, 'room', 'open', ?, ?, ?, ?, ?, ?, 'smg', ?, ?, ?, ?, 'star', ?)`,
    code, p.pid, c.now, c.now + c.cfg.rooms.openTtlMs, b64encode(randomBytes(32)), 1 + randomInt(0xfffffffe), (base | 1) >>> 0,
    maxMembers, mode, JSON.stringify(settings), visibility, ver,
  );
  c.db.run(`INSERT INTO room_members (code, slot, pid, vip, ack, joined_ms, src_ip) VALUES (?, 0, ?, ?, 1, ?, ?)`, code, p.pid, (base | 1) >>> 0, c.now, ipk || null);
  c.db.run(`UPDATE rooms SET peak_members = 1 WHERE code = ?`, code);
  c.presence.set(`${code}:0`, c.now);
  bumpDaily(c, p.pid, 'rooms');
  return code;
}

function checkMode(v, field = 'mode') {
  if (!MODES.includes(v)) throw new ApiError(400, 'bad_mode', 'mode must be coop or race', { field });
  return v;
}

/** POST /v1/rooms {"mode","maxMembers","visibility","settings"} (all optional). */
export function createRoom(c, p, body, ver, ipk) {
  const b = body === undefined ? {} : strict(body, ['mode', 'maxMembers', 'visibility', 'settings']);
  const mode = b.mode === undefined ? 'coop' : checkMode(b.mode);
  if (b.maxMembers !== undefined && !isInt(b.maxMembers, 2, c.cfg.smg.maxMembers)) {
    throw new ApiError(400, 'bad_request', `maxMembers must be 2..${c.cfg.smg.maxMembers}`, { field: 'maxMembers' });
  }
  if (b.visibility !== undefined && !VISIBILITY.includes(b.visibility)) {
    throw new ApiError(400, 'bad_request', 'visibility must be code, friends or public', { field: 'visibility' });
  }
  const opts = { mode, maxMembers: b.maxMembers ?? c.cfg.smg.maxMembers, visibility: b.visibility ?? 'code', settings: checkSettings(b.settings) };
  return c.tx(() => roomView(c, loadRoom(c, newRoom(c, p, opts, ver, ipk)), p.pid));
}

/** Can `pid` (version `ver`) join this room now? Returns null or the ApiError that says why not. */
function joinError(c, r, ms, pid, ver) {
  if (!isSmg(r)) return new ApiError(404, 'room_not_found', 'no such room');
  if (r.state === 'expired') return new ApiError(410, 'room_expired', 'that room has expired');
  if (r.state === 'closed') return new ApiError(410, 'room_closed', 'that room is closed');
  if (r.app !== ver) return new ApiError(409, 'version_mismatch', 'that room plays a different version of the game');
  if (ms.length >= r.max_members) return new ApiError(409, 'room_full', 'that room is full');
  if (r.state === 'started' && parseSettings(r.settings_json).dropIn === false) return new ApiError(409, 'room_started', 'that game has started and does not take new players');
  return null;
}

function addMember(c, r, p, ipk) {
  const ms = members(c, r.code);
  const used = new Set(ms.map((m) => m.slot));
  // a quick rejoin takes the old slot back; otherwise the first free slot (slot 0 too, once its first owner is gone)
  const back = c.db.get(`SELECT slot, left_ms, accepted FROM room_left WHERE code = ? AND pid = ?`, r.code, p.pid);
  // svcfix2 item 8: who may see the host's address. Accepted at once: a public room (the host chose it), a mutual friend of the host, a
  // member the host accepted before (a rejoin). A stranger who came with a code waits for the host's console (acceptMember).
  let accepted = 1;
  if (r.visibility === 'code' && c.cfg.rooms.requireAccept && r.host_pid !== p.pid) {
    accepted = (back && back.accepted) || isMutual(c, r.host_pid, p.pid) ? 1 : 0;
  }
  let slot = 0;
  if (back && c.now - back.left_ms <= REJOIN_MS && !used.has(back.slot)) slot = back.slot;
  else while (used.has(slot)) slot++;
  c.db.run(`DELETE FROM room_left WHERE code = ? AND pid = ?`, r.code, p.pid);
  const base = (r.vip_host & ~0xff) >>> 0;
  c.db.run(
    `INSERT INTO room_members (code, slot, pid, vip, ack, joined_ms, src_ip, accepted) VALUES (?, ?, ?, ?, 1, ?, ?, ?)`,
    r.code, slot, p.pid, (base | (slot + 1)) >>> 0, c.now, ipk || null, accepted,
  );
  c.presence.set(`${r.code}:${slot}`, c.now);
  c.db.run(`UPDATE rooms SET peak_members = MAX(peak_members, (SELECT COUNT(*) FROM room_members WHERE code = ?)) WHERE code = ?`, r.code, r.code);
  bump(c, r.code);
  settleState(c, r.code);
  return slot;
}

/** Shared by join-by-code, join-by-friend and quick match. Runs inside a transaction. */
function joinLoaded(c, p, r, ver, ipk) {
  if (!isSmg(r)) throw new ApiError(404, 'room_not_found', 'no such room');
  if (LIVE.includes(r.state) && checkHostLost(c, r)) r = loadRoom(c, r.code); // a dead host / crashed guests must not block the join
  const ms = members(c, r.code);
  const mine = ms.find((m) => m.pid === p.pid);
  if (mine && mine.slot === r.host_slot) throw new ApiError(409, 'own_room', 'that is your own room');
  if (mine && LIVE.includes(r.state)) return roomView(c, r, p.pid); // idempotent
  const err = joinError(c, r, ms, p.pid, ver);
  if (err) throw err;
  leaveOthers(c, p.pid, r.code);
  addMember(c, r, p, ipk);
  return roomView(c, loadRoom(c, r.code), p.pid);
}

/** POST /v1/rooms/join {"code":"K7QM4D"} or {"friend":"XXXX-XXXX"} (a mutual friend's room, visibility friends or public). */
export function joinRoom(c, p, body, ver, ipk) {
  strict(body, ['code', 'friend']);
  if ((body.code === undefined) === (body.friend === undefined)) throw new ApiError(400, 'bad_request', 'give either code or friend');
  if (body.code !== undefined) {
    const code = normalizeCode(body.code);
    return c.tx(() => {
      const r = loadRoom(c, code);
      // svcfix3 (review S10): a join by code respects the room's visibility. A `friends` room opens to its host's mutual friends only
      // (a stranger with the code gets the same 404 as a wrong code, so the code is not confirmed); `code` and `public` rooms take anyone.
      if (r && isSmg(r) && r.visibility === 'friends' && LIVE.includes(r.state) && r.host_pid !== p.pid) {
        const hostPid = members(c, r.code).find((m) => m.slot === r.host_slot)?.pid ?? r.host_pid;
        const inside = members(c, r.code).some((m) => m.pid === p.pid);
        // a former member (a room_left row the host's console accepted, or a friends-room entry) may come back by code even when it is no friend of the
        // CURRENT host: after a host change, or when it came in through a guest's friend path, only the room's own record is asked
        const formerly = !!c.db.get(`SELECT 1 AS x FROM room_left WHERE code = ? AND pid = ? AND accepted = 1`, r.code, p.pid);
        if (!inside && !formerly && hostPid !== p.pid && !isMutual(c, hostPid, p.pid)) throw new ApiError(404, 'room_not_found', 'no such room');
      }
      return joinLoaded(c, p, r, ver, ipk);
    });
  }
  const fcode = normalizeCode(String(body.friend).replace(/-/g, ''), 8);
  return c.tx(() => {
    const f = c.db.get(`SELECT pid FROM profiles WHERE code = ? AND banned = 0`, fcode);
    if (!f) throw new ApiError(404, 'player_not_found', 'no such player');
    if (!isMutual(c, p.pid, f.pid)) throw new ApiError(403, 'not_friends', 'you are not friends with that player');
    const room = friendRoom(c, f.pid);
    if (!room || room.visibility === 'code') throw new ApiError(404, 'room_not_found', 'that friend is not in a room you can join');
    return joinLoaded(c, p, loadRoom(c, room.code), ver, ipk);
  });
}

export function isMutual(c, a, b) {
  return !!c.db.get(
    `SELECT 1 AS x FROM friends f1 JOIN friends f2 ON f2.pid = f1.friend_pid AND f2.friend_pid = f1.pid WHERE f1.pid = ? AND f1.friend_pid = ?`,
    a, b,
  );
}

/** The live smg room a player is in (as host or guest), or null. Expiry is applied first. */
export function friendRoom(c, pid) {
  const rows = c.db.all(
    `SELECT r.code FROM room_members m JOIN rooms r ON r.code = m.code WHERE m.pid = ? AND r.game = 'smg' AND r.state IN ('open','joined','ready','started')`,
    pid,
  );
  for (const { code } of rows) {
    const r = loadRoom(c, code);
    if (r && LIVE.includes(r.state)) return r;
  }
  return null;
}

/** Whether `viewerVer` could join `r` now (lists: friends, public rooms). */
export function joinable(c, r, viewerVer, viewerPid) {
  const ms = members(c, r.code);
  if (viewerPid !== undefined && ms.some((m) => m.pid === viewerPid)) return false;
  return joinError(c, r, ms, viewerPid, viewerVer) === null;
}

function memberRoom(c, p, rawCode) {
  const code = normalizeCode(rawCode);
  const r = loadRoom(c, code);
  if (!isSmg(r)) throw new ApiError(404, 'room_not_found', 'no such room');
  const m = c.db.get(`SELECT * FROM room_members WHERE code = ? AND pid = ?`, code, p.pid);
  if (!m) {
    const moved = c.db.get(`SELECT 1 AS x FROM room_left WHERE code = ? AND pid = ? AND reason = 'moved'`, code, p.pid);
    if (moved) throw new ApiError(409, 'account_elsewhere', 'this account joined another room (from another console?)');
    throw new ApiError(403, 'not_member', 'you are not in that room');
  }
  touch(c, code, m);
  checkHostLost(c, r);
  return { r: c.db.get(`SELECT * FROM rooms WHERE code = ?`, code), m };
}

/**
 * A member's report of the slots it exchanges direct heartbeats with (query `l`, a hex digit: bit n = slot n), kept in memory with the time.
 * Only a member of a started room may report; a report is replaced by the next one and an absent `l` removes it.
 */
export function noteDirect(c, code, slot, mask) {
  if (!c.direct) return;
  const key = `${code}:${slot}`;
  if (mask === undefined) c.direct.delete(key);
  else c.direct.set(key, { at: c.now, mask: mask & 0xf & ~(1 << slot) });
}

/**
 * The service decides how the caller polls next (svcfix2): { holdSec, nextSec } = how long the next poll may be held and how long to wait after
 * an answer before asking again. The client obeys within bounds (hold 0..60 s, wait 0.3..600 s) and falls back to its old rhythm when the
 * fields are absent (an old service).
 *   - the room is forming: held polls of FORMING_HOLD (25 s), the next one at once (0.3 s): ~140 requests an hour while people gather;
 *   - started, every peer of the caller linked directly (the caller's report covers the members it peers with: the host all guests, a guest the
 *     host) and nobody quiet: the HOST holds one poll of SETTLED_HOST_HOLD (55 s) at a time (about 60 an hour: the keep-alive), a GUEST does not hold and
 *     asks again after SETTLED_GUEST_NEXT (300 s). Its direct heartbeats prove it is alive (seenAt);
 *   - started, anything else (a link down, a member quiet, a new member not yet linked, no report): held polls of 8 s and an immediate re-ask.
 */
export function pollPlan(c, r, m, hasReport) {
  const pc = c.cfg.rooms.poll;
  if (r.state !== 'started') return { holdSec: pc.formingHoldSec, nextSec: 0.3 };
  if (hasReport) {
    const ms = members(c, r.code);
    const mine = c.direct && c.direct.get(`${r.code}:${m.slot}`);
    const peers = ms.filter((x) => x.slot !== m.slot && (m.slot === r.host_slot || x.slot === r.host_slot));
    const linked = !!mine && c.now - mine.at < 5000 && peers.every((x) => ((mine.mask >> x.slot) & 1) && linkState(c, r, x.slot) === 'ok');
    if (linked && peers.length) {
      return m.slot === r.host_slot ? { holdSec: pc.settledHostHoldSec, nextSec: 1 } : { holdSec: 0, nextSec: pc.settledGuestNextSec };
    }
  }
  return { holdSec: busyHold(c, r, m, pc), nextSec: 0.3 };
}

/**
 * quota32: the held poll of a started room whose links are not settled (relay, or a direct link that never came up) used to be 8 s for the
 * whole session (~215 requests per 30 minutes per player). While nothing changes it backs off: 8 s, 15 s after 30 s of nothing, 30 s after
 * 90 s of nothing. "Nothing" = same room version, same direct-link report, no quiet peer. A change (any member's endpoint / punch / started /
 * join / leave bumps the version and wakes every held poll at once), a different link report or a quiet peer snaps it back to 8 s, and the
 * held poll itself ends early when a peer goes quiet (peersQuietNow, checked by the held-poll ticker). Events wake it, silence is checked
 * every 4 s. State lives in memory next to the link reports (lost with the object: back to 8 s).
 */
function busyHold(c, r, m, pc) {
  if (!c.direct) return pc.busyHoldSec;
  const key = `bo:${r.code}:${m.slot}`;
  const mine = c.direct.get(`${r.code}:${m.slot}`);
  const mask = mine ? mine.mask : -1;
  const quiet = members(c, r.code).some((x) => x.slot !== m.slot && linkState(c, r, x.slot) !== 'ok');
  let e = c.direct.get(key);
  if (!e || quiet || e.ver !== r.version || e.mask !== mask || c.now - e.at > 3 * pc.busyMaxHoldSec * 1000) {
    e = { since: c.now, at: c.now, ver: r.version, mask };
  } else e.at = c.now;
  c.direct.set(key, e);
  const idle = (c.now - e.since) / 1000;
  return idle >= pc.busyMaxAfterSec ? pc.busyMaxHoldSec : idle >= pc.busyMidAfterSec ? pc.busyMidHoldSec : pc.busyHoldSec;
}

/** True when some other member of the started room has gone quiet (no call for LINK_PING_SEC): ends a backed-off held poll early. */
export function peersQuietNow(c, pid, rawCode) {
  const code = normalizeCode(rawCode);
  const r = code && loadRoom(c, code);
  const m = r && r.state === 'started' && c.db.get(`SELECT slot FROM room_members WHERE code = ? AND pid = ?`, code, pid);
  return !!m && members(c, code).some((x) => x.slot !== m.slot && linkState(c, r, x.slot) !== 'ok');
}

export function getRoom(c, p, rawCode, sinceVersion, opts = {}) {
  return c.tx(() => {
    const { r, m } = memberRoom(c, p, rawCode);
    if (opts.plan) { if (r.state === 'started') noteDirect(c, r.code, m.slot, opts.mask); }
    const plan = opts.plan ? { poll: pollPlan(c, r, m, opts.mask !== undefined) } : {};
    if (sinceVersion !== undefined && sinceVersion === r.version) {
      // peersQuiet: some other member has not called for LINK_PING_SEC (12 s). A started room is polled every 8 s; this tells the client to
      // go back to the fast rhythm so a host change (after LINK_LOST_SEC, 20 s) reaches it quickly. No version bump: link states are not state.
      const peersQuiet = members(c, r.code).some((x) => x.slot !== m.slot && linkState(c, r, x.slot) !== 'ok');
      return { code: r.code, state: r.state, version: r.version, changed: false, ...(peersQuiet ? { peersQuiet: true } : {}), ...plan };
    }
    return { ...roomView(c, r, p.pid), changed: true, ...plan };
  });
}

/** The IPv4 part of an "a.b.c.d:port" claim. */
const claimHost = (ep) => String(ep).split(':')[0];

/**
 * POST /v1/rooms/:code/endpoint. The claims are only syntax-checked by checkEndpoint, so a member could name somebody else's address
 * (the peer would then send its packets there). `ipk` is the address the request really came from (cf-connecting-ip, IPv6 by /64):
 * a `public` address, or a public (srflx / observed) candidate, that is not that address (or any claim at all from an IPv6-only caller,
 * whose IPv4 cannot be checked) puts the member on the relay: its claims are dropped, `forced_relay` is set and its pairs are steered to
 * the relay ("endpoint_unverified"). Legitimate cases that differ (CGNAT, VPN, a second WAN) still play, through the relay. Without an
 * address (no header) or a loopback one (the Node fake and tests) nothing is checked. LAN endpoints are only passed on to a member behind the same
 * address (endpointFor).
 */
export function postEndpoint(c, p, rawCode, body, ipk) {
  strict(body, ['public', 'local', 'relay', 'nonce', 'nat', 'candidates']);
  const relayOnly = relayOnlyPid(c, p.pid);
  const nonce = b64decode(body.nonce);
  if (!nonce || nonce.length < 8 || nonce.length > 32) throw new ApiError(400, 'bad_nonce', 'nonce must be 8..32 bytes, base64', { field: 'nonce' });
  let pub = null;
  let local = null;
  let relay = null;
  let cands = [];
  // S1: a relay address is where the peer's console will send its traffic, so it is never a private / LAN / loopback / link-local
  // address, and only an account that was issued relay credentials within their lifetime may name one (the relay is off by default)
  let relayClaimed = false;
  if (body.relay !== undefined) { relay = checkEndpoint('relay', body.relay, { allowPrivate: false }); relayClaimed = true; }
  if (relayOnly) {
    if (body.public !== undefined || body.local !== undefined || body.candidates !== undefined) {
      throw new ApiError(400, 'relay_only_account', 'relay-only accounts send only the relayed address', { field: 'public' });
    }
    if (!relay) throw new ApiError(400, 'bad_endpoint', 'relay: required for relay-only accounts', { field: 'relay' });
  } else {
    pub = checkEndpoint('public', body.public);
    local = checkEndpoint('local', body.local);
    if (body.candidates !== undefined) {
      if (!Array.isArray(body.candidates) || body.candidates.length > 6) throw new ApiError(400, 'bad_endpoint', 'candidates: at most 6', { field: 'candidates' });
      cands = body.candidates.map((cd) => {
        strict(cd, ['type', 'addr']);
        if (!CAND_TYPES.includes(cd.type)) throw new ApiError(400, 'bad_endpoint', 'candidates: bad type', { field: 'candidates' });
        // only a `host` candidate may be a LAN address (and is passed on only to members behind the same address, see endpointFor)
        if (cd.type === 'relay') relayClaimed = true;
        return { type: cd.type, addr: checkEndpoint('candidates', cd.addr, { allowPrivate: cd.type === 'host' }) };
      });
    }
  }
  if (relayClaimed && !hasLiveMint(c, p.pid)) {
    throw new ApiError(403, 'relay_not_allowed', 'a relay address is only accepted from an account that was issued relay credentials', { field: 'relay' });
  }
  let forced = 0;
  // (no address, or loopback: the local fake service and tests, where every console is on this machine with a made-up public address;
  // Cloudflare never reports a loopback client)
  if (!relayOnly && ipk && ipk !== 'unknown' && !/^127\.|^::1$/.test(ipk)) {
    const claimed = [pub, ...cands.filter((cd) => cd.type === 'srflx' || cd.type === 'observed').map((cd) => cd.addr)].filter(Boolean).map(claimHost);
    if (claimed.some((h) => h !== ipk)) {
      forced = 1;
      pub = null;
      local = null;
      cands = [];
    }
  }
  const nat = body.nat === undefined || body.nat === null ? null : body.nat;
  if (nat !== null && !NAT_TYPES.includes(nat)) throw new ApiError(400, 'bad_request', `nat must be one of ${NAT_TYPES.join(', ')}`, { field: 'nat' });
  return c.tx(() => {
    const { r, m } = memberRoom(c, p, rawCode);
    if (r.state === 'closed' || r.state === 'expired') throw new ApiError(410, `room_${r.state}`, `that room is ${r.state}`);
    c.db.run(
      `UPDATE room_members SET public = ?, local = ?, nonce = ?, nat = ?, ep_ms = ?, relay = ?, cands = ?, src_ip = ?, forced_relay = ? WHERE code = ? AND slot = ?`,
      pub, local, body.nonce, nat, c.now, relay, cands.length ? JSON.stringify(cands) : null, ipk || null, forced, r.code, m.slot,
    );
    // a new address starts the pair(s) over: forget this side's punch verdicts
    const hs = r.host_slot;
    for (const g of members(c, r.code)) {
      if (g.slot === hs || (m.slot !== hs && g.slot !== m.slot)) continue;
      const v = parsePunch(g.punch);
      if (m.slot === hs) v.h = '-';
      else v.g = '-';
      c.db.run(`UPDATE room_members SET punch = ? WHERE code = ? AND slot = ?`, `${v.h}/${v.g}`, r.code, g.slot);
    }
    bump(c, r.code);
    settleState(c, r.code);
    return roomView(c, c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code), p.pid);
  });
}

/** {"result","path","ms"?,"peer"?}: peer = the other slot of the pair (the host must name the guest when it has more than one). */
export function reportPunch(c, p, rawCode, body) {
  strict(body, ['result', 'path', 'ms', 'peer']);
  if (body.result !== 'ok' && body.result !== 'fail') throw new ApiError(400, 'bad_request', 'result must be "ok" or "fail"', { field: 'result' });
  if (!['direct', 'lan', 'relay'].includes(body.path)) throw new ApiError(400, 'bad_request', 'path must be direct, lan or relay', { field: 'path' });
  if (body.ms !== undefined && !(Number.isInteger(body.ms) && body.ms >= 0 && body.ms <= 120000)) throw new ApiError(400, 'bad_request', 'ms out of range', { field: 'ms' });
  if (body.peer !== undefined && !isInt(body.peer, 0, 3)) throw new ApiError(400, 'bad_request', 'peer must be a slot 0..3', { field: 'peer' });
  const failed = body.result === 'fail' || body.path === 'relay';
  return c.tx(() => {
    const { r, m } = memberRoom(c, p, rawCode);
    const ms = members(c, r.code);
    const hs = r.host_slot;
    const host = ms.find((x) => x.slot === hs);
    let guest;
    if (m.slot === hs) {
      const guests = ms.filter((x) => x.slot !== hs);
      if (!guests.length) throw new ApiError(409, 'room_not_ready', 'no partner yet');
      if (body.peer === undefined) {
        if (guests.length > 1) throw new ApiError(400, 'bad_request', 'peer: say which guest', { field: 'peer' });
        guest = guests[0];
      } else {
        guest = guests.find((x) => x.slot === body.peer);
        if (!guest) throw new ApiError(409, 'not_member', 'no member in that slot', { field: 'peer' });
      }
    } else {
      if (body.peer !== undefined && body.peer !== hs) throw new ApiError(400, 'bad_request', 'guests only pair with the host (peer = hostSlot)', { field: 'peer' });
      if (!host) throw new ApiError(409, 'room_not_ready', 'no host');
      guest = m;
    }
    const v = parsePunch(guest.punch);
    const before = pairVerdict(v);
    if (m.slot === hs) v.h = failed ? 'fail' : 'ok';
    else v.g = failed ? 'fail' : 'ok';
    const after = pairVerdict(v);
    c.db.run(`UPDATE room_members SET punch = ? WHERE code = ? AND slot = ?`, `${v.h}/${v.g}`, r.code, guest.slot);
    bump(c, r.code);
    if (before !== after) {
      const [lo, hi] = pairKey(host.pid, guest.pid);
      c.db.run(`INSERT INTO pair_stats (lo_pid, hi_pid, updated_ms) VALUES (?, ?, ?) ON CONFLICT(lo_pid, hi_pid) DO NOTHING`, lo, hi, c.now);
      const dFail = (after === 'fail' ? 1 : 0) - (before === 'fail' ? 1 : 0);
      const dOk = (after === 'ok' ? 1 : 0) - (before === 'ok' ? 1 : 0);
      c.db.run(`UPDATE pair_stats SET fails = fails + ?, oks = oks + ?, updated_ms = ? WHERE lo_pid = ? AND hi_pid = ?`, dFail, dOk, c.now, lo, hi);
    }
    return roomView(c, c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code), p.pid);
  });
}

/**
 * Members call this once their link with the host works. The room is `started` once the host and at least one guest did. A guest who
 * drops in later calls it when its own pair with the host is ready.
 */
export function markStarted(c, p, rawCode) {
  return c.tx(() => {
    const { r, m } = memberRoom(c, p, rawCode);
    if (r.state === 'closed' || r.state === 'expired') throw new ApiError(410, `room_${r.state}`, `that room is ${r.state}`);
    const ms = members(c, r.code);
    const hs = r.host_slot;
    const host = ms.find((x) => x.slot === hs);
    const pairReady = m.slot === hs ? ms.some((g) => g.slot !== hs && g.ep_ms) && !!m.ep_ms : !!(m.ep_ms && host && host.ep_ms);
    if (!pairReady) throw new ApiError(409, 'room_not_ready', 'exchange endpoints first');
    c.db.run(`UPDATE room_members SET started = 1 WHERE code = ? AND slot = ?`, r.code, m.slot);
    bump(c, r.code);
    const ms2 = members(c, r.code);
    const hostStarted = ms2.some((x) => x.slot === hs && x.started);
    const startedGuests = ms2.filter((x) => x.slot !== hs && x.started);
    if (r.state !== 'started' && hostStarted && startedGuests.length) {
      c.db.run(`UPDATE rooms SET state = 'started', started_ms = ?, expires_ms = ?, version = version + 1 WHERE code = ?`, c.now, c.now + c.cfg.rooms.startedTtlMs, r.code);
    }
    if (hostStarted && m.started === 0) {
      // played at all: never pruned (accounts.pruneIdle)
      const who = m.slot === hs ? ms2.filter((x) => x.started).map((x) => x.pid) : [m.pid];
      for (const pid of who) c.db.run(`UPDATE profiles SET matches_played = matches_played + 1 WHERE pid = ? AND matches_played = 0`, pid);
    }
    return roomView(c, c.db.get(`SELECT * FROM rooms WHERE code = ?`, r.code), p.pid);
  });
}

/**
 * POST /v1/rooms/:code/accept {"slot":2} (the host): the host's console lets a member see addresses. Before that a stranger who joined a
 * code room (guessed or leaked) gets no address of the host and the host none of his. Idempotent; a slot that is not waiting is fine.
 */
export function acceptMember(c, p, rawCode, body) {
  strict(body, ['slot']);
  const slot = Number(body.slot);
  if (!Number.isInteger(slot) || slot < 0 || slot > 15) throw new ApiError(400, 'bad_request', 'slot must be a member slot');
  return c.tx(() => {
    const { r, m } = memberRoom(c, p, rawCode);
    if (m.slot !== r.host_slot) throw new ApiError(403, 'not_host', 'only the host accepts members');
    const t = c.db.get(`SELECT accepted FROM room_members WHERE code = ? AND slot = ?`, r.code, slot);
    if (t && !t.accepted) {
      c.db.run(`UPDATE room_members SET accepted = 1 WHERE code = ? AND slot = ?`, r.code, slot);
      bump(c, r.code);
    }
    return { ok: true, slot, accepted: !!t };
  });
}

/**
 * True when this account was in that room and is no longer a member: a room_left row (left, lost, ...) for it, other than 'moved' (it joined another room, which owns the summary).
 * Used for a summary that arrives after the member was removed (kicked, host lost); the caller still meters it (once per room, daily caps).
 */
export function formerMember(c, p, rawCode) {
  let code;
  try { code = normalizeCode(rawCode); } catch { return false; }
  return !!c.db.get(`SELECT 1 AS x FROM room_left WHERE code = ? AND pid = ? AND reason <> 'moved'`, code, p.pid);
}

/** Any member: its row goes and the room lives on (the host role moves on when the host leaves); the last member closes it. */
export function leaveRoom(c, p, rawCode) {
  return c.tx(() => {
    const { r, m } = memberRoom(c, p, rawCode);
    if (r.state === 'closed' || r.state === 'expired') {
      const gone = { ok: true, state: r.state };
      Object.defineProperty(gone, 'wasMember', { value: true, enumerable: false }); // it was a member of that room: a summary it sends (host_lost, ...) is still wanted (app.js meters it)
      return gone;
    }
    memberLeaves(c, r, m.slot, 'left');
    const after = c.db.get(`SELECT state, host_slot, host_epoch FROM rooms WHERE code = ?`, r.code);
    const out = { ok: true, state: after.state, hostSlot: after.host_slot, hostEpoch: after.host_epoch };
    Object.defineProperty(out, 'removedMember', { value: true, enumerable: false }); // (not part of the answer: app.js reads it to decide about the session summary)
    return out;
  });
}

// ------------------------------------------------------------------ quick match and the public list

/** POST /v1/quick {"mode"}: the oldest public room of this game, version and mode with a free slot; otherwise a new public room. */
export function quick(c, p, body, ver, ipk) {
  strict(body, ['mode']);
  const mode = checkMode(body.mode);
  return c.tx(() => {
    const rows = c.db.all(
      `SELECT r.code FROM rooms r
        WHERE r.game = 'smg' AND r.visibility = 'public' AND r.mode = ? AND r.app = ? AND r.state IN ('open','joined','ready','started')
          AND r.expires_ms >= ?
          AND (SELECT COUNT(*) FROM room_members m WHERE m.code = r.code) < r.max_members
          AND NOT EXISTS (SELECT 1 FROM room_members m WHERE m.code = r.code AND m.pid = ?)
        ORDER BY r.created_ms ASC, r.code ASC LIMIT ?`,
      mode, ver, c.now, p.pid, c.cfg.smg.quickScan,
    );
    for (const { code } of rows) {
      let r = loadRoom(c, code);
      if (!r || !LIVE.includes(r.state)) continue;
      if (checkHostLost(c, r)) { // the host stopped polling: the role moved on (or the room closed)
        r = loadRoom(c, code);
        if (!r || !LIVE.includes(r.state)) continue;
      }
      if (!joinable(c, r, ver, p.pid)) continue;
      return { ...joinLoaded(c, p, r, ver, ipk), quick: 'joined' };
    }
    const code = newRoom(c, p, { mode, maxMembers: c.cfg.smg.maxMembers, visibility: 'public', settings: { ...DEFAULT_SETTINGS } }, ver, ipk);
    return { ...roomView(c, loadRoom(c, code), p.pid), quick: 'created' };
  });
}

/** GET /v1/rooms/public?mode=coop|race: up to 20 joinable public rooms (most players first, then newest). Names only: no pid, no address. */
export function publicRooms(c, p, query, ver) {
  const mode = query.get('mode');
  if (mode !== null) checkMode(mode);
  // rooms with players waiting first (a room with a free slot and somebody in it is the one to join), then the newest; full rooms are
  // not worth a scan slot
  const rows = c.db.all(
    `SELECT r.code, (SELECT COUNT(*) FROM room_members m WHERE m.code = r.code) AS n FROM rooms r
      WHERE r.game = 'smg' AND r.visibility = 'public' AND r.app = ? AND r.state IN ('open','joined','ready','started')
        AND r.expires_ms >= ? ${mode !== null ? 'AND r.mode = ?' : ''} AND n < r.max_members
      ORDER BY n DESC, r.created_ms DESC, r.code ASC LIMIT 60`,
    ver, c.now, ...(mode !== null ? [mode] : []),
  );
  const out = [];
  for (const { code } of rows) {
    if (out.length >= 20) break;
    const r = c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
    if (linkState(c, r, r.host_slot) === 'lost' || !joinable(c, r, ver, p.pid)) continue;
    const names = c.db.all(
      `SELECT m.slot, p.name FROM room_members m JOIN profiles p ON p.pid = m.pid WHERE m.code = ? ORDER BY m.slot`, code,
    );
    const s = parseSettings(r.settings_json);
    out.push({
      code: r.code, mode: r.mode, state: r.state, players: names.length, maxMembers: r.max_members,
      host: (names.find((n) => n.slot === r.host_slot) || {}).name ?? null, names: names.map((n) => n.name),
      galaxy: s.galaxy ?? null, scenario: s.scenario ?? null,
    });
  }
  return { rooms: out };
}

/** Was this account issued relay credentials (turn_mints) that are still within their lifetime? */
export function hasLiveMint(c, pid) {
  return !!c.db.get(`SELECT 1 AS x FROM turn_mints WHERE pid = ? AND minted_ms >= ? LIMIT 1`, pid, c.now - c.cfg.turn.ttlSec * 1000);
}

/** turn.js: may this player get relay credentials for this room? */
export function turnAllowed(c, r, pid) {
  return isSmg(r) && memberSlot(c, r.code, pid) >= 0 && ['joined', 'ready', 'started'].includes(r.state);
}

/**
 * svcfix3 (review S5): does this member have a visible reason to need the relay? True when a pair it is in is steered to the relay
 * (relay-only account, unverified endpoint, symmetric NAT, repeated failures) or has a recorded 'fail' punch verdict. The pairs of a
 * Galaxy room are (host, guest); the host is in every pair. turn.js keeps the credentials without such a reason to a smaller slice of the day.
 */
export function turnNeeded(c, r, pid) {
  const rows = c.db.all(`SELECT * FROM room_members WHERE code = ? ORDER BY slot`, r.code);
  const host = rows.find((m) => m.slot === r.host_slot);
  if (!host) return false;
  return rows.some((g) => g !== host && (g.pid === pid || host.pid === pid) && (steerPair(c, host, g).relay || pairVerdict(parsePunch(g.punch)) === 'fail'));
}

/** The source address recorded for a member (the key of the per-address relay cap), or ''. */
export function memberAddr(c, code, pid) {
  return c.db.get(`SELECT src_ip FROM room_members WHERE code = ? AND pid = ?`, code, pid)?.src_ip || '';
}

/** Account deletion / ban: leave every smg room (hosted ones close). */
export function dropPlayer(c, pid) {
  leaveOthers(c, pid, null, 'left');
  c.db.run(`DELETE FROM room_members WHERE pid = ?`, pid);
}

export { CODE_ALPHABET };
