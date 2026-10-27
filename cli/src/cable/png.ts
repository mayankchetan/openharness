// A PNG writer for the dial's screenshot, on node:zlib alone. The picture arrives as RGB565 big-endian
// (devices/harness-device/firmware/docs/DEBUG_COMMANDS.md); this makes it the 8-bit RGB a viewer reads.
import { crc32, deflateSync } from 'node:zlib'

const SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])

/** The most a side may be. A bound on a bad `begin`, not a limit of the panel (466). */
const MAX_SIDE = 4096

/**
 * Two bytes a pixel, high byte first (`RRRRRGGG GGGBBBBB`), to three bytes a pixel.
 *
 * Scaled as the firmware document does, (v * 255 + half) / max, so full is exactly 255 and zero exactly 0:
 * a shift-and-copy of the top bits gives the same numbers, and this is the form the document states.
 */
export function rgb565beToRgb(pixels: Uint8Array): Buffer {
  const count = pixels.length >> 1
  const out = Buffer.alloc(count * 3)
  for (let i = 0; i < count; i++) {
    const v = (pixels[2 * i] << 8) | pixels[2 * i + 1]
    out[3 * i] = Math.floor((((v >> 11) & 31) * 255 + 15) / 31)
    out[3 * i + 1] = Math.floor((((v >> 5) & 63) * 255 + 31) / 63)
    out[3 * i + 2] = Math.floor(((v & 31) * 255 + 15) / 31)
  }
  return out
}

function chunk(tag: string, data: Buffer): Buffer {
  const body = Buffer.concat([Buffer.from(tag, 'latin1'), data])
  const out = Buffer.alloc(12 + data.length)
  out.writeUInt32BE(data.length, 0)
  body.copy(out, 4)
  out.writeUInt32BE(crc32(body), 8 + data.length)
  return out
}

/** An 8-bit RGB PNG, no interlace, every row filtered "none". `rgb` is width x height x 3 bytes. */
export function encodePng(width: number, height: number, rgb: Uint8Array): Buffer {
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > MAX_SIDE || height > MAX_SIDE) {
    throw new Error(`PNG size ${width} x ${height} is not a picture`)
  }
  if (rgb.length !== width * height * 3) throw new Error(`PNG pixels are ${rgb.length} bytes, not width x height x 3`)
  const stride = width * 3
  const raw = Buffer.alloc(height * (1 + stride))
  for (let y = 0; y < height; y++) {
    // The leading byte of a row is its filter: 0, none.
    Buffer.from(rgb.buffer, rgb.byteOffset + y * stride, stride).copy(raw, y * (1 + stride) + 1)
  }
  const head = Buffer.alloc(13)
  head.writeUInt32BE(width, 0)
  head.writeUInt32BE(height, 4)
  head.set([8, 2, 0, 0, 0], 8) // 8 bits, colour type 2 (RGB), deflate, filter method 0, no interlace
  return Buffer.concat([SIGNATURE, chunk('IHDR', head), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))])
}

/** The dial's picture to a PNG. */
export function pngFromRgb565be(width: number, height: number, pixels: Uint8Array): Buffer {
  if (pixels.length !== width * height * 2) throw new Error(`screenshot pixels are ${pixels.length} bytes, not width x height x 2`)
  return encodePng(width, height, rgb565beToRgb(pixels))
}
