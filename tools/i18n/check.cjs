// Run from any directory: node tools/i18n/check.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const han = /[\u3400-\u9fff]/;
const params = text => [...text.matchAll(/\{(\w+)\}/g)].map(m => m[1]).sort();
const printf = text => [...text.matchAll(/%(?:\d+\$)?[-+0-9.#]*[a-zA-Z%]/g)].map(m => m[0]).sort();

for (const [directory, files] of [
  ['enrollment_app/locales', [
    ...fs.readdirSync(path.join(root, 'enrollment_app/js')).map(f => 'enrollment_app/js/' + f),
    'enrollment_app/index.html.in']],
  ['installer/LockFaceSetup/frontend/src/locales', [
    'installer/LockFaceSetup/frontend/src/App.vue', 'installer/LockFaceSetup/frontend/src/i18n.ts']]
]) {
  const zh = JSON.parse(read(directory + '/zh-CN.json'));
  const en = JSON.parse(read(directory + '/en.json'));
  assert.deepEqual(Object.keys(zh).sort(), Object.keys(en).sort(), directory + ': language keys differ');
  for (const key of Object.keys(zh)) {
    assert.equal(typeof zh[key], 'string', key);
    assert.equal(typeof en[key], 'string', key);
    assert.ok(zh[key] && en[key] && !han.test(en[key]), key + ': empty or untranslated English');
    assert.deepEqual(params(zh[key]), params(en[key]), key + ': placeholders differ');
  }
  for (const file of files) {
    const source = read(file);
    for (const match of source.matchAll(/(?:lfT\(\s*['"]|data-i18n(?:-[\w-]+)?=['"])([\w-]+)/g)) {
      assert.ok(Object.hasOwn(zh, match[1]), file + ': missing key ' + match[1]);
    }
  }
  console.log(directory + ': ' + Object.keys(zh).length + ' bilingual messages checked');
}

const native = new Map([...read('common/ui_messages.inc').matchAll(/\{L("(?:\\.|[^"\\])*"), L("(?:\\.|[^"\\])*")\}/g)]
  .map(m => [JSON.parse(m[1]), JSON.parse(m[2])]));
for (const [source, translated] of native) {
  assert.ok(translated && !han.test(translated), source);
  assert.deepEqual(printf(source), printf(translated), source);
}
// Every Chinese native UI/error message, including those relayed by the worker,
// must have an English rendering. The wire values themselves stay unchanged.
const nativeFiles = ['enrollment_app/main.cpp',
  ...['credential_provider', 'face_service'].flatMap(dir => fs.readdirSync(path.join(root, dir))
    .filter(f => /\.(cpp|h)$/.test(f)).map(f => dir + '/' + f))];
for (const file of nativeFiles) {
  const tokens = read(file).match(/\/\/[^\n]*|\/\*[\s\S]*?\*\/|L"(?:\\.|[^"\\])*"|"(?:\\.|[^"\\])*"/g) || [];
  for (const token of tokens) {
    if (!token.startsWith('L"') || !han.test(token)) continue;
    const value = JSON.parse(token.slice(1));
    assert.ok(native.has(value), file + ': missing native translation: ' + value);
  }
}
const go = JSON.parse(read('installer/LockFaceSetup/internal/ui_messages.json'));
for (const [source, translated] of Object.entries(go)) {
  assert.ok(translated && !han.test(translated), source);
  assert.deepEqual(printf(source), printf(translated), source + ': Go format verbs differ');
}
for (const file of ['installer/LockFaceSetup/main.go', 'installer/LockFaceSetup/app.go', 'installer/LockFaceSetup/internal/scm.go', 'installer/LockFaceSetup/internal/uninstall_entry.go']) {
  const tokens = read(file).match(/\/\/[^\n]*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\])*"/g) || [];
  for (const token of tokens) {
    if (!token.startsWith('"') || !han.test(token)) continue;
    const value = JSON.parse(token);
    assert.ok(Object.hasOwn(go, value), file + ': missing Go translation: ' + value);
  }
}

const zhNotice = read('docs/legal/privacy-notice.txt');
const enNotice = read('docs/legal/privacy-notice.en.txt');
assert.equal(zhNotice.match(/版本：([\d.]+)/)[1], enNotice.match(/Version: ([\d.]+)/)[1], 'privacy versions differ');
const clauses = text => [...text.matchAll(/^(\d+\.\d+) /gm)].map(m => m[1]);
assert.deepEqual(clauses(zhNotice), clauses(enNotice), 'privacy clause numbers differ');
console.log('Native messages, Go format verbs and privacy editions checked');
