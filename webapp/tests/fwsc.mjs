// SPDX-License-Identifier: GPL-3.0-only
// Test fixture: an Open-style .fwsc (flash.bin + ota.bin), laid out like fm1_open sdk/Package.cs
// Wrap() (MIT, kagaimiq + M-VAVE). The flash image is filler; WebUpdate.inspectPackage only checks
// the container, CRCs, sizes and the loader.

const crc16 = (d) => {
  let c = 0;
  for (const b of d) { c ^= b << 8; for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF; }
  return c;
};
const cipher = (a, at, n, key) => { let k = key; for (let i = 0; i < n; i++) { a[at + i] ^= k & 255; k = ((k << 1) ^ (k & 0x8000 ? 0x1021 : 0)) & 0xFFFF; } };
const put = (a, at, v, n) => { for (let i = 0; i < n; i++) a[at + i] = Math.floor(v / 2 ** (8 * i)) & 255; };
const up32 = (n) => (n + 31) & ~31;

export function makeFwsc({ product = "FM-1_8404100", loader, flashSize = 0x10000, seed = 1 } = {}) {
  const flash = Uint8Array.from({ length: flashSize }, (_, i) => (i * 31 + seed * 7 + (i >> 9)) & 255);
  const loaderAt = 1024 + up32(flash.length), total = loaderAt + up32(loader.length);
  const u = new Uint8Array(total).fill(255);
  u.fill(0, 0, 224);
  const files = [flash, loader], names = ["flash.bin", "ota.bin"];
  files.forEach((f, i) => {
    const r = 64 + i * 80;
    put(u, r, i ? 100 : 0, 2); put(u, r + 2, i, 2); put(u, r + 4, crc16(f), 2);
    put(u, r + 8, i ? loaderAt : 1024, 4); put(u, r + 12, f.length, 4); put(u, r + 16, up32(f.length), 4);
    u.fill(0, r + 64, r + 80); u.set(new TextEncoder().encode(names[i]), r + 64);
    cipher(u, r, 80, 0xFFFF);
  });
  put(u, 4, total, 4); put(u, 8, 2, 2); put(u, 10, 4, 2); put(u, 12, 512, 4);
  u.fill(0, 16, 32); u.set(new TextEncoder().encode("AC791N"), 16);
  put(u, 2, crc16(u.subarray(64, 224)), 2); put(u, 0, crc16(u.subarray(2, 64)), 2);
  cipher(u, 0, 64, 0xFFFF);
  u.set(flash, 1024); u.set(loader, loaderAt);
  const out = new Uint8Array(total + 20);
  for (let s = 0, d = 0; s < total; s++, d++) {
    out[d] = u[s];
    if (s < 940 && (s + 1) % 47 === 0) { const i = Math.floor(s / 47); out[++d] = i < product.length ? (product.charCodeAt(i) + i + 1) & 255 : 125; }
  }
  return out;
}
