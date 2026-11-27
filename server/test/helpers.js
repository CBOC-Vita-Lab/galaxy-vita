// Test harness: the real application over node:sqlite with an injectable clock.

import { createApp } from '../src/core/app.js';
import { openNodeSql } from '../src/sqlite-node.js';
import { extractKeyOr, DEFAULT_SOURCE } from '../scripts/extract-hmac-key.mjs';
import { hmacMd5 } from '../src/core/md5.js';
import { b64encode } from '../src/core/util.js';

export const HMAC_HEX = extractKeyOr(DEFAULT_SOURCE, '0'.repeat(64)); // the Strikers suites need the real key (MSC_HMAC_SOURCE)
export const HMAC_KEY = Uint8Array.from(HMAC_HEX.match(/../g).map((h) => parseInt(h, 16)));
export const T0 = Date.UTC(2026, 9, 30, 12, 0, 0); // 2026-10-30 12:00 UTC (a Friday; season started 2026-10-29)

export function makeServer(envOverrides = {}, opts = {}) {
  const store = openNodeSql();
  const clock = { now: opts.t0 ?? T0 };
  const env = {
    RANK_HMAC_KEY_HEX: HMAC_HEX,
    ADMIN_TOKEN: 'test-admin-token-0123456789',
    TURN_PROVIDER: 'stub',
    RATE_LIMIT_SCALE: '1000', // effectively off unless a test sets it
    MIN_MATCH_SEC: '0', // tests finish matches instantly; the minimum-length tests set it explicitly
    REQUIRE_APP_VERSION: '0', // production sets it to 1 (wrangler.toml); tests opt in
    ...envOverrides,
  };
  const app = createApp({ sql: store.sql, tx: store.tx, env, now: () => clock.now, ...(opts.deps || {}) });
  let ipCounter = 0;
  async function call(method, path, body, token, headers = {}) {
    const h = { 'cf-connecting-ip': headers.ip || `198.51.100.${(ipCounter++ % 250) + 1}`, ...headers };
    delete h.ip;
    if (token) h.authorization = `Bearer ${token}`;
    const init = { method, headers: h };
    if (body !== undefined) {
      init.body = typeof body === 'string' ? body : JSON.stringify(body);
      h['content-type'] = 'application/json';
    }
    const resp = await app.handle(new Request(`https://test.invalid${path}`, init));
    let data = null;
    const text = await resp.text();
    try { data = JSON.parse(text); } catch { data = text; }
    return { status: resp.status, data, headers: resp.headers };
  }
  const advance = (ms) => { clock.now += ms; };
  return { app, store, clock, call, advance, env };
}

export async function signUp(srv, name, region) {
  const r = await srv.call('POST', '/v1/register', { name, ...(region ? { region } : {}) });
  if (r.status !== 200) throw new Error(`register ${name} failed: ${JSON.stringify(r.data)}`);
  return { ...r.data };
}

/** Build a 0x80 NetworkRankingSubmission-shaped blob with a valid digest. endian: 'le' | 'be'. */
export function makeBlob(name, { wins = 0, losses = 0, endian = 'le', corrupt = false } = {}) {
  const b = new Uint8Array(0x80);
  const dv = new DataView(b.buffer);
  const le = endian === 'le';
  b[0] = 30; b[1] = 10; dv.setUint16(2, 2026, le);
  dv.setInt32(4, wins, le); dv.setInt32(8, losses, le); dv.setUint16(0x0c, 2, le);
  for (let i = 0; i < name.length; i++) dv.setUint16(0x0e + i * 2, name.charCodeAt(i), le);
  b.set(hmacMd5(HMAC_KEY, b.subarray(0, 0x70)), 0x70);
  if (corrupt) b[5] ^= 1;
  return b64encode(b);
}

/** Run a full ranked-or-unranked room dance for two players and return the room codes / match id. */
export async function pairInRoom(srv, a, b) {
  const created = await srv.call('POST', '/v1/rooms', undefined, a.token);
  const code = created.data.code;
  const joined = await srv.call('POST', '/v1/rooms/join', { code }, b.token);
  return { code, created, joined, matchId: joined.data.matchId };
}

export async function startMatch(srv, a, b, code) {
  const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAAAAAAAAAA=' });
  await srv.call('POST', `/v1/rooms/${code}/endpoint`, ep(1), a.token);
  await srv.call('POST', `/v1/rooms/${code}/endpoint`, ep(2), b.token);
  await srv.call('POST', `/v1/rooms/${code}/started`, undefined, a.token);
  return srv.call('POST', `/v1/rooms/${code}/started`, undefined, b.token);
}
