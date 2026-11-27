// The real src/worker.js (edge entry) in node, in front of the real application over node:sqlite, with a counting stand-in for the
// Durable Object binding. Lets the suites in test/core measure what the Worker sends to the object (each fetch = one Durable Object request).
import { createApp } from '../src/core/app.js';
import { openNodeSql } from '../src/sqlite-node.js';

/**
 * @param {Record<string,string>} envOverrides wrangler vars
 * @param {{t0?: number}} [opts]
 * Returns { worker, env, clock, app, doRequests(), get(path, headers), reset() }.
 * Date.now is NOT patched here: worker.js reads the real clock; tests that need time travel call vi.setSystemTime (fake timers).
 */
export async function makeWorker(envOverrides = {}, opts = {}) {
  const mod = await import('../src/worker.js?fresh=' + Math.random()); // a fresh isolate: module state (memo, buckets, budget memory) starts empty
  const store = openNodeSql();
  const env = {
    SERVICE_GAME: 'smg', ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', REQUIRE_APP_VERSION: '1',
    REGISTER_POW_BITS: '0', ...envOverrides,
  };
  const stats = { doRequests: 0, byPath: new Map() };
  const app = createApp({ sql: store.sql, tx: store.tx, env, now: () => Date.now() });
  const ns = {
    idFromName: (n) => n,
    get: () => ({
      fetch: async (req) => {
        stats.doRequests++;
        const p = new URL(req.url).pathname;
        stats.byPath.set(p, (stats.byPath.get(p) || 0) + 1);
        return app.handle(req);
      },
    }),
  };
  env.GAME = ns;
  const call = (method, path, { headers = {}, body, token, ip = '203.0.113.9' } = {}) => {
    const h = { 'cf-connecting-ip': ip, 'x-msc-game': 'smg', 'x-msc-app': '3.0.0', 'user-agent': 'GalaxyVita/3.0', ...headers };
    if (token) h.authorization = `Bearer ${token}`;
    const init = { method, headers: h };
    if (body !== undefined) { init.body = JSON.stringify(body); h['content-type'] = 'application/json'; h['content-length'] = String(Buffer.byteLength(init.body)); }
    return mod.default.fetch(new Request(`https://smg.test.invalid${path}`, init), env);
  };
  return { worker: mod.default, env, app, stats, call, store };
}
