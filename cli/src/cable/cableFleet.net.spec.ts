// The fleet's second source: dials that come in over Wi-Fi (netDials.ts), beside the ones on USB.
// Real TLS-PSK dials on loopback, real sessions, the same host stub the USB fleet spec uses.
import { randomBytes } from 'node:crypto'
import { mkdtempSync, readFileSync, readdirSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { duplexPair } from './__fixtures__/duplexPair.js'
import { FakeNetDial } from './__fixtures__/fakeNetDial.js'
import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableFleet, type CableFleetOptions } from './cableFleet.js'
import { CableSession, type CableHost, type CablePort } from './cableSession.js'
import { DialLog } from './dialLog.js'
import { DIAL_KEY_FILE, DialKeys } from './dialKeys.js'
import { NetDials } from './netDials.js'
import type { DialPort } from './serial.js'

const A = 'B8:1F:3F:CA:E5:58'
const B = 'B8:1F:3F:CA:E5:59'

const cleanup: Array<() => unknown> = []
afterEach(async () => {
  for (const run of cleanup.splice(0).reverse()) await run()
})

/** A USB port with the fleet spec's Peer behaviour, for the arbitration test. */
class UsbPeer implements CablePort {
  readonly transport = 'usb' as const
  isOpen = true
  sent: Record<string, unknown>[] = []
  private decoder = new CableDecoder()
  constructor(readonly path: string, private onData: (chunk: Buffer) => void, private onClosed: (why: string) => void) {}
  async write(bytes: Uint8Array) {
    this.decoder.feed(bytes, (frame) => {
      if (frame.type === CableType.Json) this.sent.push(JSON.parse(Buffer.from(frame.payload).toString()))
    })
  }
  async close(why = 'disconnected') {
    if (this.isOpen) {
      this.isOpen = false
      this.onClosed(why)
    }
  }
  say(message: Record<string, unknown>) {
    this.onData(Buffer.from(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(message)))))
  }
}

function setup(extra: Partial<CableFleetOptions> = {}, hostOver: Partial<CableHost> = {}, Session: ConstructorParameters<typeof CableFleet>[0] = CableSession) {
  const host: CableHost = {
    localMachine: () => ({ id: 'local', name: 'Fixture' }),
    listMachines: async () => ({ machines: [], source: 'backend' }),
    selectedMachine: () => 'local',
    selectMachine: async () => ({ ok: true }),
    listSwarms: () => ({ selected: 'tab', swarms: [], tiles: [] }),
    listUnread: () => [],
    selectSwarm: vi.fn(),
    appName: () => 'harness',
    voiceLang: () => 'en',
    listAgents: async () => [{ id: 'a', name: 'A', engine: 'codex' }],
    agentTotal: () => 1,
    activeSwarm: () => 'tab',
    describe: () => ({ name: 'A', engine: 'codex', machine: 'local' }),
    sendTurn: vi.fn(),
    stopTurn: vi.fn(),
    scrolled: vi.fn(),
    answer: vi.fn(),
    focus: vi.fn(),
    openAgent: vi.fn(),
    forkAgent: async () => ({ ok: true, agentId: 'fork' }),
    updateAgent: vi.fn(),
    listModels: async () => [],
    recentSummaries: async () => [],
    transcribe: vi.fn(async (pcm: Uint8Array) => `audio-${pcm[0]}`),
    route: async () => ({ agentId: 'a', confidence: 1, reason: 'fixture' }),
    log: vi.fn(),
    onDialAttached: vi.fn(),
    onDialGone: vi.fn(),
    onDialStatus: vi.fn(),
    ...hostOver,
  }
  const dir = mkdtempSync(join(tmpdir(), 'fleet-net-'))
  cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
  const logs = join(dir, 'logs')
  let present: DialPort[] = []
  const usb: UsbPeer[] = []
  const fleet = new CableFleet(Session, host, logs, DialLog, {
    canUseUsb: () => true,
    inUse: async () => false,
    discover: async () => present,
    intervalMs: 60_000,
    open: async (path, onData, onClosed) => {
      const peer = new UsbPeer(path, onData, onClosed)
      usb.push(peer)
      return peer
    },
    ...extra,
  })
  const keys = new DialKeys(join(dir, DIAL_KEY_FILE))
  const netLog: string[] = []
  const net = new NetDials({ keys, port: 0, bind: '127.0.0.1', log: (line) => netLog.push(line), onDial: (conn) => fleet.adoptNet(conn) })
  net.start()
  cleanup.push(async () => {
    await net.stop()
    await fleet.stop()
  })
  const dial = async (id: string, key: Buffer) => {
    const d = await FakeNetDial.connect({ port: net.address()!.port, id, psk: key })
    cleanup.push(() => d.close())
    return d
  }
  return { host, fleet, keys, net, netLog, logs, usb, dial, setUsb: (p: DialPort[]) => (present = p) }
}

