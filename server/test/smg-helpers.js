// Galaxy (GAME=smg) test harness: the real application with GAME=smg, every call carrying the Galaxy client's headers.
import { makeServer } from './helpers.js';
import { makeD1 } from './stubs/d1.js';

export const SMG_HEADERS = { 'x-msc-game': 'smg', 'x-msc-app': '3.0.0' };

export const LEGACY_TIMES = { SMG_BOARD_UNVERIFIED: 'show', SMG_FLAG_NEW_STAR: '0', SMG_VERIFY_MIN_AGE_HOURS: '0' };
/** The production defaults of the finding-13 checks (overrides LEGACY_TIMES). */
export const REVIEW_TIMES = { SMG_BOARD_UNVERIFIED: 'hide', SMG_FLAG_NEW_STAR: '1', SMG_VERIFY_MIN_AGE_HOURS: '48', SMG_VERIFY_SLACK_SEC: '3' };

export function makeSmg(envOverrides = {}, opts = {}) {
  // SMG_VERIFY_SLACK_SEC: tests start a room and submit at once; a verified time must fit in the room's age + this slack (times.js)
  // REGISTER_POW_BITS 0: Galaxy turns the proof of work on by default (15 bits); the suites that test it set it themselves
  // LEGACY_TIMES: the board mechanics suites run with the finding-13 checks off (unverified rows shown, no time_new_star flag, no
  // account age); test/core/smg-forgery.test.js tests the production defaults (it passes the opposite values)
  const srv = makeServer({ GAME: 'smg', REPORTS_DB: makeD1(), // the beta's report store (a D1 stand-in; a suite may pass its own or null)
     SMG_VERIFY_SLACK_SEC: '600', REGISTER_POW_BITS: '0', ...LEGACY_TIMES, ...envOverrides }, opts);
  const raw = srv.call;
  srv.raw = raw;
  srv.d1 = srv.env.REPORTS_DB;
  // An honest client's endpoint post comes from the address it names as `public` (rooms4.postEndpoint checks that): the harness
  // rotates source addresses per call, so it sets the source to the posted public address unless a test passes its own `ip`.
  srv.call = (method, path, body, token, headers = {}) => {
    const h = { ...SMG_HEADERS, ...headers };
    if (method === 'POST' && /\/endpoint$/.test(path) && body && typeof body.public === 'string' && !h.ip) h.ip = body.public.split(':')[0];
    return raw(method, path, body, token, h);
  };
  return srv;
}

export async function smgSignUp(srv, name) {
  const r = await srv.call('POST', '/v1/register', { name });
  if (r.status !== 200) throw new Error(`register ${name} failed: ${JSON.stringify(r.data)}`);
  return r.data;
}

export const ep = (n) => ({ public: `203.0.113.${n}:41100`, local: `192.168.1.${n}:41100`, nonce: 'AAAAAAAAAAA=' });

/** A room of `players.length` members: players[0] hosts, the others join by code. Returns the code. */
export async function roomOf(srv, players, create = {}, opts = {}) {
  const made = await srv.call('POST', '/v1/rooms', create, players[0].token);
  if (made.status !== 200) throw new Error(`create failed: ${JSON.stringify(made.data)}`);
  for (const p of players.slice(1)) {
    const j = await srv.call('POST', '/v1/rooms/join', { code: made.data.code }, p.token);
    if (j.status !== 200) throw new Error(`join failed: ${JSON.stringify(j.data)}`);
    // (svcfix2 item 8: the host's console accepts a stranger who came with a code; these tests are about everything after that)
    if (opts.accept !== false) await srv.call('POST', `/v1/rooms/${made.data.code}/accept`, { slot: j.data.slot }, players[0].token);
  }
  return made.data.code;
}

export async function friends(srv, a, b) {
  await srv.call('POST', '/v1/friends', { code: b.code }, a.token);
  await srv.call('POST', '/v1/friends', { code: a.code }, b.token);
}
