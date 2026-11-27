#!/usr/bin/env node
// Prints the ranking digest key (sRankingHmacKey) from the game source as 64 hex characters.
// Usage: node scripts/extract-hmac-key.mjs [path/to/NetworkStats.cpp]
// The value goes into the RANK_HMAC_KEY_HEX secret / test config. Do not paste it into logs or tickets.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

export function extractKey(path) {
  const src = readFileSync(path, 'utf8');
  const m = /sRankingHmacKey\[32\]\s*=\s*\{([^}]*)\}/.exec(src);
  if (!m) throw new Error('sRankingHmacKey not found');
  const bytes = [...m[1].matchAll(/0x([0-9a-fA-F]{2})/g)].map((x) => x[1].toLowerCase());
  if (bytes.length !== 32) throw new Error(`expected 32 key bytes, found ${bytes.length}`);
  return bytes.join('');
}

// In the Strikers repo the game source sits next to the server. In a second deployment (Galaxy, GAME=smg) it does not:
// MSC_HMAC_SOURCE points at a Strikers checkout's NetworkStats.cpp for the msc test suites.
export const DEFAULT_SOURCE = process.env.MSC_HMAC_SOURCE ||
  resolve(dirname(fileURLToPath(import.meta.url)), '../../native/src/Game/NetworkStats.cpp');

/** The key, or `fallback` when the source is not there (GAME=smg never uses the ranking digest). */
export function extractKeyOr(path, fallback) {
  try { return extractKey(path); } catch { return fallback; }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  console.log(extractKey(process.argv[2] || DEFAULT_SOURCE));
}
