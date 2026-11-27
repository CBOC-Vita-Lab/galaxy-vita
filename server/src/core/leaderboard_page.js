// GET /leaderboard: one self-contained page (inline CSS and JS, CSS shapes only: no fonts, images or scripts from anywhere).
// It reads /v1/public/leaderboard and draws the two tabs of the game's own ranking screen (SEASON LEADERS / DAILY LEADERS).
// Names are user-typed: the script only ever uses textContent, never innerHTML. The CSP allows exactly these two inline blocks
// (by hash) and a same-origin fetch, nothing else.

import { b64encode, te } from './util.js';

const TORN = 'polygon(0% 0%, 100% 0%, 100% 90%, 97% 88%, 93.9% 80%, 91.7% 84%, 89.8% 84%, 87.1% 78%, 85.2% 96%, 83% 84%, 79.4% 80%, 77.2% 96%, 75% 80%, 72.8% 84%, 69.7% 78%, 67.8% 80%, 65.6% 96%, 63.4% 90%, 60.7% 80%, 57.6% 84%, 55.7% 100%, 53.8% 90%, 51.6% 96%, 48.9% 84%, 47% 84%, 45.1% 78%, 43.2% 96%, 39.6% 80%, 36.9% 94%, 35% 94%, 32.3% 100%, 29.2% 90%, 26.1% 84%, 24.2% 100%, 22.3% 94%, 19.6% 94%, 16.9% 84%, 14.7% 80%, 11.6% 88%, 8.5% 94%, 4.9% 78%, 2.7% 88%, 0% 92%)';

const OUTLINE = (px, c = '#000') => [[-1, -1], [1, -1], [-1, 1], [1, 1], [0, -1], [0, 1], [-1, 0], [1, 0]]
  .map(([x, y]) => `${x * px}px ${y * px}px 0 ${c}`).join(',');

