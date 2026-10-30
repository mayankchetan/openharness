/**
 * POST /api/dial/net/{provision,status,forget,rotate,mode}: a dial's Wi-Fi settings and key, on the daemon's local API
 * (hookServer.ts), for `harness dial wifi ...` (lib/dialWifiCommand.ts). The settings reach the dial over its USB cable and
 * nowhere else (cable/dialNet.ts); the daemon keeps the dial's key in its keystore (cable/dialKeys.ts) once the dial has
 * taken it. See docs/specs/cable-wifi-link.md section 5.
 *
 * This file is the HTTP half, modeled on dialDebugHttp.ts: it checks what was asked, hands it to the devices
 * (services/devices.ts `dial_net`, where the cable and the keystore are) and turns their answer into a status and a body. It
 * names no cable code: the core's process loads it, and the cable is the devices'.
 *
 * Credentials. The body of `provision` carries the Wi-Fi password. A bad field is answered by NAME ("pass must be ...") and never
 * by value; the answer to a good request carries the SSID and the address, which are not secrets, and the body that goes on
 * to the devices is rebuilt field by field, so nothing a caller added rides along. The reply is rebuilt the same way: only the
 * fields in ANSWER_FIELDS get through, so a password or a key cannot come back whatever a later change puts in the devices' answer.
 * The route logs nothing but its path (hookServer.ts).
 */
import {
  NET_FIELD_MESSAGES, isLanIPv4, validDial, validHost, validMode, validPass, validPort, validSsid, type NetField,
} from './dialNetRules.js'

export type DialNetOp = 'provision' | 'status' | 'forget' | 'rotate' | 'mode'
const OPS: readonly string[] = ['provision', 'status', 'forget', 'rotate', 'mode']

/** A status and a body. */
export interface DialNetReply { status: number; body: unknown }

/** Ask the devices; the second argument is how long to wait for their answer. */
export type AskDevices = (payload: Record<string, unknown>, waitMs: number) => Promise<Record<string, unknown>>

/**
 * How long the core waits for the devices: the dial has 5 s for each message (cable/dialNet.ts) and a request sends up to
 * three (a refusal to store a key sends the dial back what it had), plus the keystore's write.
 */
export const DIAL_NET_WAIT_MS = 20_000

/** HTTP status for each kind of failure; any other is the dial's or the cable's: 502. */
const STATUS: Record<string, number> = {
  bad_request: 400, no_dial: 404, ambiguous: 409, busy: 409, transport: 409, unsupported: 409, timeout: 504, gone: 502, device: 502, keystore: 500,
}

const object = (value: unknown): value is Record<string, unknown> => value !== null && typeof value === 'object' && !Array.isArray(value)

const bad = (field: NetField): DialNetReply => ({ status: 400, body: { error: NET_FIELD_MESSAGES[field], kind: 'bad_request', field } })

/** The payload for the devices, or the 400 to answer with. */
function payloadFor(op: DialNetOp, body: unknown): Record<string, unknown> | DialNetReply {
  const input = body === undefined ? {} : body
  if (!object(input)) return { status: 400, body: { error: 'The body must be a JSON object.', kind: 'bad_request' } }
  const payload: Record<string, unknown> = { op }
  if (input.dial !== undefined) {
    const dial = typeof input.dial === 'string' ? input.dial.trim() : input.dial
    if (!validDial(dial)) return bad('dial')
    payload.dial = dial
  }
  if (op === 'mode') {
    if (!validMode(input.mode)) return bad('mode')
    payload.mode = input.mode
  }
  if (op !== 'provision') return payload
  if (!validSsid(input.ssid)) return bad('ssid')
  if (!validPass(input.pass)) return bad('pass')
  payload.ssid = input.ssid
  payload.pass = input.pass
  if (input.host !== undefined) {
    if (!validHost(input.host) || !isLanIPv4(input.host)) return bad('host')
    payload.host = input.host
  }
  if (input.port !== undefined) {
    if (!validPort(input.port)) return bad('port')
    payload.port = input.port
  }
  if (input.mode !== undefined) {
    if (!validMode(input.mode)) return bad('mode')
    payload.mode = input.mode
  }
  return payload
}

const unavailable = (): DialNetReply => ({ status: 503, body: { error: 'The devices service did not answer. See the daemon\'s log.', kind: 'unavailable' } })

/** The only fields a reply carries, at any depth. None of them is a secret. */
const ANSWER_FIELDS = new Set([
  'ok', 'dial', 'saved', 'ssid', 'host', 'port', 'mode', 'cleared', 'removed', 'rotated', 'keyed', 'createdAt', 'lastSeen', 'lastAddr',
  'usb', 'usbProblem', 'kind', 'message', 'lan', 'hostPinned',
])

function scrub(value: unknown, depth = 0): unknown {
  if (typeof value === 'string') return value.slice(0, 200)
  if (typeof value === 'number' || typeof value === 'boolean') return value
  if (depth > 3) return undefined
  if (Array.isArray(value)) return value.slice(0, 16).map((item) => scrub(item, depth + 1)).filter((item) => item !== undefined)
  if (!object(value)) return undefined
  const out: Record<string, unknown> = {}
  for (const [key, inner] of Object.entries(value)) {
    if (!ANSWER_FIELDS.has(key)) continue
    const kept = scrub(inner, depth + 1)
    if (kept !== undefined) out[key] = kept
  }
  return out
}

const WORD = /^[a-z_]{1,24}$/
const FIELDS = new Set<string>(Object.keys(NET_FIELD_MESSAGES))

export async function dialNetRequest(op: string, body: unknown, ask: AskDevices): Promise<DialNetReply> {
  if (!OPS.includes(op)) return { status: 404, body: { error: 'not found' } }
  const payload = payloadFor(op as DialNetOp, body)
  if (!('op' in payload)) return payload as DialNetReply
  let answer: Record<string, unknown>
  try { answer = await ask(payload, DIAL_NET_WAIT_MS) } catch { return unavailable() }
  // The devices could not be asked, or refused to say. DISABLED: they hold no keystore (the Wi-Fi link is not set up in this
  // process), so the route answers as if it were not there.
  if (typeof answer.error === 'string') return answer.error === 'DISABLED' ? { status: 404, body: { error: 'not found' } } : unavailable()
  if (answer.ok === false) {
    const kind = typeof answer.kind === 'string' && Object.hasOwn(STATUS, answer.kind) ? answer.kind : 'device'
    return { status: STATUS[kind], body: {
      error: typeof answer.message === 'string' ? answer.message.slice(0, 300) : 'The dial did not do it.', kind,
      ...(typeof answer.err === 'string' && WORD.test(answer.err) ? { err: answer.err } : {}),
      ...(typeof answer.field === 'string' && FIELDS.has(answer.field) ? { field: answer.field } : {}),
    } }
  }
  if (answer.ok === true) return { status: 200, body: scrub(answer) }
  return { status: 502, body: { error: 'The devices sent back something that is not an answer.', kind: 'device' } }
}

/** What the local server is given as its `onDialNet`. */
export function dialNetHandler(ask: AskDevices): (op: string, body: unknown) => Promise<DialNetReply> {
  return (op, body) => dialNetRequest(op, body, ask)
}