const SETTINGS = {
  brightness: 60, character: 0, face: 466, muted: false, quiet: false, straightTitle: false,
  focusFace: false, scrollReversed: false, round: true, voiceLang: 'vi',
}

async function provisioned(extra: Partial<CableFleetOptions> = {}, hostOver: Partial<CableHost> = {}) {
  const s = setup(extra, hostOver)
  const keyA = randomBytes(32)
  const keyB = randomBytes(32)
  s.keys.set(A, keyA)
  s.keys.set(B, keyB)
  await s.net.settled()
  return { ...s, keyA, keyB }
}

describe('Wi-Fi dials in the fleet', () => {
  it('a dial with the right key is greeted, attached and shown as a Wi-Fi device with its address', async () => {
    const s = await provisioned()
    const dial = await s.dial(A, s.keyA)
    dial.hello()
    await dial.next('welcome')
    await vi.waitFor(() => expect(s.fleet.isConnected).toBe(true))
    expect(s.host.onDialAttached).toHaveBeenCalledTimes(1)
    expect(s.fleet.devices()).toHaveLength(1)
    expect(s.fleet.devices()[0]).toMatchObject({ id: A, mac: A, attached: true, transport: 'wifi', lan: '127.0.0.1' })
    // it is told the desk, like a dial on USB
    await vi.waitFor(() => expect(dial.received.some((m) => m.t === 'agents.end')).toBe(true))
    await s.fleet.turnStarted('a', 'Working')
    await vi.waitFor(() => expect(dial.received).toContainEqual({ t: 'turn.started', agentId: 'a', text: 'Working' }))
    // and its log is in the per-dial folder, with the link named
    await vi.waitFor(() => expect(readdirSync(s.logs)).toEqual(['usb-B8-1F-3F-CA-E5-58']))
    expect(s.host.log).toHaveBeenCalledWith(expect.stringMatching(/open on wifi:127\.0\.0\.1:\d+ \[wifi B8:1F:3F:CA:E5:58\]/))
  })

  it('writes the key nowhere: not the daemon log, not the listener log, not the dial log file', async () => {
    const s = await provisioned()
    const dial = await s.dial(A, s.keyA)
    dial.hello()
    await dial.next('agents.end')
    await s.fleet.turnStarted('a', 'Working')
    await dial.next('turn.started')
    dial.close()
    await vi.waitFor(() => expect(s.fleet.isConnected).toBe(false))
    await s.fleet.stop()
    const files = readdirSync(s.logs, { recursive: true, withFileTypes: true }).filter((f) => f.isFile())
    expect(files.length).toBeGreaterThan(0)
    const text = [
      ...vi.mocked(s.host.log).mock.calls.map((c) => String(c[0])),
      ...s.netLog,
      ...files.map((f) => readFileSync(join(f.parentPath, f.name), 'utf8')),
    ].join(' | ')
    expect(text.length).toBeGreaterThan(100)
    for (const key of [s.keyA, s.keyB]) {
      expect(text).not.toContain(key.toString('hex'))
      expect(text).not.toContain(key.toString('base64'))
    }
  })

  it('a dial that reconnects is picked up again by the same entry', async () => {
    const s = await provisioned()
    const first = await s.dial(A, s.keyA)
    first.say({ t: 'hello', product: 'harness', mac: A, fw: 'fixture', proto: 3, settings: SETTINGS })
    await first.next('welcome')
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.attached).toBe(true))
    first.close()
    // gone, but kept on the desk with what it last held, as an unplugged USB device is
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.attached).toBe(false))
    expect(s.fleet.devices()[0]).toMatchObject({ id: A, attached: false, settings: SETTINGS })
    expect(s.host.onDialGone).toHaveBeenCalledTimes(1)
    const second = await s.dial(A, s.keyA)
    second.hello()
    await second.next('welcome')
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.attached).toBe(true), { timeout: 6000 })
    expect(s.fleet.devices()).toHaveLength(1)
  })

  it('two dials at once keep separate voice buffers', async () => {
    const s = await provisioned()
    const [a, b] = [await s.dial(A, s.keyA), await s.dial(B, s.keyB)]
    a.hello()
    b.hello()
    await Promise.all([a.next('agents.end'), b.next('agents.end')])
    for (const d of [a, b]) d.say({ t: 'voice.begin', uploadId: 'same', agentId: 'a' })
    a.socket.write(encodeCableFrame(CableType.Pcm, Buffer.alloc(3200, 17)))
    b.socket.write(encodeCableFrame(CableType.Pcm, Buffer.alloc(3200, 34)))
    for (const d of [a, b]) d.say({ t: 'voice.end' })
    await vi.waitFor(() => expect(s.host.sendTurn).toHaveBeenCalledTimes(2))
    const audio = vi.mocked(s.host.transcribe).mock.calls.map((c) => c[0])
    expect(audio.map((x) => [x.length, x[0]]).sort()).toEqual([[3200, 17], [3200, 34]])
    expect(a.received.some((m) => m.t === 'voice.transcript' && m.text === 'audio-17')).toBe(true)
    expect(b.received.some((m) => m.t === 'voice.transcript' && m.text === 'audio-34')).toBe(true)
    expect(s.fleet.devices().map((d) => d.id)).toEqual([A, B])
  })

  it('changes the settings of one Wi-Fi dial, addressed by its id', async () => {
    const s = await provisioned()
    const [a, b] = [await s.dial(A, s.keyA), await s.dial(B, s.keyB)]
    for (const d of [a, b]) d.say({ t: 'hello', product: 'harness', mac: d.options.id, fw: 'fixture', proto: 3, settings: SETTINGS })
    await vi.waitFor(() => expect(s.fleet.devices().filter((d) => d.settings)).toHaveLength(2))
    expect(await s.fleet.setSettings(B, { brightness: 20 })).toEqual({ ok: true })
    await vi.waitFor(() => expect(b.received).toContainEqual({ t: 'settings.set', brightness: 20 }))
    expect(a.received.some((m) => m.t === 'settings.set')).toBe(false)
  })

  it('never offers firmware over Wi-Fi, to a dial that is behind', async () => {
    const firmwareFor = vi.fn(async () => ({ version: '9.9.9', image: Buffer.alloc(2048, 9), sha256: 'x'.repeat(64) }))
    const s = await provisioned({}, { firmwareFor })
    const dial = await s.dial(A, s.keyA)
    const old = { t: 'hello', product: 'harness', mac: A, fw: '0.0.1', proto: 3, hw: 'harness-pro', board: 'pro', ota: 'v1' }
    dial.say(old)
    await dial.next('agents.end')
    dial.say(old)
    await new Promise((resolve) => setTimeout(resolve, 100))
    expect(firmwareFor).not.toHaveBeenCalled()
    expect(dial.received.some((m) => m.t === 'fw.offer')).toBe(false)
  })

  it('closes a link whose dial says it is another dial', async () => {
    const s = await provisioned()
    const dial = await s.dial(A, s.keyA)
    dial.say({ t: 'hello', product: 'harness', mac: B, fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(dial.closed).toBe(true))
    expect(s.fleet.devices().filter((d) => d.attached)).toHaveLength(0)
    expect(s.host.log).toHaveBeenCalledWith(expect.stringMatching(/says it is .*not B8:1F:3F:CA:E5:58/))
  })

  it('five faults in a minute drop the faulty link alone', async () => {
    const s = setup({ faults: new Set([`dial.${A}`]) })
    const keyA = randomBytes(32)
    const keyB = randomBytes(32)
    s.keys.set(A, keyA)
    s.keys.set(B, keyB)
    await s.net.settled()
    const [a, b] = [await s.dial(A, keyA), await s.dial(B, keyB)]
    b.hello()
    await b.next('agents.end')
    // Every greeting the faulty dial sends fails in its session, as its firmware greets on a cadence.
    for (let i = 0; i < 5; i++) a.hello()
    await vi.waitFor(() => expect(a.closed).toBe(true), { timeout: 6000 })
    expect(s.host.log).toHaveBeenCalledWith(expect.stringMatching(/dropped \(5 faults in a minute: injected fault: dial\.B8:1F:3F:CA:E5:58\).*\[wifi B8:1F:3F:CA:E5:58\]/))
    expect(b.closed).toBe(false)
    expect(s.fleet.devices().find((d) => d.id === B)?.attached).toBe(true)
  })

  it('drops a dial whose key was removed: its link closes and its row leaves the desk', async () => {
    const s = await provisioned()
    const dial = await s.dial(A, s.keyA)
    dial.hello()
    await dial.next('welcome')
    await vi.waitFor(() => expect(s.fleet.devices()).toHaveLength(1))
    s.keys.remove(A)
    s.fleet.forgetNet(A)
    await vi.waitFor(() => expect(dial.closed).toBe(true))
    await vi.waitFor(() => expect(s.fleet.devices()).toHaveLength(0))
  })

  it('stop closes every Wi-Fi link', async () => {
    const s = await provisioned()
    const dial = await s.dial(A, s.keyA)
    dial.hello()
    await dial.next('welcome')
    await s.fleet.stop()
    await vi.waitFor(() => expect(dial.closed).toBe(true))
  })
})

