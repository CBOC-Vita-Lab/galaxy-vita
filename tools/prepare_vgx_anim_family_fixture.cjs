const fs = require('node:fs');
const path = require('node:path');

const [root, output] = process.argv.slice(2);
if (!root || !output) throw new Error('source root and output file required');
function method(file, signature) {
    const text = fs.readFileSync(path.join(root, file), 'utf8');
    const begin = text.indexOf(signature);
    if (begin < 0 || text.indexOf(signature, begin + signature.length) !== -1)
        throw new Error(`unique method required: ${signature}`);
    const open = text.indexOf('{', begin);
    let depth = 0;
    for (let i = open; i < text.length; ++i) {
        if (text[i] === '{') ++depth;
        if (text[i] === '}' && --depth === 0) return text.slice(begin, i + 1);
    }
    throw new Error(`unclosed method: ${signature}`);
}
const live = 'decomp/src/Game/LiveActor/LiveActor.cpp';
const model = 'decomp/src/Game/LiveActor/ModelManager.cpp';
fs.writeFileSync(output, [
    method(live, 'void LiveActor::calcAnim() {'),
    method(live, 'void LiveActor::calcAnmMtx() {'),
    method(model, 'void ModelManager::calcAnim() {'),
    method(model, 'void ModelManager::calc() {'),
    method(model, 'void ModelManager::updateDL(bool isDiffDL) {'),
].join('\n\n') + '\n');
