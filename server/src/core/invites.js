// Far-away invites (GAME=smg, code5): a 5-character code a host reads aloud, which the service turns into the host's SEALED direct-code
// content (address, port, secret). The service never sees what is inside: the host's console seals it with a key made from the code
// (platform/src/mp/mp_invite.cpp), the service stores the opaque text and hands it to whoever names the code. See share/core/CODE5-DESIGN.md.
//
//   POST   /v1/invites          {code, blob}   the host registers its code (it picks the 5 characters; a live one is refused: invite_taken)
//   POST   /v1/invites/:code/keep              the host extends the code while its gathering screen is still open (one request per ttl)
//   DELETE /v1/invites/:code                   the host closes it (the game starts, or the gathering screen is left): freeing, never refused
//   POST   /v1/invites/resolve  {code}         a friend names the code and gets {blob}; wrong codes are charged to the address (app.js)
//
// Life: open for INVITE_TTL_SEC (10 min), extended by `keep` up to INVITE_MAX_LIFE_SEC (60 min) from the start; a short grace
// (INVITE_GRACE_SEC, 60 s) after a missed keep so a late request does not strand a friend; closed by the host = dead at once. A closed or
// expired row stays for ROOM_KEEP_CLOSED_SEC so the friend is told "closed" / "expired" instead of "no such code".
// One live invite per account (a new one closes the old). At most 3 DIFFERENT accounts may resolve one code (the room holds 4): the
// fourth, and every stranger after the friends are in, is refused (invite_full) and learns nothing.

import { ApiError, strict } from './util.js';
import { requireDaily, bumpDaily } from './limits.js';
import { normalizeCode } from './rooms.js';

export const INVITE_LEN = 5;
/** 36 bytes sealed (8 nonce + 12 payload + 16 tag) as base64url without padding = 48 characters. */
export const BLOB_RE = /^[A-Za-z0-9_-]{48}$/;

export const normalizeInvite = (raw) => normalizeCode(raw, INVITE_LEN);

/** Lazily applies expiry; returns the fresh row (or null). */
export function loadInvite(c, code) {
  const r = c.db.get(`SELECT * FROM invites WHERE code = ?`, code);
  if (!r) return null;
  if (r.state === 'open' && c.now > r.expires_ms + c.cfg.invites.graceMs) {
    c.db.run(`UPDATE invites SET state = 'expired', closed_ms = ? WHERE code = ?`, c.now, code);
    return c.db.get(`SELECT * FROM invites WHERE code = ?`, code);
  }
  return r;
}

function closeInvite(c, r, reason) {
  c.db.run(`UPDATE invites SET state = 'closed', closed_ms = ?, reason = ? WHERE code = ? AND state = 'open'`, c.now, reason, r.code);
}

/** Rows no one can use any more go (a bounded number a call: the table never grows past the live ones plus a few minutes). */
function tidy(c) {
  c.db.run(`DELETE FROM invites WHERE state <> 'open' AND closed_ms + ? < ?`, c.cfg.rooms.keepClosedMs, c.now);
  c.db.run(`DELETE FROM invites WHERE state = 'open' AND expires_ms + ? + ? < ?`, c.cfg.invites.graceMs, c.cfg.rooms.keepClosedMs, c.now);
}

export function sweepInvites(c) {
  c.db.run(`UPDATE invites SET state = 'expired', closed_ms = ? WHERE state = 'open' AND expires_ms + ? < ?`, c.now, c.cfg.invites.graceMs, c.now);
  tidy(c);
}

const view = (c, r) => ({
  code: r.code,
  expiresMs: r.expires_ms,
  ttlSec: Math.round(c.cfg.invites.ttlMs / 1000),
  keepSec: Math.round(c.cfg.invites.ttlMs / 2000), // the client extends at half the life
});

/**
 * POST /v1/invites {"code":"KCBC4","blob":"<48 base64url>"}. `onTaken` is called (by app.js) when the code is a LIVE one of somebody else: that
 * answer is a guessing oracle, so it is charged to the address like a wrong code.
 */
