// Instrument only the actual pressure scan; do not time the instrumented build.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = process.argv[2] ? path.resolve(process.argv[2]) : fs.mkdtempSync(path.join(os.tmpdir(), 'vgx-scan-'));
if (process.argv[2]) fs.mkdirSync(out, {recursive: false});
const sourceFile = path.join(root, 'platform/vgx/src/vgx_backend_gl.cpp');
const source = fs.readFileSync(sourceFile, 'utf8');
const hash = s => crypto.createHash('sha256').update(s).digest('hex');
const sourceHash = hash(source);
const marker = '        const Program& p = kv.second;';
if (source.indexOf(marker) < 0 || source.indexOf(marker) !== source.lastIndexOf(marker)) throw Error('Pressure scan owner changed');
fs.writeFileSync(path.join(out, 'instrumented.cpp'), source.replace(marker, marker + '\n        ++::pressureScanVisits;'));
const common = `clang++ -std=c++17 -stdlib=libc++ -g -DVGX_NO_NEON -D__VGX_VITA__ -DPRESSURE_SCAN_CACHE -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0
 -ffunction-sections -fdata-sections -Wl,--gc-sections
 -Wl,--wrap=glDrawElements,--wrap=glFinish,--wrap=glGetError,--wrap=glReadPixels,--wrap=getenv,--wrap=glCreateProgram,--wrap=glDeleteProgram
 -Wl,--wrap=printf,--wrap=vprintf,--wrap=fprintf,--wrap=vfprintf,--wrap=puts,--wrap=putchar,--wrap=fputs,--wrap=fputc,--wrap=putc,--wrap=fwrite
 -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src -Iplatform/vgx/third_party`.replaceAll('\n', ' ');
for (const stage of ['checks', 'cost']) {
  const checking = stage === 'checks';
  const compile = common + (checking ? ' -O1 -fsanitize=address,undefined -fno-sanitize-link-runtime -fno-omit-frame-pointer' : ' -O2') +
    ' tests/' + (checking ? 'vgx_shader_scan_cache.cpp' : 'vgx_shader_pressure_cost.cpp') +
    ' platform/vgx/src/vgx_shadergen.cpp vita/vgx-game/silent_stdio.cpp -o /tmp/scan -lEGL -lGLESv2 -pthread' +
    (checking ? ' -l:libasan.so.8 -l:libubsan.so.1' : '');
  const script = compile + '\n' + (checking ?
    'export LD_PRELOAD=libasan.so.8 ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1\nfor mode in deadline postpone new-retry legacy utility off reset trace; do timeout 90s /tmp/scan "$mode"; done\n' :
    '/tmp/scan\n');
  const mounts = ['--mount', `type=bind,source=${root},target=/work,readonly`];
  if (checking) mounts.push('--mount', `type=bind,source=${out}/instrumented.cpp,target=/work/platform/vgx/src/vgx_backend_gl.cpp,readonly`);
  const r = spawnSync('docker', ['run', '-i', '--rm', '--pull=never', '--platform', 'linux/arm64', '--network', 'none', '--cpus=2',
    ...mounts, '-w', '/work', '-e', 'EGL_PLATFORM=surfaceless', '-e', 'LIBGL_ALWAYS_SOFTWARE=1', '-e', 'GALLIUM_DRIVER=softpipe',
    'codex-galaxy-native:20261007-arm64', 'bash', '-euo', 'pipefail', '-s'],
    {input: script, encoding: 'utf8', maxBuffer: 8 * 1024 * 1024, timeout: 600000});
  const output = (r.stdout || '') + (r.stderr || '');
  fs.writeFileSync(path.join(out, stage + '.log'), output);
  console.log(output.split('\n').filter(s => !s.startsWith('EVENT ')).join('\n'));
  if (r.status !== 0) throw Error(`${stage} failed: ${r.status} ${r.error || ''}`);
}
if (hash(fs.readFileSync(sourceFile)) !== sourceHash) throw Error('Backend changed during test');
fs.writeFileSync(path.join(out, 'result.json'), JSON.stringify({backend_sha256: sourceHash, status: 0, physical_vita: false}, null, 2) + '\n');
console.log('PASS current actual-source scan cache and complete-boundary host cost; outputs: ' + out);
