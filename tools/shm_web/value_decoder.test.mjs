import assert from 'node:assert/strict';
import test from 'node:test';

import {
  compatibleTypeForSize,
  decodeValue,
  encodeValue,
  decodeStatusWord,
  formatTypedBytes,
  typesForSize,
  variableKey,
} from './value_decoder.mjs';

test('offers only data types matching the PDO byte width', () => {
  assert.deepEqual(typesForSize(1), ['UINT8', 'INT8']);
  assert.deepEqual(typesForSize(2), ['UINT16', 'INT16']);
  assert.deepEqual(typesForSize(4), ['UINT32', 'INT32', 'FLOAT']);
  assert.deepEqual(typesForSize(3), []);
});

test('falls back when a saved type does not match the current byte width', () => {
  assert.equal(compatibleTypeForSize(4, 'INT32'), 'INT32');
  assert.equal(compatibleTypeForSize(2, 'INT32'), 'UINT16');
  assert.equal(compatibleTypeForSize(3, 'INT32'), undefined);
});

test('decodes unsigned integers as little-endian values', () => {
  assert.equal(decodeValue('FE', 'UINT8'), 254);
  assert.equal(decodeValue('3412', 'UINT16'), 0x1234);
  assert.equal(decodeValue('78563412', 'UINT32'), 0x12345678);
});

test('decodes signed integers as little-endian values', () => {
  assert.equal(decodeValue('FE', 'INT8'), -2);
  assert.equal(decodeValue('FEFF', 'INT16'), -2);
  assert.equal(decodeValue('FEFFFFFF', 'INT32'), -2);
});

test('decodes IEEE-754 single-precision little-endian values', () => {
  assert.equal(decodeValue('0000C03F', 'FLOAT'), 1.5);
});

test('formats hexadecimal bytes with the selected decoded value', () => {
  assert.equal(formatTypedBytes('0cd70300', 'UINT32'), '0C D7 03 00 = 251660');
  assert.equal(formatTypedBytes('feff', 'INT16'), 'FE FF = -2');
});

test('keys selections by slave, direction, object identity, and offset', () => {
  const variable = { index: '6040', sub_index: 0, offset: 8 };
  assert.equal(variableKey(2, 'out', variable), '2:out:6040:0:8');
  assert.notEqual(variableKey(2, 'in', variable), variableKey(2, 'out', variable));
  assert.notEqual(variableKey(3, 'out', variable), variableKey(2, 'out', variable));
});
// Statusword decoding must ignore display type and unrelated status bits.
const statusVariable = word => ({
  index: '6041', sub_index: 0, size: 2,
  bytes: [word & 255, word >> 8].map(b => b.toString(16).padStart(2, '0')).join(''),
});

test('decodes all eight CiA 402 states with unrelated bits set or cleared', () => {
  for (const [word, label] of [
    [0x00, '未准备好上电'], [0x40, '禁止上电'], [0x21, '准备好上电'],
    [0x23, '已上电'], [0x27, '运行已使能'], [0x07, '快速停机中'],
    [0x0f, '故障反应中'], [0x08, '故障'],
  ]) {
    for (const extra of [0, 0xff90]) {
      assert.equal(decodeStatusWord(statusVariable(word | extra)).tags[0].label, label);
    }
  }
  // Bit 5 is ignored in Fault, but distinguishes Quick stop from Operation enabled.
  assert.equal(decodeStatusWord(statusVariable(0x28)).tags[0].label, '故障');
  assert.equal(decodeStatusWord(statusVariable(0x01)).tags[0].label, '未知状态');
});

test('decodes the reported 08 12 example to Fault only', () => {
  const result = decodeStatusWord(statusVariable(0x1208));
  assert.equal(result.word, 4616);
  assert.deepEqual(result.tags, [{ label: '故障', tone: 'bad' }]);
  assert.equal(result.detail, 'CiA 402: 故障');
});

test('ignores every combination of bits outside the state coding mask', () => {
  const ignoredBits = [4, 7, 8, 9, 10, 11, 12, 13, 14, 15];
  for (const word of [0x00, 0x40, 0x21, 0x23, 0x27, 0x07, 0x0f, 0x08, 0x01]) {
    const expected = decodeStatusWord(statusVariable(word));
    for (let combination = 0; combination < (1 << ignoredBits.length); combination++) {
      const extra = ignoredBits.reduce((mask, bit, i) => mask | (((combination >> i) & 1) << bit), 0);
      const result = decodeStatusWord(statusVariable(word | extra));
      assert.deepEqual(result.tags, expected.tags);
      assert.equal(result.detail, expected.detail);
    }
  }
  for (const word of [0x00, 0x40, 0x0f, 0x08]) {
    assert.deepEqual(decodeStatusWord(statusVariable(word | 0x20)).tags,
      decodeStatusWord(statusVariable(word)).tags);
  }
});

test('recognizes object identity, validates width and rejects malformed bytes', () => {
  const variable = statusVariable(0x27);
  for (const index of ['6041', '0x6041', 0x6041]) {
    assert.equal(decodeStatusWord({ ...variable, index }).word, 0x27);
  }
  for (const override of [{ index: '6040' }, { index: '6041oops' }, { sub_index: 1 }]) {
    assert.equal(decodeStatusWord({ ...variable, ...override }), null);
  }
  for (const override of [{ size: 4 }, { bytes: '' }, { bytes: '27' }, { bytes: '270000' }, { bytes: 'zzzz' }]) {
    assert.equal(decodeStatusWord({ ...variable, ...override }).tags[0].label, '状态字数据无效');
  }
});


test('encodes supported OUT types as little-endian bytes without truncation', () => {
  for (const [text, type, hex] of [
    ['255', 'UINT8', 'ff'], ['-128', 'INT8', '80'],
    ['0x1234', 'UINT16', '3412'], ['-32768', 'INT16', '0080'],
    ['4294967295', 'UINT32', 'ffffffff'], ['-2147483648', 'INT32', '00000080'],
    ['1.5', 'FLOAT', '0000c03f'], ['-1.25e2', 'FLOAT', '0000fac2'],
  ]) assert.equal(encodeValue(text, type), hex);
});

test('rejects malformed and out-of-range OUT values', () => {
  for (const [text, type] of [
    ['', 'UINT16'], ['12abc', 'UINT16'], ['1.5', 'INT16'], ['-1', 'UINT16'],
    ['256', 'UINT8'], ['128', 'INT8'], ['65536', 'UINT16'], ['32768', 'INT16'],
    ['4294967296', 'UINT32'], ['2147483648', 'INT32'], ['NaN', 'FLOAT'],
    ['Infinity', 'FLOAT'], ['3.5e38', 'FLOAT'], ['0x12', 'FLOAT'], ['1', 'UNKNOWN'],
  ]) assert.throws(() => encodeValue(text, type));
});
