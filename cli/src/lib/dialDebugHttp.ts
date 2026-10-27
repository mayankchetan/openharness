/**
 * POST /api/dial/debug/{shot,tap,press,swipe}: the dial's screenshot and injected touch, on the daemon's local
 * API (hookServer.ts), for a developer or an agent testing the firmware. Only one process can own the dial's
 * USB port, so while the daemon holds it the firmware's debug commands (devices/harness-device/firmware/docs/
 * DEBUG_COMMANDS.md) are reachable here and nowhere else.
 *
 * This file is the HTTP half: it checks what was asked, hands it to the devices (services/devices.ts
 * `dial_debug`, where the cable is), and turns their answer into a status and a body. It names no cable code:
 * the core's process loads it, and the cable is the devices'. The devices' answer is plain data:
 *
 *   { ok: true, png: <base64> }            a screenshot
 *   { ok: true }                           a touch, after the dial's debug.done
 *   { ok: false, kind, message, reason? }  see cable/dialDebug.ts DebugFailureKind
 *   { error: 'SERVICE_UNAVAILABLE' ... }   the devices could not answer at all
 */

export type DialDebugOp = 'shot' | 'tap' | 'press' | 'swipe'
const OPS: readonly string[] = ['shot', 'tap', 'press', 'swipe']

/** A status and a body, or a status and a PNG. */
export type DialDebugReply = { status: number; body: unknown } | { status: 200; png: Buffer }

/** Ask the devices; the second argument is how long to wait for their answer. */
export type AskDevices = (payload: Record<string, unknown>, waitMs: number) => Promise<Record<string, unknown>>

/**
 * How long the core waits for the devices: longer than the dial's picture takes at its slowest (20 s, cable/
 * dialDebug.ts), so the dial's own timeout is what the caller hears about, not the core's.
 */
export const DIAL_DEBUG_WAIT_MS = 30_000

/** The panel is 466 pixels across; the dial itself refuses a point off the round glass (`range`). */
const LAST_PIXEL = 465

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])

/** The defaults are dial_debug.py's: a long press of 700 ms (the pet's hold starts at 650) and a swipe of 250. */
const MS = { press: { min: 50, max: 5000, default: 700 }, swipe: { min: 20, max: 3000, default: 250 } } as const

/** HTTP status for each way a request can fail; every other failure is the dial's or the cable's: 502. */
const STATUS: Record<string, number> = { no_dial: 404, busy: 409, timeout: 504 }

const bad = (error: string): DialDebugReply => ({ status: 400, body: { error, kind: 'bad_request' } })
const object = (value: unknown): value is Record<string, unknown> => value !== null && typeof value === 'object' && !Array.isArray(value)

/** The payload for the devices, or the 400 to answer with. */
function payloadFor(op: DialDebugOp, body: unknown): Record<string, unknown> | DialDebugReply {
  const input = body === undefined ? {} : body
  if (!object(input)) return bad('The body must be a JSON object.')
  const payload: Record<string, unknown> = { op }
  if (input.dial !== undefined) {
    if (typeof input.dial !== 'string' || input.dial.trim().length > 64) return bad('dial must be a MAC address or serial number of at most 64 characters.')
    if (input.dial.trim()) payload.dial = input.dial.trim()
  }
  if (op === 'shot') return payload
  const points = op === 'swipe' ? ['x1', 'y1', 'x2', 'y2'] : ['x', 'y']
  for (const key of points) {
    const value = input[key]
    if (typeof value !== 'number' || !Number.isInteger(value) || value < 0 || value > LAST_PIXEL) return bad(`${key} must be a whole number from 0 to ${LAST_PIXEL}.`)
    payload[key] = value
  }
  if (op !== 'tap') {
    const { min, max, default: fallback } = MS[op]
    const ms = input.ms ?? fallback
    if (typeof ms !== 'number' || !Number.isInteger(ms) || ms < min || ms > max) return bad(`ms must be a whole number from ${min} to ${max}.`)
    payload.ms = ms
  }
  return payload
}

const unavailable = (): DialDebugReply => ({ status: 503, body: { error: 'The devices service did not answer. See the daemon\'s log.', kind: 'unavailable' } })

export async function dialDebugRequest(op: string, body: unknown, ask: AskDevices): Promise<DialDebugReply> {
  if (!OPS.includes(op)) return { status: 404, body: { error: 'not found' } }
  const payload = payloadFor(op as DialDebugOp, body)
  if (!('op' in payload)) return payload as DialDebugReply
  let answer: Record<string, unknown>
  try { answer = await ask(payload, DIAL_DEBUG_WAIT_MS) } catch { return unavailable() }
  // The devices could not be asked, or refused to say. DISABLED is the devices' own reading of the switch: this
  // process was started with it on and they were not, so the route answers as if it were not there.
  if (typeof answer.error === 'string') return answer.error === 'DISABLED' ? { status: 404, body: { error: 'not found' } } : unavailable()
  if (answer.ok === false) {
    const kind = typeof answer.kind === 'string' ? answer.kind : 'device'
    return { status: STATUS[kind] ?? 502, body: {
      error: typeof answer.message === 'string' ? answer.message : 'The dial did not do it.', kind,
      ...(typeof answer.reason === 'string' ? { reason: answer.reason } : {}),
    } }
  }
  if (answer.ok === true && op === 'shot') {
    const png = typeof answer.png === 'string' && answer.png ? Buffer.from(answer.png, 'base64') : null
    if (png && png.subarray(0, PNG_SIGNATURE.length).equals(PNG_SIGNATURE)) return { status: 200, png }
  } else if (answer.ok === true) return { status: 200, body: { ok: true } }
  return { status: 502, body: { error: 'The devices sent back something that is not a picture.', kind: 'device' } }
}

/**
 * What the local server is given as its `onDialDebug`: nothing while the switch is off, so the routes are not
 * there and answer 404 like any route that does not exist.
 */
export function dialDebugHandler(enabled: boolean, ask: AskDevices): ((op: string, body: unknown) => Promise<DialDebugReply>) | undefined {
  return enabled ? (op, body) => dialDebugRequest(op, body, ask) : undefined
}
