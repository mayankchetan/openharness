// The Wi-Fi settings of a dial, over its USB cable: asking for them and waiting for the one answer.
//
// The firmware half is devices/harness-device/firmware/main/cable_client.c (handle_net) and config_store.c (config_net_*);
// the design is docs/specs/cable-wifi-link.md section 5. A dial holds, in its NVS, the Wi-Fi network to join, the address
// of this computer's listener and the TLS-PSK it proves (netDials.ts). It is given them here, over the cable, and never
// over the air: the dial refuses every `net.*` on any other link, and the session sends none on any other link.
//
//   net.set   {ssid, pass, host, port, key (base64, 32 bytes), mode}   all six, or none (the commit marker is written last)
//   net.get   {}                                                        the state, below
//   net.clear {}                                                        forget the network, the address and the key
//   net.mode  {mode}                                                    0 = USB only at boot, 1 = join Wi-Fi when unplugged
//   net.key   {key}                                                     a new key for a dial that is already set (rotate)
//   net.host  {host, port}                                              a new address for the listener, same network and key
//
// Every one is answered with one `net.state` carrying the request's `id`:
//
//   {t:'net.state', id, ok, saved, ssid?, host?, port?, mode?, err?}
//
//   ok     the dial did it. When false, `err` names the FIELD it did not like (ssid, pass, host, port, key, mode) or the
//          reason (not_provisioned, transport, feature, storage, op), never a value.
//   saved  the dial holds a whole configuration (its commit marker is set).
//   ssid, host, port, mode   what it holds, when saved. None of them is a secret: the password and the key are never sent back.
//
// One request at a time per dial. A late answer to an earlier request (it timed out, or the link dropped) carries an id
// that is no longer waited for and is dropped, so it can never be taken for the answer to the next one.
//
// Nothing here logs a value. The lines carry the op, the id and the outcome, and that is all.
import { NET_KEY_BYTES, NET_SSID_MAX, validHost, validKeyBytes, validPort } from '../lib/dialNetRules.js'

/** How long the dial has to answer. A write to its flash takes tens of milliseconds; this is for a dead link. */
export const NET_TIMEOUT_MS = 5_000

export type NetRequest =
  | { op: 'set'; ssid: string; pass: string; host: string; port: number; key: Buffer; mode: 0 | 1 }
  | { op: 'get' }
  | { op: 'clear' }
  | { op: 'mode'; mode: 0 | 1 }
  | { op: 'key'; key: Buffer }
  | { op: 'host'; host: string; port: number }

/** What the dial says it holds. Never the password or the key. */
export interface NetState {
  saved: boolean
  ssid?: string
  host?: string
  port?: number
  mode?: 0 | 1
}

/**
 * Why a request did not come to an answer:
 *   gone         the link dropped, or the dial is not on the wire
 *   busy         another net request is waiting
 *   timeout      the dial did not answer in time
 *   device       the dial answered ok:false; `err` is its word for why
 *   bad_request  this layer refused it before sending (a key of the wrong size)
 * The session adds `transport` (not the cable) and `unsupported` (a firmware with no net.*); the fleet adds `no_dial` and
 * `ambiguous`; the provisioning adds `keystore` (the dial agreed and this computer could not store the key).
 */
export type NetFailureKind = 'gone' | 'busy' | 'timeout' | 'device' | 'bad_request' | 'transport' | 'unsupported' | 'no_dial' | 'ambiguous' | 'keystore'

export type NetSuccess = { ok: true } & NetState
export type NetFailure = { ok: false; kind: NetFailureKind; message: string; err?: string } & Partial<NetState>
export type NetResult = NetSuccess | NetFailure

export interface DialNetDeps {
  /** One JSON frame to the dial; false when it is not on the wire. */
  send(message: { t: string; id: number; [key: string]: unknown }): Promise<boolean>
  log(line: string): void
  timeoutMs?: number
  /** For tests: the id before the first one used. Otherwise somewhere random, so a daemon that restarts does not meet its own old ids. */
  firstId?: number
}

/** The id's field is 16 bits in the debug vocabulary; the same range is kept so one habit reads both. */
const ID_MAX = 65_535

interface Pending {
  id: number
  op: NetRequest['op']
  resolve(result: NetResult): void
  timer: ReturnType<typeof setTimeout>
}

/** A plain lower-case word, as the firmware's field names and reasons are. Anything else is not trusted into a message. */
const WORD = /^[a-z_]{1,24}$/

