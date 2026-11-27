// Optional light proof of work for POST /v1/register (REGISTER_POW_BITS, off by default).
//
// Stateless server nonce: challenge = "<expMs>.<rand16hex>.<bits>.<mac16hex>", mac = HMAC-SHA256(serverKey, "<expMs>.<rand>.<bits>.<ipKey>")
// truncated to 16 hex. It is bound to the client address (an IPv6 /64) and to its difficulty, and valid for CHALLENGE_TTL_MS, once.
// The difficulty rises per address (bitsFor): REGISTER_POW_BITS for the first REGISTER_POW_FREE registrations an address made in the
// last 24 h, then one bit more for each further one, at most REGISTER_POW_MAX_BITS. A console never notices (one registration);
// a venue behind one NAT pays a little more per console; a farm of addresses is slowed by the per-address and per-day caps, not by
// the work (a laptop core does 2^20 tries in under a second): the work only prices each registration. What really limits sign-up
// farming is the per-/64 and per-/48 (IPv4 /24) daily caps (accounts.js, README "Sign-up farming"), not this.
// Client work: find a string `nonce` (1..20 characters of [0-9A-Za-z]) such that SHA-256(utf8(challenge + ":" + nonce))
// starts with at least `bits` zero bits. Expected cost 2^bits hashes (16..20 bits is well under a second on a PC; budget a
// few seconds on a Vita). The server key is generated once and kept in the settings table.

import { ApiError, randomBytes, hex, hmacSha256Hex, leadingZeroBits, safeEqual, te } from './util.js';

export const CHALLENGE_TTL_MS = 120_000;
const KEY_NAME = 'pow_key';
const NONCE_RE = /^[0-9A-Za-z]{1,20}$/;

function serverKey(c) {
  const r = c.db.get(`SELECT value FROM settings WHERE key = ?`, KEY_NAME);
  if (r) return Uint8Array.from(r.value.match(/../g).map((h) => parseInt(h, 16)));
  const k = randomBytes(32);
  c.db.run(`INSERT INTO settings (key, value) VALUES (?, ?)`, KEY_NAME, hex(k));
  return k;
}

/** Registrations this address made in the last 24 h (memory only: c.regSeen, ipk -> [ms]). */
export function recentFrom(c, ipk) {
  const list = c.regSeen.get(ipk);
  if (!list) return 0;
  const fresh = list.filter((t) => c.now - t < 86400_000);
  if (fresh.length) c.regSeen.set(ipk, fresh); else c.regSeen.delete(ipk);
  return fresh.length;
}

/** A successful registration from this address (raises the next challenge's difficulty). */
export function noteRegistered(c, ipk) {
  if (c.regSeen.size >= 20000) c.regSeen.clear();
  const list = c.regSeen.get(ipk) || [];
  list.push(c.now);
  c.regSeen.set(ipk, list.slice(-64));
}

export function bitsFor(c, ipk) {
  const r = c.cfg.register;
  if (!r.powBits) return 0;
  // (this would be the address's registration number recent + 1: the first powFree of a day cost the base difficulty)
  const perAddress = Math.min(Math.max(r.powBits, r.powMaxBits), r.powBits + Math.max(0, recentFrom(c, ipk) + 1 - r.powFree));
  // svc3 (sign-up farming): one more bit from half full, two from 80 % (accounts.js, fill.js); never above what the client will solve
  // never above what the clients will solve (POW_CLIENT_MAX_BITS, 17: mp_pow.h kMaxBits; above it a client says "online play is busy" instead of grinding)
  return Math.min(Math.max(r.powBits, r.powClientMax), perAddress + (c.fill ? c.fill.level(c.now) : 0));
}

export async function issueChallenge(c, ipk) {
  const bits = bitsFor(c, ipk);
  if (!bits) return { bits: 0 };
  const exp = c.now + CHALLENGE_TTL_MS;
  const rand = hex(randomBytes(8));
  const mac = (await hmacSha256Hex(serverKey(c), `${exp}.${rand}.${bits}.${ipk}`)).slice(0, 16);
  return {
    bits,
    algo: 'sha256',
    challenge: `${exp}.${rand}.${bits}.${mac}`,
    expiresMs: exp,
    rule: 'sha256(challenge + ":" + nonce) must start with `bits` zero bits; send {"pow":{"challenge","nonce"}} in the register body',
  };
}

/** Verifies the `pow` member of a register body. Throws 428 pow_required (with a fresh challenge) or 400 bad_pow. */
export async function verifyPow(c, ipk, pow) {
  if (!c.cfg.register.powBits) return;
  if (pow === undefined) {
    throw new ApiError(428, 'pow_required', 'solve the proof of work and resend', await issueChallenge(c, ipk));
  }
  const bad = (reason) => new ApiError(400, 'bad_pow', 'proof of work rejected', { reason });
  if (typeof pow !== 'object' || pow === null || typeof pow.challenge !== 'string' || typeof pow.nonce !== 'string') throw bad('format');
  if (Object.keys(pow).some((k) => k !== 'challenge' && k !== 'nonce')) throw bad('format');
  const m = /^(\d{10,16})\.([0-9a-f]{16})\.(\d{1,2})\.([0-9a-f]{16})$/.exec(pow.challenge);
  if (!m || !NONCE_RE.test(pow.nonce)) throw bad('format');
  const exp = Number(m[1]);
  const bits = Number(m[3]);
  if (exp < c.now) throw bad('expired');
  if (exp > c.now + CHALLENGE_TTL_MS + 1000 || bits < 1 || bits > 28) throw bad('format');
  const mac = (await hmacSha256Hex(serverKey(c), `${m[1]}.${m[2]}.${bits}.${ipk}`)).slice(0, 16);
  if (!(await safeEqual(mac, m[4]))) throw bad('mac');
  // The difficulty is bound to the state at REDEEM time (svcfix2): a challenge carries the price asked when it was issued, but if this address
  // has registered since (or the table has filled), the price now is higher and a stockpile of cheap challenges fetched in advance is
  // worth nothing: the old one is refused with a fresh, harder challenge (428 pow_required) and is NOT spent.
  const bitsNow = bitsFor(c, ipk);
  if (bits < bitsNow) {
    throw new ApiError(428, 'pow_required', 'the price of a sign-up from this address has gone up: solve the new proof of work and resend', await issueChallenge(c, ipk));
  }
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', te.encode(`${pow.challenge}:${pow.nonce}`)));
  if (leadingZeroBits(digest) < bits) throw bad('hash');
  if (c.powUsed.has(m[2])) throw bad('used');
  c.powUsed.set(m[2], exp);
  if (c.powUsed.size > 5000) for (const [k, e] of c.powUsed) if (e < c.now) c.powUsed.delete(k);
}

// exported for the tests and for a reference solver
export async function solve(challenge, bits, limit = 50_000_000) {
  for (let i = 0; i < limit; i++) {
    const nonce = i.toString(36);
    const d = new Uint8Array(await crypto.subtle.digest('SHA-256', te.encode(`${challenge}:${nonce}`)));
    if (leadingZeroBits(d) >= bits) return nonce;
  }
  throw new Error('no solution');
}