describe('one dial on both links', () => {
  const usbDevice = (serial: string): DialPort => ({ path: `/dev/${serial}`, serialNumber: serial, vendorId: 0x303a, productId: 0x1001 })

  it('USB wins: attaching on the cable closes the Wi-Fi link of the same MAC, and the desk shows one row', async () => {
    const s = await provisioned()
    const wifi = await s.dial(A, s.keyA)
    wifi.hello()
    await wifi.next('agents.end')
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.transport).toBe('wifi'))

    s.setUsb([usbDevice(A)])
    s.fleet.start()
    await vi.waitFor(() => expect(s.usb).toHaveLength(1))
    s.usb[0].say({ t: 'hello', product: 'harness', mac: A.toLowerCase(), fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(wifi.closed).toBe(true))
    expect(s.host.log).toHaveBeenCalledWith(expect.stringMatching(/superseded by USB/))
    await vi.waitFor(() => {
      const rows = s.fleet.devices()
      expect(rows).toHaveLength(1)
      expect(rows[0]).toMatchObject({ attached: true, transport: 'usb' })
    })
  })

  async function usbAttached(s: Awaited<ReturnType<typeof provisioned>>, mac: string) {
    s.setUsb([usbDevice(mac)])
    s.fleet.start()
    await vi.waitFor(() => expect(s.usb).toHaveLength(1))
    s.usb[0].say({ t: 'hello', product: 'harness', mac, fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.attached).toBe(true))
  }

  it('turns a Wi-Fi link away while the same dial is attached on USB, says so once, and leaves the USB session alone', async () => {
    const s = await provisioned()
    await usbAttached(s, A)
    for (let i = 0; i < 3; i++) {
      // refused as soon as the handshake is done: the dial may see the close before it sees the handshake finish
      const wifi = await s.dial(A, s.keyA).catch(() => null)
      if (wifi) await vi.waitFor(() => expect(wifi.closed).toBe(true))
    }
    await vi.waitFor(() => expect(s.net.authenticated).toBe(0))
    expect(vi.mocked(s.host.log).mock.calls.filter((c) => /Wi-Fi link from 127\.0\.0\.1 is refused/.test(String(c[0])))).toHaveLength(1)
    expect(s.usb[0].isOpen).toBe(true)
    expect(s.fleet.devices()).toHaveLength(1)
    expect(s.fleet.devices()[0]).toMatchObject({ attached: true, transport: 'usb' })
  })

  it('a Wi-Fi dial that says it is the dial on USB closes its own link and nothing else', async () => {
    const s = await provisioned()
    await usbAttached(s, A)
    const wifi = await s.dial(B, s.keyB)
    wifi.say({ t: 'hello', product: 'harness', mac: A, fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(wifi.closed).toBe(true))
    expect(s.usb[0].isOpen).toBe(true)
    expect(s.fleet.devices().filter((d) => d.attached)).toHaveLength(1)
    expect(s.fleet.devices()[0]).toMatchObject({ mac: A, transport: 'usb' })
  })

  it('prints at most 17 printable characters of the MAC a dial claims', async () => {
    const s = await provisioned()
    const wifi = await s.dial(A, s.keyA)
    wifi.say({ t: 'hello', product: 'harness', mac: B + String.fromCharCode(27) + '[31m' + 'X'.repeat(300), fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(wifi.closed).toBe(true))
    const line = String(vi.mocked(s.host.log).mock.calls.map((c) => c[0]).find((l) => /says it is/.test(String(l))))
    expect(line).toContain('says it is ' + B + ',')
    expect(line).not.toContain(String.fromCharCode(27))
    expect(line.length).toBeLessThan(300)
    expect(line).not.toContain('XXXX')
  })

  it('a USB attach also drops a Wi-Fi link that is waiting for its session', async () => {
    // A session that never asks for its port: the connection waits in the slot.
    class Idle {
      isConnected = false
      wantsStats = false
      constructor() {}
      start() {}
      async stop() {}
    }
    const s = setup({}, {}, Idle as unknown as ConstructorParameters<typeof CableFleet>[0])
    const key = randomBytes(32)
    s.keys.set(A, key)
    await s.net.settled()
    const wifi = await s.dial(A, key)
    const entries = (s.fleet as unknown as { netEntries: Map<string, { slot: { active: boolean } }> }).netEntries
    await vi.waitFor(() => expect(entries.get(A)?.slot.active).toBe(true))
    ;(s.fleet as unknown as { supersedeNet(mac: string): void }).supersedeNet(A)
    await vi.waitFor(() => expect(wifi.closed).toBe(true))
    expect(entries.get(A)?.slot.active).toBe(false)
  })

  it('a USB dial with a different MAC leaves the Wi-Fi dial alone', async () => {
    const s = await provisioned()
    const wifi = await s.dial(A, s.keyA)
    wifi.hello()
    await wifi.next('agents.end')
    s.setUsb([usbDevice(B)])
    s.fleet.start()
    await vi.waitFor(() => expect(s.usb).toHaveLength(1))
    s.usb[0].say({ t: 'hello', product: 'harness', mac: B, fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(s.fleet.devices().filter((d) => d.attached)).toHaveLength(2))
    expect(wifi.closed).toBe(false)
    expect(s.fleet.devices().map((d) => d.transport).sort()).toEqual(['usb', 'wifi'])
  })

  it('a port clash costs the USB dials nothing', async () => {
    const holder = await import('node:net').then(
      (net) =>
        new Promise<import('node:net').Server>((resolve) => {
          const server = net.createServer()
          server.listen(0, '127.0.0.1', () => resolve(server))
        }),
    )
    cleanup.push(() => holder.close())
    const taken = (holder.address() as import('node:net').AddressInfo).port
    const s = setup()
    const clashing = new NetDials({ keys: s.keys, port: taken, bind: '127.0.0.1', log: (l) => s.netLog.push(l), onDial: (c) => s.fleet.adoptNet(c) })
    cleanup.push(() => clashing.stop())
    s.keys.set(A, randomBytes(32))
    clashing.start()
    await clashing.settled()
    expect(clashing.listening).toBe(false)
    s.setUsb([usbDevice(B)])
    s.fleet.start()
    await vi.waitFor(() => expect(s.usb).toHaveLength(1))
    s.usb[0].say({ t: 'hello', product: 'harness', mac: B, fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.attached).toBe(true))
    expect(s.netLog.filter((l) => /in use/.test(l))).toHaveLength(1)
  })
})

describe('a dial over the tunnel in the fleet', () => {
  const usbDevice = (serial: string): DialPort => ({ path: `/dev/${serial}`, serialNumber: serial, vendorId: 0x303a, productId: 0x1001 })
  /** A dial whose TLS-PSK runs over a stream handed to NetDials.acceptStream, as the relay endpoint will do. */
  async function tunnelDial(s: Awaited<ReturnType<typeof provisioned>>, id: string, key: Buffer): Promise<FakeNetDial> {
    const [near, far] = duplexPair()
    s.net.acceptStream(near, { peer: 'tunnel', transport: 'tunnel' })
    const d = await FakeNetDial.connect({ port: 0, id, psk: key, socket: far })
    cleanup.push(() => d.close())
    return d
  }

  it('is greeted, attached and shown as a tunnel device with no LAN address; no firmware, no pet pack', async () => {
    const firmwareFor = vi.fn(async () => ({ version: '9.9.9', image: Buffer.alloc(2048, 9), sha256: 'x'.repeat(64) }))
    const s = await provisioned({}, { firmwareFor })
    const dial = await tunnelDial(s, A, s.keyA)
    dial.say({ t: 'hello', product: 'harness', mac: A, fw: '0.0.1', proto: 3, hw: 'harness-pro', board: 'pro', ota: 'v1' })
    await dial.next('agents.end')
    await vi.waitFor(() => expect(s.fleet.devices()[0]).toMatchObject({ attached: true, transport: 'tunnel' }))
    expect(s.fleet.devices()[0].lan).toBeUndefined()
    await new Promise((resolve) => setTimeout(resolve, 100))
    expect(firmwareFor).not.toHaveBeenCalled()
    expect(dial.received.some((m) => m.t === 'fw.offer' || m.t === 'pet.offer')).toBe(false)
  })

  it('a USB attach of the same MAC closes the tunnel session (superseded by USB)', async () => {
    const s = await provisioned()
    const dial = await tunnelDial(s, A, s.keyA)
    dial.hello()
    await dial.next('agents.end')
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.transport).toBe('tunnel'))
    s.setUsb([usbDevice(A)])
    s.fleet.start()
    await vi.waitFor(() => expect(s.usb).toHaveLength(1))
    s.usb[0].say({ t: 'hello', product: 'harness', mac: A.toLowerCase(), fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(dial.closed).toBe(true))
    expect(s.host.log).toHaveBeenCalledWith(expect.stringMatching(/superseded by USB/))
    await vi.waitFor(() => {
      const rows = s.fleet.devices()
      expect(rows).toHaveLength(1)
      expect(rows[0]).toMatchObject({ attached: true, transport: 'usb' })
    })
  })

  it('a tunnel dial is turned away while the same dial is on USB, and the USB session stays', async () => {
    const s = await provisioned()
    s.setUsb([usbDevice(A)])
    s.fleet.start()
    await vi.waitFor(() => expect(s.usb).toHaveLength(1))
    s.usb[0].say({ t: 'hello', product: 'harness', mac: A, fw: 'fixture', proto: 3 })
    await vi.waitFor(() => expect(s.fleet.devices()[0]?.attached).toBe(true))
    // The link is destroyed as soon as the handshake is done: the dial sees either a closed link or a handshake cut short.
    const dial = await tunnelDial(s, A, s.keyA).then((d) => d, () => null)
    if (dial) await vi.waitFor(() => expect(dial.closed).toBe(true))
    expect(s.usb[0].isOpen).toBe(true)
    expect(s.fleet.devices()[0]).toMatchObject({ attached: true, transport: 'usb' })
  })
})
