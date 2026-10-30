// Which dial a Wi-Fi settings request goes to (CableFleet.net), and what the fleet hands its owner when a dial on the
// cable says it keeps Wi-Fi settings (CableFleetOptions.onNetDial). The sessions are stand-ins; what is under test is the
// choosing, and that a request never reaches a dial that is not on the cable.
import { afterEach, describe, expect, it, vi } from 'vitest'

import { CableFleet } from './cableFleet.js'
import type { CableHost, CableSession, NetDialHandle } from './cableSession.js'
import type { DialLog } from './dialLog.js'
import type { NetRequest, NetResult } from './dialNet.js'
import type { DialPort } from './serial.js'

const MACS = new Map([['SERIAL-A', 'aa:aa:aa:aa:aa:01'], ['SERIAL-B', 'bb:bb:bb:bb:bb:02']])
const GET: NetRequest = { op: 'get' }
const STATE: NetResult = { ok: true, saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 }

const open: CableFleet[] = []
afterEach(async () => { await Promise.all(open.splice(0).map((fleet) => fleet.stop())) })

function rig(opts: { connected?: Record<string, boolean>; onNetDial?: (dial: NetDialHandle) => void | Promise<void> } = {}) {
  const made = new Map<string, { net: ReturnType<typeof vi.fn<(request: NetRequest) => Promise<NetResult>>> }>()
  class Fake {
    isConnected: boolean
    net: ReturnType<typeof vi.fn<(request: NetRequest) => Promise<NetResult>>>
    private readonly serial: string
    constructor(private readonly host: CableHost, _log: DialLog, _open: unknown) {
      this.serial = [...MACS.keys()][made.size]
      this.isConnected = opts.connected?.[this.serial] ?? true
      this.net = vi.fn<(request: NetRequest) => Promise<NetResult>>(async () => STATE)
      made.set(this.serial, this)
    }
    start() {
      this.host.onDialAttached?.()
      this.host.onDialStatus?.({ attached: true, mac: MACS.get(this.serial) })
      void Promise.resolve(this.host.onNetCapable?.({ mac: MACS.get(this.serial)!, net: (request) => this.net(request) })).catch(() => {})
    }
    async stop() {}
  }
  const ports: DialPort[] = [...MACS.keys()].map((serialNumber, i) => ({ path: `COM${i + 3}`, vendorId: 0x303a, productId: 0x1001, serialNumber }))
  const host = { log: () => {}, onDialStatus: () => {}, onDialAttached: () => {}, onDialGone: () => {} } as unknown as CableHost
  const fleet = new CableFleet(Fake as unknown as typeof CableSession, host, 'unused-logs', class { constructor(public dir: string) {} } as unknown as typeof DialLog,
    { discover: async () => ports, canUseUsb: () => true, inUse: async () => false, intervalMs: 1_000_000, onNetDial: opts.onNetDial })
  const scan = () => (fleet as unknown as { scan(): Promise<void> }).scan()
  fleet.start()
  open.push(fleet)
  return { fleet, made, scan }
}

