// The dial's host debug commands (devices/harness-device/firmware/docs/DEBUG_COMMANDS.md), as the layer that asks
// and waits for them: no dial, no port. The dial is played by these tests, frame by frame, in the order the
// document gives: debug.shot.begin, the 0x06 chunks, debug.shot.end.
import { crc32 } from 'node:zlib'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { DialDebug, MAX_SHOT_BYTES, type DebugFailure, type ShotResult } from './dialDebug.js'

// A 4 x 2 picture is 16 bytes; at 6 bytes a chunk that is 3 chunks, the last one short — the same shape as the
// real 466 x 466 picture (425 chunks of 1024, the last of 136).
const W = 4, H = 2, CHUNK = 6
const PIXELS = Buffer.from(Array.from({ length: W * H * 2 }, (_, i) => (i * 13 + 5) & 0xff))
const PARTS = [PIXELS.subarray(0, 6), PIXELS.subarray(6, 12), PIXELS.subarray(12)]

const begin = (id: number, over: Record<string, unknown> = {}) =>
  ({ t: 'debug.shot.begin', id, w: W, h: H, fmt: 'rgb565be', bytes: PIXELS.length, chunk: CHUNK, chunks: PARTS.length, ...over })
const end = (id: number, over: Record<string, unknown> = {}) =>
  ({ t: 'debug.shot.end', id, chunks: PARTS.length, crc32: crc32(PIXELS), ...over })
/** A 0x06 payload: id u16le, index u16le, pixels. */
const chunk = (id: number, index: number, pixels: Uint8Array): Buffer => {
  const head = Buffer.alloc(4)
  head.writeUInt16LE(id, 0)
  head.writeUInt16LE(index, 2)
  return Buffer.concat([head, pixels])
}

function rig(over: Partial<ConstructorParameters<typeof DialDebug>[0]> = {}) {
  const sent: Array<Record<string, unknown>> = []
  const state = { onWire: true }
  const debug = new DialDebug({ send: async (message) => { sent.push(message); return state.onWire }, log: () => {}, firstId: 6, ...over })
  /** Everything the dial says for one picture, in order. */
  const play = (id: number, parts = PARTS, tail: Record<string, unknown> = {}) => {
    debug.message(begin(id))
    parts.forEach((part, index) => debug.chunk(chunk(id, index, part)))
    debug.message(end(id, tail))
  }
  return { debug, sent, state, play }
}

const failure = (result: unknown): DebugFailure => {
  const r = result as ShotResult
  if (r.ok) throw new Error('expected a failure')
  return r
}

