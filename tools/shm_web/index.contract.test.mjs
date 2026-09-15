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


test('only mapped supported OUT rows have an editor and updates preserve input', () => {
  const context = vm.createContext({ ...decoder, selectedTypes: new Map() });
  vm.runInContext(html.slice(html.indexOf('  function renderTypeSelector'), html.indexOf('  function setText')), context);
  const variable = { index: '607A', sub_index: 0, size: 4, offset: 0, bytes: '00000000', name: 'Target Position' };
  assert.match(context.renderVariableRow(0, 'out', variable), /form class="output-editor"/);
  assert.doesNotMatch(context.renderVariableRow(0, 'in', variable), /form class="output-editor"/);
  assert.doesNotMatch(context.renderVariableRow(0, 'out', { ...variable, index: '0000' }), /form class="output-editor"/);
  assert.doesNotMatch(context.renderVariableRow(0, 'out', { ...variable, size: 3 }), /form class="output-editor"/);
  const cells = { '.raw-value': {}, '.status-tags': {} };
  // Any attempt to replace the row or touch the editor during a snapshot fails here.
  const row = { dataset: {}, querySelector: selector => { assert.ok(selector in cells); return cells[selector]; } };
  context.updateVariableRow({ querySelector: () => row }, 0, 'out', { ...variable, bytes: '01000000' });
  assert.equal(cells['.raw-value'].textContent, '01 00 00 00 = 1');
});

test('OUT form submits typed bytes once with exact target and reports failures', async () => {
  const result = { textContent: '' };
  const button = { disabled: false };
  const row = {
    dataset: { key: '2:out:607A:0:14', size: '4' },
    querySelector: () => ({ value: 'INT32' }),
  };
  const form = {
    dataset: {}, elements: { value: { value: '-123' } },
    closest: () => row,
    querySelector: selector => selector === 'button' ? button : result,
  };
  const calls = [];
  const context = vm.createContext({
    ...decoder, currentMasterId: 7, canWrite: true, AbortController, setTimeout, clearTimeout,
    $: () => ({ addEventListener: (_, handler) => { context.submit = handler; } }),
    fetch: async (url, options) => { calls.push({ url, ...options }); return { ok: true, json: async () => ({ ok: true }) }; },
  });
  vm.runInContext(html.slice(html.indexOf("  $('slaves').addEventListener('submit'"), html.indexOf("  $('slaves').addEventListener('change'")), context);
  const event = { target: { closest: () => form }, preventDefault() {} };
  await context.submit(event);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, '/api/output');
  assert.equal(calls[0].method, 'POST');
  assert.equal(calls[0].headers['X-Rocos-Write'], '1');
  assert.equal(calls[0].body, '7 2 24698 0 14 4 85ffffff');
  assert.match(result.textContent, /已写入/);
  assert.equal(button.disabled, false);
  form.elements.value.value = '2147483648';
  await context.submit(event);
  assert.equal(calls.length, 1);
  assert.match(result.textContent, /范围/);
  form.elements.value.value = '123';
  context.fetch = async () => ({ ok: false, json: async () => ({ error: '配置已改变' }) });
  await context.submit(event);
  assert.equal(result.textContent, '配置已改变');
  context.canWrite = false;
  await context.submit(event);
  assert.match(result.textContent, /连接不可用/);
  assert.equal(button.disabled, true);
});
