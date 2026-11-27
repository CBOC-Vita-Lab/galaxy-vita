import { fileURLToPath } from 'node:url';   // (a path with a space must not become %20)
import { defineConfig } from 'vitest/config';
import { cloudflareTest } from '@cloudflare/vitest-plugin';
import { extractKeyOr, DEFAULT_SOURCE } from './scripts/extract-hmac-key.mjs';

// The ranking digest key is read from the game source so the tests use the real value without it being copied into the repo.
// (MSC_HMAC_SOURCE points at a Strikers checkout when the game source is not next to the server; GAME=smg never uses the key)
const HMAC = extractKeyOr(DEFAULT_SOURCE, '0'.repeat(64));

export default defineConfig({
  test: {
    projects: [
      {
        // 'cloudflare:workers' is a platform module: the node suites import src/worker.js through a stand-in (test/worker-node.js)
        resolve: { alias: { 'cloudflare:workers': fileURLToPath(new URL('./test/stubs/cloudflare-workers.js', import.meta.url)) } },
        test: { name: 'core', include: ['test/core/**/*.test.js'], environment: 'node', testTimeout: 20000 },
      },
      {
        plugins: [
          cloudflareTest({
            wrangler: { configPath: './wrangler.toml' },
            miniflare: {
              bindings: {
                RANK_HMAC_KEY_HEX: HMAC,
                ADMIN_TOKEN: 'test-admin-token-0123456789',
                TURN_PROVIDER: 'stub',
                ALLOW_TEST_CLOCK: '1',
                RATE_LIMIT_SCALE: '1000',
                REQUIRE_APP_VERSION: '0', // production sets 1 in wrangler.toml; this suite sends no version headers
                MIN_MATCH_SEC: '0',
              },
            },
          }),
        ],
        test: { name: 'worker', include: ['test/worker/**/*.test.js'], testTimeout: 30000 },
      },
      {
        // smg-online (Galaxy): the same code with wrangler.smg.toml (SERVICE_GAME=smg)
        plugins: [
          cloudflareTest({
            wrangler: { configPath: './wrangler.smg.toml' },
            miniflare: {
              bindings: { ADMIN_TOKEN: 'test-admin-token-0123456789', TURN_PROVIDER: 'stub', ALLOW_TEST_CLOCK: '1', RATE_LIMIT_SCALE: '1000', REGISTER_POW_BITS: '0' },
            },
          }),
        ],
        test: { name: 'worker-smg', include: ['test/worker-smg/**/*.test.js'], testTimeout: 30000 },
      },
    ],
  },
});