describe('a screenshot asked of the dial', () => {
  it('asks with an id, and assembles the picture the dial sends back', async () => {
    const { debug, sent, play } = rig()
    const pending = debug.shot()
    await vi.waitFor(() => expect(sent).toEqual([{ t: 'debug.shot', id: 7 }]))
    play(7)
    const result = await pending
    expect(result).toMatchObject({ ok: true, width: W, height: H })
    expect(result.ok && Buffer.compare(result.pixels, PIXELS)).toBe(0)
  })

  it('takes chunks that arrive before the begin, since only the index orders them', async () => {
    const { debug, sent } = rig()
    const pending = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    debug.chunk(chunk(7, 1, PARTS[1]))
    debug.message(begin(7))
    debug.chunk(chunk(7, 0, PARTS[0]))
    debug.chunk(chunk(7, 2, PARTS[2]))
    debug.message(end(7))
    expect(await pending).toMatchObject({ ok: true })
  })

  it('does not take another request\'s replies or chunks for its own', async () => {
    const { debug, sent, play } = rig()
    const pending = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    // A late picture of an earlier request (id 3), whole: it must neither complete nor damage this one.
    play(3)
    debug.message({ t: 'debug.done', id: 3, cmd: 'tap' })
    debug.message({ t: 'debug.error', id: 3, cmd: 'shot', reason: 'memory' })
    play(7)
    expect(await pending).toMatchObject({ ok: true })
  })

  it('reports a lost chunk, names it, and returns no picture', async () => {
    const { debug, sent } = rig()
    const pending = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    debug.message(begin(7))
    debug.chunk(chunk(7, 0, PARTS[0]))
    debug.chunk(chunk(7, 2, PARTS[2]))
    debug.message(end(7))
    const lost = failure(await pending)
    expect(lost).toMatchObject({ ok: false, kind: 'incomplete' })
    expect(lost.message).toMatch(/lost 1 of 3 chunks \(first: 1\)/)
    expect('pixels' in lost).toBe(false)
  })

  it('refuses a picture whose checksum does not match', async () => {
    const { debug, sent } = rig()
    const pending = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    const damaged = Buffer.from(PARTS[1])
    damaged[2] ^= 0x40
    debug.message(begin(7))
    debug.chunk(chunk(7, 0, PARTS[0]))
    debug.chunk(chunk(7, 1, damaged))
    debug.chunk(chunk(7, 2, PARTS[2]))
    debug.message(end(7))
    const result = failure(await pending)
    expect(result.kind).toBe('checksum')
    expect(result.message).toMatch(/crc32/)
  })

  it('refuses a chunk sent twice, and a total that is not the declared size', async () => {
    const twice = rig()
    let pending = twice.debug.shot()
    await vi.waitFor(() => expect(twice.sent).toHaveLength(1))
    twice.debug.message(begin(7))
    for (const index of [0, 1, 1, 2]) twice.debug.chunk(chunk(7, index, PARTS[index]))
    twice.debug.message(end(7))
    expect(failure(await pending)).toMatchObject({ kind: 'incomplete', message: expect.stringMatching(/chunk 1 .*twice/) })

    const short = rig()
    pending = short.debug.shot()
    await vi.waitFor(() => expect(short.sent).toHaveLength(1))
    short.debug.message(begin(7))
    short.debug.chunk(chunk(7, 0, PARTS[0]))
    short.debug.chunk(chunk(7, 1, PARTS[1].subarray(0, 4)))
    short.debug.chunk(chunk(7, 2, PARTS[2]))
    short.debug.message(end(7))
    expect(failure(await pending)).toMatchObject({ kind: 'incomplete', message: expect.stringMatching(/14 bytes, expected 16/) })
  })

  it('refuses an end that disagrees with the begin, an end with no begin, and a chunk past the last', async () => {
    const a = rig()
    let pending = a.debug.shot()
    await vi.waitFor(() => expect(a.sent).toHaveLength(1))
    a.play(7, PARTS, { chunks: 4 })
    expect(failure(await pending).kind).toBe('incomplete')

    const b = rig()
    pending = b.debug.shot()
    await vi.waitFor(() => expect(b.sent).toHaveLength(1))
    b.debug.message(end(7))
    expect(failure(await pending)).toMatchObject({ kind: 'incomplete', message: expect.stringMatching(/before .*begin/) })

    const c = rig()
    pending = c.debug.shot()
    await vi.waitFor(() => expect(c.sent).toHaveLength(1))
    c.debug.message(begin(7))
    c.debug.chunk(chunk(7, 3, PARTS[0]))
    c.debug.message(end(7))
    expect(failure(await pending).kind).toBe('incomplete')
  })

  it('refuses a begin that is not a picture of the size it claims, or too big to hold', async () => {
    for (const bad of [{ bytes: 15 }, { fmt: 'rgb888' }, { w: 0 }, { chunk: 0 }, { chunks: 2 }, { w: 1025, h: 1025, bytes: 1025 * 1025 * 2, chunk: 1024, chunks: Math.ceil((1025 * 1025 * 2) / 1024) }]) {
      expect(1025 * 1025 * 2).toBeGreaterThan(MAX_SHOT_BYTES)
      const { debug, sent } = rig()
      const pending = debug.shot()
      await vi.waitFor(() => expect(sent).toHaveLength(1))
      debug.message(begin(7, bad))
      expect(failure(await pending), JSON.stringify(bad)).toMatchObject({ kind: 'device', message: expect.stringMatching(/begin/) })
    }
  })

  it('passes the device\'s own refusal on, with its reason; busy is busy', async () => {
    const { debug, sent } = rig()
    let pending = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    debug.message({ t: 'debug.error', id: 7, cmd: 'shot', reason: 'asleep' })
    expect(failure(await pending)).toMatchObject({ ok: false, kind: 'device', reason: 'asleep' })

    pending = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(2))
    debug.message({ t: 'debug.error', id: 8, cmd: 'shot', reason: 'busy' })
    expect(failure(await pending)).toMatchObject({ kind: 'busy', reason: 'busy' })
  })

  describe('when the dial goes quiet', () => {
    beforeEach(() => { vi.useFakeTimers() })
    afterEach(() => { vi.useRealTimers() })

    it('gives up after the time limit, says how far it got, and is free for the next request', async () => {
      const { debug, sent, play } = rig({ shotTimeoutMs: 5_000 })
      const pending = debug.shot()
      await vi.advanceTimersByTimeAsync(0)
      debug.message(begin(7))
      debug.chunk(chunk(7, 0, PARTS[0]))
      await vi.advanceTimersByTimeAsync(5_000)
      const result = failure(await pending)
      expect(result).toMatchObject({ kind: 'timeout' })
      expect(result.message).toMatch(/1 of 3 chunks/)
      // The picture that finally arrives belongs to nobody now.
      play(7)
      const next = debug.shot()
      await vi.advanceTimersByTimeAsync(0)
      expect(sent.at(-1)).toEqual({ t: 'debug.shot', id: 8 })
      play(8)
      expect(await next).toMatchObject({ ok: true })
    })

    it('times a touch by its own length, and not before', async () => {
      const { debug, sent } = rig()
      const pending = debug.touch({ cmd: 'press', x: 233, y: 233, ms: 5_000 })
      await vi.advanceTimersByTimeAsync(0)
      expect(sent).toEqual([{ t: 'debug.press', id: 7, x: 233, y: 233, ms: 5_000 }])
      await vi.advanceTimersByTimeAsync(5_000)
      let settled = false
      void pending.then(() => { settled = true })
      await vi.advanceTimersByTimeAsync(0)
      expect(settled).toBe(false)
      await vi.advanceTimersByTimeAsync(10_000)
      expect(failure(await pending)).toMatchObject({ kind: 'timeout' })
    })
  })
})

