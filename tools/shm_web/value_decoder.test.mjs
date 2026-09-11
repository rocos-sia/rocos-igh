import assert from 'node:assert/strict';
import test from 'node:test';

import {
  compatibleTypeForSize,
  decodeValue,
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