// Checks that need nothing but the request itself, shared by the Worker (so junk never wakes the Durable Object, which is
// where the free tier's request and row budgets are spent) and by the application (defence in depth, and the fake server).

import { ApiError, VERSION_RE, cmpVersion } from './util.js';
import { rawGame, adminTokenOk, versionGate } from './config.js';
import { LIVE_PATTERNS } from './budget.js';
import { REPORT_BODY_MAX } from './reports.js';

export const MAX_BODY = 8192;

/**
 * Every endpoint: [method, pattern, auth, open].
 * auth: 'none' | 'player' (Bearer token, 43 url-safe chars) | 'admin' (Bearer secret).
 * open: no version header required (info, time, servers and the two public leaderboard routes, which a browser calls). Open routes are never
 * switched off, except /v1/public/leaderboard, which follows the `ranking` kill switch (app.js: `web`).
 * app.js asserts (and a test checks) that its routing table is exactly this list.
 */
export const ROUTE_META = [
  ['GET', '/v1/info', 'none', true],
  ['GET', '/v1/servers', 'none', true],
  ['GET', '/v1/time', 'none', true],
  ['GET', '/v1/register/challenge', 'none', false],
  ['POST', '/v1/register', 'none', false],
  ['GET', '/v1/me', 'player', false],
  ['PUT', '/v1/me', 'player', false],
  ['DELETE', '/v1/me', 'player', false],
  ['GET', '/v1/me/matches', 'player', false],
  ['GET', '/v1/players/:code', 'player', false],
  ['POST', '/v1/rooms', 'player', false],
  ['POST', '/v1/rooms/join', 'player', false],
  ['GET', '/v1/rooms/:code', 'player', false],
  ['POST', '/v1/rooms/:code/endpoint', 'player', false],
  ['POST', '/v1/rooms/:code/punch', 'player', false],
  ['POST', '/v1/rooms/:code/started', 'player', false],
  ['POST', '/v1/rooms/:code/leave', 'player', false],
  ['DELETE', '/v1/rooms/:code', 'player', false],
  ['POST', '/v1/ranked/queue', 'player', false],
  ['POST', '/v1/ranked/queue/resume', 'player', false],
  ['GET', '/v1/ranked/queue', 'player', false],
  ['DELETE', '/v1/ranked/queue', 'player', false],
  ['POST', '/v1/matches/:id/result', 'player', false],
  ['GET', '/v1/matches/:id', 'player', false],
  ['POST', '/v1/turn', 'player', false],
  ['POST', '/v1/turn/usage', 'player', false],
  ['POST', '/v1/rank/put', 'player', false],
  ['POST', '/v1/rank/get', 'player', false],
  ['GET', '/v1/public/leaderboard', 'none', true],
  ['GET', '/leaderboard', 'none', true],
  ['GET', '/v1/admin/stats', 'admin', false],
  ['GET', '/v1/admin/export', 'admin', false],
  ['POST', '/v1/admin/import', 'admin', false],
  ['POST', '/v1/admin/switches', 'admin', false],
  ['POST', '/v1/admin/ban', 'admin', false],
  ['GET', '/v1/admin/flags', 'admin', false],
  ['POST', '/v1/admin/flags/clear', 'admin', false],
];

/**
 * GAME=smg (Galaxy, worker smg-online): accounts, rooms of up to 4, quick match, friends + presence, star times. No DWC ranking, no
 * ranked queue, no match records, no Strikers web board. Order matters where two patterns match one path (/v1/rooms/public before
 * /v1/rooms/:code): the first route with the right method wins, in the app and here.
 */
