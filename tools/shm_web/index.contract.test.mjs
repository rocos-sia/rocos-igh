import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';
import * as decoder from './value_decoder.mjs';

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


test('renders and refreshes status tags while type changes preserve them', () => {
  const context = vm.createContext({ ...decoder, selectedTypes: new Map() });
  const functions = html.slice(html.indexOf('  function renderTypeSelector'), html.indexOf('  function setText'));
  vm.runInContext(functions, context);
  const variable = { index: '6041', sub_index: 0, size: 2, offset: 14, bytes: '0812', name: 'Status Word' };
  const rendered = context.renderVariableRow(0, 'in', variable);
  assert.match(rendered, /08 12 = 4616/);
  assert.match(rendered, /status-bad/);
  assert.match(rendered, /故障/);
  assert.equal((rendered.match(/class="status-tag /g) || []).length, 1);
  assert.doesNotMatch(rendered, /远程控制|模式专用位|bit 12/);
  const cells = { '.raw-value': {}, '.status-tags': {} };
  const row = { dataset: {}, querySelector: selector => cells[selector] };
  const container = { querySelector: () => row };
  context.updateVariableRow(container, 0, 'in', { ...variable, bytes: '2700' });
  assert.equal(cells['.raw-value'].textContent, '27 00 = 39');
  assert.match(cells['.status-tags'].innerHTML, /运行已使能/);
  assert.doesNotMatch(cells['.status-tags'].innerHTML, /status-bad/);
  const tagsBefore = cells['.status-tags'].innerHTML;
  context.$ = () => ({ addEventListener: (_, handler) => { context.change = handler; } });
  vm.runInContext(html.slice(html.indexOf("  $('slaves').addEventListener('change'"), html.indexOf('  // 优先用 SSE')), context);
  context.change({ target: { closest: () => ({ dataset: { key: 'test' }, value: 'INT16', closest: () => row }) } });
  assert.equal(cells['.status-tags'].innerHTML, tagsBefore);
  assert.equal(context.renderStatusTags({ ...variable, index: '6040' }), '');
  context.updateVariableRow(container, 0, 'in', { ...variable, bytes: '' });
  assert.equal(cells['.raw-value'].textContent, '-');
  assert.match(cells['.status-tags'].innerHTML, /状态字数据无效/);
});
