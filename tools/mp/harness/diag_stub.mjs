// Beta diagnostics harness stub (local only). The tree's service has no /v1/report and no /v1/turn/check yet, so this stands in for them:
//   * the real fake online service (server/fake/server.mjs, in-process) answers everything else (sign-up, rooms...) on a private port,
//   * POST /v1/report: needs a player token; the nth call answers the nth status of --reports (default "201"; after the list: the last one);
//     every body is written to <out>/report-<n>.json and its headers to <out>/report-<n>.headers.json; a 201 answers {"id":"<8 chars>"},
//   * POST /v1/turn/check: needs a player token; answers {"servers":[stun entry, turn entry with a TURN REST user]} pointing at the TURN stub below,
//   * a STUN + TURN server on UDP 127.0.0.1:<stun-port> (and :53 when it can be bound): Binding, Allocate (401 challenge, then allowed), Refresh,
//     CreatePermission, ChannelBind, and the relayed socket forwards what arrives from a permitted IP as a Data indication (what the relay loopback needs).
//     Counters go to <out>/stub.log lines: "turn allocate", "turn permission ip=", "turn data-ind from=", "turn released".
// Usage: node diag_stub.mjs --port 18797 --out DIR [--reports 429,201] [--stun-port 3478] [--delay-ms 4000] ; prints "ready" when listening.
import http from 'node:http';
import dgram from 'node:dgram';
import { createHash, randomBytes } from 'node:crypto';
import { mkdirSync, writeFileSync, appendFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf(`--${n}`); return i > 0 ? process.argv[i + 1] : d; };
const PORT = Number(arg('port', 18797));
const BACK = PORT + 1;
const OUT = arg('out', '/tmp/diag-stub');
const REPORTS = arg('reports', '201').split(',').map(Number);
const STUN_PORT = Number(arg('stun-port', 3478));
const ALT_B = process.argv.includes('--stun-alt-b');   // STUN tells every source port but the first one that its public IP is 127.0.0.2 (a NAT that pools addresses): the game must ask the relay for a permission for that IP too
let firstStunPort = 0;
const DELAY = Number(arg('delay-ms', 0));   // the service answers /v1/report and /v1/turn/check this late (so the waiting screens can be seen)
mkdirSync(OUT, { recursive: true });
const log = (s) => appendFileSync(join(OUT, 'stub.log'), `${Date.now()} ${s}\n`);

// ---- the real fake service on a private port
process.env.GAME = 'smg';
process.env.REGISTER_POW_BITS = '0';
const { startFake } = await import(join(HERE, '../../../server/fake/server.mjs'));
await startFake({ httpPort: BACK, https: false });

// ---- STUN / TURN
const MAGIC = 0x2112A442;
const attr = (t, v) => { const pad = (4 - (v.length % 4)) % 4; const b = Buffer.alloc(4 + v.length + pad); b.writeUInt16BE(t, 0); b.writeUInt16BE(v.length, 2); v.copy(b, 4); return b; };
const msg = (type, txid, attrs) => { const body = Buffer.concat(attrs); const h = Buffer.alloc(20); h.writeUInt16BE(type, 0); h.writeUInt16BE(body.length, 2); h.writeUInt32BE(MAGIC, 4); txid.copy(h, 8); return Buffer.concat([h, body]); };
const xorAddr = (ip, port) => { const v = Buffer.alloc(8); v[1] = 1; v.writeUInt16BE(port ^ (MAGIC >>> 16), 2); const o = ip.split('.').map(Number); const m = [0x21, 0x12, 0xA4, 0x42]; for (let i = 0; i < 4; i++) v[4 + i] = o[i] ^ m[i]; return v; };
const readXor = (v) => { const m = [0x21, 0x12, 0xA4, 0x42]; const port = v.readUInt16BE(2) ^ 0x2112; const ip = [0, 1, 2, 3].map((i) => v[4 + i] ^ m[i]).join('.'); return { ip, port }; };
const parse = (d) => { if (d.length < 20 || (d[0] & 0xC0) || d.readUInt32BE(4) !== MAGIC) return null; const len = d.readUInt16BE(2); const a = {}; let o = 20; while (o + 4 <= 20 + len && o + 4 <= d.length) { const t = d.readUInt16BE(o), l = d.readUInt16BE(o + 2); a[t] = d.subarray(o + 4, o + 4 + l); o += 4 + ((l + 3) & ~3); } return { type: d.readUInt16BE(0), txid: d.subarray(8, 20), a }; };
const errAttr = (code, text) => { const v = Buffer.alloc(4 + text.length); v[2] = Math.floor(code / 100); v[3] = code % 100; v.write(text, 4); return attr(0x0009, v); };
const allocs = new Map();   // "ip:port" of the client -> { relay: socket, ip, port, permitted:Set, sock }

