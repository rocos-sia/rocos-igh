const TYPES_BY_SIZE = Object.freeze({
  1: Object.freeze(['UINT8', 'INT8']),
  2: Object.freeze(['UINT16', 'INT16']),
  4: Object.freeze(['UINT32', 'INT32', 'FLOAT']),
});

export function typesForSize(size) {
  return TYPES_BY_SIZE[size] || [];
}

export function compatibleTypeForSize(size, selectedType) {
  const types = typesForSize(size);
  return types.includes(selectedType) ? selectedType : types[0];
}

export function variableKey(slaveId, direction, variable) {
  return [slaveId, direction, variable.index, variable.sub_index, variable.offset].join(':');
}

function parseBytes(hexBytes) {
  const pairs = hexBytes.match(/../g) || [];
  return Uint8Array.from(pairs, pair => Number.parseInt(pair, 16));
}

export function decodeValue(hexBytes, type) {
  const bytes = parseBytes(hexBytes);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  switch (type) {
    case 'UINT8': return view.getUint8(0);
    case 'INT8': return view.getInt8(0);
    case 'UINT16': return view.getUint16(0, true);
    case 'INT16': return view.getInt16(0, true);
    case 'UINT32': return view.getUint32(0, true);
    case 'INT32': return view.getInt32(0, true);
    case 'FLOAT': return view.getFloat32(0, true);
    default: throw new RangeError(`Unsupported data type: ${type}`);
  }
}

export function formatTypedBytes(hexBytes, type) {
  const hex = (hexBytes.match(/../g) || [])
    .map(pair => pair.toUpperCase())
    .join(' ');
  if (!hex) return '-';
  return `${hex} = ${decodeValue(hexBytes, type)}`;
}