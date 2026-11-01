// Giving a dial its Wi-Fi settings and its key, over the cable, and keeping the daemon's keystore in step
// (docs/specs/cable-wifi-link.md section 5.1; task W9). This is the devices' half of `harness dial wifi ...`: the core's
// route (lib/dialNetHttp.ts) checks the request and hands it here as an `AskDevices` payload.
//
//   provision  {dial?, ssid, pass, host?, mode?}  a fresh key, net.set, and the key goes into the keystore AFTER ok:true
//   rotate     {dial?}                            a fresh key, net.key; the keystore moves after ok:true
//   forget     {dial?}                            net.clear, then the key leaves the keystore
//   mode       {dial?, mode}                      net.mode (0 = USB only at boot, 1 = join Wi-Fi when unplugged)
//   status     {dial?}                            what the keystore holds, and what the dial on the cable says
//
// The one rule: the keystore moves only after the dial said ok:true. A refusal, a timeout or a dial that went away leaves
// it exactly as it was, so the dial and the daemon never hold different keys because a message was lost. The one place it
// can still happen is a keystore that will not write AFTER the dial agreed: then the dial is put back as it was (cleared on
// a first set, its old key sent again on a rotate), and the failure says so.
//
// When the dial's state is not known. A `net.set` or `net.key` that times out may have been taken, and a rollback that fails leaves the
// dial holding a key this computer lacks (or the other way round). The answer then says so, in words the CLI prints as they are: set the dial
// again. Nothing is claimed that was not seen.
//
// Credentials. The password, the key and the SSID come in as payload fields and go out as a net.set; none of them is
// logged, put in an error, or sent back. An answer carries the SSID and the address only because they are not secrets.
// Every field is checked again here (lib/dialNetRules.ts), whatever the route did.
//
// The daemon's address. A dial is told where to find this computer: the LAN IPv4 of the interface the default route leaves
// by, or the address the person gave. It must be a private (RFC 1918) address. When this computer's address changes while a
// dial is plugged in, the next attach sends the new one (`resyncHost`, the spec's 5.5); a dial that is already wireless
// can only be fixed by the person, over the cable.
import { randomBytes } from 'node:crypto'
import { createSocket } from 'node:dgram'

import {
  NET_FIELD_MESSAGES, isLanIPv4, validDial, validMode, validPass, validPort, validSsid, type NetField,
} from '../lib/dialNetRules.js'
import type { NetDialHandle } from './cableSession.js'
import type { FleetNetResult } from './cableFleet.js'
import type { DialKeys } from './dialKeys.js'
import type { NetFailure, NetRequest } from './dialNet.js'
import { PSK_LENGTH, normalizePskIdentity } from './tlsPsk.js'

export interface NetProvisionDeps {
  /** Ask the dial on the cable (CableFleet.net). */
  net(dial: string | undefined, request: NetRequest): Promise<FleetNetResult>
  keys: DialKeys
  /** This computer's LAN IPv4 on the default route, or null (lanAddress). */
  lan(): Promise<string | null>
  /** The port of the listener (CABLE_NET_PORT): what a dial is told. */
  port: number
  /** False when the listener is switched off (CABLE_NET_ENABLE=false): no dial can be given a key that this computer would honour. Absent means on. */
  enabled?: boolean
  log(line: string): void
}

type Answer = Record<string, unknown>

const MAC = /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/

/** Added to a failure after which the dial and the keystore may disagree. */
export const STATE_UNKNOWN_NOTE = 'The dial may hold a key this computer lacks. Run `harness dial wifi set` again.'

const bad = (field: NetField | undefined, message: string): Answer => ({ ok: false, kind: 'bad_request', ...(field ? { field } : {}), message })
const badField = (field: NetField): Answer => bad(field, NET_FIELD_MESSAGES[field])
const failure = (result: NetFailure, note = ''): Answer => ({
  ok: false, kind: result.kind, message: note ? `${result.message} ${note}` : result.message, ...(result.err ? { err: result.err } : {}),
})

