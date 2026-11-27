const fs = require('node:fs');
const path = require('node:path');
const {execFileSync} = require('node:child_process');
const dir = process.argv[2];
execFileSync(process.execPath, [path.join(__dirname, 'prepare_vgx_frame_sink_fixture.cjs'), dir]);
const root = path.resolve(__dirname, '..');
let source = fs.readFileSync(path.join(root, 'platform/vgx/src/vgx_backend_null.cpp'), 'utf8');
const begin = source.indexOf('void Execute(const CmdBuf& buf) {');
const end = source.indexOf('\nconst std::vector<uint8_t>& LastDisplayCopy', begin);
if (begin < 0 || end < begin) throw new Error('null Execute boundary not found');
let backend = source.slice(0, begin) + source.slice(end);
const warmPending = 'unsigned WarmPending() { return 0; }';
if (backend.split(warmPending).length !== 2) throw new Error('null WarmPending boundary not found');
backend = backend.replace(warmPending,
    'static unsigned fixedCapWarmPending = 0;\nunsigned WarmPending() { return fixedCapWarmPending; }');
fs.writeFileSync(path.join(dir, 'vgx_backend_fixture.cpp'), backend +
    '\nnamespace vgx::backend { void FixedCapSetStats(const Stats& stats) { g_stats = stats; }\n' +
    'void FixedCapSetWarmPending(unsigned count) { fixedCapWarmPending = count; } }\n');
const draw = fs.readFileSync(path.join(root, 'platform/vgx/src/vgx_draw.cpp'), 'utf8');
fs.writeFileSync(path.join(dir, 'vgx_draw_fixture.cpp'), draw + '\nnamespace vgx { unsigned FixedCapHudDepth() { return g_cha.hudDepth; } }\n');
if (process.argv[3] === 'remove-marker') {
    const state = fs.readFileSync(path.join(root, 'platform/vgx/src/vgx_state.cpp'), 'utf8');
    fs.writeFileSync(path.join(dir, 'vgx_state_mutant.cpp'), state.replace('PresentationMarker(kind);', '(void)kind;'));
}
