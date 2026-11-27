// Test-only hostile service for tests/mp/service_fuzz.sh (attacker "e" of the security review: a malicious or compromised service).
// Every answer is a seeded mutation of what the real service would send: room views (members, endpoints, keys, slots, versions,
// punch times), info, friends, boards, room lists, challenges, registrations and errors, with wrong types, huge and negative numbers,
// NaN-looking strings, long strings, deep nesting, huge arrays, redirects to other hosts, absurd retryAfterSec, truncation and resets.
// Local only (127.0.0.1). Prints "fuzz http://127.0.0.1:PORT".
import http from 'node:http';

let seed = Number(process.argv[2] || 1) >>> 0 || 1;
const rnd = () => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
const pick = (a) => a[Math.floor(rnd() * a.length)];

const NUMS = [0, -1, 1, 3, 4, 7, 255, 65536, 2 ** 31, 2 ** 32, 2 ** 53, -(2 ** 53), 1e308, -1e308, 1.5, 0.0001, 1e-300];
const STRS = ['', 'x', 'A'.repeat(5000), '\u0000', '‮', '<script>', '../../', '%n%s', 'NaN', 'Infinity', '-1', '1e999', 'AAAA', '!!!!',
  '203.0.113.5:41000', '999.1.1.1:99999', '0.0.0.0:0', 'K7QM4D', 'K7QM-4DXT', 'é'.repeat(300)];
function junk(depth = 0) {
  const r = rnd();
  if (r < 0.2) return pick(NUMS);
  if (r < 0.4) return pick(STRS);
  if (r < 0.5) return rnd() < 0.5;
  if (r < 0.55) return null;
  if (depth > 3) return 1;
  if (r < 0.75) return Array.from({ length: Math.floor(rnd() * 6) }, () => junk(depth + 1));
  const o = {};
  for (let i = 0; i < 1 + Math.floor(rnd() * 5); i++) o[pick(['code', 'slot', 'pid', 'name', 'x', 'endpoint', 'state'])] = junk(depth + 1);
  return o;
}
/** Replace some fields of a valid object with junk, drop some, add some. */
function mutate(obj, rate = 0.25) {
  if (Array.isArray(obj)) {
    if (rnd() < 0.1) return Array.from({ length: 3000 }, () => obj[0] ?? 1);
    return obj.map((x) => (rnd() < rate ? junk() : mutate(x, rate)));
  }
  if (obj && typeof obj === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(obj)) {
      const r = rnd();
      if (r < rate * 0.3) continue;
      out[k] = r < rate ? junk() : mutate(v, rate);
    }
    if (rnd() < 0.1) out[pick(['extra', '__proto__', 'constructor'])] = junk();
    return out;
  }
  return rnd() < rate ? junk() : obj;
}