export class NetProvisioner {
  constructor(private readonly deps: NetProvisionDeps) {}

  async handle(payload: Answer): Promise<Answer> {
    if (payload.dial !== undefined && !validDial(payload.dial)) return badField('dial')
    const dial = typeof payload.dial === 'string' ? payload.dial : undefined
    switch (payload.op) {
      case 'provision': return this.provision(dial, payload)
      case 'rotate': return this.rotate(dial)
      case 'forget': return this.forget(dial)
      case 'mode': return this.mode(dial, payload.mode)
      case 'status': return this.status(dial)
      default: return bad(undefined, 'That is not a Wi-Fi request the devices know.')
    }
  }

  private async provision(dial: string | undefined, payload: Answer): Promise<Answer> {
    // Before anything is asked of the dial or made: a dial that took these settings could never reach a listener that is off, and would sit on its
    // retry ladder with a key nobody honours (review Low 4 of 2026-11-01; welcome.features still names `net`, so the CLI would otherwise succeed).
    if (this.deps.enabled === false) {
      return bad(undefined, 'The Wi-Fi listener of this computer is switched off (CABLE_NET_ENABLE=false), so a dial set up now could not reach it. Turn it on, restart Harness, and run this again.')
    }
    const { ssid, pass } = payload
    if (!validSsid(ssid)) return badField('ssid')
    if (!validPass(pass)) return badField('pass')
    if (payload.port !== undefined && !validPort(payload.port)) return badField('port')
    if (payload.mode !== undefined && !validMode(payload.mode)) return badField('mode')
    const host = payload.host === undefined ? await this.deps.lan() : payload.host
    if (typeof host !== 'string' || !isLanIPv4(host)) {
      return bad('host', payload.host === undefined
        ? 'No private LAN address was found for this computer. Pass --host <IPv4>.'
        : 'host must be a private LAN IPv4 address (10.x, 172.16-31.x or 192.168.x).')
    }
    const port = typeof payload.port === 'number' ? payload.port : this.deps.port
    const mode = payload.mode === 0 ? 0 : 1
    const key = randomBytes(PSK_LENGTH)
    const result = await this.deps.net(dial, { op: 'set', ssid, pass, host, port, key, mode })
    // A timeout may still have been a save: the dial's state is not known.
    if (!result.ok) return failure(result, result.kind === 'timeout' ? STATE_UNKNOWN_NOTE : '')
    // The commit marker is the dial's word that it holds all of it.
    if (!result.saved) return failure({ ok: false, kind: 'device', message: 'the dial answered but did not save the settings' })
    const mac = result.dial
    if (!mac || !MAC.test(mac)) {
      const cleared = await this.putBack(dial)
      return failure({ ok: false, kind: 'device', message: 'the dial did not say its MAC, so its key cannot be filed.' }, cleared ? '' : STATE_UNKNOWN_NOTE)
    }
    try {
      // `--host` pins the address: it is not changed on attach. Without it the pin is gone (a new key starts the record again).
      this.deps.keys.set(mac, key, { hostPinned: payload.host !== undefined })
    } catch (error) {
      this.deps.log(`cable: net: the keystore would not take the key of ${normalizePskIdentity(mac)}: ${error instanceof Error ? error.message : String(error)}`)
      const cleared = await this.putBack(mac)
      return failure({ ok: false, kind: 'keystore', message: cleared
        ? 'The dial took its settings but this computer could not store the key, so the dial was cleared again. See the daemon log.'
        : 'The dial took its settings but this computer could not store the key, and the dial could not be cleared again. See the daemon log.' }, cleared ? '' : STATE_UNKNOWN_NOTE)
    }
    this.deps.log(`cable: net: ${normalizePskIdentity(mac)} was given Wi-Fi settings and a key (mode ${mode})`)
    return { ok: true, dial: normalizePskIdentity(mac), saved: true, ssid, host, port, mode }
  }

