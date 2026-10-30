// What the security review of the provisioning found (W9): a key that could not be removed was reported as removed, a rollback or a timeout
// left the dial and the keystore apart without saying so, a Wi-Fi dial could not be forgotten, and an address given by hand was
// overwritten on attach.
import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it } from 'vitest'

import { CableFleet, type FleetNetResult } from './cableFleet.js'
import type { CableHost, CableSession } from './cableSession.js'
import { DIAL_KEY_FILE, DialKeys } from './dialKeys.js'
import type { DialLog } from './dialLog.js'
import type { NetRequest } from './dialNet.js'
import { NetProvisioner, STATE_UNKNOWN_NOTE } from './netProvision.js'

const MAC = 'B8:1F:3F:CA:E5:58'
const LAN = '192.168.1.20'
const PASS = 'correct horse battery'
const KEY = Buffer.alloc(32, 5)

const cleanup: Array<() => unknown> = []
afterEach(() => { for (const run of cleanup.splice(0).reverse()) run() })

/** The real keystore, except that the named methods throw: a disk that will not take a write. */
const refusing = (real: DialKeys, ...methods: string[]): DialKeys => new Proxy(real, {
  get: (target, key) => {
    if (methods.includes(String(key))) return () => { throw new Error('disk full') }
    const value = target[key as keyof DialKeys]
    return typeof value === 'function' ? (value as (...args: unknown[]) => unknown).bind(target) : value
  },
})

function rig(net: (dial: string | undefined, request: NetRequest) => Promise<FleetNetResult>, wrap: (keys: DialKeys) => DialKeys = (keys) => keys) {
  const dir = mkdtempSync(join(tmpdir(), 'net-review-'))
  cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
  const real = new DialKeys(join(dir, DIAL_KEY_FILE))
  cleanup.push(() => real.close())
  const calls: NetRequest[] = []
  const lines: string[] = []
  const prov = new NetProvisioner({
    net: async (dial, request) => { calls.push(request); return net(dial, request) },
    keys: wrap(real), lan: async () => LAN, port: 18480, log: (line) => lines.push(line),
  })
  return { prov, keys: real, calls, lines }
}

const ok = (over: Record<string, unknown> = {}): FleetNetResult => ({ ok: true, saved: true, dial: MAC, ...over } as FleetNetResult)
const timeout: FleetNetResult = { ok: false, kind: 'timeout', message: 'the dial did not answer in 5 s' }
const provision = { op: 'provision', ssid: 'home-net', pass: PASS }

describe('forgetting a dial that is on Wi-Fi', () => {
  it('removes the key when the real fleet says the named dial is on Wi-Fi (transport), and touches the dial not at all', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'net-review-'))
    cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
    const keys = new DialKeys(join(dir, DIAL_KEY_FILE))
    cleanup.push(() => keys.close())
    keys.set(MAC, KEY)
    const sessionCalls: unknown[] = []
    const fleet = new CableFleet(class {} as unknown as typeof CableSession, { log: () => {} } as unknown as CableHost, 'unused', class {} as unknown as typeof DialLog, {})
    // A dial that came in over Wi-Fi: the map holds it the way the listener's hand-over leaves it.
    ;(fleet as unknown as { netEntries: Map<string, unknown> }).netEntries.set(MAC, { session: { net: async (r: unknown) => { sessionCalls.push(r); return { ok: true, saved: true } } }, attached: true, status: { mac: MAC.toLowerCase() } })
    const prov = new NetProvisioner({ net: (dial, request) => fleet.net(dial, request), keys, lan: async () => LAN, port: 18480, log: () => {} })
    expect(await fleet.net(MAC, { op: 'get' })).toMatchObject({ ok: false, kind: 'transport' })
    expect(await prov.handle({ op: 'forget', dial: MAC.toLowerCase() })).toEqual({ ok: true, dial: MAC, cleared: false, removed: true })
    expect(keys.has(MAC)).toBe(false)
    expect(sessionCalls).toEqual([])
    ;(fleet as unknown as { netEntries: Map<string, unknown> }).netEntries.clear()
  })

  it('still needs a named dial and a held key: nothing is removed on a guess', async () => {
    const { prov, keys } = rig(async () => ({ ok: false, kind: 'transport', message: 'on Wi-Fi' }))
    keys.set(MAC, KEY)
    expect(await prov.handle({ op: 'forget' })).toMatchObject({ ok: false, kind: 'transport' })
    expect(await prov.handle({ op: 'forget', dial: 'B8:1F:3F:CA:E5:59' })).toMatchObject({ ok: false, kind: 'transport' })
    expect(keys.has(MAC)).toBe(true)
  })
})