const CSS = `
:root{color-scheme:dark;--u:min(1vw,.5vh);--orange:#ffa800;--ink:#000;--font:"Avenir Next Condensed","Arial Narrow","Roboto Condensed",sans-serif-condensed,"Franklin Gothic Medium","DejaVu Sans Condensed",Impact,"Arial Black",system-ui,sans-serif}
*{box-sizing:border-box}
html{background:#0b0c0e}
body{margin:0;min-height:100vh;padding:14px 6px 18px;font-family:var(--font);font-weight:800;font-stretch:condensed;color:#fff;
 background:radial-gradient(ellipse at 50% 30%,#2b2f34 0%,#16181b 55%,#08090a 100%),#0b0c0e}
body::before{content:"";position:fixed;inset:0;pointer-events:none;opacity:.35;
 background:repeating-linear-gradient(115deg,transparent 0 46px,rgba(255,255,255,.035) 46px 48px)}
.hub{position:relative;max-width:780px;margin:0 auto 10px;text-align:center;text-transform:uppercase;line-height:1.25}
.hub h1{margin:0;font-size:clamp(22px,calc(6.2*var(--u)),34px);font-style:italic;font-weight:900;letter-spacing:.02em;color:#fff;text-shadow:${OUTLINE(2)},0 4px 0 #000,0 0 14px rgba(0,0,0,.8)}
.hub p{margin:2px 0 0;font-size:clamp(13px,calc(3.6*var(--u)),17px);letter-spacing:.02em;text-shadow:${OUTLINE(1)},0 2px 3px rgba(0,0,0,.9)}
.dots{display:flex;justify-content:center;gap:16px;margin:0 0 8px;padding:0;list-style:none}
.dots button{display:block;width:11px;height:11px;padding:0;border:0;border-radius:50%;background:#8b9096;box-shadow:0 0 0 1px #000;cursor:pointer}
.dots button[aria-current="true"]{border-radius:2px;background:#fff;box-shadow:0 0 0 1px #000,0 0 6px rgba(255,255,255,.6)}
.stage{position:relative;display:grid;grid-template-columns:clamp(16px,calc(5*var(--u)),38px) minmax(0,1fr) clamp(16px,calc(5*var(--u)),38px);gap:clamp(3px,calc(1.2*var(--u)),10px);max-width:780px;margin:0 auto}
.arrow{position:sticky;top:42vh;align-self:start;height:clamp(52px,calc(15*var(--u)),100px);margin-top:clamp(40px,calc(10*var(--u)),90px);padding:0;border:0;cursor:pointer;color:transparent;font-size:0;
 background:linear-gradient(90deg,#ff3b2a,#a1100a);filter:drop-shadow(0 0 0 #000) drop-shadow(1px 0 0 #000) drop-shadow(-1px 0 0 #000) drop-shadow(0 2px 3px rgba(0,0,0,.7))}
.arrow.prev{clip-path:polygon(100% 0,0 50%,100% 100%)}
.arrow.next{clip-path:polygon(0 0,100% 50%,0 100%);background:linear-gradient(270deg,#ff3b2a,#a1100a)}
.arrow:hover{filter:brightness(1.25)}
.arrow:focus-visible{outline:3px solid #fff;outline-offset:3px}
.frame{position:relative;min-width:0;padding:clamp(5px,calc(1.6*var(--u)),10px);border-radius:clamp(10px,calc(3*var(--u)),18px);border:2px solid #050606;
 background:linear-gradient(180deg,#575b5f 0%,#33363a 14%,#26292c 60%,#1a1c1e 100%);
 box-shadow:inset 0 2px 0 rgba(255,255,255,.22),inset 0 -2px 0 rgba(0,0,0,.6),0 8px 30px rgba(0,0,0,.7)}
.board{overflow:hidden;border-radius:clamp(6px,calc(2*var(--u)),12px);background:#2c7fa6;box-shadow:0 0 0 2px #0a2230,0 0 0 4px rgba(120,200,235,.35)}
.title{position:relative;height:clamp(54px,calc(14*var(--u)),86px);display:flex;align-items:center;justify-content:center;background:#12161a;overflow:hidden}
.title::before{content:"";position:absolute;inset:0;clip-path:${TORN};
 background:radial-gradient(ellipse at 22% 105%,rgba(255,236,120,.9),transparent 55%),radial-gradient(ellipse at 85% 0%,rgba(150,20,10,.8),transparent 55%),
 repeating-linear-gradient(100deg,rgba(90,20,0,.14) 0 9px,transparent 9px 23px),
 linear-gradient(180deg,#c22a14 0%,#e8571a 34%,#f48a1c 62%,#f9c21d 100%)}
.title::after{content:"";position:absolute;inset:0;clip-path:${TORN};opacity:.5;
 background:radial-gradient(circle at 12% 30%,rgba(255,255,255,.7) 0 1px,transparent 2px),radial-gradient(circle at 74% 62%,rgba(255,255,255,.6) 0 1px,transparent 2px),
 radial-gradient(circle at 90% 22%,rgba(255,255,255,.6) 0 1.5px,transparent 3px),radial-gradient(circle at 40% 80%,rgba(60,10,0,.5) 0 2px,transparent 5px)}
.title h2{position:relative;z-index:1;margin:0;padding:0 .5em;font-size:clamp(24px,calc(7.4*var(--u)),44px);font-style:italic;font-weight:900;line-height:1;letter-spacing:.03em;text-transform:uppercase;white-space:nowrap;
 text-shadow:${OUTLINE(2.5)},0 4px 0 #000,0 0 10px rgba(0,0,0,.6)}
.cols,.row{display:grid;grid-template-columns:clamp(22px,calc(8*var(--u)),56px) minmax(0,1fr) clamp(72px,calc(22*var(--u)),150px) clamp(54px,calc(15*var(--u)),110px) clamp(32px,calc(10*var(--u)),64px);align-items:center;column-gap:clamp(1px,calc(.8*var(--u)),8px)}
.cols{height:clamp(30px,calc(8*var(--u)),42px);padding:0 clamp(5px,calc(2.6*var(--u)),20px);text-transform:uppercase;text-align:center;font-size:clamp(12px,calc(3.8*var(--u)),21px);
 background:linear-gradient(180deg,#d4e2ea 0%,#a9bdc9 45%,#8aa3b2 100%);box-shadow:inset 0 -3px 0 rgba(30,80,110,.55),inset 0 2px 0 rgba(255,255,255,.6);
 text-shadow:${OUTLINE(1.2)}}
.cols span:first-child,.row .rk{text-align:left}
.cols span:last-child,.row .pt{text-align:right}
.rows{margin:0;padding:clamp(4px,calc(1.2*var(--u)),8px) clamp(4px,calc(1.4*var(--u)),10px) clamp(6px,calc(1.6*var(--u)),10px);list-style:none;min-height:120px;
 background:linear-gradient(180deg,#3a8db0 0%,#2f7ca2 50%,#2a7094 100%)}
.row{height:clamp(32px,calc(8.6*var(--u)),44px);margin:0 0 clamp(3px,calc(.9*var(--u)),6px);padding:0 clamp(5px,calc(1.8*var(--u)),14px);border-radius:clamp(6px,calc(1.6*var(--u)),9px);border:1px solid #1b2329;
 font-size:clamp(12px,calc(3.9*var(--u)),21px);line-height:1;
 background:linear-gradient(180deg,#2e373e 0%,#4d5961 22%,#6f7c84 52%,#566169 74%,#3a444b 100%);
 box-shadow:inset 0 1px 0 rgba(255,255,255,.28),inset 0 -1px 0 rgba(0,0,0,.5),0 2px 0 rgba(150,225,250,.45);
 text-shadow:${OUTLINE(1)}}
.row .nm{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-align:center;color:var(--orange);font-size:1.08em;text-shadow:${OUTLINE(1.2)}}
.row .rc{text-align:center;white-space:nowrap;font-size:.94em}
.row .lp{text-align:center;white-space:nowrap;font-size:.72em;font-weight:700;color:#d8e6ee}
.pick{display:flex;justify-content:center;margin:0;padding:8px 0 0;background:#2c7fa6}
.pick select{font:inherit;font-size:14px;padding:4px 8px;border-radius:6px;border:2px solid #0a2230;background:#12161a;color:#fff}
.pick[hidden]{display:none}
.row .pt{font-variant-numeric:tabular-nums}
@media (max-width:440px){.row .nm{font-size:.95em;letter-spacing:-.04em}.row .rc{font-size:.88em;letter-spacing:-.03em}.cols,.row{grid-template-columns:22px minmax(0,1fr) 68px 46px 32px;column-gap:4px}}
.note{margin:0;padding:clamp(22px,calc(7*var(--u)),44px) 12px;text-align:center;text-transform:uppercase;font-size:clamp(16px,calc(4.8*var(--u)),24px);line-height:1.2;
 text-shadow:${OUTLINE(1.2)},0 2px 3px rgba(0,0,0,.6)}
.foot{max-width:780px;margin:14px auto 0;text-align:center;font-size:12px;font-weight:700;letter-spacing:.03em;color:#8d949b}
.sr{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}
@media (prefers-reduced-motion:no-preference){.row{animation:in .25s ease-out both}@keyframes in{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}}
`;

