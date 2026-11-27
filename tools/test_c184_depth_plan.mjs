import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(root, 'platform/vgx/src/vgx_backend_gl.cpp'), 'utf8');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'c184-depth-plan-'));
const between = (a, b, start = 0) => {
    const first = source.indexOf(a, start), last = source.indexOf(b, first);
    if (first < 0 || last < first) throw new Error(`Production fixture not found: ${a}`);
    return source.slice(first, last);
};
try {
    const events = between('enum : uint8_t { EV_READ', 'bool g_valid');
    const init = between('const uint32_t none = static_cast<uint32_t>(positions);', '    if (s26On)', source.indexOf('void PlanFrame('));
    const reverse = between('for (size_t p = positions; p-- > 0;)', '    g_valid = true;', source.indexOf('void PlanFrame('));
    fs.writeFileSync(path.join(out, 'depth_plan_fixture.inc'), `${events}\n__attribute__((noinline)) void Build(bool tail) {\nconst size_t positions = g_ev.size() - 1;\n${init}\ng_live[positions] = tail ? 1 : 0;\n${reverse}\n}\n`);
    const originalInit = `const uint32_t none = static_cast<uint32_t>(positions);
g_live.assign(positions + 1, 0);
g_nextLeave.assign(positions + 1, none);
g_nextRead.assign(positions + 1, none);
g_nextWrite.assign(positions + 1, none);
g_nextFull.assign(positions + 1, none);
g_liveC.assign(positions + 1, 1);
g_liveA.assign(positions + 1, 1);`;
    fs.writeFileSync(path.join(out, 'depth_plan_baseline.inc'), `namespace frozen {
std::vector<uint8_t> g_live, g_liveC, g_liveA;
std::vector<uint32_t> g_nextLeave, g_nextRead, g_nextWrite, g_nextFull;
__attribute__((noinline)) void Build(bool tail) {
const size_t positions = ::g_ev.size() - 1;
${originalInit}
g_live[positions] = tail ? 1 : 0;
${reverse}
}
}\n`);
    const run = (command, args) => {
        const r = spawnSync(command, args, { stdio: 'inherit', cwd: root });
        if (r.status !== 0) process.exitCode = r.status ?? 1;
        return r.status === 0;
    };
    const binary = path.join(out, 'depth-plan');
    const common = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + out, '-Iplatform/vgx/src', 'tests/c184_depth_plan.cpp'];
    if (run(process.env.CXX || 'c++', [...common, '-fsanitize=address,undefined', '-o', binary]) && run(binary, [])) {
        if (run(process.env.CXX || 'c++', [...common, '-DC184_DEPTH_BENCH', '-o', binary])) run(binary, []);
    }
} finally { fs.rmSync(out, { recursive: true, force: true }); }
