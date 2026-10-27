import { crc32, inflateSync } from 'node:zlib'
import { describe, expect, it } from 'vitest'

import { encodePng, pngFromRgb565be, rgb565beToRgb } from './png.js'

/** One chunk of a PNG, read back with its CRC checked: what any viewer does before it trusts the bytes. */
function chunks(png: Buffer): Array<{ tag: string; data: Buffer }> {
  expect(png.subarray(0, 8).toString('hex')).toBe('89504e470d0a1a0a')
  const out: Array<{ tag: string; data: Buffer }> = []
  for (let at = 8; at < png.length;) {
    const length = png.readUInt32BE(at)
    const tag = png.toString('latin1', at + 4, at + 8)
    const data = png.subarray(at + 8, at + 8 + length)
    expect(png.readUInt32BE(at + 8 + length), `${tag} crc`).toBe(crc32(png.subarray(at + 4, at + 8 + length)))
    out.push({ tag, data })
    at += 12 + length
  }
  return out
}

/** The pixels of a PNG made by this encoder (8-bit RGB, filter 0 on every row), as [width, height, rgb]. */
function decode(png: Buffer): { width: number; height: number; rgb: number[] } {
  const parts = chunks(png)
  expect(parts.map((part) => part.tag)).toEqual(['IHDR', 'IDAT', 'IEND'])
  const head = parts[0].data
  const width = head.readUInt32BE(0), height = head.readUInt32BE(4)
  // bit depth 8, colour type 2 (RGB), deflate, no filter method change, no interlace
  expect([...head.subarray(8)]).toEqual([8, 2, 0, 0, 0])
  const raw = inflateSync(parts[1].data)
  expect(raw.length).toBe(height * (1 + width * 3))
  const rgb: number[] = []
  for (let y = 0; y < height; y++) {
    const row = y * (1 + width * 3)
    expect(raw[row], `filter of row ${y}`).toBe(0)
    rgb.push(...raw.subarray(row + 1, row + 1 + width * 3))
  }
  return { width, height, rgb }
}

/** Two bytes a pixel, high byte first: what the dial sends. */
const be = (...values: number[]): Buffer => Buffer.from(values.flatMap((v) => [v >> 8, v & 0xff]))

describe('the screenshot as a PNG', () => {
  it('turns RGB565 big-endian into RGB888 the way the firmware document says', () => {
    // red, green, blue, white, black, and a mid-grey where the 5/6/5-bit scaling shows (r=16 g=32 b=16)
    const rgb = rgb565beToRgb(be(0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0x8410))
    expect([...rgb]).toEqual([
      255, 0, 0,
      0, 255, 0,
      0, 0, 255,
      255, 255, 255,
      0, 0, 0,
      132, 130, 132,
    ])
  })

  it('encodes a tiny known image that decodes back to the same pixels', () => {
    const png = pngFromRgb565be(3, 2, be(0xf800, 0x07e0, 0x001f, 0xffff, 0x0000, 0x8410))
    expect(decode(png)).toEqual({
      width: 3, height: 2,
      rgb: [255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 132, 130, 132],
    })
  })

  it('encodes raw RGB directly, one row per scanline', () => {
    const png = encodePng(1, 2, Buffer.from([1, 2, 3, 4, 5, 6]))
    expect(decode(png)).toEqual({ width: 1, height: 2, rgb: [1, 2, 3, 4, 5, 6] })
  })

  it('refuses pixels that are not exactly width x height', () => {
    expect(() => pngFromRgb565be(2, 2, be(1, 2, 3))).toThrow(/width x height/)
    expect(() => encodePng(2, 2, Buffer.alloc(11))).toThrow(/width x height/)
    expect(() => encodePng(0, 2, Buffer.alloc(0))).toThrow(/size/)
  })

  it('handles a whole 466 x 466 face', () => {
    const pixels = Buffer.alloc(466 * 466 * 2, 0xff)
    const { width, height, rgb } = decode(pngFromRgb565be(466, 466, pixels))
    expect([width, height, rgb.length]).toEqual([466, 466, 466 * 466 * 3])
    expect(new Set(rgb)).toEqual(new Set([255]))
  })
})