export function registerInvite(c, p, body, ver, onTaken) {
  strict(body, ['code', 'blob']);
  const code = normalizeInvite(body.code);
  if (typeof body.blob !== 'string' || !BLOB_RE.test(body.blob)) throw new ApiError(400, 'bad_blob', 'blob must be 48 base64url characters', { field: 'blob' });
  return c.tx(() => {
    requireDaily(c, p.pid, 'rooms', c.cfg.caps.roomsPerAccountDay, 'rooms');
    tidy(c);
    const old = c.db.get(`SELECT * FROM invites WHERE pid = ? AND state = 'open'`, p.pid);
    if (old) closeInvite(c, old, 'replaced');
    const there = loadInvite(c, code);
    if (there && there.state === 'open') {
      if (onTaken) onTaken();
      throw new ApiError(409, 'invite_taken', 'that code is in use, pick another');
    }
    if (there) c.db.run(`DELETE FROM invites WHERE code = ?`, code); // a dead one: the code is free again
    const live = c.db.get(`SELECT COUNT(*) AS n FROM invites WHERE state = 'open'`).n;
    if (live >= c.cfg.invites.maxLive) throw new ApiError(503, 'busy', 'too many invites right now, try again soon', { retryAfterSec: 60 }, { 'retry-after': '60' });
    c.db.run(
      `INSERT INTO invites (code, pid, blob, app, created_ms, expires_ms) VALUES (?, ?, ?, ?, ?, ?)`,
      code, p.pid, body.blob, ver, c.now, c.now + c.cfg.invites.ttlMs,
    );
    bumpDaily(c, p.pid, 'rooms');
    return view(c, c.db.get(`SELECT * FROM invites WHERE code = ?`, code));
  });
}

function ownInvite(c, p, rawCode) {
  const code = normalizeInvite(rawCode);
  const r = loadInvite(c, code);
  if (!r || r.pid !== p.pid) throw new ApiError(404, 'invite_not_found', 'no such invite');
  return r;
}

/** POST /v1/invites/:code/keep: the host's gathering screen is still open. Extends from now, never past the longest life. */
export function keepInvite(c, p, rawCode) {
  return c.tx(() => {
    const r = ownInvite(c, p, rawCode);
    if (r.state !== 'open') throw new ApiError(410, r.state === 'expired' ? 'invite_expired' : 'invite_closed', 'that invite is over');
    const end = Math.min(c.now + c.cfg.invites.ttlMs, r.created_ms + c.cfg.invites.maxLifeMs);
    if (end > r.expires_ms) c.db.run(`UPDATE invites SET expires_ms = ? WHERE code = ?`, end, r.code);
    return view(c, c.db.get(`SELECT * FROM invites WHERE code = ?`, r.code));
  });
}

/** DELETE /v1/invites/:code: the host is done with it. Idempotent. */
export function closeOwnInvite(c, p, rawCode) {
  return c.tx(() => {
    const code = normalizeInvite(rawCode);
    const r = loadInvite(c, code);
    if (!r || r.pid !== p.pid) return { ok: true };
    if (r.state === 'open') closeInvite(c, r, 'host_closed');
    return { ok: true };
  });
}

/**
 * POST /v1/invites/resolve {"code"}. Unknown code: 404 invite_not_found (app.js charges the address). A real one hands over the sealed blob.
 * Over, never the host's own, other version, and the 4th account: each answers its own code and none of them gives anything away.
 */
export function resolveInvite(c, p, body, ver) {
  strict(body, ['code']);
  const code = normalizeInvite(body.code);
  return c.tx(() => {
    const r = loadInvite(c, code);
    if (!r) throw new ApiError(404, 'invite_not_found', 'no such invite');
    if (r.state === 'expired') throw new ApiError(410, 'invite_expired', 'that invite has expired');
    if (r.state === 'closed') throw new ApiError(410, 'invite_closed', 'that invite is closed');
    if (r.pid === p.pid) throw new ApiError(409, 'own_invite', 'that is your own invite');
    if (r.app !== ver) throw new ApiError(409, 'version_mismatch', 'that game is a different version');
    if (r.resolves >= c.cfg.invites.maxResolves) throw new ApiError(429, 'rate_limited', 'too many tries', { retryAfterSec: 60 }, { 'retry-after': '60' });
    const seen = r.resolvers ? r.resolvers.split(',').map(Number) : [];
    if (!seen.includes(p.pid)) {
      if (seen.length >= c.cfg.invites.maxResolvers) throw new ApiError(409, 'invite_full', 'that game is full');
      c.db.run(`UPDATE invites SET resolvers = ? WHERE code = ?`, [...seen, p.pid].join(','), r.code);
    }
    c.db.run(`UPDATE invites SET resolves = resolves + 1 WHERE code = ?`, r.code);
    return { blob: r.blob, expiresMs: r.expires_ms };
  });
}
