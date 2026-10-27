// Asking the dial for a screenshot or an injected touch, and waiting for the answer.
//
// The firmware half is devices/harness-device/firmware/main/cable_debug.c and its document, docs/DEBUG_COMMANDS.md;
// tools/dial_debug.py is the same client for a script that holds the port itself. Only one process can own
// the port, so when the daemon is running these go through it, and this is the layer that makes the dial's
// replies into an answer.
//
// What is asked is a JSON request with a whole-number `id`; the dial repeats the id in every reply:
//
//   debug.shot  → debug.shot.begin, N frames of CableType.Shot, debug.shot.end     (or debug.error)
//   debug.tap, debug.press, debug.swipe → debug.done                               (or debug.error)
//
// One request at a time per dial. The dial itself does one capture and one gesture at a time and answers
// `busy` to a second, so a second here is turned away before it is sent rather than left to race: a late
// reply to an old request must never be taken for the new one, and with a single slot it cannot be.
//
// A picture is returned only when it is whole. A lost frame, a repeated one, a total of the wrong size or a
// checksum that does not match is an error, never a partial image: a half picture shown as complete is how
// a bug gets blamed on the wrong side.
import { crc32 } from 'node:zlib'

import { CABLE_MAX_PAYLOAD } from './cableFrame.js'

/** A request this layer can send that is not a screenshot. Points are pixels from the top left, 0..465. */
export type DebugTouch =
  | { cmd: 'tap'; x: number; y: number }
  | { cmd: 'press'; x: number; y: number; ms: number }
  | { cmd: 'swipe'; x1: number; y1: number; x2: number; y2: number; ms: number }

/**
 * Why a request did not come to an answer:
 *   no_dial     there is no such dial on this computer (the fleet says this; this layer never does)
 *   busy        a request is already in flight here, or the dial said `busy`
 *   device      the dial refused, with its `reason` (asleep, memory, range ...), or sent something unreadable
 *   timeout     the dial did not finish in time
 *   incomplete  the picture is missing frames, repeats one or is the wrong size
 *   checksum    the picture's CRC-32 does not match what the dial said it sent
 *   gone        the link dropped, or a write failed
 */
export type DebugFailureKind = 'no_dial' | 'busy' | 'device' | 'timeout' | 'incomplete' | 'checksum' | 'gone'

export interface DebugFailure {
  ok: false
  kind: DebugFailureKind
  /** The dial's own word for it (`debug.error.reason`), when it gave one. */
  reason?: string
  message: string
}

export type ShotResult = { ok: true; width: number; height: number; pixels: Buffer } | DebugFailure
export type DoneResult = { ok: true } | DebugFailure

/** The most pixel bytes taken for one picture. The panel is 466 x 466 x 2 = 434,312; this is a bound on a bad `begin`. */
export const MAX_SHOT_BYTES = 2 * 1024 * 1024

/** The dial captures in about two seconds and sends in about half a second more; this is for a link that is slow, not for a dial that is dead. */
const SHOT_TIMEOUT_MS = 20_000
/** A tap is 60 ms on the glass, and the dial answers from its next tick. */
const TAP_TIMEOUT_MS = 8_000
/** A press or a swipe takes its `ms`; the dial itself gives up on a gesture 2 s after that. */
const GESTURE_GRACE_MS = 5_000

/** The id's field is 16 bits, and 0 means "the request had none". */
const ID_MAX = 65_535
/** Each frame's head, before the pixels: id:u16le | index:u16le. */
const CHUNK_HEAD = 4

export interface DialDebugDeps {
  /** One JSON frame to the dial; false when it is not on the wire. */
  send(message: { t: string; id: number; [key: string]: unknown }): Promise<boolean>
  log(line: string): void
  shotTimeoutMs?: number
  /** For tests: the id before the first one used. Otherwise somewhere random, so a daemon that restarts does not meet its own old ids. */
  firstId?: number
}

interface Begin { width: number; height: number; bytes: number; chunks: number }

