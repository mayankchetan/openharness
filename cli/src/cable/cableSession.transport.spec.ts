// What a session does differently on a port that says which link it is (CablePort.transport, CablePort.peer).
// Wi-Fi never gets a firmware image (docs/specs/cable-wifi-link.md sections 2.2 and 6.1): the OTA path takes plain HTTP and
// an image with no signature, so an image is offered over the cable only.
import { mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it, vi } from 'vitest'

import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableSession, type CableHost, type CablePort, type DialStatus } from './cableSession.js'
import { DialLog } from './dialLog.js'

class Port implements CablePort {
  isOpen = true
  readonly sent: Array<Record<string, unknown>> = []
  private decoder = new CableDecoder()
  constructor(
    readonly path: string,
    private readonly onData: (chunk: Buffer) => void,
    private readonly onClosed: (why: string) => void,
    readonly transport?: CablePort['transport'],
    readonly peer?: string,
  ) {}
  async write(bytes: Uint8Array): Promise<void> {
    this.decoder.feed(Buffer.from(bytes), (frame) => {
      if (frame.type === CableType.Json) this.sent.push(JSON.parse(Buffer.from(frame.payload).toString('utf8')))
    })
  }
  async close(why = 'closed'): Promise<void> {
    if (!this.isOpen) return
    this.isOpen = false
    this.onClosed(why)
  }
  say(msg: Record<string, unknown>): void {
    this.onData(Buffer.from(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(msg), 'utf8'))))
  }
  types(): string[] {
    return this.sent.map((m) => m.t as string)
  }
}

function host(over: Partial<CableHost> = {}): CableHost {
  return {
    localMachine: () => ({ id: 'mac-local', name: 'MacBook Pro' }),
    listMachines: async () => ({ machines: [{ id: 'mac-local', name: 'MacBook Pro', state: 'ready', local: true }], source: 'backend' as const }),
    selectedMachine: () => 'mac-local',
    selectMachine: async () => ({ ok: true as const }),
    listSwarms: () => ({ selected: '', swarms: [], tiles: [] }),
    listUnread: () => [],
    selectSwarm: vi.fn(),
    appName: () => 'harness',
    voiceLang: () => 'en',
    listAgents: async () => [],
    agentTotal: () => 0,
    activeSwarm: () => 't1',
    describe: () => undefined,
    sendTurn: vi.fn(),
    stopTurn: vi.fn(),
    scrolled: vi.fn(),
    answer: vi.fn(),
    focus: vi.fn(),
    openAgent: vi.fn(),
    forkAgent: async (id) => ({ ok: true as const, agentId: `${id}-fork` }),
    updateAgent: vi.fn(),
    listModels: async () => [],
    recentSummaries: async () => [],
    transcribe: async () => '',
    route: async () => ({ agentId: '', confidence: 0, reason: '' }),
    log: () => {},
    ...over,
  }
}

async function connect(h: CableHost, transport?: CablePort['transport'], peer?: string) {
  let port!: Port
  const log = new DialLog(mkdtempSync(join(tmpdir(), 'cable-transport-')))
  const session = new CableSession(h, log, async (onData, onClosed) => {
    port = new Port(transport ? `${transport}:${peer}:5000` : '/dev/loopback', onData, onClosed, transport, peer)
    return port
  })
  session.start()
  await vi.waitFor(() => expect(port).toBeDefined())
  return { session, port }
}

const settle = () => new Promise((resolve) => setTimeout(resolve, 20))
const image = Buffer.alloc(2048, 9)
const firmwareFor = () => vi.fn(async (_version: string, _key: string) => ({ version: '9.9.9', image, sha256: 'x'.repeat(64) }))
const OLD_PRO = { t: 'hello', product: 'harness', fw: '0.0.1', proto: 3, mac: 'aa:bb', hw: 'harness-pro', board: 'pro', ota: 'v1' }

describe('firmware is never offered over Wi-Fi', () => {
  it.each(['wifi', 'ble', 'tunnel'] as const)('a %s port: maybeOfferFirmware is never called and no image is looked up', async (transport) => {
    const lookup = firmwareFor()
    const { session, port } = await connect(host({ firmwareFor: lookup }), transport, '192.168.1.40')
    const offer = vi.spyOn(session as unknown as { maybeOfferFirmware(v: string): Promise<void> }, 'maybeOfferFirmware')
    port.say(OLD_PRO)
    await vi.waitFor(() => expect(port.types()).toContain('agents.end'))
    // a second greeting, as the dial sends every 15 s
    port.say(OLD_PRO)
    await settle()
    expect(offer).not.toHaveBeenCalled()
    expect(lookup).not.toHaveBeenCalled()
    expect(port.types()).not.toContain('fw.offer')
    expect((session as unknown as { transfer: unknown }).transfer).toBeNull()
    await session.stop()
  })

  it('the offer function itself also refuses on such a port, were it called', async () => {
    const lookup = firmwareFor()
    const { session, port } = await connect(host({ firmwareFor: lookup }), 'wifi', '192.168.1.40')
    port.say({ ...OLD_PRO, fw: '' })
    await vi.waitFor(() => expect(port.types()).toContain('agents.end'))
    await (session as unknown as { maybeOfferFirmware(v: string): Promise<void> }).maybeOfferFirmware('0.0.1')
    expect(lookup).not.toHaveBeenCalled()
    expect(port.types()).not.toContain('fw.offer')
    await session.stop()
  })

  it.each([undefined, 'usb'] as const)('control: a port whose transport is %s is still offered the image', async (transport) => {
    const lookup = firmwareFor()
    const { session, port } = await connect(host({ firmwareFor: lookup }), transport, transport ? 'cable' : undefined)
    port.say(OLD_PRO)
    await vi.waitFor(() => expect(lookup).toHaveBeenCalledWith('0.0.1', 'harness-pro'))
    expect(port.types().filter((t) => t === 'fw.offer')).toHaveLength(1)
    await session.stop()
  })
})

describe('the status names the link', () => {
  async function statuses(transport: CablePort['transport'], peer?: string): Promise<DialStatus[]> {
    const seen: DialStatus[] = []
    const { session, port } = await connect(host({ onDialStatus: (s) => void seen.push(s) }), transport, peer)
    port.say({ t: 'hello', product: 'harness', fw: '0.0.1', proto: 3, mac: 'aa:bb' })
    await vi.waitFor(() => expect(port.types()).toContain('agents.end'))
    await session.stop()
    return seen
  }

  it('says wifi and the dial\'s address on a Wi-Fi port', async () => {
    const seen = await statuses('wifi', '192.168.1.40')
    const attached = seen.find((s) => s.attached)!
    expect(attached.transport).toBe('wifi')
    expect(attached.lan).toBe('192.168.1.40')
    expect(attached.mac).toBe('aa:bb')
  })

  it('says usb and no address on a USB port', async () => {
    const attached = (await statuses('usb')).find((s) => s.attached)!
    expect(attached.transport).toBe('usb')
    expect(attached.lan).toBeUndefined()
  })

  it('adds nothing to a port that does not say (a status stays what it was)', async () => {
    const attached = (await statuses(undefined)).find((s) => s.attached)!
    expect('transport' in attached).toBe(false)
    expect('lan' in attached).toBe(false)
  })
})
