const fs = require('node:fs');
const path = require('node:path');

const [root, output] = process.argv.slice(2);
if (!root || !output) throw new Error('source root and output paths required');
function source(file) { return fs.readFileSync(path.join(root, file), 'utf8'); }
function block(text, signature) {
    const begin = text.indexOf(signature);
    if (begin < 0 || text.indexOf(signature, begin + signature.length) !== -1)
        throw new Error(`unique signature required: ${signature}`);
    const open = text.indexOf('{', begin);
    let depth = 0;
    for (let i = open; i < text.length; ++i) {
        if (text[i] === '{') ++depth;
        if (text[i] === '}' && --depth === 0) return text.slice(begin, i + 1);
    }
    throw new Error(`unclosed method: ${signature}`);
}
const header = source('decomp/include/Game/Scene/SceneFunction.hpp');
const scene = source('decomp/src/Game/Scene/GameScene.cpp');
const typed = source('decomp/src/Game/Scene/SceneFunction.cpp');
const executor = source('decomp/src/Game/Scene/SceneExecutor.cpp');
const lists = source('decomp/src/Game/NameObj/NameObjListExecutor.cpp');
const categories = source('decomp/src/Game/NameObj/NameObjCategoryList.cpp');
const enums = ['MovementType', 'CalcAnimType', 'DrawType'].map(name => block(header, `enum ${name} {`) + ';').join('\n');
fs.writeFileSync(path.join(output, 'vgx_anim_enums.inc'), `namespace MR {\n${enums}\n}\n`);
const methods = [
    block(scene, 'void GameScene::calcAnim() {'),
    block(typed, 'void CategoryList::execute(MR::MovementType type) {'),
    block(typed, 'void CategoryList::execute(MR::CalcAnimType type) {'),
    block(typed, 'void CategoryList::execute(MR::DrawType type) {'),
    block(executor, 'void SceneFunction::executeCalcAnimList() {'),
    block(executor, 'void SceneFunction::executeCalcAnimListOnPlayingMovie() {'),
    block(lists, 'void NameObjListExecutor::executeMovement(int category) {'),
    block(lists, 'void NameObjListExecutor::executeCalcAnim(int category) {'),
    block(categories, 'void NameObjCategoryList::execute(int idx) {'),
];
fs.writeFileSync(path.join(output, 'vgx_anim_methods.inc'), methods.join('\n\n') + '\n');
