// Giving a dial its Wi-Fi settings and its key over the cable, and keeping the daemon's keystore in step (netProvision.ts).
// The dial is a stand-in answering net.* the way the firmware does; the keystore is the real one on a temp folder.
//
// The rule under test everywhere: the keystore moves only after the dial said ok:true. A refusal, a timeout or a dial that
// went away leaves it exactly as it was.
import { createHash } from 'node:crypto'
import { mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { pskConnect } from './__fixtures__/pskClient.js'
import type { FleetNetResult } from './cableFleet.js'
import { DIAL_KEY_FILE, DialKeys } from './dialKeys.js'
import type { NetRequest } from './dialNet.js'
import { NetDials } from './netDials.js'
import { NetProvisioner, lanAddress } from './netProvision.js'

const MAC = 'B8:1F:3F:CA:E5:58'
const OTHER = 'B8:1F:3F:CA:E5:59'
const LAN = '192.168.1.20'
const PASS = 'correct horse battery'
const SSID = 'home-net'

const cleanup: Array<() => unknown> = []
afterEach(async () => { for (const run of cleanup.splice(0).reverse()) await run() })

type Net = (dial: string | undefined, request: NetRequest) => Promise<FleetNetResult>

function rig(over: { net?: Net; lan?: () => Promise<string | null>; keys?: DialKeys; port?: number; enabled?: boolean } = {}) {
  const dir = mkdtempSync(join(tmpdir(), 'net-provision-'))
  cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
  const keys = over.keys ?? new DialKeys(join(dir, DIAL_KEY_FILE))
  const calls: Array<{ dial: string | undefined; request: NetRequest }> = []
  const seen: Array<{ op: string; hadKey: boolean }> = []
  const lines: string[] = []
  // A dial that holds nothing until it is set, as the firmware does.
  const held: { ssid?: string; host?: string; port?: number; mode?: 0 | 1 | 2; ble?: boolean } = {}
  const net: Net = over.net ?? (async (dial, request) => {
    calls.push({ dial, request })
    seen.push({ op: request.op, hadKey: keys.has(MAC) })
    switch (request.op) {
      case 'set': Object.assign(held, { ssid: request.ssid, host: request.host, port: request.port, mode: request.mode }); return { ok: true, saved: true, ...held, dial: MAC }
      case 'get': return { ok: true, saved: !!held.ssid, ...held, dial: MAC }
      case 'set_ble': held.ble = true; held.mode ??= 0; return { ok: true, saved: !!held.ssid, ...held, dial: MAC }
      case 'clear': for (const k of Object.keys(held)) delete held[k as keyof typeof held]; return { ok: true, saved: false, dial: MAC }
      case 'mode': return (request.mode === 2 ? held.ble : held.ssid || held.ble) ? (held.mode = request.mode, { ok: true, saved: !!held.ssid, ...held, dial: MAC }) : { ok: false, kind: 'device', err: 'not_provisioned', message: 'the dial refused: not_provisioned', saved: false, dial: MAC }
      case 'key': return held.ssid ? { ok: true, saved: true, ...held, dial: MAC } : { ok: false, kind: 'device', err: 'not_provisioned', message: 'the dial refused: not_provisioned', saved: false, dial: MAC }
      case 'host': Object.assign(held, { host: request.host, port: request.port }); return { ok: true, saved: true, ...held, dial: MAC }
    }
  })
  const wrapped: Net = over.net ? async (dial, request) => { calls.push({ dial, request }); return over.net!(dial, request) } : net
  const prov = new NetProvisioner({ net: wrapped, keys, lan: over.lan ?? (async () => LAN), port: over.port ?? 18480, log: (line) => lines.push(line), enabled: over.enabled })
  return { prov, keys, calls, seen, held, lines, dir, file: join(dir, DIAL_KEY_FILE) }
}

const provision = { op: 'provision', ssid: SSID, pass: PASS }
/** The real keystore, but a disk that will not take a write: its private state is reached through the real object. */
const refusingSet = (real: DialKeys): DialKeys => new Proxy(real, {
  get: (target, key) => key === 'set' ? () => { throw new Error('disk full') } : typeof target[key as keyof DialKeys] === 'function' ? (target[key as keyof DialKeys] as (...args: unknown[]) => unknown).bind(target) : target[key as keyof DialKeys],
})
const refusal = (err = 'pass'): FleetNetResult => ({ ok: false, kind: 'device', err, message: `the dial refused: ${err}`, saved: false, dial: MAC })

describe('provision with the listener switched off (review Low 4 of 2026-11-01)', () => {
  it('is refused before the dial is asked or a key is made: a dial set up now could not reach this computer', async () => {
    const { prov, keys, calls, lines } = rig({ enabled: false })
    const answer = await prov.handle(provision)
    expect(answer).toMatchObject({ ok: false, kind: 'bad_request' })
    expect(String(answer.message)).toMatch(/CABLE_NET_ENABLE/)
    expect(calls).toHaveLength(0)
    expect(keys.has(MAC)).toBe(false)
    expect(lines.join('\n')).not.toContain(PASS)
  })

  it('still lets a dial be forgotten or asked about: only a new key is withheld', async () => {
    const { prov, calls } = rig({ enabled: false })
    expect(await prov.handle({ op: 'status' })).toMatchObject({ ok: true })
    await prov.handle({ op: 'forget' })
    expect(calls.length).toBeGreaterThan(0)
  })

  it('is as before when the listener is on, whether said or left out', async () => {
    for (const enabled of [true, undefined]) {
      const { prov } = rig({ enabled })
      expect(await prov.handle(provision)).toMatchObject({ ok: true, dial: MAC })
    }
  })
})

describe('provision', () => {
  it('sends the network, this computer\'s address and a fresh 32-byte key, and stores the key once the dial says ok', async () => {
    const { prov, keys, calls, seen } = rig()
    const answer = await prov.handle(provision)
    expect(answer).toEqual({ ok: true, dial: MAC, saved: true, ssid: SSID, host: LAN, port: 18480, mode: 1 })
    expect(calls).toHaveLength(1)
    const sent = calls[0].request as Extract<NetRequest, { op: 'set' }>
    expect(sent).toMatchObject({ op: 'set', ssid: SSID, pass: PASS, host: LAN, port: 18480, mode: 1 })
    expect(sent.key).toHaveLength(32)
    // the daemon holds the very key the dial was given, and did not hold it while the dial was still deciding
    expect(keys.get(MAC)).toEqual(sent.key)
    expect(seen).toEqual([{ op: 'set', hadKey: false }])
    expect(JSON.stringify(answer)).not.toContain(PASS)
  })

  it('a different key every time', async () => {
    const { prov, calls } = rig()
    await prov.handle(provision)
    await prov.handle(provision)
    const [a, b] = calls.map((call) => (call.request as { key: Buffer }).key)
    expect(a.equals(b)).toBe(false)
  })

  it('passes --dial on, and a host and mode that were asked for', async () => {
    const { prov, calls } = rig()
    await prov.handle({ ...provision, dial: 'serial-1', host: '10.0.0.7', mode: 0 })
    expect(calls[0].dial).toBe('serial-1')
    expect(calls[0].request).toMatchObject({ host: '10.0.0.7', mode: 0 })
  })

  it('uses the listener\'s port as the daemon\'s', async () => {
    const { prov, calls } = rig({ port: 18999 })
    await prov.handle(provision)
    expect(calls[0].request).toMatchObject({ port: 18999 })
  })

  it('leaves the keystore as it was when the dial refuses, and says which field, without a value', async () => {
    const old = Buffer.alloc(32, 9)
    const { prov, keys, file } = rig({ net: async () => refusal('pass') })
    keys.set(MAC, old)
    const before = readFileSync(file, 'utf8')
    const answer = await prov.handle(provision)
    expect(answer).toMatchObject({ ok: false, kind: 'device', err: 'pass' })
    expect(JSON.stringify(answer)).not.toContain(PASS)
    expect(keys.get(MAC)).toEqual(old)
    expect(readFileSync(file, 'utf8')).toBe(before)
  })

  it('leaves the keystore as it was on a timeout, and when the dial is gone, busy, or not on the cable', async () => {
    for (const kind of ['timeout', 'gone', 'busy', 'no_dial', 'ambiguous', 'transport', 'unsupported'] as const) {
      const { prov, keys } = rig({ net: async () => ({ ok: false, kind, message: `no: ${kind}` }) })
      const answer = await prov.handle(provision)
      expect(answer, kind).toMatchObject({ ok: false, kind })
      expect(keys.ids(), kind).toEqual([])
    }
  })

  it('refuses a dial that answered ok without saying it saved', async () => {
    const { prov, keys } = rig({ net: async () => ({ ok: true, saved: false, dial: MAC }) })
    expect(await prov.handle(provision)).toMatchObject({ ok: false, kind: 'device' })
    expect(keys.ids()).toEqual([])
  })

  it('replaces the key of a dial that is set again, only after the dial agreed', async () => {
    const { prov, keys } = rig()
    await prov.handle(provision)
    const first = keys.get(MAC)!
    await prov.handle(provision)
    expect(keys.get(MAC)!.equals(first)).toBe(false)
    expect(keys.size).toBe(1)
  })

  it('does not store a key for a dial that did not say its MAC, and puts the dial back', async () => {
    const { prov, keys, calls } = rig({ net: async (_dial, request) => ({ ok: true, saved: request.op === 'set' }) })
    expect(await prov.handle(provision)).toMatchObject({ ok: false, kind: 'device' })
    expect(keys.ids()).toEqual([])
    expect(calls.map((call) => call.request.op)).toEqual(['set', 'clear'])
  })

  it('puts the dial back when the keystore will not take the key, so the two agree', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'net-provision-'))
    cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
    const real = new DialKeys(join(dir, DIAL_KEY_FILE))
    const keys = refusingSet(real)
    const { prov, calls, lines } = rig({ keys })
    const answer = await prov.handle(provision)
    expect(answer).toMatchObject({ ok: false, kind: 'keystore' })
    expect(calls.map((call) => call.request.op)).toEqual(['set', 'clear'])
    expect(lines.join('\n')).not.toContain(PASS)
  })

  it('needs an address: none found and none given is a bad request about the host', async () => {
    const { prov, calls } = rig({ lan: async () => null })
    expect(await prov.handle(provision)).toMatchObject({ ok: false, kind: 'bad_request', field: 'host' })
    expect(calls).toEqual([])
  })

  it('refuses an address that is not a private LAN one, found or given', async () => {
    const found = rig({ lan: async () => '8.8.8.8' })
    expect(await found.prov.handle(provision)).toMatchObject({ ok: false, kind: 'bad_request', field: 'host' })
    for (const host of ['8.8.8.8', '127.0.0.1', '169.254.1.1', 'dial.local', '']) {
      const given = rig()
      expect(await given.prov.handle({ ...provision, host }), host).toMatchObject({ ok: false, kind: 'bad_request', field: 'host' })
      expect(given.calls).toEqual([])
    }
  })

  it('checks every field again, here, before it asks the dial: the field is named, the value never', async () => {
    const cases: Array<[Record<string, unknown>, string]> = [
      [{ ssid: '', pass: PASS }, 'ssid'], [{ ssid: 7, pass: PASS }, 'ssid'], [{ ssid: 'x'.repeat(33), pass: PASS }, 'ssid'],
      [{ ssid: SSID, pass: 'short' }, 'pass'], [{ ssid: SSID, pass: 'x'.repeat(64) }, 'pass'], [{ ssid: SSID }, 'pass'],
      [{ ssid: SSID, pass: PASS, port: 80 }, 'port'], [{ ssid: SSID, pass: PASS, mode: 2 }, 'mode'], [{ ssid: SSID, pass: PASS, dial: 'a\nb' }, 'dial'],
    ]
    for (const [fields, field] of cases) {
      const { prov, calls } = rig()
      const answer = await prov.handle({ op: 'provision', ...fields })
      expect(answer, field).toMatchObject({ ok: false, kind: 'bad_request', field })
      expect(calls).toEqual([])
      for (const value of Object.values(fields)) if (typeof value === 'string' && value.length > 3) expect(JSON.stringify(answer)).not.toContain(value)
    }
  })

  it('an open network (empty password) is allowed', async () => {
    const { prov, calls } = rig()
    expect(await prov.handle({ op: 'provision', ssid: SSID, pass: '' })).toMatchObject({ ok: true })
    expect(calls[0].request).toMatchObject({ pass: '' })
  })
})

