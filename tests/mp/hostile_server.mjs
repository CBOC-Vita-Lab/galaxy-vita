// Test-only HTTP server for tests/mp/service_errors_test.cpp: answers every request the way a broken or hostile service (or a proxy in
// front of it) might. The first path segment is the mode, the rest is the real path:  http://127.0.0.1:PORT/<mode>/v1/friends
// Prints "hostile http://127.0.0.1:PORT" when ready. Local only; never run it against anything real.
import http from 'node:http';

const TOKEN = 'T'.repeat(43);
const json = (res, status, obj, headers = {}) => {
  const b = typeof obj === 'string' ? obj : JSON.stringify(obj);
  res.writeHead(status, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(b), ...headers });
  res.end(b);
};
const err = (res, status, code, extra = {}, headers = {}) => json(res, status, { error: { code, message: code, ...extra } }, headers);

const hits = {};   // quota32: requests seen per "<mode><path>" (GET /hits/x reads them)
const server = http.createServer((req, res) => {
  const chunks = [];
  req.on('data', (d) => chunks.push(d));
  req.on('end', () => {
    const m = /^\/([a-z0-9]+)(\/.*)$/.exec(req.url.split('?')[0]);
    const mode = m ? m[1] : 'none';
    const path = m ? m[2] : req.url;
    hits[mode + path] = (hits[mode + path] || 0) + 1;
    let body = {};
    try { body = JSON.parse(Buffer.concat(chunks).toString() || '{}'); } catch { /* keep {} */ }
    const ok = () => {
      if (path === '/v1/register') return json(res, 200, { pid: 7, code: 'ABCD-EFGH', token: TOKEN, name: body.name || 'Tester', region: 2 });
      if (path === '/v1/friends' && req.method === 'GET') return json(res, 200, { friends: [{ code: 'K7QM-4DXT', name: 'Ben', mutual: true, online: true, room: { code: 'R9TQ2M', joinable: true, mode: 'race' } }], incoming: [] });
      if (path === '/v1/friends') return json(res, 200, { ok: true, added: true });
      if (path === '/v1/friends/remove') return json(res, 200, { ok: true, removed: true });
      if (path === '/v1/times/get') return json(res, 200, { rows: [{ rank: 1, name: 'Ana', frames: 3600, players: 1, verified: false, you: true }], total: 1 });
      if (path === '/v1/times') return json(res, 200, { ok: true, best: true, rank: 1 });
      if (path === '/v1/rooms/public') return json(res, 200, { rooms: [{ code: 'K7QM4D', players: 1, maxMembers: 4 }] });
      return err(res, 404, 'not_found');
    };
    switch (mode) {
      case 'ok': return ok();
      case 'hits': return json(res, 200, hits);
      // quota32: Cloudflare's own answers when the free plan's day is spent (not our JSON): 1027 page (429), a 1101 page (500, the object refused the
      // call on an older Worker), the Worker's own 503 over_budget
      case 'cf1027': res.writeHead(429, { 'content-type': 'text/plain; charset=UTF-8', 'retry-after': '3600' }); return res.end('error code: 1027');
      case 'cf1027html': res.writeHead(429, { 'content-type': 'text/html' }); return res.end('<!DOCTYPE html><html><head><title>Error 1027</title></head><body><h1>Error 1027</h1><p>Worker exceeded free tier daily request limit.</p></body></html>');
      case 'cf1101': res.writeHead(500, { 'content-type': 'text/html' }); return res.end('<!DOCTYPE html><html><body>Error 1101 Worker threw exception</body></html>');
      case 'overbudget': return err(res, 503, 'over_budget', { retryAfterSec: 120, reason: 'object_unavailable' }, { 'retry-after': '120' });
      case 's500': return err(res, 500, 'internal');
      case 's502': res.writeHead(502, { 'content-type': 'text/html' }); return res.end('<html>Bad gateway</html>');
      case 's503': return err(res, 503, 'service_disabled', { retryAfterSec: 300 });
      case 's429': return err(res, 429, 'rate_limited', { retryAfterSec: 30 }, { 'retry-after': '30' });
      case 's401': return err(res, 401, 'unauthorized');
      case 's403': return err(res, 403, 'banned');
      case 's404': return err(res, 404, 'not_found');
      case 's426': return err(res, 426, 'update_required', { minVersion: '9.9.9' });
      case 's409full': return err(res, 409, 'room_full');
      case 's400name': return err(res, 400, 'bad_name', { reason: 'filtered' });
      case 's400mismatch': return err(res, 400, 'game_mismatch');
      case 'bad200': return json(res, 200, 'this is not json');
      case 'html200': res.writeHead(200, { 'content-type': 'text/html' }); return res.end('<html><body>Sign in to the hotspot</body></html>');
      case 'empty200': res.writeHead(200, { 'content-length': '0' }); return res.end();
      case 'redirect': res.writeHead(302, { location: 'http://captive.invalid/' }); return res.end();
      case 'trunc': res.writeHead(200, { 'content-type': 'application/json', 'content-length': '1000' }); res.write('{"friends":['); return setTimeout(() => req.socket.destroy(), 20);
      case 'rst': return req.socket.destroy();
      case 'huge': { const b = '{"x":"' + 'a'.repeat(400 * 1024) + '"}'; return json(res, 200, b); }
      case 'hang': return; // never answers
      case 'deep': return json(res, 200, '['.repeat(50000) + ']'.repeat(50000));
      case 'manynodes': return json(res, 200, { friends: Array.from({ length: 3000 }, (_, i) => ({ code: 'K7QM-4DXT', name: `F${i}x`, mutual: true, online: true })), rows: Array.from({ length: 3000 }, (_, i) => ({ rank: i + 1, name: 'Ana', frames: 1, players: 1 })), rooms: Array.from({ length: 3000 }, () => ({ code: 'K7QM4D', players: 1, maxMembers: 4 })) });
      case 'wrongtypes': return json(res, 200, { friends: 'x', rows: { a: 1 }, rooms: 5, pid: 'x', token: 5, name: [], code: {} });
      case 'hostilerows': {
        const names = ['<script>', 'A'.repeat(500), 'éé', '', 'ok', 'Fine Name', 'x'.repeat(11), 'a\nb'];
        return json(res, 200, {
          friends: names.map((n) => ({ code: 'K7QM-4DXT', name: n, mutual: true, online: true })).concat([{ code: 'nope', name: 'Ben' }, { code: 5, name: 'Ben' }, null, 7, 'x', [], { name: 'Ben' }]),
          rows: names.map((n) => ({ rank: 1, name: n, frames: 1, players: 1 })).concat([{ rank: -1, name: 'Ana', frames: 1, players: 1 }, { rank: 1, name: 'Ana', frames: 1e20, players: 1 }, { rank: 1, name: 'Ana', frames: 1, players: 9 }, { rank: 1e12, name: 'Ana', frames: 1, players: 1 }, { rank: 'x', name: 'Ana', frames: 1, players: 1 }]),
        });
      }
      case 'badregister': return json(res, 200, { pid: 7, code: 'ABCD-EFGH', token: 'short', name: 'Tester' });
      case 'badregister2': return json(res, 200, { pid: -3, code: 'ABCD-EFGH', token: TOKEN, name: 'Tester' });
      // a service that asks for more work than a console should do (svcfix2: the client refuses above mp_pow::kMaxBits = 17, about 10 s)
      case 'powhard': return path === '/v1/register/challenge' ? json(res, 200, { bits: 20, algo: 'sha256', challenge: '1794000000000.0123456789abcdef.20.0123456789abcdef' }) : ok();
      case 'badregister3': return json(res, 200, { pid: 7, code: 'ABCD-EFGH', token: TOKEN, name: '<b>Tester</b>' });
      default: return err(res, 404, 'not_found');
    }
  });
});
server.listen(0, '127.0.0.1', () => console.log(`hostile http://127.0.0.1:${server.address().port}`));