export const SMG_ROUTE_META = [
  ['GET', '/v1/info', 'none', true],
  ['GET', '/v1/prove', 'none', true],
  ['GET', '/v1/servers', 'none', true],
  ['GET', '/v1/time', 'none', true],
  ['GET', '/v1/register/challenge', 'none', false],
  ['POST', '/v1/register', 'none', false],
  ['GET', '/v1/me', 'player', false],
  ['PUT', '/v1/me', 'player', false],
  ['DELETE', '/v1/me', 'player', false],
  ['GET', '/v1/players/:code', 'player', false],
  ['POST', '/v1/rooms', 'player', false],
  ['POST', '/v1/rooms/join', 'player', false],
  ['POST', '/v1/rooms/:code/accept', 'player', false],
  ['GET', '/v1/rooms/public', 'player', false],
  ['GET', '/v1/rooms/:code', 'player', false],
  ['POST', '/v1/rooms/:code/endpoint', 'player', false],
  ['POST', '/v1/rooms/:code/punch', 'player', false],
  ['POST', '/v1/rooms/:code/started', 'player', false],
  ['POST', '/v1/rooms/:code/leave', 'player', false],
  ['DELETE', '/v1/rooms/:code', 'player', false],
  ['POST', '/v1/invites', 'player', false],
  ['POST', '/v1/invites/resolve', 'player', false],
  ['POST', '/v1/invites/:code/keep', 'player', false],
  ['POST', '/v1/invites/:code/close', 'player', false],
  ['DELETE', '/v1/invites/:code', 'player', false],
  ['POST', '/v1/quick', 'player', false],
  ['GET', '/v1/friends', 'player', false],
  ['POST', '/v1/friends', 'player', false],
  ['POST', '/v1/friends/remove', 'player', false],
  ['POST', '/v1/friends/dismiss', 'player', false],
  ['POST', '/v1/friends/block', 'player', false],
  ['POST', '/v1/friends/unblock', 'player', false],
  ['DELETE', '/v1/friends/:code', 'player', false],
  ['POST', '/v1/times', 'player', false],
  ['POST', '/v1/times/get', 'player', false],
  ['POST', '/v1/turn', 'player', false],
  ['POST', '/v1/turn/usage', 'player', false],
  ['POST', '/v1/turn/check', 'player', false],
  ['POST', '/v1/report', 'player', false],
  ['GET', '/v1/public/times', 'none', true],
  ['GET', '/times', 'none', true],
  ['POST', '/v1/admin/turn-selfcheck', 'admin', false],
  ['GET', '/v1/admin/stats', 'admin', false],
  ['GET', '/v1/admin/export', 'admin', false],
  ['POST', '/v1/admin/import', 'admin', false],
  ['POST', '/v1/admin/switches', 'admin', false],
  ['POST', '/v1/admin/ban', 'admin', false],
  ['GET', '/v1/admin/flags', 'admin', false],
  ['POST', '/v1/admin/flags/clear', 'admin', false],
  ['GET', '/v1/admin/times', 'admin', false],
  ['POST', '/v1/admin/times/remove', 'admin', false],
  ['POST', '/v1/admin/times/approve', 'admin', false],
  ['POST', '/v1/admin/times/ban', 'admin', false],
  ['GET', '/v1/admin/reports', 'admin', false],
  ['GET', '/v1/admin/reports/:id', 'admin', false],
  ['GET', '/v1/admin/summaries', 'admin', false],
];

export const routeMetaFor = (game) => (game === 'smg' ? SMG_ROUTE_META : ROUTE_META);
export const maxBodyFor = (game) => (game === 'smg' ? 4096 : MAX_BODY);
/** Routes with their own body cap (bytes): a problem report carries up to 48,000 bytes of log. "<METHOD> <pattern>" -> cap. Galaxy only. */
export const ROUTE_BODY_CAPS = { 'POST /v1/report': REPORT_BODY_MAX };
export const bodyCapFor = (game, method, pattern) => (game === 'smg' && ROUTE_BODY_CAPS[`${method} ${pattern}`]) || maxBodyFor(game);

/** pattern -> RegExp with named groups; one path segment is 1..64 characters. */
export const compilePattern = (pattern) => new RegExp(`^${pattern.replace(/:(\w+)/g, '(?<$1>[^/]{1,64})')}$`);

const compileAll = (meta) => meta.map(([method, pattern, auth, open]) => ({ method, pattern, auth, open, re: compilePattern(pattern) }));
const COMPILED = { msc: compileAll(ROUTE_META), smg: compileAll(SMG_ROUTE_META) };
const GAME_RE = /^(msc|smg)$/;