describe('the other operations', () => {
  it('knows only the ones it has', async () => {
    const { prov } = rig()
    expect(await prov.handle({ op: 'reboot' })).toMatchObject({ ok: false, kind: 'bad_request' })
    expect(await prov.handle({})).toMatchObject({ ok: false, kind: 'bad_request' })
  })

  describe('rotate', () => {
    it('sends a new key and replaces the stored one after the dial said ok', async () => {
      const { prov, keys, calls, seen } = rig()
      await prov.handle(provision)
      const old = keys.get(MAC)!
      calls.length = 0; seen.length = 0
      expect(await prov.handle({ op: 'rotate' })).toEqual({ ok: true, dial: MAC, rotated: true })
      const sent = (calls[0].request as Extract<NetRequest, { op: 'key' }>).key
      expect(sent).toHaveLength(32)
      expect(sent.equals(old)).toBe(false)
      expect(keys.get(MAC)).toEqual(sent)
    })

    it('leaves the stored key alone when the dial refuses or does not answer', async () => {
      for (const result of [refusal('not_provisioned'), { ok: false, kind: 'timeout', message: 'late' } as FleetNetResult]) {
        const old = Buffer.alloc(32, 4)
        const { prov, keys } = rig({ net: async () => result })
        keys.set(MAC, old)
        expect(await prov.handle({ op: 'rotate' })).toMatchObject({ ok: false })
        expect(keys.get(MAC)).toEqual(old)
      }
    })

    it('puts the old key back on the dial when the keystore will not take the new one', async () => {
      const dir = mkdtempSync(join(tmpdir(), 'net-provision-'))
      cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
      const real = new DialKeys(join(dir, DIAL_KEY_FILE))
      const old = Buffer.alloc(32, 5)
      real.set(MAC, old)
      const keys = refusingSet(real)
      const { prov, calls } = rig({ keys, net: async () => ({ ok: true, saved: true, dial: MAC }) })
      expect(await prov.handle({ op: 'rotate' })).toMatchObject({ ok: false, kind: 'keystore' })
      const sent = calls.map((call) => call.request as { op: string; key?: Buffer })
      expect(sent.map((s) => s.op)).toEqual(['key', 'key'])
      expect(sent[1].key).toEqual(old)
    })

    it('recovers a dial that is set while this computer holds no key for it: the new key is stored', async () => {
      const { prov, keys } = rig()
      await prov.handle(provision)
      keys.remove(MAC)
      const answer = await prov.handle({ op: 'rotate' })
      expect(answer).toMatchObject({ ok: true, rotated: true })
      expect(keys.has(MAC)).toBe(true)
    })

    it('the old key fails the TLS handshake afterwards, the new one passes (the listener of netDials.ts, a PSK client)', async () => {
      const dir = mkdtempSync(join(tmpdir(), 'net-provision-'))
      cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
      const keys = new DialKeys(join(dir, DIAL_KEY_FILE))
      const { prov } = rig({ keys })
      await prov.handle(provision)
      const first = keys.get(MAC)!
      const dials = new NetDials({ keys, port: 0, bind: '127.0.0.1', log: () => {}, onDial: (connection) => { connection.socket.on('error', () => {}); cleanup.push(() => connection.socket.destroy()) } })
      cleanup.push(() => dials.stop())
      dials.start()
      await dials.settled()
      const port = dials.address()!.port
      const ok = await pskConnect({ port, identity: MAC, psk: first })
      ok.destroy()
      expect(await prov.handle({ op: 'rotate' })).toMatchObject({ ok: true })
      await expect(pskConnect({ port, identity: MAC, psk: first })).rejects.toBeDefined()
      const second = keys.get(MAC)!
      expect(second.equals(first)).toBe(false)
      const again = await pskConnect({ port, identity: MAC, psk: second })
      again.destroy()
    })
  })

  describe('forget', () => {
    it('clears the dial, then removes its key', async () => {
      const { prov, keys, held } = rig()
      await prov.handle(provision)
      expect(await prov.handle({ op: 'forget' })).toEqual({ ok: true, dial: MAC, cleared: true, removed: true })
      expect(keys.has(MAC)).toBe(false)
      expect(held.ssid).toBeUndefined()
    })

    it('keeps the key when the dial would not clear: the dial still holds it', async () => {
      const { prov, keys } = rig({ net: async () => ({ ok: false, kind: 'timeout', message: 'late' }) })
      keys.set(MAC, Buffer.alloc(32, 1))
      expect(await prov.handle({ op: 'forget' })).toMatchObject({ ok: false, kind: 'timeout' })
      expect(keys.has(MAC)).toBe(true)
    })

    it('removes the key of a dial that is named and not on the cable, and says the dial itself was not touched', async () => {
      const { prov, keys } = rig({ net: async () => ({ ok: false, kind: 'no_dial', message: 'none' }) })
      keys.set(MAC, Buffer.alloc(32, 1))
      expect(await prov.handle({ op: 'forget', dial: MAC.toLowerCase() })).toMatchObject({ ok: true, dial: MAC, cleared: false, removed: true })
      expect(keys.has(MAC)).toBe(false)
    })

    it('does not guess which key to remove when no dial is named and none is on the cable', async () => {
      const { prov, keys } = rig({ net: async () => ({ ok: false, kind: 'no_dial', message: 'none' }) })
      keys.set(MAC, Buffer.alloc(32, 1))
      expect(await prov.handle({ op: 'forget' })).toMatchObject({ ok: false, kind: 'no_dial' })
      expect(await prov.handle({ op: 'forget', dial: OTHER })).toMatchObject({ ok: false, kind: 'no_dial' })
      expect(keys.has(MAC)).toBe(true)
    })

    it('a dial that was cleared and had no key here is still a success', async () => {
      const { prov } = rig()
      expect(await prov.handle({ op: 'forget' })).toEqual({ ok: true, dial: MAC, cleared: true, removed: false })
    })
  })

  describe('mode', () => {
    it('switches the dial and touches no key', async () => {
      const { prov, keys, calls } = rig()
      await prov.handle(provision)
      const key = keys.get(MAC)!
      calls.length = 0
      expect(await prov.handle({ op: 'mode', mode: 0 })).toEqual({ ok: true, dial: MAC, mode: 0 })
      expect(calls[0].request).toEqual({ op: 'mode', mode: 0 })
      expect(keys.get(MAC)).toEqual(key)
    })

    it('needs a mode of 0 or 1, and passes the dial\'s refusal on', async () => {
      const { prov } = rig()
      expect(await prov.handle({ op: 'mode', mode: 3 })).toMatchObject({ ok: false, kind: 'bad_request', field: 'mode' })
      expect(await prov.handle({ op: 'mode', mode: 1 })).toMatchObject({ ok: false, kind: 'device', err: 'not_provisioned' })
    })
  })

  describe('status', () => {
    it('lists the dials this computer holds a key for, without a key, and what the dial on the cable says', async () => {
      const { prov, keys } = rig()
      await prov.handle(provision)
      const key = keys.get(MAC)!
      const answer = await prov.handle({ op: 'status' })
      expect(answer).toMatchObject({
        ok: true, lan: LAN, port: 18480,
        keyed: [{ dial: MAC, createdAt: expect.any(Number) }],
        usb: { dial: MAC, saved: true, ssid: SSID, host: LAN, port: 18480, mode: 1, keyed: true },
      })
      const text = JSON.stringify(answer)
      for (const secret of [key.toString('base64'), key.toString('hex'), createHash('sha256').update(key).digest('hex'), PASS]) expect(text).not.toContain(secret)
    })

    it('still answers with the keystore when no dial is on the cable', async () => {
      const { prov, keys } = rig({ net: async () => ({ ok: false, kind: 'no_dial', message: 'none' }) })
      keys.set(MAC, Buffer.alloc(32, 1))
      const answer = await prov.handle({ op: 'status' })
      expect(answer).toMatchObject({ ok: true, keyed: [{ dial: MAC }] })
      expect(answer.usb).toBeUndefined()
      expect(answer.usbProblem).toBeUndefined()
    })

    it('says why the dial on the cable could not be read when it is not simply absent', async () => {
      const { prov } = rig({ net: async () => ({ ok: false, kind: 'ambiguous', message: 'two dials' }) })
      expect(await prov.handle({ op: 'status' })).toMatchObject({ ok: true, usbProblem: { kind: 'ambiguous', message: 'two dials' } })
    })

    it('narrows the keystore list to the dial that was named', async () => {
      const { prov, keys } = rig({ net: async () => ({ ok: false, kind: 'no_dial', message: 'none' }) })
      keys.set(MAC, Buffer.alloc(32, 1))
      keys.set(OTHER, Buffer.alloc(32, 2))
      const answer = await prov.handle({ op: 'status', dial: OTHER.toLowerCase() })
      expect(answer.keyed).toEqual([expect.objectContaining({ dial: OTHER })])
    })

    it('answers without an address when none is found', async () => {
      const { prov } = rig({ lan: async () => null })
      const answer = await prov.handle({ op: 'status' })
      expect(answer.ok).toBe(true)
      expect(answer.lan).toBeUndefined()
    })
  })
})