function onUdp(sock) {
  sock.on('message', async (d, r) => {
    const m = parse(d);
    if (!m) return;
    const reply = (type, attrs) => sock.send(msg(type, m.txid, attrs), r.port, r.address);
    const key = `${r.address}:${r.port}`;
    switch (m.type) {
      case 0x0001: { if (!firstStunPort) firstStunPort = r.port; reply(0x0101, [attr(0x0020, xorAddr(ALT_B && r.port !== firstStunPort ? '127.0.0.2' : r.address, r.port))]); } log(`stun binding from=${key}`); break;
      case 0x0003: {   // Allocate
        if (!m.a[0x0006]) { reply(0x0113, [errAttr(401, 'Unauthorized'), attr(0x0014, Buffer.from('stub.test')), attr(0x0015, Buffer.from('nonce-' + randomBytes(4).toString('hex')))]); break; }
        let al = allocs.get(key);
        if (!al) {
          const rs = dgram.createSocket('udp4');
          al = { relay: rs, permitted: new Set(), port: 0, sock, r };
          rs.on('message', (pd, pr) => {
            if (!al.permitted.has(pr.address)) { log(`turn drop (no permission) from=${pr.address}:${pr.port}`); return; }
            const tx = randomBytes(12);
            sock.send(msg(0x0017, tx, [attr(0x0012, xorAddr(pr.address, pr.port)), attr(0x0013, pd)]), r.port, r.address);
            log(`turn data-ind from=${pr.address}:${pr.port} bytes=${pd.length}`);
          });
          rs.bind(0, '127.0.0.1');
          await new Promise((ok) => rs.once('listening', ok));
          al.port = rs.address().port;
          allocs.set(key, al);
          log(`turn allocate client=${key} relayed=127.0.0.1:${al.port}`);
        }
        reply(0x0103, [attr(0x0016, xorAddr('127.0.0.1', al.port)), attr(0x0020, xorAddr(r.address, r.port)), attr(0x000D, Buffer.from([0, 0, 1, 0x2C]))]);
        break;
      }
      case 0x0004: {   // Refresh
        const al = allocs.get(key);
        const life = m.a[0x000D] ? m.a[0x000D].readUInt32BE(0) : 300;
        if (al && life === 0) { al.relay.close(); allocs.delete(key); log('turn released'); }
        reply(0x0104, [attr(0x000D, Buffer.from([0, 0, 0, life === 0 ? 0 : 0xFF]))]);
        break;
      }
      case 0x0008: case 0x0009: {   // CreatePermission / ChannelBind
        const al = allocs.get(key);
        if (!al || !m.a[0x0012]) { reply(m.type | 0x0110, [errAttr(437, 'Allocation Mismatch')]); break; }
        const p = readXor(m.a[0x0012]);
        al.permitted.add(p.ip);
        log(`turn ${m.type === 8 ? 'permission' : 'channelbind'} ip=${p.ip} port=${p.port}`);
        reply(m.type | 0x0100, []);
        break;
      }
      default: break;
    }
  });
}
for (const p of [STUN_PORT, 53]) {
  const s = dgram.createSocket('udp4');
  s.on('error', (e) => log(`udp ${p} not bound: ${e.code}`));
  s.bind(p, '127.0.0.1', () => { log(`udp ${p} listening`); });
  onUdp(s);
}

// ---- HTTP front
let nReport = 0;
const hasToken = (req) => /^Bearer\s+\S{12,}/.test(req.headers.authorization || '');
http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method === 'POST' && (url.pathname === '/v1/report' || url.pathname === '/v1/turn/check')) {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', async () => {
      const body = Buffer.concat(chunks);
      if (DELAY) await new Promise((r) => setTimeout(r, DELAY));
      const json = (st, o) => res.writeHead(st, { 'content-type': 'application/json' }).end(JSON.stringify(o));
      if (!hasToken(req)) return json(401, { error: { code: 'unauthorized', message: 'sign in' } });
      if (url.pathname === '/v1/turn/check') {
        log('turn check minted');
        return json(200, { servers: [{ urls: [`stun:127.0.0.1:${STUN_PORT}`] }, { urls: [`turn:127.0.0.1:${STUN_PORT}?transport=udp`], username: `${Math.floor(Date.now() / 1000) + 300}:stub`, credential: 'c3R1Yg==' }], ttl: 300 });
      }
      const n = ++nReport;
      writeFileSync(join(OUT, `report-${n}.json`), body);
      writeFileSync(join(OUT, `report-${n}.headers.json`), JSON.stringify(req.headers, null, 1));
      const st = REPORTS[Math.min(n - 1, REPORTS.length - 1)];
      log(`report ${n} bytes=${body.length} status=${st}`);
      if (st === 201) return json(201, { id: 'K7QM2XPA' });
      return json(st, { error: { code: st === 429 ? 'rate_limited' : 'error', message: 'stub' } });
    });
    return;
  }
  const up = http.request({ host: '127.0.0.1', port: BACK, path: req.url, method: req.method, headers: req.headers }, (r) => { res.writeHead(r.statusCode, r.headers); r.pipe(res); });
  up.on('error', () => { res.writeHead(502).end(); });
  req.pipe(up);
}).listen(PORT, '127.0.0.1', () => { console.log(`ready http://127.0.0.1:${PORT}`); log('ready'); });
