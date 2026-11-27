#!/usr/bin/env node
// Local fake of the online service for the Vita client's host tests.
//
// It is NOT a mock: it runs the very same application code as the Worker (src/core) over an in-memory SQLite
// database, behind plain HTTP and HTTPS. The HTTPS side uses a throw-away root certificate generated on first
// start (needs the `openssl` command), so the client's real certificate checks can be exercised: load
// fake/certs/root.pem (or root.der) as the only trusted root.
//
//   node fake/server.mjs [--host 127.0.0.1] [--http-port 8787] [--https-port 8788] [--db :memory:|file.sqlite]
//
// Extras for tests (loopback only, outside the real protocol):
//   POST /__fake/clock   {"offsetMs": 600000} | {"setMs": 1790000000000} | {"reset": true}   move the server clock
//   POST /__fake/reset                                                                       wipe all data
//   POST /__fake/fault   {"latencyMs": 250, "failRate": 0.1, "dropNext": 1}                  add delay / 503s / hang-ups
//   GET  /__fake/status                                                                      ports, clock offset, faults
// Galaxy: GAME=smg node fake/server.mjs --no-https --http-port 8790   (SMG_* variables pass through too; see SMG-PROTOCOL.txt)
// Environment: ADMIN_TOKEN (default "fake-admin-token-0123456789"; at least 24 characters, a shorter one leaves the admin routes off), RANK_HMAC_KEY_HEX (default: read from the game source),
// FAKE_RATE=1 to enable the real rate limits, FAKE_SAN="DNS:foo,IP:10.0.0.5" for extra certificate names,
// plus any variable the Worker understands (see README.md), e.g. KILL_SWITCH, RANK_PUT_POLICY.