describe('a touch asked of the dial', () => {
  it('sends the request as the document gives it and is done on debug.done', async () => {
    const { debug, sent } = rig()
    const tap = debug.touch({ cmd: 'tap', x: 233, y: 100 })
    await vi.waitFor(() => expect(sent).toEqual([{ t: 'debug.tap', id: 7, x: 233, y: 100 }]))
    // another request's done, and its error, are not ours
    debug.message({ t: 'debug.done', id: 6, cmd: 'tap' })
    debug.message({ t: 'debug.error', id: 6, cmd: 'tap', reason: 'busy' })
    debug.message({ t: 'debug.done', id: 7, cmd: 'tap' })
    expect(await tap).toEqual({ ok: true })

    const swipe = debug.touch({ cmd: 'swipe', x1: 380, y1: 233, x2: 80, y2: 233, ms: 250 })
    await vi.waitFor(() => expect(sent.at(-1)).toEqual({ t: 'debug.swipe', id: 8, x1: 380, y1: 233, x2: 80, y2: 233, ms: 250 }))
    debug.message({ t: 'debug.done', id: 8, cmd: 'swipe' })
    expect(await swipe).toEqual({ ok: true })
  })

  it('returns the device\'s refusal with its reason', async () => {
    const { debug, sent } = rig()
    const tap = debug.touch({ cmd: 'tap', x: 0, y: 0 })
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    debug.message({ t: 'debug.error', id: 7, cmd: 'tap', reason: 'range' })
    expect(failure(await tap)).toMatchObject({ ok: false, kind: 'device', reason: 'range' })
  })

  it('does not take a screenshot\'s done for a touch\'s, or the reverse', async () => {
    const { debug, sent, play } = rig()
    const shot = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    debug.message({ t: 'debug.done', id: 7, cmd: 'tap' })
    play(7)
    expect(await shot).toMatchObject({ ok: true })
    const tap = debug.touch({ cmd: 'tap', x: 5, y: 5 })
    await vi.waitFor(() => expect(sent).toHaveLength(2))
    debug.message(begin(8))
    debug.message(end(8))
    debug.message({ t: 'debug.done', id: 8, cmd: 'tap' })
    expect(await tap).toEqual({ ok: true })
  })
})

