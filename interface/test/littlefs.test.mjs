import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import ts from 'typescript';
import { rollup } from 'rollup';

const source = await readFile(new URL('../vite-plugin-littlefs.ts', import.meta.url), 'utf8');
const compiled = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext } }).outputText;
const { default: littleFS } = await import(`data:text/javascript;base64,${Buffer.from(compiled).toString('base64')}`);
const deviceName = 'abcdefghijklmnopqrstuvwx';

async function generate(output) {
 const bundle = await rollup({
  input: deviceName,
  plugins: [{
   name: 'isolated-device-assets',
   resolveId(id) { if (id === deviceName || id === 'lazy') return id; },
   load(id) {
    if (id === deviceName) {
     this.emitFile({ type: 'asset', name: `${deviceName}.css`, source: 'body{color:inherit}' });
     return "export const load = () => import('lazy');";
    }
    if (id === 'lazy') return 'export const value = 42;';
   }
  }, ...littleFS()]
 });
 try { return (await bundle.generate({ format: 'es', ...output })).output; }
 finally { await bundle.close(); }
}

for (const functional of [false, true]) {
 test(`generated ESP assets fit 32-byte basenames with ${functional ? 'functional' : 'string'} naming templates`, async () => {
  const pattern = '[name].[hash:12]';
  const output = await generate({
   entryFileNames: functional ? () => `${pattern}.js` : `${pattern}.js`,
   chunkFileNames: functional ? () => '[hash:12].js' : '[hash:12].js',
   assetFileNames: functional ? () => `${pattern}[extname]` : `${pattern}[extname]`
  });
  const names = output.map(item => item.fileName);
  assert.ok(names.includes(`${deviceName}.js`), 'long entry must stay addressable within the filesystem limit');
  assert.ok(names.includes(`${deviceName}.css`), 'long stylesheet must stay addressable within the filesystem limit');
  assert.ok(output.some(item => item.type === 'chunk' && item.isDynamicEntry && item.code.includes('42')));
  assert.equal(new Set(names).size, names.length, 'lazy entry cannot collide with the main entry');
  for (const name of names) assert.ok(Buffer.byteLength(name.split('/').at(-1)) <= 32, `${name} exceeds the ESP filesystem basename limit`);
 });
}
