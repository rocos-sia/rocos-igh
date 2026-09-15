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

// CiA 402 PDS state coding: only bits 0–3, 5–6 participate.
// Bit 5 is also ignored for states whose pattern marks it as x (mask 0x004f).
// https://doc.synapticon.com/circulo/sw5.1/objects_html/6xxx/6041.html
const STATUS_STATES = [
  [0x004f, 0x0000, '未准备好上电', 'muted'],
  [0x004f, 0x0040, '禁止上电', 'muted'],
  [0x006f, 0x0021, '准备好上电', 'info'],
  [0x006f, 0x0023, '已上电', 'info'],
  [0x006f, 0x0027, '运行已使能', 'ok'],
  [0x006f, 0x0007, '快速停机中', 'warn'],
  [0x004f, 0x000f, '故障反应中', 'bad'],
  [0x004f, 0x0008, '故障', 'bad'],
];
export function decodeStatusWord(variable) {
  // The server sends the object index as hexadecimal text, without a prefix.
  const index = typeof variable.index === 'number' ? variable.index
    : /^(?:0x)?6041$/i.test(variable.index) ? 0x6041 : NaN;
  if (index !== 0x6041 || variable.sub_index !== 0) return null;
  if (variable.size !== 2 || typeof variable.bytes !== 'string'
      || !/^[0-9a-f]{4}$/i.test(variable.bytes)) {
    return { tags: [{ label: '状态字数据无效', tone: 'warn' }], detail: '0x6041:00 需要完整的 2 字节小端数据' };
  }
  const word = decodeValue(variable.bytes, 'UINT16');
  const state = STATUS_STATES.find(([mask, value]) => (word & mask) === value);
  const tags = [{ label: state ? state[2] : '未知状态', tone: state ? state[3] : 'warn' }];
  const detail = `CiA 402: ${tags[0].label}`;
  return { word, tags, detail };
}