const JS = String.raw`
(function () {
  var BOARDS = ['season', 'daily', 'alltime', 'archive'];
  var TITLES = ['SEASON LEADERS', 'DAILY LEADERS', 'ALL-TIME LEADERS', 'PAST SEASONS'];
  var EMPTY = ['NO MATCHES PLAYED YET THIS SEASON', 'NO MATCHES PLAYED YET TODAY', 'NO RANKED MATCHES PLAYED YET', 'NO SEASON HAS ENDED YET'];
  var REFRESH_MS = 60000;
  var $ = function (id) { return document.getElementById(id); };
  var cache = [null, null, null, null];
  var pastSeason = null;   // the archived season shown on the last tab (its start, ms)
  var tabOf = function (h) { var i = BOARDS.indexOf(String(h).replace('#', '')); return i < 0 ? 0 : i; };
  var tab = tabOf(location.hash);

  // a date in the reader's own format (the browser's locale and time zone); the service gives the hour
  function day(ms) {
    if (typeof ms !== 'number' || !isFinite(ms) || ms <= 0) return '-';
    try { return new Date(ms).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }); } catch (e) { return '-'; }
  }

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = String(text);
    return e;
  }

  function span(ms) {
    var m = Math.max(0, Math.floor(ms / 60000));
    return { d: Math.floor(m / 1440), h: Math.floor(m / 60) % 24, m: m % 60 };
  }

  function clock() {
    var data = cache[tab] || cache[1 - tab];
    var line = $('time');
    if (!data || !data.season) { line.textContent = ''; return; }
    var now = Date.now();
    var s = span(data.season.endMs - now);
    var text = 'SEASON TIME REMAINING: ' + s.d + ' D, ' + s.h + ' H, ' + s.m + ' M';
    if (tab === 3 && cache[3] && cache[3].archived) {
      text = 'SEASON ' + (cache[3].archived.index + 1) + ': ' + day(cache[3].archived.startMs) + ' - ' + day(cache[3].archived.endMs - 1);
    }
    if (tab === 1 && cache[1] && cache[1].day) {
      var d = span(cache[1].day.endMs - now);
      text = 'NEXT DAILY RESET: ' + d.h + ' H, ' + d.m + ' M';
    }
    line.textContent = text;
  }

  function note(text) {
    var rows = $('rows');
    rows.replaceChildren();
    var li = el('li', 'note', text);
    rows.appendChild(li);
  }

  function draw() {
    var data = cache[tab];
    $('title').textContent = TITLES[tab];
    document.title = TITLES[tab] + ' - Strikers Charged';
    var dots = document.querySelectorAll('.dots button');
    for (var i = 0; i < dots.length; i++) dots[i].setAttribute('aria-current', i === tab ? 'true' : 'false');
    clock();
    if (!data) return;
    if (data.error) { note(data.error); return; }
    if (!data.rows.length) { note(EMPTY[tab]); return; }
    var rows = $('rows');
    rows.replaceChildren();
    data.rows.forEach(function (r) {
      var li = el('li', 'row');
      li.appendChild(el('span', 'rk', r.rank));
      li.appendChild(el('span', 'nm', r.name));
      li.appendChild(el('span', 'rc', 'W: ' + r.wins + ' - L: ' + r.losses));
      li.appendChild(el('span', 'lp', day(r.lastPlayedMs)));
      li.appendChild(el('span', 'pt', r.points));
      rows.appendChild(li);
    });
  }

  function failure(status, body) {
    if (status === 503) return (body && body.error && body.error.message ? String(body.error.message) : 'THE LEADERBOARD IS SWITCHED OFF FOR NOW').toUpperCase();
    if (status === 429) return 'TOO MANY REQUESTS. TRY AGAIN IN A MINUTE';
    return 'THE LEADERBOARD CANNOT BE REACHED RIGHT NOW';
  }

  // the past-seasons picker: one option per archived season, newest first (text only; values are numbers from the service)
  function picker(list) {
    var box = $('pick'), sel = $('season');
    box.hidden = tab !== 3;
    if (!list) return;
    sel.replaceChildren();
    list.forEach(function (x) {
      var o = el('option', '', 'SEASON ' + (Number(x.index) + 1) + ' (' + day(x.startMs) + ')');
      o.value = String(Number(x.startMs));
      sel.appendChild(o);
    });
    if (pastSeason !== null) sel.value = String(pastSeason);
  }

  function load(i) {
    var q = '?board=' + BOARDS[i] + '&limit=50';
    if (i === 3 && pastSeason !== null) q += '&season=' + pastSeason;
    fetch('/v1/public/leaderboard' + q, { headers: { accept: 'application/json' } })
      .then(function (resp) {
        return resp.json().catch(function () { return null; }).then(function (body) { return { resp: resp, body: body }; });
      })
      .then(function (x) {
        if (x.resp.ok && x.body && Array.isArray(x.body.rows)) cache[i] = x.body;
        else cache[i] = { error: failure(x.resp.status, x.body), rows: [] };
      }, function () { cache[i] = { error: failure(0), rows: [] }; })
      .then(function () {
        if (i === 3 && cache[3] && Array.isArray(cache[3].seasons)) {
          // the list came first: show it, then the newest season's standings
          picker(cache[3].seasons);
          if (cache[3].seasons.length) { pastSeason = Number(cache[3].seasons[0].startMs); picker(cache[3].seasons); load(3); return; }
        }
        if (i === tab) draw(); else clock();
      });
  }

  function show(i) {
    tab = (i + BOARDS.length) % BOARDS.length;
    try { history.replaceState(null, '', '#' + BOARDS[tab]); } catch (e) { /* sandboxed */ }
    $('pick').hidden = tab !== 3;
    if (!cache[tab] || cache[tab].error) { $('title').textContent = TITLES[tab]; note('LOADING...'); load(tab); } else draw();
  }

  $('prev').addEventListener('click', function () { show(tab - 1); });
  $('next').addEventListener('click', function () { show(tab + 1); });
  var dots = document.querySelectorAll('.dots button');
  for (var k = 0; k < dots.length; k++) (function (n) { dots[n].addEventListener('click', function () { show(n); }); })(k);
  document.addEventListener('keydown', function (e) {
    if (e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return;
    if (e.key === 'ArrowLeft') { show(tab - 1); e.preventDefault(); }
    else if (e.key === 'ArrowRight') { show(tab + 1); e.preventDefault(); }
  });
  window.addEventListener('hashchange', function () { var n = tabOf(location.hash); if (n !== tab) show(n); });
  $('season').addEventListener('change', function () { pastSeason = Number($('season').value); cache[3] = null; $('title').textContent = TITLES[3]; note('LOADING...'); load(3); });
  setInterval(clock, 15000);
  setInterval(function () { if (!document.hidden) { load(tab); } }, REFRESH_MS);
  document.addEventListener('visibilitychange', function () { if (!document.hidden) load(tab); });
  show(tab);
})();
`;

