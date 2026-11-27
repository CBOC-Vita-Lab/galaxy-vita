// Small shared helpers: errors, JSON responses, base64, hashing, randomness.

export class ApiError extends Error {
  /**
   * @param {number} status HTTP status
   * @param {string} code stable machine-readable code (see PROTOCOL.md)
   * @param {string} [message] human text, never contains secrets
   * @param {object} [extra] merged into the error object (e.g. retryAfterSec)
   * @param {Record<string,string>} [headers]
   */
  constructor(status, code, message, extra, headers) {
    super(message || code);
    this.status = status;
    this.code = code;
    this.extra = extra || {};
    this.headers = headers || {};
  }
}

export function json(status, body, headers) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      'content-type': 'application/json; charset=utf-8',
      'cache-control': 'no-store',
      'x-content-type-options': 'nosniff',
      ...(headers || {}),
    },
  });
}

export function errorResponse(e) {
  const body = { error: { code: e.code, message: e.message, ...e.extra } };
  return json(e.status, body, e.headers);
}

export const te = new TextEncoder();

export function b64encode(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x2000) {
    s += String.fromCharCode(...bytes.subarray(i, i + 0x2000));
  }
  return btoa(s);
}

/** Strict standard base64 (padded). Returns null when malformed. */
export function b64decode(str) {
  if (typeof str !== 'string' || str.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(str)) return null;
  try {
    const s = atob(str);
    const out = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
    return out;
  } catch {
    return null;
  }
}

export function b64url(bytes) {
  return b64encode(bytes).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

export function hex(bytes) {
  let s = '';
  for (const b of bytes) s += b.toString(16).padStart(2, '0');
  return s;
}

export function randomBytes(n) {
  const a = new Uint8Array(n);
  crypto.getRandomValues(a);
  return a;
}

/** Uniform integer in [0, n) without modulo bias. */
export function randomInt(n) {
  const limit = Math.floor(0x100000000 / n) * n;
  const a = new Uint32Array(1);
  for (;;) {
    crypto.getRandomValues(a);
    if (a[0] < limit) return a[0] % n;
  }
}

export async function sha256hex(input) {
  const data = typeof input === 'string' ? te.encode(input) : input;
  return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', data)));
}

/** Constant-time string compare via equal-length digests. */
export async function safeEqual(a, b) {
  const [ha, hb] = await Promise.all([sha256hex(String(a)), sha256hex(String(b))]);
  let d = 0;
  for (let i = 0; i < ha.length; i++) d |= ha.charCodeAt(i) ^ hb.charCodeAt(i);
  return d === 0;
}

export function isInt(v, min, max) {
  return Number.isInteger(v) && v >= min && v <= max;
}

/** Parse a numeric env var with a default and clamps. */
export function envInt(env, name, def, min = 0, max = Number.MAX_SAFE_INTEGER) {
  const raw = env && env[name];
  if (raw === undefined || raw === null || raw === '') return def;
  const n = Number(raw);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, Math.trunc(n))) : def;
}

export function dottedIp(u32) {
  return `${(u32 >>> 24) & 255}.${(u32 >>> 16) & 255}.${(u32 >>> 8) & 255}.${u32 & 255}`;
}

/** Strict IPv4:port parser. Returns {ip:[a,b,c,d], port} or null. */
export function parseEndpoint(s) {
  if (typeof s !== 'string' || s.length > 21) return null;
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/.exec(s);
  if (!m) return null;
  const ip = [m[1], m[2], m[3], m[4]].map((x) => (x.length > 1 && x[0] === '0' ? NaN : Number(x)));
  const port = Number(m[5]);
  if (ip.some((x) => !(x >= 0 && x <= 255)) || !(port >= 1 && port <= 65535) || (m[5].length > 1 && m[5][0] === '0')) return null;
  return { ip, port };
}

/**
 * Strict allowlist: the body must be a JSON object whose keys are all in `allowed`. Unknown keys are refused
 * (a client that sends something we do not know is either newer than the server or broken).
 */
export function strict(body, allowed) {
  if (typeof body !== 'object' || body === null || Array.isArray(body)) throw new ApiError(400, 'bad_request', 'JSON object expected');
  for (const k of Object.keys(body)) {
    if (!allowed.includes(k)) throw new ApiError(400, 'unknown_field', 'unexpected field', { field: k.slice(0, 32) });
  }
  return body;
}

