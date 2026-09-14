import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import test from 'node:test';

const html = readFileSync(new URL('./index.html', import.meta.url), 'utf8');
const cmake = readFileSync(new URL('./CMakeLists.txt', import.meta.url), 'utf8');
const server = readFileSync(new URL('./shm_web_server.cpp', import.meta.url), 'utf8');
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
  assert.match(html, /font-family:\s*["']Alibaba PuHuiTi["']/);
  assert.match(html, /--ui:[^;]*["']Alibaba PuHuiTi["']/);
  assert.match(html, /url\(["']\.\/AlibabaPuHuiTi-3-55-Regular\.woff2["']\)/);
  assert.equal(
    existsSync(new URL('./AlibabaPuHuiTi-3-55-Regular.woff2', import.meta.url)),
    true,
    'expected the Alibaba PuHuiTi webfont asset',
  );
  assert.match(cmake, /AlibabaPuHuiTi-3-55-Regular\.woff2/);
  assert.match(server, /\/AlibabaPuHuiTi-3-55-Regular\.woff2/);
  assert.match(server, /font\/woff2/);
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
