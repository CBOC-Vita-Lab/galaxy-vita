// GET /times (GAME=smg): one self-contained page (inline CSS and JS, CSS shapes only: no fonts, images or scripts from anywhere) that
// reads /v1/public/times and shows the fastest Star Race times per galaxy and star. Same rules as leaderboard_page.js: names are
// user-typed, so the script only uses textContent; the CSP allows exactly the two inline blocks (by hash) and a same-origin fetch.
// Galaxy-themed text and colours only: no game art.

import { b64encode, te } from './util.js';

const CSS = `
:root{color-scheme:dark;--gold:#ffd84a;--ink:#05061a;--font:"Avenir Next","Trebuchet MS","DejaVu Sans",system-ui,sans-serif}
*{box-sizing:border-box}
html{background:#05061a}
body{margin:0;min-height:100vh;padding:16px 8px 24px;font-family:var(--font);color:#f2f4ff;
 background:radial-gradient(circle at 12% 18%,rgba(255,255,255,.9) 0 1px,transparent 2px),radial-gradient(circle at 78% 9%,rgba(255,255,255,.8) 0 1px,transparent 2px),
 radial-gradient(circle at 64% 72%,rgba(255,255,255,.7) 0 1px,transparent 2px),radial-gradient(circle at 30% 84%,rgba(255,255,255,.6) 0 1px,transparent 2px),
 radial-gradient(circle at 90% 55%,rgba(255,255,255,.7) 0 1.5px,transparent 3px),radial-gradient(ellipse at 50% 0%,#2a2f8f 0%,#141650 40%,#05061a 100%)}
main{max-width:720px;margin:0 auto}
header{text-align:center;margin:0 0 14px}
h1{margin:0;font-size:clamp(24px,6vw,36px);letter-spacing:.04em;color:var(--gold);text-shadow:0 0 12px rgba(255,216,74,.55),0 2px 0 #000}
h1::before,h1::after{content:"";display:inline-block;width:.8em;height:.8em;margin:0 .35em;vertical-align:-.05em;background:var(--gold);
 clip-path:polygon(50% 0,61% 35%,98% 35%,68% 57%,79% 91%,50% 70%,21% 91%,32% 57%,2% 35%,39% 35%)}
header p{margin:4px 0 0;color:#b9c0ff;font-size:14px}
.pick{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin:0 0 12px}
select{font:inherit;font-size:15px;color:#fff;background:#1d2170;border:1px solid #5d66d8;border-radius:8px;padding:6px 10px}
.panel{border-radius:16px;padding:10px;background:linear-gradient(180deg,rgba(64,72,190,.55),rgba(20,22,80,.75));box-shadow:0 0 0 2px #7a83ff55,0 10px 30px #000a}
table{width:100%;border-collapse:separate;border-spacing:0 6px}
th{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:#c9cdfd;text-align:left;padding:0 10px}
td{padding:9px 10px;background:#0c0e3acc;font-size:16px}
td:first-child{border-radius:10px 0 0 10px;width:3em;color:var(--gold);font-weight:700}
td:last-child{border-radius:0 10px 10px 0;text-align:right;color:#c9cdfd;font-size:13px}
td.t{font-variant-numeric:tabular-nums;font-weight:700}
.v{color:#7dffb2}
.note{text-align:center;color:#c9cdfd;padding:22px 0}
footer{text-align:center;color:#8e95e6;font-size:12px;margin-top:16px}
`;