describe('keeping the dial\'s copy of this computer\'s address right when it is plugged in', () => {
  const handle = (mac: string, state: FleetNetResult) => {
    const asked: NetRequest[] = []
    return { asked, dial: { mac, net: async (request: NetRequest): Promise<FleetNetResult> => { asked.push(request); return request.op === 'get' ? state : { ok: true, saved: true } } } }
  }
  const SAVED: FleetNetResult = { ok: true, saved: true, ssid: SSID, host: '192.168.1.99', port: 18480, mode: 1 }

  it('sends net.host when the dial holds another address than the one this computer has now', async () => {
    const { prov, keys, lines } = rig()
    keys.set(MAC, Buffer.alloc(32, 1))
    const { asked, dial } = handle(MAC, SAVED)
    await prov.resyncHost(dial)
    expect(asked).toEqual([{ op: 'get' }, { op: 'host', host: LAN, port: 18480 }])
    expect(lines.join('\n')).toContain(`192.168.1.99:18480 -> ${LAN}:18480`)
  })

  it('also when only the port differs', async () => {
    const { prov, keys } = rig({ port: 18999 })
    keys.set(MAC, Buffer.alloc(32, 1))
    const { asked, dial } = handle(MAC, { ...SAVED, host: LAN })
    await prov.resyncHost(dial)
    expect(asked.map((r) => r.op)).toEqual(['get', 'host'])
  })

  it('sends nothing when the dial already holds this address', async () => {
    const { prov, keys } = rig()
    keys.set(MAC, Buffer.alloc(32, 1))
    const { asked, dial } = handle(MAC, { ...SAVED, host: LAN })
    await prov.resyncHost(dial)
    expect(asked).toEqual([{ op: 'get' }])
  })

  it('sends nothing for a dial with no Wi-Fi settings, or when the dial cannot be read', async () => {
    const { prov, keys } = rig()
    keys.set(MAC, Buffer.alloc(32, 1))
    const unset = handle(MAC, { ok: true, saved: false })
    await prov.resyncHost(unset.dial)
    expect(unset.asked).toEqual([{ op: 'get' }])
    const broken = handle(MAC, { ok: false, kind: 'timeout', message: 'late' })
    await prov.resyncHost(broken.dial)
    expect(broken.asked).toEqual([{ op: 'get' }])
  })

  it('sends nothing for a dial this computer holds no key for, and says to set it again, once per attach', async () => {
    const { prov, lines } = rig()
    const { asked, dial } = handle(MAC, SAVED)
    await prov.resyncHost(dial)
    expect(asked).toEqual([{ op: 'get' }])
    expect(lines.join('\n')).toContain('harness dial wifi set')
  })

  it('sends nothing when this computer has no private address now', async () => {
    for (const lan of [null, '8.8.8.8']) {
      const { prov, keys } = rig({ lan: async () => lan })
      keys.set(MAC, Buffer.alloc(32, 1))
      const { asked, dial } = handle(MAC, SAVED)
      await prov.resyncHost(dial)
      expect(asked, String(lan)).toEqual([{ op: 'get' }])
    }
  })

  it('logs a failed send without any value but the two addresses', async () => {
    const { prov, keys, lines } = rig()
    keys.set(MAC, Buffer.alloc(32, 1))
    const dial = { mac: MAC, net: async (request: NetRequest): Promise<FleetNetResult> => request.op === 'get' ? SAVED : { ok: false, kind: 'timeout', message: 'late' } }
    await prov.resyncHost(dial)
    expect(lines.join('\n')).toContain('failed (timeout)')
  })
})