import http from 'node:http';
import https from 'node:https';
import { readFileSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';
import { randomBytes } from 'node:crypto';
import { createApp } from '../src/core/app.js';
import { openNodeSql } from '../src/sqlite-node.js';
import { extractKeyOr, DEFAULT_SOURCE } from '../scripts/extract-hmac-key.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

/** Creates root + leaf certificates (once) and returns their paths. */
export function ensureCerts(dir = join(HERE, 'certs'), extraSan = process.env.FAKE_SAN || '') {
  const p = (f) => join(dir, f);
  if (existsSync(p('server.pem')) && existsSync(p('root.pem'))) return { dir, root: p('root.pem'), rootDer: p('root.der'), key: p('server.key'), cert: p('server.pem') };
  mkdirSync(dir, { recursive: true });
  const run = (...args) => execFileSync('openssl', args, { stdio: ['ignore', 'ignore', 'pipe'] });
  const san = ['DNS:localhost', 'IP:127.0.0.1', 'IP:::1', ...extraSan.split(',').map((s) => s.trim()).filter(Boolean)].join(',');
  run('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '3650', '-sha256', '-keyout', p('root.key'), '-out', p('root.pem'),
    '-subj', '/CN=MSC Fake Test Root', '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  run('req', '-newkey', 'rsa:2048', '-nodes', '-keyout', p('server.key'), '-out', p('server.csr'), '-subj', '/CN=localhost');
  writeFileSync(p('ext.cnf'), `basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=${san}\n`);
  run('x509', '-req', '-in', p('server.csr'), '-CA', p('root.pem'), '-CAkey', p('root.key'), '-CAcreateserial', '-out', p('server.pem'),
    '-days', '825', '-sha256', '-extfile', p('ext.cnf'));
  run('x509', '-in', p('root.pem'), '-outform', 'DER', '-out', p('root.der'));
  return { dir, root: p('root.pem'), rootDer: p('root.der'), key: p('server.key'), cert: p('server.pem') };
}

function defaultHmac() {
  return extractKeyOr(DEFAULT_SOURCE, randomBytes(32).toString('hex'));
}

/**
 * @param {{host?: string, httpPort?: number, httpsPort?: number, db?: string, env?: object, certDir?: string, https?: boolean}} opts
 */
export async function startFake(opts = {}) {
  const host = opts.host || '127.0.0.1';
  const loopback = ['127.0.0.1', '::1', 'localhost'].includes(host);
  const env = {
    ADMIN_TOKEN: 'fake-admin-token-0123456789',
    RANK_HMAC_KEY_HEX: defaultHmac(),
    TURN_PROVIDER: 'stub',
    ALLOW_TEST_CLOCK: '1',
    RATE_LIMIT_SCALE: process.env.FAKE_RATE === '1' ? '1' : '1000',
    // the host test scripts finish matches instantly and repeat the same pair: abuse controls are off unless the variable is set
    MIN_MATCH_SEC: '0', RATED_PER_PAIR_DAY: '0', RATED_PER_ACCOUNT_DAY: '0', ROOMS_PER_ACCOUNT_DAY: '0', MATCHES_PER_ACCOUNT_DAY: '0',
    ...Object.fromEntries(Object.entries(process.env).filter(([k]) => /^(GAME|SMG_|KILL_SWITCH|RANK_|TURN_|ROOM_|QUEUE_|RESULT_|MAX_|NAME_|FORFEIT|ABANDON|ELO|PUNCH|STUN|MIN_CLIENT|MIN_APP|LATEST_APP|REQUIRE_APP|MIN_MATCH|MAINTENANCE|RENAME|ADMIN_TOKEN|MATCH_|RATED_|ONE_OPP|DODGE_|REGISTER_|ROOMS_PER|MATCHES_PER|DAILY_REQUEST|LIVE_RESERVE|ADDR_DAILY|PID_DAILY|POLL_PACE|PRUNE_|ACCOUNT_)/.test(k))),
    ...(opts.env || {}),
  };

  let store;
  let app;
  const clock = { offset: 0 };
  const fault = { latencyMs: 0, failRate: 0, dropNext: 0 };
  const boot = () => {
    store?.close();
    store = openNodeSql(opts.db || ':memory:');
    app = createApp({ sql: store.sql, tx: store.tx, env, now: () => Date.now() + clock.offset });
  };
  boot();
  const sweeper = setInterval(() => { try { app.sweep(); } catch { /* keep serving */ } }, 5000);
  sweeper.unref();

  async function handler(req, res, scheme) {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const body = Buffer.concat(chunks);
    const remote = (req.socket.remoteAddress || '').replace(/^::ffff:/, '');
    const url = new URL(req.url, `${scheme}://${req.headers.host || 'localhost'}`);

    if (url.pathname.startsWith('/__fake/')) {
      if (!loopback && !opts.allowControl) { res.writeHead(404).end(); return; }
      const j = body.length ? JSON.parse(body.toString('utf8')) : {};
      const reply = (o) => res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(o));
      if (url.pathname === '/__fake/clock') {
        if (j.reset) clock.offset = 0;
        else if (typeof j.setMs === 'number') clock.offset = j.setMs - Date.now();
        else if (typeof j.offsetMs === 'number') clock.offset += j.offsetMs;
        return reply({ offsetMs: clock.offset, nowMs: Date.now() + clock.offset });
      }
      if (url.pathname === '/__fake/reset') { boot(); return reply({ ok: true }); }
      if (url.pathname === '/__fake/fault') { Object.assign(fault, { latencyMs: 0, failRate: 0, dropNext: 0 }, j); return reply({ ...fault }); }
      if (url.pathname === '/__fake/status') return reply({ clockOffsetMs: clock.offset, fault, ports: { http: info.httpPort, https: info.httpsPort } });
      res.writeHead(404).end();
      return;
    }

    if (fault.dropNext > 0) { fault.dropNext--; req.socket.destroy(); return; }
    if (fault.latencyMs) await new Promise((r) => setTimeout(r, fault.latencyMs));
    if (fault.failRate && Math.random() < fault.failRate) {
      res.writeHead(503, { 'content-type': 'application/json' }).end('{"error":{"code":"fault_injected","message":"injected failure"}}');
      return;
    }

    const headers = new Headers();
    for (const [k, v] of Object.entries(req.headers)) if (typeof v === 'string') headers.set(k, v);
    headers.set('cf-connecting-ip', remote || '127.0.0.1');
    const hasBody = !['GET', 'HEAD'].includes(req.method);
    const resp = await app.handle(new Request(url, { method: req.method, headers, body: hasBody && body.length ? body : undefined }));
    const out = {};
    resp.headers.forEach((v, k) => { out[k] = v; });
    res.writeHead(resp.status, out).end(Buffer.from(await resp.arrayBuffer()));
  }

  const wrap = (scheme) => (req, res) => handler(req, res, scheme).catch((e) => {
    if (!res.headersSent) res.writeHead(500, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ error: { code: 'internal', message: String(e && e.message) } }));
  });

  const info = { host, httpPort: 0, httpsPort: 0, rootPem: null, rootDer: null, certs: null };
  const servers = [];
  const httpServer = http.createServer(wrap('http'));
  await new Promise((r) => httpServer.listen(opts.httpPort ?? 8787, host, r));
  info.httpPort = httpServer.address().port;
  servers.push(httpServer);
  if (opts.https !== false) {
    const certs = ensureCerts(opts.certDir);
    info.certs = certs;
    info.rootPem = readFileSync(certs.root, 'utf8');
    info.rootDer = certs.rootDer;
    const httpsServer = https.createServer({ key: readFileSync(certs.key), cert: readFileSync(certs.cert), minVersion: 'TLSv1.2' }, wrap('https'));
    await new Promise((r) => httpsServer.listen(opts.httpsPort ?? 8788, host, r));
    info.httpsPort = httpsServer.address().port;
    servers.push(httpsServer);
  }

  return {
    ...info,
    httpUrl: `http://${host}:${info.httpPort}`,
    httpsUrl: info.httpsPort ? `https://localhost:${info.httpsPort}` : null,
    env,
    get app() { return app; },
    setClock: (ms) => { clock.offset = ms - Date.now(); },
    close: async () => {
      clearInterval(sweeper);
      for (const s of servers) { s.closeAllConnections?.(); await new Promise((r) => s.close(r)); }
      store.close();
    },
  };
}

function parseArgs(argv) {
  const o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i];
    if (a === '--host') o.host = next();
    else if (a === '--http-port') o.httpPort = Number(next());
    else if (a === '--https-port') o.httpsPort = Number(next());
    else if (a === '--db') o.db = next();
    else if (a === '--no-https') o.https = false;
    else if (a === '--allow-control') o.allowControl = true;
    else if (a === '--help') { console.log(readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n').slice(1, 22).map((l) => l.replace(/^\/\/ ?/, '')).join('\n')); process.exit(0); }
    else { console.error(`unknown option ${a}`); process.exit(2); }
  }
  return o;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const f = await startFake(parseArgs(process.argv.slice(2)));
  console.log(`fake service up:\n  ${f.httpUrl}\n  ${f.httpsUrl || '(https off)'}${f.certs ? `\n  trusted root: ${f.certs.root} (DER: ${f.certs.rootDer})` : ''}\n  admin token: ${f.env.ADMIN_TOKEN}`);
}
