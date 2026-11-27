// Test-only HTTP proxy in front of the local fake service (tests/mp/signal_backoff_test.sh): passes requests through, or fails them the
// way an exhausted or overloaded Cloudflare service does. Local only (127.0.0.1); never point it at anything real.
//   node flaky_proxy.mjs <target http://127.0.0.1:PORT> <log file>
// Control: GET /__mode/<mode>   pass | s503 (JSON 503) | s429 (JSON, retryAfterSec from /__mode/s429-<sec>) | html429 (Cloudflare-style
// "1015" page, no JSON) | s500 | hang (never answers) | rst (connection reset) | over (JSON 503 over_budget, retryAfterSec from /__mode/over-<sec>,
// x-msc-budget: over) | pace (passes through, but adds "pollScale":1.4 to every 200 JSON answer of a room: the service asking the clients to stretch their polls). Every non-control request appends "<ms> <method> <path>" to the log.
import http from 'node:http';
import { appendFileSync, writeFileSync } from 'node:fs';

const [target, logFile] = process.argv.slice(2);
const t = new URL(target);
if (t.hostname !== '127.0.0.1') throw new Error('local targets only');
writeFileSync(logFile, '');
let mode = 'pass';
let retryAfter = 30;

const server = http.createServer((req, res) => {
  const m = /^\/__mode\/([a-z0-9-]+)$/.exec(req.url);
  if (m) {
    const [name, sec] = m[1].split('-');
    mode = name;
    if (sec) retryAfter = Number(sec);
    res.writeHead(200, { 'content-length': '2' });
    return res.end('ok');
  }
  appendFileSync(logFile, `${Date.now()} ${req.method} ${req.url} ua=${(req.headers['user-agent'] || '-').replace(/\s/g, '_')}\n`);
  const chunks = [];
  req.on('data', (d) => chunks.push(d));
  req.on('end', () => {
    const json = (status, obj, h = {}) => {
      const b = JSON.stringify(obj);
      res.writeHead(status, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(b), ...h });
      res.end(b);
    };
    switch (mode) {
      case 's503': return json(503, { error: { code: 'service_disabled', message: 'off', retryAfterSec: 300 } });
      case 'over': return json(503, { error: { code: 'over_budget', message: 'online play is busy, please try again later', retryAfterSec: retryAfter } }, { 'retry-after': String(retryAfter), 'x-msc-budget': 'over' });
      case 's500': return json(500, { error: { code: 'internal', message: 'internal error' } });
      case 's429': return json(429, { error: { code: 'rate_limited', message: 'slow down', retryAfterSec: retryAfter } }, { 'retry-after': String(retryAfter) });
      case 'html429': {
        const b = '<html><head><title>Error 1015</title></head><body>You are being rate limited</body></html>';
        res.writeHead(429, { 'content-type': 'text/html', 'content-length': Buffer.byteLength(b) });
        return res.end(b);
      }
      case 'cf1027': {   // quota32: the free plan's daily limit: Cloudflare answers itself (HTTP 429, not our JSON)
        const b = 'error code: 1027';
        res.writeHead(429, { 'content-type': 'text/plain; charset=UTF-8', 'content-length': Buffer.byteLength(b) });
        return res.end(b);
      }
      case 'hang': return undefined;
      case 'rst': return req.socket.destroy();
      default: {
        const body = Buffer.concat(chunks);
        const up = http.request({ host: t.hostname, port: t.port, method: req.method, path: req.url, headers: req.headers }, (ur) => {
          if (mode === 'pace' && ur.statusCode === 200 && /^\/v1\/rooms\//.test(req.url)) {
            const parts = [];
            ur.on('data', (d) => parts.push(d));
            ur.on('end', () => {
              let b = Buffer.concat(parts).toString();
              try { const o = { ...JSON.parse(b), pollScale: 1.4 }; delete o.poll; b = JSON.stringify(o); } catch { /* not JSON: as is */ } // (a service with pacing but without the poll plan: the old rhythm)
              const h = { ...ur.headers, 'content-length': String(Buffer.byteLength(b)) };
              res.writeHead(200, h);
              res.end(b);
            });
            return;
          }
          res.writeHead(ur.statusCode, ur.headers);
          ur.pipe(res);
        });
        up.on('error', () => { res.writeHead(502); res.end(); });
        up.end(body);
      }
    }
  });
});
server.listen(0, '127.0.0.1', () => console.log(`proxy http://127.0.0.1:${server.address().port}`));
