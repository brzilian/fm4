// SPDX-License-Identifier: GPL-3.0-only
// FM-4 webapp: a minimal uncompressed ("stored") ZIP writer and reader for backup sets.

const TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
export function crc32(bytes) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) c = TABLE[(c ^ bytes[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

/** files: [{name, data: Uint8Array}] -> Uint8Array */
export function makeZip(files) {
  const enc = new TextEncoder(), parts = [], central = [];
  let offset = 0;
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.data), n = f.data.length;
    const local = new Uint8Array(30 + name.length), v = new DataView(local.buffer);
    v.setUint32(0, 0x04034b50, true); v.setUint16(4, 20, true); v.setUint16(8, 0, true);
    v.setUint32(14, crc, true); v.setUint32(18, n, true); v.setUint32(22, n, true); v.setUint16(26, name.length, true);
    local.set(name, 30);
    const cen = new Uint8Array(46 + name.length), w = new DataView(cen.buffer);
    w.setUint32(0, 0x02014b50, true); w.setUint16(4, 20, true); w.setUint16(6, 20, true);
    w.setUint32(16, crc, true); w.setUint32(20, n, true); w.setUint32(24, n, true); w.setUint16(28, name.length, true);
    w.setUint32(42, offset, true); cen.set(name, 46);
    parts.push(local, f.data); central.push(cen);
    offset += local.length + n;
  }
  const size = central.reduce((a, c) => a + c.length, 0);
  const end = new Uint8Array(22), e = new DataView(end.buffer);
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
  e.setUint32(12, size, true); e.setUint32(16, offset, true);
  const all = [...parts, ...central, end], out = new Uint8Array(all.reduce((a, p) => a + p.length, 0));
  let at = 0; for (const p of all) { out.set(p, at); at += p.length; }
  return out;
}

/** Read a stored ZIP written by makeZip; verifies every CRC. */
export function readZip(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), dec = new TextDecoder(), files = [];
  let at = 0;
  while (at + 30 <= bytes.length && v.getUint32(at, true) === 0x04034b50) {
    const crc = v.getUint32(at + 14, true), n = v.getUint32(at + 18, true), len = v.getUint16(at + 26, true), extra = v.getUint16(at + 28, true);
    const name = dec.decode(bytes.subarray(at + 30, at + 30 + len)), start = at + 30 + len + extra;
    const data = bytes.slice(start, start + n);
    if (data.length !== n || crc32(data) !== crc) throw new Error(`Backup file ${name} is damaged`);
    files.push({ name, data });
    at = start + n;
  }
  return files;
}
