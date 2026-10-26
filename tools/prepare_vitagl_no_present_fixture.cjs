const fs = require('node:fs');
const path = require('node:path');
const [sourceRoot, output] = process.argv.slice(2);
if (!sourceRoot || !output) throw new Error('usage: preparer source-root output');
const dir = fs.existsSync(path.join(sourceRoot, 'gxm.c')) ? sourceRoot : path.join(sourceRoot, 'source');
const text = fs.readFileSync(path.join(dir, 'gxm.c'), 'utf8');
function functionText(name) {
    const pattern = new RegExp('(?:static inline __attribute__\\(\\(always_inline\\)\\) )?void ' + name + '\\(void\\) \\{');
    const match = pattern.exec(text);
    if (!match) throw new Error('actual patched gxm.c lacks ' + name + '; apply the no-present patch first');
    let depth = 1, end = match.index + match[0].length;
    for (; end < text.length && depth; ++end) {
        if (text[end] === '{') ++depth;
        else if (text[end] === '}') --depth;
    }
    if (depth) throw new Error('unbalanced function: ' + name);
    return text.slice(match.index, end);
}
const header = fs.readFileSync(path.join(dir, 'vitaGL.h'), 'utf8');
if (!header.includes('void vglSubmitWithoutSwap(void);')) throw new Error('public helper declaration missing');
fs.writeFileSync(output, functionText('scene_end') + '\n' + functionText('vglSubmitWithoutSwap') + '\n' + functionText('glFinish') + '\n');
