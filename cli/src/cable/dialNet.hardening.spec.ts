// W10 review, daemon side: the same host and key rules as the firmware, and the two reply shapes the firmware now has (a refusal with no
// `saved` in it, and `net.get` answered "busy" while a write runs).
import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'

import type { FleetNetResult } from './cableFleet.js'
import { DIAL_KEY_FILE, DialKeys } from './dialKeys.js'
import { DialNet, type NetFailure, type NetRequest } from './dialNet.js'
import { NetProvisioner } from './netProvision.js'
import { validHost, validKeyBytes } from '../lib/dialNetRules.js'

const MAC = 'B8:1F:3F:CA:E5:58'
const cleanup: Array<() => unknown> = []
afterEach(() => { vi.useRealTimers(); for (const run of cleanup.splice(0).reverse()) run() })

describe('a host the dial could be sent to', () => {
  it('is a dotted IPv4 that is none of 0.0.0.0, 255.255.255.255, loopback or multicast', () => {
    for (const bad of ['0.0.0.0', '255.255.255.255', '127.0.0.1', '127.255.255.254', '224.0.0.1', '230.1.2.3', '239.255.255.255']) expect(validHost(bad), bad).toBe(false)
    for (const good of ['0.0.0.1', '126.255.255.255', '128.0.0.1', '223.255.255.254', '240.0.0.1', '255.255.255.254', '10.0.0.1', '192.168.1.20', '172.16.0.1']) expect(validHost(good), good).toBe(true)
  })
})

describe('a key', () => {
  it('is 32 bytes and not all zero', () => {
    expect(validKeyBytes(Buffer.alloc(32, 1))).toBe(true)
    const almost = Buffer.alloc(32)
    almost[31] = 1
    expect(validKeyBytes(almost)).toBe(true)
    expect(validKeyBytes(Buffer.alloc(32))).toBe(false)
    expect(validKeyBytes(Buffer.alloc(31, 1))).toBe(false)
    expect(validKeyBytes(Buffer.alloc(33, 1))).toBe(false)
  })

  it('an all-zero key is refused before anything is sent, for net.set and for net.key', async () => {
    const sent: unknown[] = []
    const net = new DialNet({ send: async (m) => { sent.push(m); return true }, log: () => {} })
    const zero = Buffer.alloc(32)
    const set: NetRequest = { op: 'set', ssid: 'home-net', pass: 'correct horse', host: '192.168.1.20', port: 18480, key: zero, mode: 1 }
    expect(await net.request(set)).toMatchObject({ ok: false, kind: 'bad_request' })
    expect(await net.request({ op: 'key', key: zero })).toMatchObject({ ok: false, kind: 'bad_request' })
    expect(sent).toEqual([])
  })
})

describe('the replies the firmware gives while it is busy or refuses off the cable', () => {
  function request(op: NetRequest, reply: Record<string, unknown>) {
    vi.useFakeTimers()
    const sent: Array<Record<string, unknown>> = []
    const net = new DialNet({ send: async (m) => { sent.push(m); return true }, log: () => {}, firstId: 9 })
    const pending = net.request(op)
    return vi.advanceTimersByTimeAsync(0).then(() => {
      net.message({ t: 'net.state', id: sent[0].id, ...reply })
      return pending
    })
  }

  it('net.state without a "saved" is read as "not saved", with the reason', async () => {
    const busy = await request({ op: 'get' }, { ok: false, err: 'busy' }) as NetFailure
    expect(busy).toMatchObject({ ok: false, kind: 'device', err: 'busy', saved: false })
    const off = await request({ op: 'get' }, { ok: false, err: 'transport' }) as NetFailure
    expect(off).toMatchObject({ ok: false, kind: 'device', err: 'transport', saved: false })
  })

  function prov(net: (dial: string | undefined, request: NetRequest) => Promise<FleetNetResult>) {
    const dir = mkdtempSync(join(tmpdir(), 'net-hard-'))
    cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
    const keys = new DialKeys(join(dir, DIAL_KEY_FILE))
    cleanup.push(() => keys.close())
    keys.set(MAC, Buffer.alloc(32, 3))
    const calls: string[] = []
    return { keys, calls, prov: new NetProvisioner({ net: async (d, r) => { calls.push(r.op); return net(d, r) }, keys, lan: async () => '192.168.1.20', port: 18480, log: () => {} }) }
  }
  const busy: FleetNetResult = { ok: false, kind: 'device', err: 'busy', message: 'the dial refused: busy', saved: false }

  it('status shows a dial that is busy as a problem reading it, not as one with nothing set', async () => {
    const { prov: p } = prov(async () => busy)
    const answer = await p.handle({ op: 'status' })
    expect(answer).toMatchObject({ ok: true, usbProblem: { kind: 'device', message: 'the dial refused: busy' } })
    expect(answer.usb).toBeUndefined()
  })

  it('attach sends nothing to a dial that answers busy to net.get', async () => {
    const { prov: p, calls } = prov(async () => busy)
    const asked: string[] = []
    await p.resyncHost({ mac: MAC, net: async (r) => { asked.push(r.op); return busy } })
    expect(asked).toEqual(['get'])
    expect(calls).toEqual([])
  })

  it('a provision whose dial says busy leaves the keystore as it was, and says busy', async () => {
    const { prov: p, keys } = prov(async () => busy)
    const before = keys.get(MAC)
    const answer = await p.handle({ op: 'provision', ssid: 'home-net', pass: 'correct horse' })
    expect(answer).toMatchObject({ ok: false, kind: 'device', err: 'busy' })
    expect(keys.get(MAC)).toEqual(before)
  })
})