interface Pending {
  id: number
  kind: 'shot' | 'touch'
  resolve(result: ShotResult | DoneResult): void
  timer: ReturnType<typeof setTimeout>
  begin?: Begin
  /** The chunks that have arrived, by index. */
  chunks: Map<number, Buffer>
  held: number
  /** The first thing seen wrong with a picture; reported when its end arrives. */
  damage?: string
}

const text = (value: unknown): string => (typeof value === 'string' ? value.slice(0, 40) : '')

export class DialDebug {
  private pending: Pending | null = null
  private last: number

  constructor(private readonly deps: DialDebugDeps) {
    this.last = deps.firstId ?? 1 + Math.floor(Math.random() * ID_MAX)
  }

  shot(): Promise<ShotResult> {
    return this.ask('shot', { t: 'debug.shot' }, this.deps.shotTimeoutMs ?? SHOT_TIMEOUT_MS) as Promise<ShotResult>
  }

  touch(request: DebugTouch): Promise<DoneResult> {
    const { cmd, ...fields } = request
    const timeout = request.cmd === 'tap' ? TAP_TIMEOUT_MS : request.ms + GESTURE_GRACE_MS
    return this.ask('touch', { t: `debug.${cmd}`, ...fields }, timeout) as Promise<DoneResult>
  }

  /** The link went away (or the session stopped): whatever was asked will not be answered. */
  abort(why: string): void {
    if (this.pending) this.finish(this.pending, { ok: false, kind: 'gone', message: why })
  }

  /**
   * A JSON message from the dial. True when it is a `debug.*` one, whoever it was for: the session has no
   * other use for it, and its "unhandled message" line would only be noise for a reply that came late.
   */
  message(msg: { t?: unknown; [key: string]: unknown }): boolean {
    if (typeof msg.t !== 'string' || !msg.t.startsWith('debug.')) return false
    const p = this.pending
    // A reply to something asked earlier (timed out, or the link dropped): not this request's.
    if (!p || msg.id !== p.id) return true
    switch (msg.t) {
      case 'debug.shot.begin': if (p.kind === 'shot') this.onBegin(p, msg); break
      case 'debug.shot.end': if (p.kind === 'shot') this.onEnd(p, msg); break
      case 'debug.done': if (p.kind === 'touch') this.finish(p, { ok: true }); break
      case 'debug.error': {
        const reason = text(msg.reason) || 'unknown'
        this.finish(p, { ok: false, kind: reason === 'busy' ? 'busy' : 'device', reason, message: `the dial refused: ${reason}` })
        break
      }
    }
    return true
  }

  /** One CableType.Shot frame's payload. */
  chunk(payload: Uint8Array): void {
    const p = this.pending
    if (!p || p.kind !== 'shot' || payload.length < CHUNK_HEAD) return
    if ((payload[0] | (payload[1] << 8)) !== p.id) return
    const index = payload[2] | (payload[3] << 8)
    if (p.chunks.has(index)) { p.damage ??= `chunk ${index} arrived twice`; return }
    const pixels = payload.subarray(CHUNK_HEAD)
    // The frame cap is 8 KB and an id is 16 bits: without a ceiling a dial that never sends `end` could fill memory.
    if (p.held + pixels.length > MAX_SHOT_BYTES) { p.damage ??= 'more pixels arrived than any picture holds'; return }
    p.chunks.set(index, Buffer.from(pixels))
    p.held += pixels.length
  }

  private nextId(): number {
    this.last = (this.last % ID_MAX) + 1
    return this.last
  }

  private ask(kind: Pending['kind'], request: { t: string; [key: string]: unknown }, timeoutMs: number): Promise<ShotResult | DoneResult> {
    if (this.pending) return Promise.resolve({ ok: false, kind: 'busy', message: 'another debug request is still waiting for the dial' })
    const id = this.nextId()
    return new Promise((resolve) => {
      const p: Pending = { id, kind, resolve, chunks: new Map(), held: 0,
        timer: setTimeout(() => this.finish(p, { ok: false, kind: 'timeout', message: this.stalled(p, timeoutMs) }), timeoutMs) }
      p.timer.unref?.()
      this.pending = p
      const gone: DebugFailure = { ok: false, kind: 'gone', message: 'the dial is not on the wire' }
      this.deps.send({ ...request, id }).then((sent) => { if (!sent) this.finish(p, gone) }, () => this.finish(p, gone))
    })
  }

