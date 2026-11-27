#!/usr/bin/env node
// Writes the static files of the optional "static assets" deployment into ./public (svc3): the web page /times, exactly as the Worker would
// answer it (same bytes, same security headers). Cloudflare serves files of an assets directory WITHOUT running the Worker, and requests for
// static assets are free and not counted against the 100,000 Worker requests a day (Cloudflare "Static Assets" pricing: free and unlimited on
// the Free plan; read 12 Nov 2026 from memory of the docs, check before relying on it). The Worker stays the fallback for every path with no file.
//   node scripts/build-assets.mjs [outDir]        default ./public
// The data of the page (GET /v1/public/times) is NOT static: it stays a Worker route (memoised, rate limited).
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { timesPageResponse } from '../src/core/times_page.js';

const out = process.argv[2] || 'public';
mkdirSync(out, { recursive: true });
const r = await timesPageResponse();
writeFileSync(join(out, 'times.html'), await r.text());
// _headers (assets feature): the same headers the Worker sends, for the one path
const lines = ['/times', ...[...r.headers].filter(([k]) => k !== 'content-type').map(([k, v]) => `  ${k}: ${v}`)];
writeFileSync(join(out, '_headers'), lines.join('\n') + '\n');
console.log(`wrote ${join(out, 'times.html')} and ${join(out, '_headers')}`);
