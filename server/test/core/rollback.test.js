// Deploy safety (1.3): a rollback to the deployed code (e46c3e3) after the new code migrated the database must keep working: the old code
// starts on the migrated database (it ignores migration 3, the added columns have defaults, the added table is unknown to it) and answers
// the 1.2 client's request set exactly as before. Runs when the git history is at hand (a checkout), skips otherwise.
import { describe, it, expect } from 'vitest';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { openNodeSql } from '../../src/sqlite-node.js';
import { createApp } from '../../src/core/app.js';
import { runOldClient } from '../compat-requests.js';
import { HMAC_HEX, T0, makeBlob } from '../helpers.js';

const DEPLOYED = 'e46c3e3';
let have = false;
try { execFileSync('git', ['cat-file', '-e', `${DEPLOYED}^{commit}`], { stdio: 'ignore' }); have = true; } catch { have = false; }

describe.skipIf(!have)('rollback to the deployed code after the migration', () => {
  it('the old code runs on the migrated database and answers the 1.2 client as it did', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'msc-rollback-'));
    try {
      const tar = execFileSync('git', ['-C', '..', 'archive', DEPLOYED, 'server/src']);
      execFileSync('tar', ['-x', '-C', dir], { input: tar });
      const { createApp: oldApp } = await import(pathToFileURL(join(dir, 'server/src/core/app.js')).href);
      const env = { RANK_HMAC_KEY_HEX: HMAC_HEX, ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '1', RATE_LIMIT_SCALE: '1000', MIN_MATCH_SEC: '60' };
      const store = openNodeSql();
      const clock = { now: T0 };
      createApp({ sql: store.sql, tx: store.tx, env, now: () => clock.now }); // the new code migrates
      const app = oldApp({ sql: store.sql, tx: store.tx, env, now: () => clock.now }); // then the rollback
      let ip = 0;
      const call = async (method, path, body, headers) => {
        const h = { 'cf-connecting-ip': `198.51.100.${(ip++ % 250) + 1}`, ...headers };
        const init = { method, headers: h };
        if (body !== undefined) { init.body = JSON.stringify(body); h['content-type'] = 'application/json'; }
        const r = await app.handle(new Request(`https://t.invalid${path}`, init));
        const text = await r.text();
        let data = null; try { data = JSON.parse(text); } catch { data = null; }
        return { status: r.status, data };
      };
      const got = await runOldClient(call, (ms) => { clock.now += ms; }, (n) => makeBlob(n));
      const fixture = JSON.parse(readFileSync(new URL('../fixtures/compat-1.2-shapes.json', import.meta.url), 'utf8'));
      expect(got.map((r) => [r.name, r.status])).toEqual(fixture.requests.map((r) => [r.name, r.status]));
      expect(() => app.sweep()).not.toThrow();
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