  private stalled(p: Pending, timeoutMs: number): string {
    const seconds = `${Math.round(timeoutMs / 100) / 10} s`
    return p.kind === 'shot'
      ? `the dial did not finish the picture in ${seconds} (${p.chunks.size} of ${p.begin?.chunks ?? '?'} chunks)`
      : `the dial did not answer in ${seconds}`
  }

  private finish(p: Pending, result: ShotResult | DoneResult): void {
    if (this.pending !== p) return
    clearTimeout(p.timer)
    this.pending = null
    if (!result.ok) this.deps.log(`cable: debug ${p.kind} ${p.id} failed: ${result.kind}${result.reason ? ` (${result.reason})` : ''}: ${result.message}`)
    p.resolve(result)
  }

  private onBegin(p: Pending, msg: { [key: string]: unknown }): void {
    const int = (value: unknown): number => (typeof value === 'number' && Number.isInteger(value) ? value : -1)
    const width = int(msg.w), height = int(msg.h), bytes = int(msg.bytes), chunk = int(msg.chunk), chunks = int(msg.chunks)
    const why = msg.fmt !== 'rgb565be' ? `format ${text(msg.fmt) || 'missing'}`
      : width < 1 || height < 1 || bytes !== width * height * 2 ? 'bytes is not w x h x 2'
      : bytes > MAX_SHOT_BYTES ? `${bytes} bytes is more than any picture holds`
      : chunk < 1 || chunk > CABLE_MAX_PAYLOAD - CHUNK_HEAD || chunks !== Math.ceil(bytes / chunk) ? 'chunks does not fit bytes and chunk'
      : ''
    if (why) {
      this.finish(p, { ok: false, kind: 'device', message: `the dial sent a debug.shot.begin that is not a picture (${why})` })
      return
    }
    p.begin = { width, height, bytes, chunks }
  }

  private onEnd(p: Pending, msg: { [key: string]: unknown }): void {
    const bad = (kind: 'incomplete' | 'checksum', message: string): void => this.finish(p, { ok: false, kind, message })
    const begin = p.begin
    if (!begin) return bad('incomplete', 'debug.shot.end arrived before debug.shot.begin')
    if (msg.chunks !== begin.chunks) return bad('incomplete', `the end says ${String(msg.chunks).slice(0, 12)} chunks, the begin said ${begin.chunks}`)
    if (p.damage) return bad('incomplete', p.damage)
    const past = [...p.chunks.keys()].find((index) => index >= begin.chunks)
    if (past !== undefined) return bad('incomplete', `chunk ${past} is past the last (${begin.chunks})`)
    const missing: number[] = []
    for (let index = 0; index < begin.chunks; index++) if (!p.chunks.has(index)) missing.push(index)
    if (missing.length) return bad('incomplete', `lost ${missing.length} of ${begin.chunks} chunks (first: ${missing[0]})`)
    const pixels = Buffer.concat([...p.chunks.keys()].sort((a, b) => a - b).map((index) => p.chunks.get(index)!))
    if (pixels.length !== begin.bytes) return bad('incomplete', `${pixels.length} bytes, expected ${begin.bytes}`)
    const got = crc32(pixels) >>> 0
    if (got !== msg.crc32) return bad('checksum', `crc32 ${got.toString(16).padStart(8, '0')} does not match the dial's ${typeof msg.crc32 === 'number' ? msg.crc32.toString(16).padStart(8, '0') : 'missing value'}`)
    this.finish(p, { ok: true, width: begin.width, height: begin.height, pixels })
  }
}