describe('one request at a time', () => {
  it('answers busy at once to a second request and sends nothing for it', async () => {
    const { debug, sent, play } = rig()
    const first = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    expect(failure(await debug.shot())).toMatchObject({ kind: 'busy' })
    expect(failure(await debug.touch({ cmd: 'tap', x: 1, y: 1 }))).toMatchObject({ kind: 'busy' })
    expect(sent).toHaveLength(1)
    play(7)
    expect(await first).toMatchObject({ ok: true })
    // and it is free again afterwards
    const again = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(2))
    play(8)
    expect(await again).toMatchObject({ ok: true })
  })

  it('is cleaned up when the link drops, and when the write fails', async () => {
    const { debug, sent, state } = rig()
    const pending = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    debug.abort('the dial went away')
    expect(failure(await pending)).toMatchObject({ kind: 'gone', message: 'the dial went away' })
    // nothing is held over to the next link
    debug.abort('again')
    state.onWire = false
    expect(failure(await debug.touch({ cmd: 'tap', x: 5, y: 5 }))).toMatchObject({ kind: 'gone' })
    state.onWire = true
    const next = debug.shot()
    await vi.waitFor(() => expect(sent).toHaveLength(3))
    expect(sent.at(-1)).toEqual({ t: 'debug.shot', id: 9 })
    debug.abort('over')
    await next
  })
})

describe('ids', () => {
  it('count from 1 to 65535 and wrap, never using 0 (the device\'s "no id")', async () => {
    const { debug, sent } = rig({ firstId: 65_533 })
    for (const expected of [65_534, 65_535, 1, 2]) {
      const pending = debug.touch({ cmd: 'tap', x: 5, y: 5 })
      await vi.waitFor(() => expect(sent.at(-1)?.id).toBe(expected))
      debug.message({ t: 'debug.done', id: expected, cmd: 'tap' })
      await pending
    }
  })

  it('start somewhere a restart is not likely to meet an old reply, when not told where', async () => {
    const sent: Array<Record<string, unknown>> = []
    const debug = new DialDebug({ send: async (message) => { sent.push(message); return true }, log: () => {} })
    const pending = debug.touch({ cmd: 'tap', x: 5, y: 5 })
    await vi.waitFor(() => expect(sent).toHaveLength(1))
    expect(sent[0].id).toBeGreaterThanOrEqual(1)
    expect(sent[0].id).toBeLessThanOrEqual(65_535)
    debug.abort('over')
    await pending
  })
})

describe('what is and is not the debug layer\'s', () => {
  it('takes every debug.* message, idle or not, and nothing else', () => {
    const { debug } = rig()
    expect(debug.message({ t: 'debug.done', id: 1, cmd: 'tap' })).toBe(true)
    expect(debug.message({ t: 'debug.future', id: 1 })).toBe(true)
    expect(debug.message({ t: 'hello' })).toBe(false)
    expect(debug.message({ t: 'debugger' })).toBe(false)
    // a chunk with nothing waiting, or too short to be one, is dropped without a throw
    expect(() => debug.chunk(chunk(1, 0, PARTS[0]))).not.toThrow()
    expect(() => debug.chunk(Buffer.alloc(2))).not.toThrow()
  })

  it('reads a 0x06 frame off the wire as the decoder hands it up', () => {
    const wire = Buffer.concat([
      Buffer.from('boot text '), // console text between frames is stepped over
      Buffer.from(encodeCableFrame(CableType.Shot, chunk(7, 2, PARTS[2]))),
    ])
    const seen: Array<{ type: number; payload: Uint8Array }> = []
    new CableDecoder().feed(wire, (frame) => seen.push(frame))
    expect(seen).toHaveLength(1)
    expect(seen[0].type).toBe(0x06)
    expect(Buffer.from(seen[0].payload).readUInt16LE(0)).toBe(7)
    expect(Buffer.from(seen[0].payload).readUInt16LE(2)).toBe(2)
    expect(Buffer.from(seen[0].payload).subarray(4).equals(PARTS[2])).toBe(true)
  })

  it('still hands up a frame type it does not know, and the frames on both sides of it', () => {
    const wire = Buffer.concat([
      Buffer.from(encodeCableFrame(CableType.Json, Buffer.from('{"t":"hello"}'))),
      Buffer.from(encodeCableFrame(0x7f, Buffer.from([1, 2, 3]))),
      Buffer.from(encodeCableFrame(CableType.Shot, chunk(7, 0, PARTS[0]))),
    ])
    const seen: number[] = []
    new CableDecoder().feed(wire, (frame) => seen.push(frame.type))
    expect(seen).toEqual([CableType.Json, 0x7f, CableType.Shot])
  })
})