const PRINTABLE = /^[^\u0000-\u001f\u007f]+$/

/** The dial's state as the daemon will believe it: each field only if it has the right type and shape. */
function readState(msg: { [key: string]: unknown }): NetState {
  const state: NetState = { saved: msg.saved === true }
  if (typeof msg.ssid === 'string' && msg.ssid.length > 0 && Buffer.byteLength(msg.ssid) <= NET_SSID_MAX && PRINTABLE.test(msg.ssid)) state.ssid = msg.ssid
  if (validHost(msg.host)) state.host = msg.host
  if (validPort(msg.port)) state.port = msg.port
  if (msg.mode === 0 || msg.mode === 1) state.mode = msg.mode
  return state
}

export class DialNet {
  private pending: Pending | null = null
  private last: number

  constructor(private readonly deps: DialNetDeps) {
    this.last = deps.firstId ?? 1 + Math.floor(Math.random() * ID_MAX)
  }

  /** Ask, and wait for the answer. Never rejects: every way it can fail is a result. */
  request(request: NetRequest): Promise<NetResult> {
    const wire = this.wire(request)
    if ('error' in wire) return Promise.resolve({ ok: false, kind: 'bad_request', message: wire.error })
    if (this.pending) return Promise.resolve({ ok: false, kind: 'busy', message: 'another Wi-Fi request is still waiting for the dial' })
    const id = this.nextId()
    const timeoutMs = this.deps.timeoutMs ?? NET_TIMEOUT_MS
    return new Promise((resolve) => {
      const p: Pending = { id, op: request.op, resolve,
        timer: setTimeout(() => this.finish(p, { ok: false, kind: 'timeout', message: `the dial did not answer in ${timeoutMs / 1000} s` }), timeoutMs) }
      p.timer.unref?.()
      this.pending = p
      const gone: NetFailure = { ok: false, kind: 'gone', message: 'the dial is not on the wire' }
      this.deps.send({ ...wire.message, id }).then((sent) => { if (!sent) this.finish(p, gone) }, () => this.finish(p, gone))
    })
  }

  /** The link went away (or the session stopped): whatever was asked will not be answered. */
  abort(why: string): void {
    if (this.pending) this.finish(this.pending, { ok: false, kind: 'gone', message: why })
  }

  /**
   * A JSON message from the dial. True when it is a `net.state`, whoever it was for: the session has no other use
   * for it, and an answer that came late is dropped here.
   */
  message(msg: { t?: unknown; [key: string]: unknown }): boolean {
    if (msg.t !== 'net.state') return false
    const p = this.pending
    if (!p || msg.id !== p.id) return true
    const state = readState(msg)
    if (msg.ok === true) { this.finish(p, { ok: true, ...state }); return true }
    const err = typeof msg.err === 'string' && WORD.test(msg.err) ? msg.err : 'unknown'
    this.finish(p, { ok: false, kind: 'device', err, message: `the dial refused: ${err}`, ...state })
    return true
  }

  private nextId(): number {
    this.last = (this.last % ID_MAX) + 1
    return this.last
  }

  /** The flat message for a request, or why it cannot be one. The key travels as base64; nothing else is encoded. */
  private wire(request: NetRequest): { message: { t: string; [key: string]: unknown } } | { error: string } {
    const key = 'key' in request ? request.key : undefined
    if (key && !validKeyBytes(key)) return { error: `a dial key is ${NET_KEY_BYTES} bytes and not all zero` }
    switch (request.op) {
      case 'set': return { message: { t: 'net.set', ssid: request.ssid, pass: request.pass, host: request.host, port: request.port, key: request.key.toString('base64'), mode: request.mode } }
      case 'get': return { message: { t: 'net.get' } }
      case 'clear': return { message: { t: 'net.clear' } }
      case 'mode': return { message: { t: 'net.mode', mode: request.mode } }
      case 'key': return { message: { t: 'net.key', key: request.key.toString('base64') } }
      case 'host': return { message: { t: 'net.host', host: request.host, port: request.port } }
    }
  }

  private finish(p: Pending, result: NetResult): void {
    if (this.pending !== p) return
    clearTimeout(p.timer)
    this.pending = null
    this.deps.log(`cable: net.${p.op} ${p.id} ${result.ok ? 'done' : `failed: ${result.kind}${result.kind === 'device' && result.err ? ` (${result.err})` : ''}`}`)
    p.resolve(result)
  }
}
