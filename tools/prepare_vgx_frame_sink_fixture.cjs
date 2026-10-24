// Observe the real wait predicate without changing queue decisions or adding a sleep.
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'platform/src/gx/vgx_galaxy.cpp'), 'utf8');
const start = source.indexOf('void FrameSink(vgx::CmdBuf& buf) {');
const end = source.indexOf('\n// A display copy', start);
if (start < 0 || end < start) throw new Error('FrameSink not found');
const body = source.slice(start, end);
const predicate = 'return !sFree.empty() && sJobs.size() < sQueueLimit;';
if (body.split(predicate).length !== 2) throw new Error('Queue predicate changed');
let instrumented = body.replace(predicate,
  'const bool ready = !sFree.empty() && sJobs.size() < sQueueLimit; frame_sink_observe_ready(ready); return ready;');
const mutation = process.argv[3] || 'none';
if (mutation === 'bypass-limit') instrumented = instrumented.replace(
  'const bool ready = !sFree.empty() && sJobs.size() < sQueueLimit;', 'const bool ready = true;');
else if (mutation === 'early-finish') instrumented = instrumented.replace(
  'void FrameSink(vgx::CmdBuf& buf) {', 'void FrameSink(vgx::CmdBuf& buf) { PeFinish();');
else if (mutation !== 'none') throw new Error('Unknown fixture mutation');
fs.mkdirSync(process.argv[2], {recursive: true});
fs.writeFileSync(path.join(process.argv[2], 'vgx_frame_sink_fixture.cpp'), source.slice(0, start) + instrumented + source.slice(end));
