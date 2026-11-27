const fs = require('node:fs');

const [sourcePath, outputPath] = process.argv.slice(2);
if (!sourcePath || !outputPath) throw new Error('source and output paths required');
const source = fs.readFileSync(sourcePath, 'utf8');
const start = source.indexOf('void GameScene::calcAnim() {');
const end = source.indexOf('\nvoid GameScene::notifyEndScenarioStarter()', start);
if (start < 0 || end < 0) throw new Error('unique GameScene calcAnim boundaries required');
const body = source.slice(start, end);
const names = ['port_workload_view_entry_begin()', 'SceneFunction::executeCalcViewAndEntryList()', 'port_workload_view_entry_end()'];
let previous = -1;
for (const name of names) {
    const at = body.indexOf(name);
    if (at <= previous || body.indexOf(name, at + name.length) !== -1)
        throw new Error('calcAnim must bracket its one existing view/entry call with paired workload hooks');
    previous = at;
}
fs.writeFileSync(outputPath, body);