describe('the address of this computer on the network', () => {
  it('is the source address the routing table picks for a route out, found without sending a packet', async () => {
    const sockets: Array<{ connected?: [number, string]; closed: boolean }> = []
    const make = () => {
      const state: { connected?: [number, string]; closed: boolean } = { closed: false }
      sockets.push(state)
      return {
        once: () => undefined,
        connect: (port: number, host: string, done: () => void) => { state.connected = [port, host]; done() },
        address: () => ({ address: '192.168.1.20', family: 'IPv4', port: 51000 }),
        close: () => { state.closed = true },
      }
    }
    expect(await lanAddress(make as never)).toBe('192.168.1.20')
    expect(sockets[0].connected?.[1]).toBe('192.0.2.1')
    expect(sockets[0].closed).toBe(true)
  })

  it('is null when there is no route, or the socket cannot be made', async () => {
    const failing = () => ({ once: (_event: string, run: () => void) => { queueMicrotask(run) }, connect: () => undefined, address: () => ({ address: '' }), close: () => undefined })
    expect(await lanAddress(failing as never)).toBeNull()
    expect(await lanAddress((() => { throw new Error('no udp') }) as never)).toBeNull()
    const throwing = () => ({ once: () => undefined, connect: () => { throw new Error('unreachable') }, address: () => ({ address: '' }), close: () => { throw new Error('closed twice') } })
    expect(await lanAddress(throwing as never)).toBeNull()
    const noAddress = () => ({ once: () => undefined, connect: (_p: number, _h: string, done: () => void) => done(), address: () => { throw new Error('not bound') }, close: () => undefined })
    expect(await lanAddress(noAddress as never)).toBeNull()
  })
})