/**
 * X-Msc-Game: which game the client is (absent = msc, so every Strikers client keeps working unchanged). A malformed value is
 * 400 bad_game; a request for the other game is 400 game_mismatch. Open routes (a browser sends no header) accept a missing header.
 * Admin routes are not checked (the backup scripts send none).
 */
export function gameHeaderError(headers, game, { open }) {
  const g = headers.get('x-msc-game');
  if (g !== null && !GAME_RE.test(g)) return new ApiError(400, 'bad_game', 'X-Msc-Game must be msc or smg', { field: 'x-msc-game' });
  if (g === null && open) return null;
  if ((g === null ? 'msc' : g) !== game) {
    return new ApiError(400, 'game_mismatch', 'this service is for another game', { game });
  }
  return null;
}

export const PLAYER_AUTH_RE = /^Bearer [A-Za-z0-9_-]{43}$/;
export const ADMIN_AUTH_RE = /^Bearer [\x21-\x7e]{1,256}$/;
const PROTO_RE = /^[0-9]{1,4}$/;
const BUILD_RE = /^[0-9a-fA-F]{8,64}$/;

/**
 * Format checks of the version headers. Returns an ApiError or null.
 * A malformed X-Msc-Proto used to be ignored (Number('abc') is NaN, which passed every comparison): version gating failed open.
 */
export function versionHeaderError(headers, { requireApp, open }) {
  const proto = headers.get('x-msc-proto');
  if (proto !== null && !PROTO_RE.test(proto)) {
    return new ApiError(400, 'bad_app_version', 'X-Msc-Proto must be a whole number such as 1', { field: 'x-msc-proto' });
  }
  const app = headers.get('x-msc-app');
  if (app !== null && !VERSION_RE.test(app)) return new ApiError(400, 'bad_app_version', 'X-Msc-App must look like 1.2.0');
  if (app === null && requireApp && !open) return new ApiError(400, 'bad_app_version', 'X-Msc-App header is required');
  const build = headers.get('x-msc-build');
  if (build !== null && !BUILD_RE.test(build)) return new ApiError(400, 'bad_app_version', 'X-Msc-Build must be a hex build hash');
  return null;
}

/**
 * What the Worker may decide without the object: unknown path, wrong method, oversized body, malformed Authorization
 * header, malformed or missing version headers. Returns an ApiError or null (= forward to the object).
 * @param {Request} request
 * @param {{ADMIN_TOKEN?: string, REQUIRE_APP_VERSION?: string}} [env]
 */