describe('a key that cannot be removed', () => {
  it('is not reported as forgotten when the dial was cleared: kind keystore, and the key is still held', async () => {
    const { prov, keys } = rig(async () => ok({ saved: false }), (real) => refusing(real, 'remove'))
    keys.set(MAC, KEY)
    const answer = await prov.handle({ op: 'forget' })
    expect(answer).toMatchObject({ ok: false, kind: 'keystore' })
    expect(answer.removed).toBeUndefined()
    expect(keys.has(MAC)).toBe(true)
  })

  it('is not reported as forgotten for a dial that is away either', async () => {
    const { prov, keys } = rig(async () => ({ ok: false, kind: 'no_dial', message: 'none' }), (real) => refusing(real, 'remove'))
    keys.set(MAC, KEY)
    expect(await prov.handle({ op: 'forget', dial: MAC })).toMatchObject({ ok: false, kind: 'keystore' })
    expect(keys.has(MAC)).toBe(true)
  })

  it('a key that was never held is still not a failure', async () => {
    const { prov } = rig(async () => ok({ saved: false }))
    expect(await prov.handle({ op: 'forget' })).toEqual({ ok: true, dial: MAC, cleared: true, removed: false })
  })
})

describe('when the dial and the keystore may disagree, the answer says so', () => {
  it('a net.set that times out: it may have been saved', async () => {
    const { prov, keys } = rig(async () => timeout)
    const answer = await prov.handle(provision)
    expect(answer).toMatchObject({ ok: false, kind: 'timeout' })
    expect(answer.message).toContain(STATE_UNKNOWN_NOTE)
    expect(keys.ids()).toEqual([])
  })

  it('a net.key that times out: the dial may hold the new key', async () => {
    const { prov, keys } = rig(async () => timeout)
    keys.set(MAC, KEY)
    const answer = await prov.handle({ op: 'rotate', dial: MAC })
    expect(answer.message).toContain(STATE_UNKNOWN_NOTE)
    expect(keys.get(MAC)).toEqual(KEY)
  })

  it('other failures that left the dial untouched do not say it (a refusal, nobody there)', async () => {
    for (const result of [{ ok: false, kind: 'device', err: 'pass', message: 'the dial refused: pass' }, { ok: false, kind: 'no_dial', message: 'none' }] as FleetNetResult[]) {
      const { prov } = rig(async () => result)
      expect((await prov.handle(provision)).message).not.toContain(STATE_UNKNOWN_NOTE)
    }
  })

  it('a first set whose key cannot be stored, and whose clear then fails', async () => {
    const { prov, calls } = rig(async (_dial, request) => request.op === 'set' ? ok() : timeout, (real) => refusing(real, 'set'))
    const answer = await prov.handle(provision)
    expect(answer).toMatchObject({ ok: false, kind: 'keystore' })
    expect(answer.message).toContain('could not be cleared again')
    expect(answer.message).toContain(STATE_UNKNOWN_NOTE)
    expect(calls.map((c) => c.op)).toEqual(['set', 'clear'])
  })

  it('the same, when the dial did clear: no warning', async () => {
    const { prov } = rig(async () => ok(), (real) => refusing(real, 'set'))
    const answer = await prov.handle(provision)
    expect(answer.message).toContain('was cleared again')
    expect(answer.message).not.toContain(STATE_UNKNOWN_NOTE)
  })

  it('a dial that did not say its MAC and could not be cleared', async () => {
    const { prov } = rig(async (_dial, request) => request.op === 'set' ? { ok: true, saved: true } : timeout)
    const answer = await prov.handle(provision)
    expect(answer).toMatchObject({ ok: false, kind: 'device' })
    expect(answer.message).toContain(STATE_UNKNOWN_NOTE)
    const cleared = rig(async (_dial, request) => ({ ok: true, saved: request.op === 'set' }))
    expect((await cleared.prov.handle(provision)).message).not.toContain(STATE_UNKNOWN_NOTE)
  })

  it('a rotate whose new key cannot be stored and whose old key cannot be sent back', async () => {
    let sent = 0
    const { prov, keys } = rig(async () => (++sent === 1 ? ok() : timeout), (real) => refusing(real, 'set'))
    keys.set(MAC, KEY)
    const answer = await prov.handle({ op: 'rotate', dial: MAC })
    expect(answer).toMatchObject({ ok: false, kind: 'keystore' })
    expect(answer.message).toContain(STATE_UNKNOWN_NOTE)
  })

  it('a rotate whose old key was sent back says nothing of it', async () => {
    const { prov, keys, calls } = rig(async () => ok(), (real) => refusing(real, 'set'))
    keys.set(MAC, KEY)
    const answer = await prov.handle({ op: 'rotate', dial: MAC })
    expect(answer.message).not.toContain(STATE_UNKNOWN_NOTE)
    expect((calls[1] as { key: Buffer }).key).toEqual(KEY)
  })

  it('a rotate with no old key to send back says so', async () => {
    const { prov, calls } = rig(async () => ok(), (real) => refusing(real, 'set'))
    const answer = await prov.handle({ op: 'rotate', dial: MAC })
    expect(answer.message).toContain(STATE_UNKNOWN_NOTE)
    expect(calls).toHaveLength(1)
  })
})

