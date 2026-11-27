// svcfix2 item 7 (independent review): the name filter was beaten by leetspeak, repeats, separators and staff look-alikes. One normaliser
// and one list (src/core/name_filter.js) now serve the service and, compiled into the game (platform/src/mp/mp_name_filter_data.inc),
// the names of other consoles on a local Wi-Fi. The test names live in test/fixtures/name_vectors.txt and are checked by the game's
// tests/mp/name_filter_test.sh too. Nothing here prints the list.
import { describe, it, expect } from 'vitest';
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { nameFilterReason, validateName } from '../../src/core/names.js';

const here = (p) => fileURLToPath(new URL(p, import.meta.url));
const vectors = fs.readFileSync(here('../fixtures/name_vectors.txt'), 'utf8').split('\n').filter((l) => l && !l.startsWith('#')).map((l) => l.split('\t'));

describe('name filter', () => {
  it('every shared test name gets the expected answer', () => {
    const wrong = vectors.filter(([n, want]) => (nameFilterReason(n) || 'ok') !== want).length;
    expect(wrong).toBe(0);
    expect(vectors.length).toBeGreaterThan(25);
  });
  it('a refused name is bad_name at sign-up, an honest one is not', () => {
    for (const [n, want] of vectors) {
      let ok = true;
      try { validateName(n); } catch (e) { ok = false; expect(e.code || e.error || e.message).toMatch(/bad_name/); }
      // (a name the character rules reject for other reasons, such as a space at the edge, is not what this checks)
      if (want !== 'ok') expect(ok).toBe(false);
    }
  });
  it('the game data file is the one generated from the service list', () => {
    execFileSync('node', [here('../../scripts/gen-name-filter.mjs'), '--check'], { stdio: 'pipe' });
  });
});