  /** Undo a first set whose key could not be filed: the dial and this computer must agree. True when the dial said it is cleared. */
  private async putBack(dial: string | undefined): Promise<boolean> {
    return (await this.deps.net(dial, { op: 'clear' })).ok
  }

  private async rotate(dial: string | undefined): Promise<Answer> {
    const key = randomBytes(PSK_LENGTH)
    const result = await this.deps.net(dial, { op: 'key', key })
    if (!result.ok) return failure(result, result.kind === 'timeout' ? STATE_UNKNOWN_NOTE : '')
    const mac = result.dial
    if (!mac || !MAC.test(mac)) return failure({ ok: false, kind: 'device', message: 'the dial did not say its MAC, so its key cannot be filed' })
    const old = this.deps.keys.get(mac)
    try {
      this.deps.keys.set(mac, key, { hostPinned: this.deps.keys.record(mac)?.hostPinned === true })
    } catch (error) {
      this.deps.log(`cable: net: the keystore would not take the new key of ${normalizePskIdentity(mac)}: ${error instanceof Error ? error.message : String(error)}`)
      // The dial holds the new key and this computer the old one: send the old one back. Without one there is nothing to restore.
      const restored = old ? (await this.deps.net(mac, { op: 'key', key: old })).ok : false
      return failure({ ok: false, kind: 'keystore', message: 'This computer could not store the new key. See the daemon log.' }, restored ? '' : STATE_UNKNOWN_NOTE)
    }
    this.deps.log(`cable: net: ${normalizePskIdentity(mac)} has a new key`)
    return { ok: true, dial: normalizePskIdentity(mac), rotated: true }
  }

  private async forget(dial: string | undefined): Promise<Answer> {
    const result = await this.deps.net(dial, { op: 'clear' })
    if (result.ok) {
      const mac = result.dial ? normalizePskIdentity(result.dial) : undefined
      const removed = mac ? this.removeKey(mac) : false
      if (removed === 'failed') {
        return failure({ ok: false, kind: 'keystore', message: 'The dial forgot its Wi-Fi settings, but this computer could not remove its key. See the daemon log.' })
      }
      this.deps.log(`cable: net: ${mac ?? 'a dial'} forgot its Wi-Fi settings${removed === true ? ' and this computer forgot its key' : ''}`)
      return { ok: true, ...(mac ? { dial: mac } : {}), cleared: true, removed: removed === true }
    }
    // A dial that is away (the very state of a Wi-Fi dial: it is on Wi-Fi, so only `transport` can be said of it) cannot be cleared, but its
    // key can be taken from this computer: it then cannot connect any more. Only for a dial that was named: which key to drop is never guessed.
    if ((result.kind === 'no_dial' || result.kind === 'transport') && dial && MAC.test(dial) && this.deps.keys.has(dial)) {
      const mac = normalizePskIdentity(dial)
      const removed = this.removeKey(mac)
      if (removed !== true) {
        return failure({ ok: false, kind: 'keystore', message: 'This computer could not remove the key of the dial. See the daemon log.' })
      }
      this.deps.log(`cable: net: ${mac} is not on the cable; this computer forgot its key, the dial's own settings are untouched`)
      return { ok: true, dial: mac, cleared: false, removed: true }
    }
    return failure(result)
  }

  /** True: the key was held and is gone. False: none was held. 'failed': it was held and the keystore would not let go of it. */
  private removeKey(mac: string): boolean | 'failed' {
    try { return this.deps.keys.remove(mac) } catch (error) {
      this.deps.log(`cable: net: the keystore would not forget ${mac}: ${error instanceof Error ? error.message : String(error)}`)
      return 'failed'
    }
  }