const JS = `
(function () {
  'use strict';
  var $ = function (id) { return document.getElementById(id); };
  var stars = [];
  function fmt(f) {
    var cs = Math.floor((f % 60) * 100 / 60), s = Math.floor(f / 60), m = Math.floor(s / 60);
    s = s % 60;
    return m + ':' + (s < 10 ? '0' : '') + s + '.' + (cs < 10 ? '0' : '') + cs;
  }
  function note(t) { var b = $('rows'); b.textContent = ''; var tr = document.createElement('tr'); var td = document.createElement('td'); td.colSpan = 5; td.className = 'note'; td.textContent = t; tr.appendChild(td); b.appendChild(tr); }
  function cell(tr, text, cls) { var td = document.createElement('td'); td.textContent = text; if (cls) td.className = cls; tr.appendChild(td); return td; }
  function show(data) {
    var b = $('rows'); b.textContent = '';
    if (!data.rows || !data.rows.length) { note('No times yet.'); return; }
    data.rows.forEach(function (r) {
      var tr = document.createElement('tr');
      cell(tr, String(r.rank));
      cell(tr, r.name);
      cell(tr, fmt(r.frames), 't');
      cell(tr, r.players > 1 ? r.players + ' players' + (r.verified ? ' \\u2713' : '') : 'solo', r.verified ? 'v' : '');
      cell(tr, new Date(r.dateMs).toLocaleDateString());
      b.appendChild(tr);
    });
  }
  function load() {
    var v = $('star').value; if (!v) return;
    var p = v.split(':'); var cat = $('cat').value;
    note('Loading...');
    fetch('/v1/public/times?galaxy=' + p[0] + '&star=' + p[1] + (cat ? '&category=' + cat : '') + '&limit=50')
      .then(function (r) { return r.json(); }).then(show).catch(function () { note('The board is not available right now.'); });
  }
  fetch('/v1/public/times').then(function (r) { return r.json(); }).then(function (d) {
    stars = d.stars || [];
    var sel = $('star'); sel.textContent = '';
    if (!stars.length) { note('No times yet.'); return; }
    stars.forEach(function (s) {
      var o = document.createElement('option'); o.value = s.galaxy + ':' + s.star;
      o.textContent = 'Galaxy ' + s.galaxy + ', Star ' + s.star + ' (' + s.players + ')'; sel.appendChild(o);
    });
    load();
  }).catch(function () { note('The board is not available right now.'); });
  $('star').addEventListener('change', load);
  $('cat').addEventListener('change', load);
})();
`;

export const TIMES_BODY = `<main>
<header><h1>Star Race Times</h1><p>The fastest Power Stars, online</p></header>
<p class="pick"><label for="star" hidden>Star</label><select id="star"></select>
<label for="cat" hidden>Players</label><select id="cat"><option value="">All players</option><option value="1">Solo</option><option value="2">2 players</option><option value="3">3 players</option><option value="4">4 players</option></select></p>
<section class="panel"><table><thead><tr><th>#</th><th>Name</th><th>Time</th><th>Run</th><th>Date</th></tr></thead><tbody id="rows"><tr><td colspan="5" class="note">Loading...</td></tr></tbody></table></section>
<footer>Galaxy on PS Vita &mdash; fan project. &#x2713; = confirmed by the other players of the room.</footer>
</main>`;

export function timesHtml() {
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<meta name="theme-color" content="#141650">
<meta name="referrer" content="no-referrer">
<title>Star Race Times</title>
<link rel="icon" href="data:,">
<style>${CSS}</style>
</head>
<body>
${TIMES_BODY}
<script>${JS}</script>
</body>
</html>
`;
}

async function sha256b64(text) {
  return b64encode(new Uint8Array(await crypto.subtle.digest('SHA-256', te.encode(text))));
}

let memo = null;

export async function timesPageResponse() {
  if (!memo) {
    const csp = [
      "default-src 'none'",
      `script-src 'sha256-${await sha256b64(JS)}'`,
      `style-src 'sha256-${await sha256b64(CSS)}'`,
      "connect-src 'self'",
      'img-src data:',
      "base-uri 'none'",
      "form-action 'none'",
      "frame-ancestors 'none'",
    ].join('; ');
    memo = { html: timesHtml(), csp };
  }
  return new Response(memo.html, {
    status: 200,
    headers: {
      'content-type': 'text/html; charset=utf-8',
      'content-security-policy': memo.csp,
      'cache-control': 'public, max-age=300',
      'x-content-type-options': 'nosniff',
      'x-frame-options': 'DENY',
      'referrer-policy': 'no-referrer',
      'permissions-policy': 'camera=(), microphone=(), geolocation=()',
    },
  });
}

export const TIMES_CSS = CSS;
export const TIMES_JS = JS;
