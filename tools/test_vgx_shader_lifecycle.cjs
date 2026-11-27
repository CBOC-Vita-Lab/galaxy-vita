// Extract current owning functions, not copied policy, into host boundary tests.
const fs = require('fs'), path = require('path'), os = require('os');
const {spawnSync} = require('child_process');
const root = path.resolve(__dirname, '..');
const out = process.env.RESIDENCY_TEST_OUT || fs.mkdtempSync(path.join(os.tmpdir(), 'vgx-lifecycle-'));
fs.mkdirSync(out, {recursive: true});
function section(text, start, end, from = 0) {
  const a = text.indexOf(start, from), b = text.indexOf(end, a);
  if (a < 0 || b < a) throw Error('Owning source boundary changed');
  return text.slice(a, b);
}
const host = fs.readFileSync(path.join(root, 'platform/vgx/vita/vgx_host_galaxy_vita.cpp'), 'utf8');
fs.writeFileSync(path.join(out, 'hold_function.inc'), section(host, 'void NoteTitleVisible(', '\nvoid Fail('));
const calls = [...host.matchAll(/NoteTitleVisible\("[^"\n]*"(?:, true)?\)/g)].map(m => m[0]);
if (calls.length !== 3 || calls.filter(s => s.endsWith(', true)')).length !== 1 ||
    !host.includes('port_boot_title_is_visible()) NoteTitleVisible("title logo", true);'))
  throw Error('Review genuine-title provenance before changing hold semantics');
const galaxy = fs.readFileSync(path.join(root, 'platform/src/gx/vgx_galaxy.cpp'), 'utf8');
fs.writeFileSync(path.join(out, 'idle_loop.inc'), section(galaxy, '        while (sJobs.empty()) {', '\n#else', galaxy.indexOf('static const bool warmIdleOn')));
function run(cmd, args) {
  const r = spawnSync(cmd, args, {encoding: 'utf8', timeout: 120000});
  process.stdout.write((r.stdout || '') + (r.stderr || ''));
  if (r.status !== 0) throw Error(`${cmd} failed: ${r.status} ${r.error || ''}`);
}
for (const name of ['hold', 'idle']) {
  const binary = path.join(out, name);
  run(process.env.CXX || 'clang++', ['-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-I' + out,
    path.join(root, 'tests/vgx_shader_' + name + '.cpp'), '-o', binary]);
  const modes = name === 'idle' ? [[]] : ['hold-off', 'hold-finite', 'hold-unlimited', 'capacity-finite',
    'capacity-unlimited', 'fallback-capacity-finite', 'fallback-capacity-unlimited'].map(s => [s]);
  for (const args of modes) run(binary, args);
}
if (!process.env.RESIDENCY_TEST_OUT) fs.rmSync(out, {recursive: true});
