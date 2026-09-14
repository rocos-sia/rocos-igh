import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const html = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const runtimeIds = [
  'dot', 'statustext', 'masterid', 'state', 'reqstate', 'auth', 'slavenum',
  'dt', 'cur', 'min', 'max', 'avg', 'ts', 'mode', 'reset', 'slaves',
];

test('keeps every runtime-owned DOM target exactly once', () => {
  for (const id of runtimeIds) {
    const matches = html.match(new RegExp(`id=["']${id}["']`, 'g')) || [];
    assert.equal(matches.length, 1, `expected one #${id}, found ${matches.length}`);
  }
});

test('provides the engineering workbench layout hooks', () => {
  assert.match(html, /class=["'][^"']*workbench-bar/);
  assert.match(html, /class=["'][^"']*metrics-strip/);
  assert.match(html, /class=\\?['"][^'"]*table-scroll/);
});

test('provides responsive and keyboard-focus styling', () => {
  assert.match(html, /@media\s*\([^)]*max-width/);
  assert.match(html, /:focus-visible/);
  assert.match(html, /overflow-x:\s*auto/);
});

test('uses the snapshot master ID and a modern Chinese UI font', () => {
  assert.match(html, /setText\(['"]masterid['"],\s*snapshot\.master_id/);
  assert.match(html, /--ui:[^;]*["']Noto Sans CJK SC["']/);
});

test('declares a light interface color scheme', () => {
  assert.match(html, /color-scheme:\s*light/);
});

test('provides a persisted light and dark theme toggle', () => {
  assert.match(html, /id=["']theme-toggle["']/);
  assert.match(html, /\[data-theme=["']dark["']\]/);
  assert.match(html, /localStorage\.getItem\(["']rocos-igh-theme["']\)/);
  assert.match(html, /localStorage\.setItem\(["']rocos-igh-theme["']/);
});