describe('which dial a Wi-Fi settings request goes to', () => {
  it('says there is no dial when none is on the cable', async () => {
    const { fleet } = rig()
    expect(await fleet.net(undefined, GET)).toMatchObject({ ok: false, kind: 'no_dial' })
  })

  it('takes the only connected dial by default, passes the request on, and says which dial answered', async () => {
    const { fleet, made, scan } = rig({ connected: { 'SERIAL-A': false } })
    await scan()
    expect(await fleet.net(undefined, GET)).toEqual({ ...STATE, dial: 'bb:bb:bb:bb:bb:02' })
    expect(made.get('SERIAL-B')!.net).toHaveBeenCalledWith(GET)
    expect(made.get('SERIAL-A')!.net).not.toHaveBeenCalled()
  })

  it('does not choose between two connected dials: setting the wrong one\'s Wi-Fi is worse than asking', async () => {
    const { fleet, made, scan } = rig()
    await scan()
    expect(await fleet.net(undefined, GET)).toMatchObject({ ok: false, kind: 'ambiguous' })
    expect(made.get('SERIAL-A')!.net).not.toHaveBeenCalled()
    expect(made.get('SERIAL-B')!.net).not.toHaveBeenCalled()
  })

  it('takes the dial named by its serial or its mac, in any case', async () => {
    const { fleet, made, scan } = rig()
    await scan()
    await fleet.net('serial-b', GET)
    expect(await fleet.net('AA:AA:AA:AA:AA:01', { op: 'clear' })).toMatchObject({ ok: true, dial: 'aa:aa:aa:aa:aa:01' })
    expect(made.get('SERIAL-B')!.net).toHaveBeenCalledWith(GET)
    expect(made.get('SERIAL-A')!.net).toHaveBeenCalledWith({ op: 'clear' })
  })

  it('says so for a dial it does not know or one that is not connected, and never falls back to another', async () => {
    const { fleet, made, scan } = rig({ connected: { 'SERIAL-A': false } })
    await scan()
    expect(await fleet.net('nobody', GET)).toMatchObject({ ok: false, kind: 'no_dial', message: expect.stringContaining('nobody') })
    expect(await fleet.net('SERIAL-A', GET)).toMatchObject({ ok: false, kind: 'no_dial', message: expect.stringContaining('not connected') })
    expect(made.get('SERIAL-A')!.net).not.toHaveBeenCalled()
    expect(made.get('SERIAL-B')!.net).not.toHaveBeenCalled()
  })

  it('turns a session that throws into an answer', async () => {
    const { fleet, made, scan } = rig({ connected: { 'SERIAL-A': false } })
    await scan()
    made.get('SERIAL-B')!.net.mockRejectedValue(new Error('port exploded'))
    expect(await fleet.net(undefined, GET)).toMatchObject({ ok: false, kind: 'gone', message: expect.stringContaining('port exploded') })
  })
})

describe('a dial that is on Wi-Fi', () => {
  it('is not sent Wi-Fi settings: they go over the cable', async () => {
    const { fleet, made } = rig()
    // adoptNet is the listener's hand-over; here a live Wi-Fi entry is put in the map the way it leaves one.
    ;(fleet as unknown as { netEntries: Map<string, unknown> }).netEntries.set('CC:CC:CC:CC:CC:03', { session: { net: vi.fn() }, attached: true, status: { mac: 'cc:cc:cc:cc:cc:03' } })
    const result = await fleet.net('cc:cc:cc:cc:cc:03', GET)
    expect(result).toMatchObject({ ok: false, kind: 'transport', message: expect.stringContaining('USB cable') })
    expect(made.size).toBe(0)
    // left as it was found: stop() would close a real entry
    ;(fleet as unknown as { netEntries: Map<string, unknown> }).netEntries.clear()
  })
})

describe('handing the owner a dial on the cable that keeps Wi-Fi settings', () => {
  it('calls onNetDial for each such dial, with its mac and a way to ask it', async () => {
    const onNetDial = vi.fn()
    const { scan, made } = rig({ onNetDial })
    await scan()
    expect(onNetDial).toHaveBeenCalledTimes(2)
    expect(onNetDial.mock.calls.map(([dial]) => dial.mac).sort()).toEqual(['aa:aa:aa:aa:aa:01', 'bb:bb:bb:bb:bb:02'])
    expect(await onNetDial.mock.calls[0][0].net(GET)).toEqual(STATE)
    expect(made.size).toBe(2)
  })

  it('lets a session see nothing when the fleet has no owner for it', async () => {
    const { scan } = rig()
    await expect(scan()).resolves.toBeUndefined()
  })

  it('passes the owner\'s rejection back to the session, which logs it', async () => {
    const onNetDial = vi.fn(async () => { throw new Error('owner broke') })
    const { scan } = rig({ onNetDial })
    await scan()
    expect(onNetDial).toHaveBeenCalled()
  })
})
