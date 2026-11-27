// The fake server speaks the same protocol over HTTP and over HTTPS with its own root.
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import https from 'node:https';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { startFake } from '../../fake/server.mjs';

let fake;
beforeAll(async () => {
  fake = await startFake({ httpPort: 0, httpsPort: 0, certDir: mkdtempSync(join(tmpdir(), 'fakecerts-')) });
}, 60000);
afterAll(async () => { await fake?.close(); });

const j = async (base, method, path, body, token) => {
  const r = await fetch(`${base}${path}`, {
    method,
    headers: { ...(body ? { 'content-type': 'application/json' } : {}), ...(token ? { authorization: `Bearer ${token}` } : {}) },
    body: body ? JSON.stringify(body) : undefined,
  });
  return { status: r.status, data: await r.json() };
};

function httpsGet(url, ca) {
  return new Promise((resolve, reject) => {
    https.get(url, { ca }, (res) => {
      let s = '';
      res.on('data', (d) => { s += d; });
      res.on('end', () => resolve({ status: res.statusCode, data: JSON.parse(s) }));
    }).on('error', reject);
  });
}

import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

// The fake runs in this process, so the scripts must run asynchronously (a blocking spawn would deadlock the server).
const run = (file, args, env, input) => new Promise((resolve, reject) => {
  const p = spawn(process.execPath, [file, ...args], { env });
  let out = '';
  let err = '';
  p.stdout.on('data', (d) => { out += d; });
  p.stderr.on('data', (d) => { err += d; });
  p.on('error', reject);
  p.on('close', (status) => resolve({ status, stdout: out, stderr: err }));
  p.stdin.end(input || '');
});

describe('backup and restore scripts', () => {
  it('copy a populated service into a wiped one', async () => {
    const script = (n) => fileURLToPath(new URL(`../../scripts/${n}`, import.meta.url));
    const env = { ...process.env, ADMIN_TOKEN: 'fake-admin-token-0123456789' };
    await j(fake.httpUrl, 'POST', '/__fake/reset', {});
    const a = (await j(fake.httpUrl, 'POST', '/v1/register', { name: 'Backup1' })).data;
    const dump = await run(script('backup.mjs'), [fake.httpUrl], env);
    expect(dump.status).toBe(0);
    expect(JSON.parse(dump.stdout).tables.profiles).toHaveLength(1);
    await j(fake.httpUrl, 'POST', '/__fake/reset', {});
    expect((await j(fake.httpUrl, 'GET', '/v1/me', undefined, a.token)).status).toBe(401);
    const rest = await run(script('restore.mjs'), [fake.httpUrl], env, dump.stdout);
    expect(rest.status).toBe(0);
    expect((await j(fake.httpUrl, 'GET', '/v1/me', undefined, a.token)).data.name).toBe('Backup1');
  });
});

describe('fake server', () => {
  it('serves the protocol over plain HTTP', async () => {
    const r = await j(fake.httpUrl, 'GET', '/v1/info');
    expect(r.data.proto).toBe(1);
    const a = await j(fake.httpUrl, 'POST', '/v1/register', { name: 'FakeOne' });
    expect(a.data.pid).toBeGreaterThan(100000);
    expect((await j(fake.httpUrl, 'GET', '/v1/me', undefined, a.data.token)).data.name).toBe('FakeOne');
  });

  it('serves HTTPS that verifies against the fake root and fails without it', async () => {
    expect(fake.rootPem).toContain('BEGIN CERTIFICATE');
    const ok = await httpsGet(`${fake.httpsUrl}/v1/time`, fake.rootPem);
    expect(ok.status).toBe(200);
    expect(ok.data.year).toBeGreaterThan(2000);
    await expect(httpsGet(`${fake.httpsUrl}/v1/time`, undefined)).rejects.toThrow(/self.signed|unable to (get|verify)|certificate/i);
  });

  it('control endpoints move the clock, inject faults and reset data', async () => {
    const t0 = (await j(fake.httpUrl, 'GET', '/v1/time')).data.utcMs;
    await j(fake.httpUrl, 'POST', '/__fake/clock', { offsetMs: 3600_000 });
    expect((await j(fake.httpUrl, 'GET', '/v1/time')).data.utcMs - t0).toBeGreaterThan(3500_000);
    await j(fake.httpUrl, 'POST', '/__fake/clock', { reset: true });
    await j(fake.httpUrl, 'POST', '/__fake/fault', { failRate: 1 });
    expect((await j(fake.httpUrl, 'GET', '/v1/time')).status).toBe(503);
    await j(fake.httpUrl, 'POST', '/__fake/fault', {});
    await j(fake.httpUrl, 'POST', '/__fake/reset', {});
    expect((await j(fake.httpUrl, 'POST', '/v1/register', { name: 'FakeOne' })).status).toBe(200); // data wiped, name free again
  });

  it('a full ranked session works against the fake (same code path as production)', async () => {
    await j(fake.httpUrl, 'POST', '/__fake/reset', {});
    const a = (await j(fake.httpUrl, 'POST', '/v1/register', { name: 'Anna' })).data;
    const b = (await j(fake.httpUrl, 'POST', '/v1/register', { name: 'Bert' })).data;
    await j(fake.httpUrl, 'POST', '/v1/ranked/queue', undefined, a.token);
    const q = (await j(fake.httpUrl, 'POST', '/v1/ranked/queue', undefined, b.token)).data;
    expect(q.status).toBe('matched');
    expect((await j(fake.httpUrl, 'GET', '/v1/ranked/queue', undefined, a.token)).data.roomCode).toBe(q.roomCode);
    const turn = await j(fake.httpUrl, 'POST', '/v1/turn', { room: q.roomCode }, a.token);
    expect(turn.status).toBe(200);
  });
});
