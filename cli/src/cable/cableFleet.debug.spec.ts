// Which dial a debug request goes to (CableFleet.debug): the one asked for by serial or by mac, otherwise the
// first one that is connected. The sessions are stand-ins; what is under test is the choosing.
import { afterEach, describe, expect, it, vi } from 'vitest'

import { CableFleet } from './cableFleet.js'
import type { CableHost, CableSession } from './cableSession.js'
import type { DialLog } from './dialLog.js'
import type { DialPort } from './serial.js'

const MACS = new Map([['SERIAL-A', 'aa:aa:aa:aa:aa:01'], ['SERIAL-B', 'bb:bb:bb:bb:bb:02']])
const PICTURE = { ok: true as const, width: 2, height: 1, pixels: Buffer.from([0xf8, 0x00, 0x07, 0xe0]) }

const open: CableFleet[] = []
afterEach(async () => { await Promise.all(open.splice(0).map((fleet) => fleet.stop())) })

function rig(opts: { connected?: Record<string, boolean> } = {}) {
  const made = new Map<string, { debugShot: ReturnType<typeof vi.fn>; debugTouch: ReturnType<typeof vi.fn> }>()
  class Fake {
    isConnected: boolean
    debugShot: ReturnType<typeof vi.fn>
    debugTouch: ReturnType<typeof vi.fn>
    private readonly serial: string
    constructor(private readonly host: CableHost, _log: DialLog, _open: unknown) {
      // The fleet builds a session per port in discovery order; the log folder name carries the id.
      this.serial = [...MACS.keys()][made.size]
      this.isConnected = opts.connected?.[this.serial] ?? true
      this.debugShot = vi.fn(async () => PICTURE)
      this.debugTouch = vi.fn(async () => ({ ok: true as const }))
      made.set(this.serial, this)
    }
    start() {
      this.host.onDialAttached?.()
      this.host.onDialStatus?.({ attached: true, mac: MACS.get(this.serial) })
    }
    async stop() {}
  }
  const ports: DialPort[] = [...MACS.keys()].map((serialNumber, i) => ({ path: `COM${i + 3}`, vendorId: 0x303a, productId: 0x1001, serialNumber }))
  const host = { log: () => {}, onDialStatus: () => {}, onDialAttached: () => {}, onDialGone: () => {} } as unknown as CableHost
  const fleet = new CableFleet(Fake as unknown as typeof CableSession, host, 'unused-logs', class { constructor(public dir: string) {} } as unknown as typeof DialLog,
    { discover: async () => ports, canUseUsb: () => true, inUse: async () => false, intervalMs: 1_000_000 })
  const scan = () => (fleet as unknown as { scan(): Promise<void> }).scan()
  // Started so scans run; its own timer is a long way off, and the tests scan by hand.
  fleet.start()
  open.push(fleet)
  return { fleet, made, scan }
}

describe('which dial a debug request goes to', () => {
  it('says there is no dial when none is on the desk', async () => {
    const { fleet } = rig()
    expect(await fleet.debug(undefined, { cmd: 'shot' })).toMatchObject({ ok: false, kind: 'no_dial' })
  })

  it('takes the first connected dial by default, and passes the request on', async () => {
    const { fleet, made, scan } = rig({ connected: { 'SERIAL-A': false } })
    await scan()
    expect(await fleet.debug(undefined, { cmd: 'shot' })).toEqual(PICTURE)
    expect(made.get('SERIAL-A')!.debugShot).not.toHaveBeenCalled()
    expect(made.get('SERIAL-B')!.debugShot).toHaveBeenCalledTimes(1)
    expect(await fleet.debug(undefined, { cmd: 'tap', x: 233, y: 233 })).toEqual({ ok: true })
    expect(made.get('SERIAL-B')!.debugTouch).toHaveBeenCalledWith({ cmd: 'tap', x: 233, y: 233 })
  })

  it('takes the dial named by its serial or its mac, in any case', async () => {
    const { fleet, made, scan } = rig()
    await scan()
    await fleet.debug('serial-b', { cmd: 'shot' })
    await fleet.debug('AA:AA:AA:AA:AA:01', { cmd: 'swipe', x1: 1, y1: 2, x2: 3, y2: 4, ms: 100 })
    expect(made.get('SERIAL-B')!.debugShot).toHaveBeenCalledTimes(1)
    expect(made.get('SERIAL-A')!.debugShot).not.toHaveBeenCalled()
    expect(made.get('SERIAL-A')!.debugTouch).toHaveBeenCalledWith({ cmd: 'swipe', x1: 1, y1: 2, x2: 3, y2: 4, ms: 100 })
  })

  it('says so for a dial it does not know or one that is not connected, and never falls back to another', async () => {
    const { fleet, made, scan } = rig({ connected: { 'SERIAL-A': false } })
    await scan()
    expect(await fleet.debug('nobody', { cmd: 'shot' })).toMatchObject({ ok: false, kind: 'no_dial', message: expect.stringContaining('nobody') })
    expect(await fleet.debug('SERIAL-A', { cmd: 'shot' })).toMatchObject({ ok: false, kind: 'no_dial', message: expect.stringContaining('not connected') })
    expect(made.get('SERIAL-A')!.debugShot).not.toHaveBeenCalled()
    expect(made.get('SERIAL-B')!.debugShot).not.toHaveBeenCalled()
  })

  it('turns a session that throws into an answer', async () => {
    const { fleet, made, scan } = rig()
    await scan()
    made.get('SERIAL-A')!.debugShot.mockRejectedValue(new Error('port exploded'))
    expect(await fleet.debug(undefined, { cmd: 'shot' })).toMatchObject({ ok: false, kind: 'gone', message: expect.stringContaining('port exploded') })
  })
})
