#!/usr/bin/env node
// Records test/fixtures/compat-1.2-shapes.json: the 1.2 client's request set (compat-requests.js) against the DEPLOYED server code.
//   rm -rf /tmp/old && mkdir /tmp/old && git -C .. archive e46c3e3 server/src | tar -x -C /tmp/old && node test/make-compat-fixture.mjs /tmp/old/server/src
import { writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { runOldClient } from './compat-requests.js';
import { HMAC_HEX, T0, makeBlob } from './helpers.js';

const src = resolve(process.argv[2] || '');
const { createApp } = await import(pathToFileURL(`${src}/core/app.js`));
const { openNodeSql } = await import(pathToFileURL(`${src}/sqlite-node.js`));
export const COMPAT_ENV = { RANK_HMAC_KEY_HEX: HMAC_HEX, ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '1', RATE_LIMIT_SCALE: '1000', MIN_MATCH_SEC: '60' };
const store = openNodeSql();
const clock = { now: T0 };
const app = createApp({ sql: store.sql, tx: store.tx, env: COMPAT_ENV, now: () => clock.now });
let ip = 0;
const call = async (method, path, body, headers) => {
  const h = { 'cf-connecting-ip': `198.51.100.${(ip++ % 250) + 1}`, ...headers };
  const init = { method, headers: h };
  if (body !== undefined) { init.body = JSON.stringify(body); h['content-type'] = 'application/json'; }
  const r = await app.handle(new Request(`https://test.invalid${path}`, init));
  const t = await r.text();
  let data = null; try { data = JSON.parse(t); } catch { data = null; }
  return { status: r.status, data };
};
const out = await runOldClient(call, (ms) => { clock.now += ms; }, (n) => makeBlob(n));
writeFileSync(new URL('./fixtures/compat-1.2-shapes.json', import.meta.url), JSON.stringify({ server: 'e46c3e3', requests: out }, null, 1) + '\n');
console.log(`${out.length} requests recorded`);
