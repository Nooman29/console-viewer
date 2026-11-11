/*
 * Draws the app icon and writes it to assets/icon.png.
 * Pure Node, no image library: run `npm run icon` to regenerate it.
 *
 * The picture: a dark rounded square, a blue screen, a white play triangle.
 */
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

const SIZE = 256;
const SAMPLES = 4; // 4x4 samples per pixel for smooth edges

const DARK = [18, 21, 27];
const BLUE = [79, 140, 255];
const WHITE = [255, 255, 255];

// Is the point inside a rectangle with rounded corners?
function inRoundedRect(x, y, left, top, right, bottom, radius) {
  if (x < left || x > right || y < top || y > bottom) return false;
  const cx = Math.min(Math.max(x, left + radius), right - radius);
  const cy = Math.min(Math.max(y, top + radius), bottom - radius);
  return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2;
}

function inTriangle(x, y, [ax, ay], [bx, by], [cx, cy]) {
  const side = (px, py, qx, qy) => (x - qx) * (py - qy) - (px - qx) * (y - qy);
  const d1 = side(ax, ay, bx, by);
  const d2 = side(bx, by, cx, cy);
  const d3 = side(cx, cy, ax, ay);
  return !((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0));
}

// Colour and opacity at one point, in 0..256 coordinates.
function sample(x, y) {
  if (!inRoundedRect(x, y, 8, 8, 248, 248, 52)) return null; // transparent
  if (inTriangle(x, y, [108, 92], [108, 164], [166, 128])) return WHITE;
  if (inRoundedRect(x, y, 44, 64, 212, 192, 20)) return BLUE;
  return DARK;
}

const pixels = Buffer.alloc(SIZE * SIZE * 4);
for (let py = 0; py < SIZE; py += 1) {
  for (let px = 0; px < SIZE; px += 1) {
    let r = 0; let g = 0; let b = 0; let hits = 0;
    for (let sy = 0; sy < SAMPLES; sy += 1) {
      for (let sx = 0; sx < SAMPLES; sx += 1) {
        const colour = sample(px + (sx + 0.5) / SAMPLES, py + (sy + 0.5) / SAMPLES);
        if (colour) { r += colour[0]; g += colour[1]; b += colour[2]; hits += 1; }
      }
    }
    const at = (py * SIZE + px) * 4;
    if (hits) {
      pixels[at] = Math.round(r / hits);
      pixels[at + 1] = Math.round(g / hits);
      pixels[at + 2] = Math.round(b / hits);
      pixels[at + 3] = Math.round((hits / (SAMPLES * SAMPLES)) * 255);
    }
  }
}

// ---- Minimal PNG writer ----
function chunk(type, data) {
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(zlib.crc32(body) >>> 0);
  return Buffer.concat([length, body, crc]);
}

const header = Buffer.alloc(13);
header.writeUInt32BE(SIZE, 0);
header.writeUInt32BE(SIZE, 4);
header[8] = 8;  // bits per channel
header[9] = 6;  // colour type: RGBA

const rows = Buffer.alloc(SIZE * (SIZE * 4 + 1));
for (let y = 0; y < SIZE; y += 1) {
  pixels.copy(rows, y * (SIZE * 4 + 1) + 1, y * SIZE * 4, (y + 1) * SIZE * 4);
}

const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', header),
  chunk('IDAT', zlib.deflateSync(rows, { level: 9 })),
  chunk('IEND', Buffer.alloc(0)),
]);

const out = path.join(__dirname, '..', 'assets', 'icon.png');
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, png);
console.log(`Wrote ${out} (${png.length} bytes)`);