/** Endpoints without parameters accept no body, or an empty object. */
export function noBody(body) {
  if (body === undefined) return;
  strict(body, []);
}

export const VERSION_RE = /^\d{1,4}(\.\d{1,4}){0,3}(-[a-z0-9.]{1,12})?$/;

/** Compare dotted versions numerically (suffix after '-' ignored). */
export function cmpVersion(a, b) {
  const pa = String(a).split('-')[0].split('.').map(Number);
  const pb = String(b).split('-')[0].split('.').map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d) return d < 0 ? -1 : 1;
  }
  return 0;
}

/**
 * Rate-limit key for a client address. IPv4 stays as is; IPv6 is reduced to its /64 (one subscriber normally owns a whole
 * /64, so per-address keys would let a single home connection mint millions of buckets and sidestep every per-address
 * limit); IPv4-mapped IPv6 is treated as the IPv4 address. Only ever used as an in-memory key.
 */
export function ipKey(ip) {
  let s = String(ip || 'unknown').trim().toLowerCase().split('%')[0];
  if (!s.includes(':')) return s.slice(0, 64);
  const mapped = /^(?:0{0,4}:){2,5}(?:ffff:)(\d{1,3}(?:\.\d{1,3}){3})$/.exec(s) || /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/.exec(s);
  if (mapped) return mapped[1];
  let head = s;
  let tail = '';
  const dc = s.indexOf('::');
  if (dc >= 0) { head = s.slice(0, dc); tail = s.slice(dc + 2); }
  let h = head ? head.split(':') : [];
  let t = tail ? tail.split(':') : [];
  const dotted = (arr) => { // a trailing dotted quad counts as two groups
    if (arr.length && arr[arr.length - 1].includes('.')) { arr.pop(); arr.push('0', '0'); }
    return arr;
  };
  h = dotted(h);
  t = dotted(t);
  const missing = dc >= 0 ? 8 - h.length - t.length : 0;
  const groups = [...h, ...Array(Math.max(0, missing)).fill('0'), ...t];
  if (groups.length !== 8 || !groups.every((g) => /^[0-9a-f]{1,4}$/.test(g))) return `v6:${s.slice(0, 39)}`;
  return `v6:${groups.slice(0, 4).map((g) => g.padStart(4, '0')).join(':')}`;
}

/**
 * Key of the DAILY per-address ceilings (budget.js gateAddress / gateBadAuth / reserve): IPv4 as ipKey gives it; IPv6 reduced to its /48 (ipKey keeps the /64
 * for the short-window limits). One subscriber owns a /48 at most in the common allocation (65,536 /64s): keyed per /64 the daily caps never bind
 * (sec34, VitAdhoc review finding 4). Not netKey (that one folds an IPv4 address into its /24, which would pool unrelated players here).
 */
export function dailyAddrKey(ipk) {
  const s = String(ipk);
  return s.startsWith('v6:') ? s.split(':').slice(0, 4).join(':') : s;
}

/** HMAC-SHA-256 as hex, key given as bytes. */
export async function hmacSha256Hex(keyBytes, message) {
  const key = await crypto.subtle.importKey('raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  return hex(new Uint8Array(await crypto.subtle.sign('HMAC', key, te.encode(message))));
}

/** Number of leading zero bits of a byte array. */
export function leadingZeroBits(bytes) {
  let n = 0;
  for (const b of bytes) {
    if (b === 0) { n += 8; continue; }
    n += Math.clz32(b) - 24;
    break;
  }
  return n;
}

/**
 * Reads a request body as text, never holding more than `max` bytes: the stream is cancelled the moment it goes past the cap
 * (Content-Length can be missing or wrong). Returns null when the cap was exceeded.
 * @param {Request} request @param {number} max
 */
export async function readBodyCapped(request, max) {
  if (!request.body) return '';
  const reader = request.body.getReader();
  const parts = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > max) {
      try { await reader.cancel(); } catch { /* the stream is gone anyway */ }
      return null;
    }
    parts.push(value);
  }
  const all = new Uint8Array(total);
  let at = 0;
  for (const p of parts) { all.set(p, at); at += p.byteLength; }
  return new TextDecoder().decode(all);
}