  private async mode(dial: string | undefined, mode: unknown): Promise<Answer> {
    if (!validMode(mode)) return badField('mode')
    const result = await this.deps.net(dial, { op: 'mode', mode })
    if (!result.ok) return failure(result)
    return { ok: true, ...(result.dial ? { dial: normalizePskIdentity(result.dial) } : {}), mode: result.mode ?? mode }
  }

  private async status(dial: string | undefined): Promise<Answer> {
    const wanted = dial && MAC.test(dial) ? normalizePskIdentity(dial) : undefined
    const keyed = this.deps.keys.ids().filter((id) => !wanted || id === wanted).map((id) => ({ dial: id, ...this.deps.keys.record(id) }))
    const answer: Answer = { ok: true, keyed, port: this.deps.port }
    const lan = await this.deps.lan()
    if (lan) answer.lan = lan
    const result = await this.deps.net(dial, { op: 'get' })
    if (result.ok) {
      const mac = result.dial ? normalizePskIdentity(result.dial) : undefined
      answer.usb = {
        ...(mac ? { dial: mac, keyed: this.deps.keys.has(mac) } : {}),
        saved: result.saved,
        ...(result.ssid !== undefined ? { ssid: result.ssid } : {}),
        ...(result.host !== undefined ? { host: result.host } : {}),
        ...(result.port !== undefined ? { port: result.port } : {}),
        ...(result.mode !== undefined ? { mode: result.mode } : {}),
      }
    } else if (result.kind !== 'no_dial') {
      answer.usbProblem = { kind: result.kind, message: result.message }
    }
    return answer
  }

  /**
   * A dial on the cable keeps Wi-Fi settings (CableFleetOptions.onNetDial). If it holds some, and this computer holds its key,
   * and the address it was given is not this computer's address now, it is given the new one (net.host). A dial that holds
   * settings but whose key this computer lacks is said so, once per attach: it needs to be set again.
   */
  async resyncHost(dial: NetDialHandle): Promise<void> {
    const state = await dial.net({ op: 'get' })
    if (!state.ok || !state.saved) return
    const mac = normalizePskIdentity(dial.mac)
    if (!this.deps.keys.has(mac)) {
      this.deps.log(`cable: net: ${mac} holds Wi-Fi settings but this computer has no key for it. Run: harness dial wifi set`)
      return
    }
    // An address the person gave by hand (`--host`) is theirs to change, not this computer's to follow.
    if (this.deps.keys.record(mac)?.hostPinned) return
    const lan = await this.deps.lan()
    if (!lan || !isLanIPv4(lan)) return
    if (state.host === lan && state.port === this.deps.port) return
    const sent = await dial.net({ op: 'host', host: lan, port: this.deps.port })
    this.deps.log(`cable: net: ${mac} daemon address ${state.host ?? '?'}:${state.port ?? '?'} -> ${lan}:${this.deps.port} ${sent.ok ? 'sent' : `failed (${sent.kind})`}`)
  }
}

/** The subset of a UDP socket used to read the source address. */
interface UdpProbe {
  once(event: 'error', run: () => void): unknown
  connect(port: number, host: string, done: () => void): unknown
  address(): { address: string }
  close(): unknown
}

/**
 * This computer's IPv4 address on the interface its default route leaves by. A UDP socket "connected" to a far address
 * sends nothing: the system only picks the route and the source address it would use, which `address()` then reports. 192.0.2.1
 * is TEST-NET-1 (RFC 5737), an address nothing owns. Null with no route at all.
 */
export function lanAddress(make: () => UdpProbe = () => createSocket('udp4')): Promise<string | null> {
  return new Promise((resolve) => {
    let socket: UdpProbe
    try { socket = make() } catch { resolve(null); return }
    const done = (value: string | null) => {
      try { socket.close() } catch { /* already closed */ }
      resolve(value)
    }
    socket.once('error', () => done(null))
    try {
      socket.connect(9, '192.0.2.1', () => {
        try { done(socket.address().address) } catch { done(null) }
      })
    } catch { done(null) }
  })
}
