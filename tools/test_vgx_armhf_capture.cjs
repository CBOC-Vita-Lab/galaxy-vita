const fs = require('fs'), path = require('path'), os = require('os');
const {spawnSync} = require('child_process');
const root = path.resolve(__dirname, '..');
const out = process.argv[2] ? path.resolve(process.argv[2]) : fs.mkdtempSync(path.join(os.tmpdir(), 'vgx-capture-'));
if (process.argv[2]) fs.mkdirSync(out);
const source = fs.readFileSync(path.join(root, 'platform/src/gx/vgx_galaxy.cpp'), 'utf8');
const a = source.indexOf('extern "C" unsigned long long port_vgx_capture(const char* path, int) {');
const end = source.indexOf('\nextern "C" void port_gx_select_backend', a);
if (a < 0 || end < a) throw Error('Actual capture boundary changed');
fs.writeFileSync(path.join(out, 'capture_function.inc'), source.slice(a, end));
const completion = '        if (cap) { sCapWanted = false; sCapDone = true; sCapFrame = shown; }';
if (source.indexOf(completion) < 0 || source.indexOf(completion) !== source.lastIndexOf(completion)) throw Error('Actual completion owner changed');
fs.writeFileSync(path.join(out, 'capture_completion.inc'), completion + '\n');
let output = '';
function run(cmd, args) {
  const r = spawnSync(cmd, args, {encoding: 'utf8', timeout: 30000});
  output += (r.stdout || '') + (r.stderr || '');
  fs.writeFileSync(path.join(out, 'capture.log'), output);
  if (r.status !== 0) throw Error(`${cmd} failed ${r.status}: ${r.stderr || ''}`);
  return r;
}
for (const name of ['vgx_armhf_capture', 'vgx_capture_boundary']) {
  run(process.env.CXX || 'clang++', ['-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-pthread', '-DPORT_ARMHF_HARNESS=1', '-I' + out,
    '-I' + path.join(root, 'platform/include'),
    path.join(root, 'tests/' + name + '.cpp'), path.join(root, 'armhf/glue/stubs.cpp'), '-o', path.join(out, name)]);
  const r = run(path.join(out, name), []);
  const markers = r.stderr.trim().split('\n').filter(line => line.startsWith('ARMHF_CAPTURE '));
  const expected = name === 'vgx_armhf_capture' ? [
    'ARMHF_CAPTURE active=1 frame=42', 'ARMHF_CAPTURE active=1 frame=0',
    'ARMHF_CAPTURE active=0 frame=0', 'ARMHF_CAPTURE active=1 frame=43',
  ] : ['ARMHF_CAPTURE active=1 frame=42', 'ARMHF_CAPTURE active=1 frame=0', 'ARMHF_CAPTURE active=1 frame=0'];
  if (JSON.stringify(markers) !== JSON.stringify(expected)) throw Error('Capture evidence differs from actual fixture outcomes');
  if (r.stderr.includes('ARMHF_VCLOCK_FALLBACK')) throw Error('Clock fallback occurred');
}
console.log(output);
console.log('PASS forwarding and actual capture boundary; outputs: ' + out);