describe('the phone mode (B6)', () => {
  const phoneSet = { op: 'phone_set' }
  const lastOps = (calls: Array<{ request: NetRequest }>) => calls.map((c) => c.request.op)

  describe('phone_set', () => {
    it('sends the key-only set, then mode 2, and files the same key after the dial said ok', async () => {
      const { prov, keys, calls, seen, held } = rig()
      const answer = await prov.handle(phoneSet)
      expect(answer).toEqual({ ok: true, dial: MAC, ble: true, mode: 2 })
      expect(lastOps(calls)).toEqual(['set_ble', 'mode'])
      const sent = calls[0].request as Extract<NetRequest, { op: 'set_ble' }>
      expect(sent.key).toHaveLength(32)
      expect(Object.keys(sent).sort()).toEqual(['key', 'op'])
      expect(calls[1].request).toEqual({ op: 'mode', mode: 2 })
      expect(keys.get(MAC)).toEqual(sent.key)
      // the keystore did not hold the key while the dial was still deciding
      expect(seen[0]).toEqual({ op: 'set_ble', hadKey: false })
      expect(held).toMatchObject({ ble: true, mode: 2 })
    })

    it('stays off with mode 0: only the key-only set is sent', async () => {
      const { prov, calls, keys } = rig()
      const answer = await prov.handle({ ...phoneSet, mode: 0 })
      expect(answer).toEqual({ ok: true, dial: MAC, ble: true, mode: 0 })
      expect(lastOps(calls)).toEqual(['set_ble'])
      expect(keys.has(MAC)).toBe(true)
    })

    it('refuses a mode that is not 0 or 2, a dial name that is not one, and sends nothing', async () => {
      const { prov, calls } = rig()
      expect(await prov.handle({ ...phoneSet, mode: 1 })).toMatchObject({ ok: false, kind: 'bad_request', field: 'mode' })
      expect(await prov.handle({ ...phoneSet, dial: 5 })).toMatchObject({ ok: false, kind: 'bad_request', field: 'dial' })
      expect(calls).toEqual([])
    })

    it('a different key every time', async () => {
      const { prov, calls } = rig()
      await prov.handle(phoneSet)
      await prov.handle(phoneSet)
      const keysSent = calls.filter((c) => c.request.op === 'set_ble').map((c) => (c.request as { key: Buffer }).key)
      expect(keysSent[0].equals(keysSent[1])).toBe(false)
    })

    it('leaves the keystore as it was when the dial refuses (a firmware without the link says feature), and names the reason', async () => {
      const old = Buffer.alloc(32, 9)
      const { prov, keys, file } = rig({ net: async () => refusal('feature') })
      keys.set(MAC, old)
      const before = readFileSync(file, 'utf8')
      const answer = await prov.handle(phoneSet)
      expect(answer).toMatchObject({ ok: false, kind: 'device', err: 'feature' })
      expect(keys.get(MAC)).toEqual(old)
      expect(readFileSync(file, 'utf8')).toBe(before)
    })

    it('says the state is unknown after a timeout, and leaves the keystore alone', async () => {
      const { prov, keys } = rig({ net: async () => ({ ok: false, kind: 'timeout', message: 'the dial did not answer in 5 s' }) })
      const answer = await prov.handle(phoneSet)
      expect(answer).toMatchObject({ ok: false, kind: 'timeout' })
      expect(String(answer.message)).toContain('harness dial phone set')
      expect(keys.has(MAC)).toBe(false)
    })

    it('does not file a key when the dial answered ok but does not hold phone settings', async () => {
      const { prov, keys } = rig({ net: async (_d, request) => (request.op === 'set_ble' ? { ok: true, saved: false, ble: false, dial: MAC } : { ok: true, saved: false, dial: MAC }) })
      const answer = await prov.handle(phoneSet)
      expect(answer).toMatchObject({ ok: false, kind: 'device' })
      expect(keys.has(MAC)).toBe(false)
    })

    it('clears the dial again when it did not say its MAC, so a key it holds is not left unfiled', async () => {
      const seenOps: string[] = []
      const { prov, keys } = rig({
        net: async (_d, request) => {
          seenOps.push(request.op)
          return request.op === 'set_ble' ? { ok: true, saved: false, ble: true } : { ok: true, saved: false }
        },
      })
      const answer = await prov.handle(phoneSet)
      expect(answer).toMatchObject({ ok: false, kind: 'device' })
      expect(seenOps).toEqual(['set_ble', 'clear'])
      expect(keys.has(MAC)).toBe(false)
    })

    it('puts a first set back (clear) when the keystore will not take the key', async () => {
      const real = rig()
      const { prov, calls, held } = rig({ keys: refusingSet(real.keys) })
      const answer = await prov.handle(phoneSet)
      expect(answer).toMatchObject({ ok: false, kind: 'keystore' })
      expect(lastOps(calls)).toEqual(['set_ble', 'clear'])
      expect(held).toEqual({})
    })

    it('sends the old key back when the keystore will not take a new one for a dial that already had one', async () => {
      const dir = rig()
      const old = Buffer.alloc(32, 4)
      dir.keys.set(MAC, old)
      const { prov, calls } = rig({ keys: refusingSet(dir.keys) })
      const answer = await prov.handle(phoneSet)
      expect(answer).toMatchObject({ ok: false, kind: 'keystore' })
      expect(lastOps(calls)).toEqual(['set_ble', 'key'])
      expect((calls[1].request as { key: Buffer }).key).toEqual(old)
    })

    it('when mode 2 fails after the key is filed, says so and tells the person what to run; the key stays (the dial holds it)', async () => {
      const { prov, keys, calls } = rig({
        net: async (_d, request) => (request.op === 'mode' ? refusal('feature') : { ok: true, saved: false, ble: true, dial: MAC }),
      })
      const answer = await prov.handle(phoneSet)
      expect(answer).toMatchObject({ ok: false, kind: 'device', err: 'feature' })
      expect(String(answer.message)).toContain('harness dial phone on')
      expect(keys.has(MAC)).toBe(true)
      expect(lastOps(calls)).toEqual(['set_ble', 'mode'])
    })

    it('keeps the Wi-Fi settings and the pinned address of a dial that has both: one new key, filed once', async () => {
      const { prov, keys, held } = rig()
      await prov.handle({ ...provision, host: '10.0.0.7' })
      const wifiKey = keys.get(MAC)
      expect(keys.record(MAC)?.hostPinned).toBe(true)
      await prov.handle(phoneSet)
      expect(keys.get(MAC)?.equals(wifiKey!)).toBe(false)
      expect(keys.record(MAC)?.hostPinned).toBe(true)
      expect(held).toMatchObject({ ssid: SSID, ble: true, mode: 2 })
    })

    it('puts no key in the log or the answer', async () => {
      const { prov, keys, lines } = rig()
      const answer = await prov.handle(phoneSet)
      const key = keys.get(MAC)!
      const text = JSON.stringify(answer) + lines.join('\n')
      for (const secret of [key.toString('base64'), key.toString('hex'), createHash('sha256').update(key).digest('hex')]) expect(text).not.toContain(secret)
      expect(lines.some((l) => /phone/.test(l))).toBe(true)
    })

    it('the dial it files for can complete a TLS-PSK handshake at NetDials.acceptStream (the tunnel door)', async () => {
      const { prov, keys } = rig()
      await prov.handle(phoneSet)
      const dir = mkdtempSync(join(tmpdir(), 'phone-door-'))
      cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
      const { duplexPair } = await import('./__fixtures__/duplexPair.js')
      const conns: string[] = []
      const dials = new NetDials({ keys, port: 0, enabled: false, log: () => {}, onDial: (c) => { conns.push(`${c.id}:${c.transport}`); c.socket.on('error', () => {}) } })
      dials.start()
      cleanup.push(() => dials.stop())
      const [near, far] = duplexPair()
      dials.acceptStream(near, { peer: 'tunnel', transport: 'tunnel' })
      const client = await pskConnect({ port: 0, identity: MAC, psk: keys.get(MAC)!, socket: far })
      client.on('error', () => {})
      cleanup.push(() => client.destroy())
      await vi.waitFor(() => expect(conns).toEqual([`${MAC}:tunnel`]))
    })
  })

  describe('phone_mode', () => {
    it('on sends mode 2 and off sends mode 0, to a dial that holds phone settings', async () => {
      const { prov, calls, held } = rig()
      await prov.handle({ ...phoneSet, mode: 0 })
      calls.length = 0
      expect(await prov.handle({ op: 'phone_mode', mode: 2 })).toMatchObject({ ok: true, dial: MAC, mode: 2 })
      expect(held.mode).toBe(2)
      expect(await prov.handle({ op: 'phone_mode', mode: 0 })).toMatchObject({ ok: true, dial: MAC, mode: 0 })
      expect(held.mode).toBe(0)
    })

    it('off does not turn off a dial that is in Wi-Fi mode: that is `harness dial wifi off`', async () => {
      const { prov, calls, held } = rig()
      await prov.handle(provision)
      calls.length = 0
      const answer = await prov.handle({ op: 'phone_mode', mode: 0 })
      expect(answer).toMatchObject({ ok: false, kind: 'bad_request' })
      expect(String(answer.message)).toContain('harness dial wifi off')
      expect(held.mode).toBe(1)
      expect(lastOps(calls)).toEqual(['get'])
    })

    it('on passes the dial\'s refusal through when it holds no phone settings', async () => {
      const { prov } = rig()
      expect(await prov.handle({ op: 'phone_mode', mode: 2 })).toMatchObject({ ok: false, kind: 'device', err: 'not_provisioned' })
    })

    it('refuses a mode that is not 0 or 2 and sends nothing', async () => {
      const { prov, calls } = rig()
      expect(await prov.handle({ op: 'phone_mode', mode: 1 })).toMatchObject({ ok: false, kind: 'bad_request', field: 'mode' })
      expect(await prov.handle({ op: 'phone_mode' })).toMatchObject({ ok: false, kind: 'bad_request', field: 'mode' })
      expect(calls).toEqual([])
    })
  })

  describe('phone_forget', () => {
    it('clears a dial that holds phone settings only, and forgets its key here', async () => {
      const { prov, keys, calls, held } = rig()
      await prov.handle(phoneSet)
      calls.length = 0
      const answer = await prov.handle({ op: 'phone_forget' })
      expect(answer).toMatchObject({ ok: true, dial: MAC, cleared: true, removed: true })
      expect(lastOps(calls)).toEqual(['get', 'clear'])
      expect(held).toEqual({})
      expect(keys.has(MAC)).toBe(false)
    })

    it('refuses when the dial also holds Wi-Fi settings: they share the key, and the clear would take both', async () => {
      const { prov, keys, calls, held } = rig()
      await prov.handle(provision)
      await prov.handle(phoneSet)
      calls.length = 0
      const answer = await prov.handle({ op: 'phone_forget' })
      expect(answer).toMatchObject({ ok: false, kind: 'bad_request' })
      expect(String(answer.message)).toContain('harness dial wifi forget')
      expect(lastOps(calls)).toEqual(['get'])
      expect(held).toMatchObject({ ssid: SSID, ble: true })
      expect(keys.has(MAC)).toBe(true)
    })
  })

  describe('status', () => {
    it('carries ble and the phone mode of the dial on the cable', async () => {
      const { prov } = rig()
      await prov.handle(phoneSet)
      const answer = await prov.handle({ op: 'status' })
      expect(answer).toMatchObject({ ok: true, usb: { dial: MAC, keyed: true, saved: false, ble: true, mode: 2 } })
    })
  })
})
