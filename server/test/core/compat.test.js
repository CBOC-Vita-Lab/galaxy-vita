// Deploy safety (1.3): the clients already out there (the 1.2 test builds; 1.1 has no online code) must see the same service after the deploy.
// Their request set (test/compat-requests.js) runs against the new server with the production settings (REQUIRE_APP_VERSION=1); every
// answer must have the status the deployed code gave (test/fixtures/compat-1.2-shapes.json, recorded from e46c3e3) and every JSON key it had.
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { runOldClient } from '../compat-requests.js';
import { makeServer, makeBlob } from '../helpers.js';

const fixture = JSON.parse(readFileSync(new URL('../fixtures/compat-1.2-shapes.json', import.meta.url), 'utf8'));

describe('the 1.2 client against the new server', () => {
  it('same statuses, no key removed or renamed', async () => {
    const s = makeServer({ REQUIRE_APP_VERSION: '1', MIN_MATCH_SEC: '60' });
    const call = (method, path, body, headers) => s.call(method, path, body, headers.authorization ? headers.authorization.slice(7) : undefined,
      Object.fromEntries(Object.entries(headers).filter(([k]) => k !== 'authorization')));
    const got = await runOldClient(call, s.advance, (n) => makeBlob(n));
    expect(got.map((r) => r.name)).toEqual(fixture.requests.map((r) => r.name));
    for (let i = 0; i < got.length; i++) {
      const old = fixture.requests[i];
      expect([got[i].name, got[i].status]).toEqual([old.name, old.status]);
      const missing = old.keys.filter((k) => !got[i].keys.includes(k));
      expect([old.name, missing]).toEqual([old.name, []]);
    }
  });
});