export const PAGE_BODY = `<main>
<header class="hub"><h1>Online Leaderboard</h1><p id="time" aria-live="off"></p></header>
<ul class="dots" aria-label="Boards"><li><button type="button" aria-label="Season leaders" aria-current="true"></button></li><li><button type="button" aria-label="Daily leaders" aria-current="false"></button></li><li><button type="button" aria-label="All-time leaders" aria-current="false"></button></li><li><button type="button" aria-label="Past seasons" aria-current="false"></button></li></ul>
<div class="stage">
<button type="button" id="prev" class="arrow prev" aria-label="Previous board">Previous</button>
<section class="frame" aria-labelledby="title"><div class="board">
<div class="title"><h2 id="title" aria-live="polite">SEASON LEADERS</h2></div>
<p class="pick" id="pick" hidden><label class="sr" for="season">Season</label><select id="season"></select></p>
<div class="cols" aria-hidden="true"><span>Rank</span><span>Name</span><span>Record</span><span>Last played</span><span>Pts</span></div>
<ol class="rows" id="rows"><li class="note">LOADING...</li></ol>
</div></section>
<button type="button" id="next" class="arrow next" aria-label="Next board">Next</button>
</div>
<p class="foot">Mario Strikers Charged on PS Vita &mdash; fan project</p>
</main>`;

export const PAGE_CSS = CSS;
export const PAGE_JS = JS;

export function pageHtml() {
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<meta name="theme-color" content="#16181b">
<meta name="referrer" content="no-referrer">
<title>Leaderboard - Strikers Charged</title>
<link rel="icon" href="data:,">
<style>${CSS}</style>
</head>
<body>
${PAGE_BODY}
<script>${JS}</script>
</body>
</html>
`;
}

async function sha256b64(text) {
  return b64encode(new Uint8Array(await crypto.subtle.digest('SHA-256', te.encode(text))));
}

let memo = null;

/** The page with its headers: a strict CSP (the two inline blocks by hash, same-origin fetch only) and a short public cache. */
export async function pageResponse() {
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
    memo = { html: pageHtml(), csp };
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