const member = (slot) => ({
  slot, pid: 1000 + slot, name: `P${slot}xx`, vip: `10.64.0.${slot + 1}`, link: pick(['ok', 'quiet', 'lost']),
  endpoint: { public: `203.0.113.${slot + 1}:41000`, local: `192.168.1.${slot + 1}:41000`, relay: null, nonce: 'AAECAwQFBgc=', nat: 'cone',
    candidates: [{ type: 'srflx', addr: `203.0.113.${slot + 1}:41001` }], ageMs: 10, stale: false },
  punchAtMs: Date.now() + 1000, steer: { relay: false, reason: null },
});
let version = 1;
const roomView = () => ({
  code: 'K7QM4D', game: 'smg', kind: 'room', mode: 'race', visibility: 'code', topology: 'star', maxMembers: 4, settings: { dropIn: true },
  state: pick(['open', 'joined', 'ready', 'started']), version: ++version, role: 'host', slot: 0, hostSlot: 0, hostEpoch: 0,
  createdMs: Date.now(), expiresMs: Date.now() + 600000, closedReason: null, you: { pid: 1000, name: 'P0xx', vip: '10.64.0.1' },
  members: [member(0), member(1), member(2)],
  session: { key: 'AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=', cookie: 12345, punchAtMs: Date.now() + 1000, punch: { intervalMs: 700, tries: 7 }, stun: ['stun.example.invalid:3478'] },
  started: { you: false, peer: false, room: false }, changed: true,
});
const base = {
  '/v1/info': () => ({ proto: 1, minProto: 1, game: 'smg', serverMs: Date.now(), kill: [], message: null, stun: ['stun.example.invalid:3478'], app: { min: '3.0.0', latest: '3.0.0' }, servers: [{ url: 'https://other.example.invalid' }] }),
  '/v1/register/challenge': () => ({ bits: pick([0, 1, 4, 30, -3, 1e9]), algo: 'sha256', challenge: pick(['1762800000000.0123456789abcdef.4.0123456789abcdef', 'x', '', 'A'.repeat(300)]) }),
  '/v1/register': () => ({ pid: 7, code: 'ABCD-EFGH', token: 'T'.repeat(43), name: 'Tester', region: 2 }),
  '/v1/friends': () => ({ friends: [{ code: 'K7QM-4DXT', name: 'Ben', mutual: true, online: true, room: { code: 'R9TQ2M', joinable: true, mode: 'race' } }], incoming: [{ code: 'K7QM-4DXA', name: 'Cy' }], max: 50 }),
  '/v1/times/get': () => ({ rows: [{ rank: 1, name: 'Ana', frames: 3600, players: 1, verified: false, you: true }], total: 1 }),
  '/v1/rooms/public': () => ({ rooms: [{ code: 'K7QM4D', players: 1, maxMembers: 4 }] }),
  '/v1/times': () => ({ ok: true, best: true, rank: 1 }),
  '/v1/me': () => ({ pid: 7, code: 'ABCD-EFGH', name: 'Tester', relayOnly: false }),
};
const errorBody = () => ({ error: mutate({ code: pick(['rate_limited', 'service_disabled', 'room_full', 'not_member', 'account_elsewhere', 'unauthorized', 'bad_pow', 'name_taken']), message: 'm', retryAfterSec: pick([1, 30, 1e9, -5, 1e308, 'x']), latestVersion: pick(['3.0.1', 'A'.repeat(500), 5]), reason: 'daily_cap' }) });

let served = 0;
const server = http.createServer((req, res) => {
  const chunks = [];
  req.on('data', (d) => chunks.push(d));
  req.on('end', () => {
    served++;
    const path = req.url.split('?')[0];
    const send = (status, body, headers = {}) => {
      const b = typeof body === 'string' ? body : JSON.stringify(body);
      res.writeHead(status, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(b), ...headers });
      res.end(b);
    };
    const r = rnd();
    if (r < 0.03) return req.socket.destroy();
    if (r < 0.06) { res.writeHead(302, { location: pick(['https://evil.example.invalid/v1/info', 'http://127.0.0.1:1/', '//x', 'A'.repeat(3000)]) }); return res.end(); }
    if (r < 0.09) { res.writeHead(200, { 'content-type': 'application/json', 'content-length': '5000' }); res.write('{"state":"open","members":['); return setTimeout(() => req.socket.destroy(), 5); }
    if (r < 0.12) return send(200, '['.repeat(20000) + ']'.repeat(20000));
    if (r < 0.25) return send(pick([400, 401, 403, 404, 409, 410, 426, 429, 500, 503]), errorBody());
    if (r < 0.3) return send(200, junk());
    const make = path.startsWith('/v1/rooms/') && path !== '/v1/rooms/public' ? roomView
      : path === '/v1/rooms' || path === '/v1/rooms/join' || path === '/v1/quick' ? roomView : base[path];
    const valid = make ? make() : { ok: true };
    return send(200, r < 0.4 ? valid : mutate(valid, rnd() < 0.5 ? 0.15 : 0.5));
  });
});
server.listen(0, '127.0.0.1', () => console.log(`fuzz http://127.0.0.1:${server.address().port}`));
