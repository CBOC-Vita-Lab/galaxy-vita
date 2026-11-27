const fs = require('node:fs');
const path = require('node:path');

const [root, output] = process.argv.slice(2);
if (!root || !output) throw new Error('source root and output file required');
function section(file, first, last) {
    const source = fs.readFileSync(path.join(root, file), 'utf8');
    const begin = source.indexOf(first);
    const end = source.indexOf(last, begin);
    if (begin < 0 || end < 0 || source.indexOf(first, begin + first.length) !== -1)
        throw new Error(`unique bounded production section required: ${first}`);
    return source.slice(begin, end);
}
fs.writeFileSync(output, 'namespace vgx {\n' + section('platform/vgx/src/vgx_common.h',
    'inline long EnvInt(', '// Quiet builds ignore observers') + '}\n' +
    '#define getenv TraceGetenv\n#undef errno\n#define errno TraceErrno()\n' +
    section('platform/src/port/perf.cpp', '// GXV_SLIP=1 (default 0): bounded slip,',
        'uint32_t port_frameskip_count(void)') + '\n#undef getenv\n#undef errno\n');