export function edgeCheck(request, env = {}) {
  const path = new URL(request.url).pathname;
  const raw = rawGame(env);
  if (raw !== undefined && raw !== 'msc' && raw !== 'smg') return new ApiError(500, 'server_misconfigured', 'unknown SERVICE_GAME');
  const game = raw === 'smg' ? 'smg' : 'msc';
  let route = null;
  const allow = [];
  for (const r of COMPILED[game]) {
    if (!r.re.test(path)) continue;
    if (!allow.includes(r.method)) allow.push(r.method);
    if (r.method === request.method && !route) route = r;
  }
  if (!route) {
    if (allow.length) return new ApiError(405, 'method_not_allowed', 'method not allowed', {}, { allow: allow.join(', ') });
    return new ApiError(404, 'not_found', 'no such endpoint');
  }
  // A body must announce its size (a chunked upload has no Content-Length: 411), and the announced size is capped. The object also reads the
  // stream with a hard cap (readBodyCapped), so a lying length cannot get past it either.
  if (request.method === 'POST' || request.method === 'PUT') {
    const cl = request.headers.get('content-length');
    if (cl === null ? request.body !== null : !/^\d{1,9}$/.test(cl)) {
      return new ApiError(411, 'length_required', 'Content-Length is required');
    }
  }
  if (Number(request.headers.get('content-length') || 0) > bodyCapFor(game, route.method, route.pattern)) return new ApiError(413, 'too_large', 'request body too large');
  const auth = request.headers.get('authorization') || '';
  if (route.auth === 'admin') {
    if (!adminTokenOk(env.ADMIN_TOKEN)) return new ApiError(404, 'not_found', 'no such endpoint');
    if (!ADMIN_AUTH_RE.test(auth)) return new ApiError(401, 'unauthorized', 'bad admin token', {}, { 'www-authenticate': 'Bearer' });
    return null;
  }
  const verr = versionHeaderError(request.headers, { requireApp: env.REQUIRE_APP_VERSION === '1', open: route.open });
  if (verr) return verr;
  const gerr = gameHeaderError(request.headers, game, { open: route.open });
  if (gerr) return gerr;
  // secreview32 (F1): the version gate itself, not only the headers' format. The object throws these two 426 answers BEFORE it looks at the
  // token or charges any per-address ceiling, so a request with an old X-Msc-App (or X-Msc-Proto: 0) used to cost one Durable Object
  // request each, without limit. Answered here it costs none. Same order, codes and fields as app.js (which keeps its own check).
  const gate = versionGate(env);
  const proto = request.headers.get('x-msc-proto');
  if (proto !== null && Number(proto) < gate.minProto) return new ApiError(426, 'upgrade_required', 'this client is too old for the server', { minProto: gate.minProto });
  const app = request.headers.get('x-msc-app');
  if (app !== null && gate.minApp && cmpVersion(app, gate.minApp) < 0) return new ApiError(426, 'update_required', 'please update the game', { minVersion: gate.minApp });
  if (route.auth === 'player' && !PLAYER_AUTH_RE.test(auth)) {
    return new ApiError(401, 'unauthorized', 'missing or malformed token', {}, { 'www-authenticate': 'Bearer' });
  }
  return null;
}

/**
 * The class of a request for the per-address ceilings (budget.js): 'open' (no token), 'player' (a token), 'live' (a token, on a room or relay
 * route a running session uses), 'admin'. Decided from the route alone, so the Worker and the object agree. null when no route matches.
 */
export function requestClass(request, env = {}) {
  const path = new URL(request.url).pathname;
  const game = rawGame(env) === 'smg' ? 'smg' : 'msc';
  for (const r of COMPILED[game]) {
    if (r.method !== request.method || !r.re.test(path)) continue;
    if (r.auth === 'admin') return 'admin';
    if (r.auth === 'none') return 'open';
    return game === 'smg' && LIVE_PATTERNS.has(r.pattern) ? 'live' : 'player';
  }
  return null;
}

/** "<METHOD> <pattern>" of the route a request matches (a stable key for the Worker's per-route verdicts: the path itself carries room codes), or null. */
export function routeKey(request, env = {}) {
  const path = new URL(request.url).pathname;
  const game = rawGame(env) === 'smg' ? 'smg' : 'msc';
  for (const r of COMPILED[game]) if (r.method === request.method && r.re.test(path)) return `${r.method} ${r.pattern}`;
  return null;
}

/** Requests that FREE resources (a member leaving, the host closing the room): the budget never refuses them, in the Worker or the object. */
export function isFreeing(request, env = {}) {
  if (rawGame(env) !== 'smg') return false;
  const path = new URL(request.url).pathname;
  return (request.method === 'POST' && /^\/v1\/rooms\/[^/]{1,64}\/leave$/.test(path)) || (request.method === 'DELETE' && /^\/v1\/rooms\/[^/]{1,64}$/.test(path) && path !== '/v1/rooms/join')
    || (request.method === 'DELETE' && /^\/v1\/invites\/[^/]{1,64}$/.test(path)) || (request.method === 'POST' && /^\/v1\/invites\/[^/]{1,64}\/close$/.test(path)); // code5: the host closing its invite
}

/** Join and times: live traffic for some callers, so the Worker's "new sessions refused" answer does not apply to them (the object decides). */
export function isMaybeLive(request, env = {}) {
  if (rawGame(env) !== 'smg' || request.method !== 'POST') return false;
  const p = new URL(request.url).pathname;
  return p === '/v1/rooms/join' || p === '/v1/times' || p === '/v1/invites/resolve'; // (code5: a friend using a code that was issued before the budget ran short)
}