describe('an address given by hand is kept', () => {
  const SAVED = ok({ ssid: 'home-net', host: '192.168.1.99', port: 18480, mode: 1 })

  it('--host pins it in the keystore record; no --host leaves it unpinned and clears an older pin', async () => {
    const { prov, keys } = rig(async () => ok())
    await prov.handle({ ...provision, host: '10.0.0.7' })
    expect(keys.record(MAC)?.hostPinned).toBe(true)
    await prov.handle(provision)
    expect(keys.record(MAC)?.hostPinned).toBeUndefined()
  })

  it('a rotate keeps the pin', async () => {
    const { prov, keys } = rig(async () => ok())
    await prov.handle({ ...provision, host: '10.0.0.7' })
    await prov.handle({ op: 'rotate' })
    expect(keys.record(MAC)?.hostPinned).toBe(true)
    const unpinned = rig(async () => ok())
    await unpinned.prov.handle(provision)
    await unpinned.prov.handle({ op: 'rotate' })
    expect(unpinned.keys.record(MAC)?.hostPinned).toBeUndefined()
  })

  it('attach does not send a new address to a pinned dial, and does to an unpinned one', async () => {
    const asked: string[] = []
    const dial = { mac: MAC, net: async (request: NetRequest): Promise<FleetNetResult> => { asked.push(request.op); return request.op === 'get' ? SAVED : ok() } }
    const pinned = rig(async () => ok())
    await pinned.prov.handle({ ...provision, host: '10.0.0.7' })
    await pinned.prov.resyncHost(dial)
    expect(asked).toEqual(['get'])
    asked.length = 0
    const free = rig(async () => ok())
    await free.prov.handle(provision)
    await free.prov.resyncHost(dial)
    expect(asked).toEqual(['get', 'host'])
  })

  it('status shows the pin, and the pin survives a restart of the keystore', async () => {
    const { prov, keys } = rig(async () => ok())
    await prov.handle({ ...provision, host: '10.0.0.7' })
    const answer = await prov.handle({ op: 'status' })
    expect(answer.keyed).toEqual([expect.objectContaining({ dial: MAC, hostPinned: true })])
    keys.touch(MAC, '192.168.1.40')   // a dial connecting rewrites its record; the pin stays
    expect(keys.record(MAC)).toMatchObject({ hostPinned: true, lastAddr: '192.168.1.40' })
    const again = new DialKeys(keys.file)
    cleanup.push(() => again.close())
    expect(again.record(MAC)?.hostPinned).toBe(true)
  })
})
