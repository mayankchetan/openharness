// ffmpeg as a decoder of the same bytes. Skipped, cleanly, when ffmpeg is not installed (set ADPCM_FFMPEG to its path to
// use one that is not on PATH).
//
// What this proves: the WAV container fields in docs/specs/cable-wifi-link.md section 2.6 are right (ffmpeg accepts the
// block and returns the right number of samples), and the nibble order, step table, index table, header fields and
// padding rule agree with an independent decoder.
//
// What it does NOT prove, and why: ffmpeg's `adpcm_ima_wav` computes the step difference as ((2 * code + 1) * step) >> 3
// in one multiply, while the wire spec (and the vectors, and the firmware) use the reference series of shifts. The two
// agree on the step index at every sample and differ in the predictor by a few least-significant bits, which add up
// along a block (measured: at most 15 of 32768, ffmpeg 4.2.2 and 4.2.3). So this test checks ffmpeg in two ways:
//   - exactly, against a decoder in this file that uses ffmpeg's rounding (everything but the rounding rule is shared);
//   - against the vector samples within SANITY_BOUND, which is a sanity bound and not a derived one.
import { spawnSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import { ADPCM_BLOCK_SAMPLES, adpcmStepSize, decodeAdpcmBlock } from './adpcm.js'

const FFMPEG = process.env.ADPCM_FFMPEG || 'ffmpeg'
const hasFfmpeg = spawnSync(FFMPEG, ['-version'], { encoding: 'utf8' }).status === 0
const SANITY_BOUND = 32

const VECTORS = join(dirname(fileURLToPath(import.meta.url)), 'vectors', 'adpcm_ima.txt')
interface Enc { name: string; payload: Buffer; pcm: Int16Array }
function encVectors(): Enc[] {
  const out: Enc[] = []
  for (const line of readFileSync(VECTORS, 'utf8').split('\n')) {
    const f = line.replace(/\r$/, '').split('\t')
    if (f[0] !== 'enc') continue
    const raw = Buffer.from(f[2], 'hex')
    out.push({ name: f[1], payload: Buffer.from(f[6], 'hex'), pcm: Int16Array.from({ length: raw.length / 2 }, (_, i) => raw.readInt16LE(i * 2)) })
  }
  return out
}

/** A WAV file of IMA ADPCM mono blocks: format 0x0011, 16 kHz, block align 256, 505 samples per block. */
function wav(blocks: Buffer[]): Buffer {
  const fmt = Buffer.alloc(20)
  fmt.writeUInt16LE(0x0011, 0) // IMA / DVI ADPCM
  fmt.writeUInt16LE(1, 2) // channels
  fmt.writeUInt32LE(16000, 4) // sample rate
  fmt.writeUInt32LE(Math.floor(16000 * 256 / ADPCM_BLOCK_SAMPLES), 8) // average bytes per second
  fmt.writeUInt16LE(256, 12) // block align
  fmt.writeUInt16LE(4, 14) // bits per sample
  fmt.writeUInt16LE(2, 16) // cbSize
  fmt.writeUInt16LE(ADPCM_BLOCK_SAMPLES, 18) // samples per block
  const data = Buffer.concat(blocks)
  const fact = Buffer.alloc(4)
  fact.writeUInt32LE(blocks.length * ADPCM_BLOCK_SAMPLES)
  const chunk = (id: string, body: Buffer) => {
    const head = Buffer.alloc(8)
    head.write(id, 0, 'ascii')
    head.writeUInt32LE(body.length, 4)
    return Buffer.concat([head, body])
  }
  const body = Buffer.concat([Buffer.from('WAVE'), chunk('fmt ', fmt), chunk('fact', fact), chunk('data', data)])
  const riff = Buffer.alloc(8)
  riff.write('RIFF', 0, 'ascii')
  riff.writeUInt32LE(body.length, 4)
  return Buffer.concat([riff, body])
}

/** payload[5:], zero-padded to the 256-byte block a WAV file requires. */
const wavBlock = (payload: Buffer): Buffer => {
  const out = Buffer.alloc(256)
  payload.subarray(5).copy(out)
  return out
}

function ffmpegDecode(file: Buffer): Int16Array {
  const run = spawnSync(FFMPEG, ['-v', 'error', '-f', 'wav', '-i', 'pipe:0', '-f', 's16le', '-acodec', 'pcm_s16le', 'pipe:1'],
    { input: file, maxBuffer: 64 * 1024 * 1024 })
  expect(run.status, String(run.stderr)).toBe(0)
  expect(String(run.stderr)).toBe('')
  const raw = run.stdout as Buffer
  return Int16Array.from({ length: raw.length / 2 }, (_, i) => raw.readInt16LE(i * 2))
}

/** A decoder with ffmpeg's rounding, written from the format text and ffmpeg's documented behaviour. */
function ffmpegRounding(payload: Buffer): Int16Array {
  const n = payload.readUInt16LE(2)
  let pred = payload.readInt16LE(5)
  let index = payload[7]
  const out = new Int16Array(n)
  out[0] = pred
  const adjust = [-1, -1, -1, -1, 2, 4, 6, 8]
  for (let i = 1; i < n; i++) {
    const code = (payload[9 + ((i - 1) >> 1)] >> (((i - 1) & 1) * 4)) & 15
    const diff = ((2 * (code & 7) + 1) * adpcmStepSize(index)) >> 3
    pred = Math.max(-32768, Math.min(32767, code & 8 ? pred - diff : pred + diff))
    index = Math.max(0, Math.min(88, index + adjust[code & 7]))
    out[i] = pred
  }
  return out
}

describe.skipIf(!hasFfmpeg)('ffmpeg as a third decoder of the ADPCM vectors', () => {
  it('decodes every encoder vector, one block per file, like a decoder with its rounding', () => {
    const vectors = encVectors()
    expect(vectors.length).toBeGreaterThan(20)
    let worst = 0
    for (const v of vectors) {
      const n = v.pcm.length
      const decoded = ffmpegDecode(wav([wavBlock(v.payload)]))
      // A short block is padded to 256 bytes, so ffmpeg returns a full 505 samples; the first n are the block.
      expect(decoded.length, v.name).toBe(ADPCM_BLOCK_SAMPLES)
      const head = decoded.subarray(0, n)
      expect(Array.from(head), v.name).toEqual(Array.from(ffmpegRounding(v.payload)))
      const ours = decodeAdpcmBlock(Uint8Array.from(v.payload))
      expect(ours.ok).toBe(true)
      if (!ours.ok) continue
      expect(head[0]).toBe(v.pcm[0]) // the first sample is verbatim in every decoder
      for (let i = 0; i < n; i++) worst = Math.max(worst, Math.abs(head[i] - ours.block.samples[i]))
    }
    expect(worst).toBeLessThanOrEqual(SANITY_BOUND)
  })

  it('decodes several blocks in one file, each restarting from its own header', () => {
    const chain = encVectors().filter(v => v.name.startsWith('chirp_chain_'))
    expect(chain.length).toBe(3)
    const decoded = ffmpegDecode(wav(chain.map(v => wavBlock(v.payload))))
    expect(decoded.length).toBe(3 * ADPCM_BLOCK_SAMPLES)
    chain.forEach((v, i) => {
      const block = decoded.subarray(i * ADPCM_BLOCK_SAMPLES, i * ADPCM_BLOCK_SAMPLES + v.pcm.length)
      expect(Array.from(block), v.name).toEqual(Array.from(ffmpegRounding(v.payload)))
    })
  })

  it('rejects a step index above 88 the way the wire spec does', () => {
    const bad = Buffer.from(encVectors().find(v => v.name === 'sine_1khz')!.payload)
    bad[7] = 89
    const run = spawnSync(FFMPEG, ['-v', 'error', '-f', 'wav', '-i', 'pipe:0', '-f', 's16le', 'pipe:1'],
      { input: wav([wavBlock(bad)]), maxBuffer: 64 * 1024 * 1024 })
    // ffmpeg errors on the block or returns nothing for it; either way it does not return a full block of audio.
    expect((run.stdout as Buffer).length).toBeLessThan(ADPCM_BLOCK_SAMPLES * 2)
  })
})

describe.skipIf(hasFfmpeg)('ffmpeg is not installed', () => {
  it('skips the cross-check (install ffmpeg or set ADPCM_FFMPEG to run it)', () => {
    expect(hasFfmpeg).toBe(false)
  })
})
