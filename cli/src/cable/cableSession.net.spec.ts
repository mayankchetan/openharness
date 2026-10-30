// A session and the Wi-Fi settings of its dial (docs/specs/cable-wifi-link.md section 5; dialNet.ts): sent over the cable
// only, and only to a dial that said it understands them. The dial's `hello.net` is "v1"; the daemon's `welcome.features`
// then carries "net".
import { mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it, vi } from 'vitest'

import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableSession, type CableHost, type CablePort, type NetDialHandle } from './cableSession.js'
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
  const dir = mkdtempSync(join(tmpdir(), 'cable-net-'))
  const session = new CableSession(h, new DialLog(dir), async (onData, onClosed) => {
    port = new Port(transport ? `${transport}:${peer}:5000` : '/dev/loopback', onData, onClosed, transport, peer)
    return port
  })
  session.start()
  await vi.waitFor(() => expect(port).toBeDefined())
  return { session, port, dir }
}

const MAC = 'B8:1F:3F:CA:E5:58'
const HELLO = { t: 'hello', product: 'harness', fw: '0.0.1', proto: 3, mac: MAC, hw: 'cst9217+axp2101', net: 'v1' }
const KEY = Buffer.alloc(32, 3)
const SET = { op: 'set', ssid: 'home-net', pass: 'correct horse', host: '192.168.1.20', port: 18480, key: KEY, mode: 1 } as const

const featuresOf = (port: Port): string[] => (port.sent.find((m) => m.t === 'welcome')?.features ?? []) as string[]

describe('the "net" feature in the welcome', () => {
  it('is offered to a dial that said hello.net = "v1" on the cable', async () => {
    const { session, port } = await connect(host(), 'usb')
    port.say(HELLO)
    await vi.waitFor(() => expect(port.types()).toContain('welcome'))
    expect(featuresOf(port)).toContain('net')
    await session.stop()
  })

  it.each(['wifi', 'ble', 'tunnel'] as const)('is never offered on a %s link, even to a dial that asks', async (transport) => {
    const { session, port } = await connect(host(), transport, '192.168.1.40')
    port.say(HELLO)
    await vi.waitFor(() => expect(port.types()).toContain('welcome'))
    expect(featuresOf(port)).not.toContain('net')
    await session.stop()
  })

  it('is not offered on a port that does not say which link it is', async () => {
    const { session, port } = await connect(host())
    port.say(HELLO)
    await vi.waitFor(() => expect(port.types()).toContain('welcome'))
    expect(featuresOf(port)).not.toContain('net')
    await session.stop()
  })

  it('is not offered to a dial that did not ask (an older firmware)', async () => {
    const { session, port } = await connect(host(), 'usb')
    port.say({ ...HELLO, net: undefined })
    await vi.waitFor(() => expect(port.types()).toContain('welcome'))
    expect(featuresOf(port)).not.toContain('net')
    port.say({ ...HELLO, net: 'v2' })
    await new Promise((resolve) => setTimeout(resolve, 20))
    expect(featuresOf(port)).not.toContain('net')
    await session.stop()
  })
})

describe('net requests', () => {
  async function ready(transport: CablePort['transport'] | undefined, hello: Record<string, unknown> = HELLO) {
    const rig = await connect(host(), transport, transport && transport !== 'usb' ? '192.168.1.40' : undefined)
    rig.port.say(hello)
    await vi.waitFor(() => expect(rig.session.isConnected).toBe(true))
    return rig
  }
  const wireNet = (port: Port) => port.sent.filter((m) => String(m.t).startsWith('net.'))

  it('go to a dial on the cable, and the dial\'s net.state is the answer', async () => {
    const { session, port } = await ready('usb')
    const pending = session.net(SET)
    await vi.waitFor(() => expect(wireNet(port)).toHaveLength(1))
    const sent = wireNet(port)[0]
    expect(sent).toMatchObject({ t: 'net.set', ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1, key: KEY.toString('base64') })
    port.say({ t: 'net.state', id: sent.id, ok: true, saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 })
    expect(await pending).toEqual({ ok: true, saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 })
    await session.stop()
  })

  it.each(['wifi', 'ble', 'tunnel'] as const)('are never sent on a %s link, and say so', async (transport) => {
    const { session, port } = await ready(transport)
    for (const request of [SET, { op: 'get' }, { op: 'clear' }, { op: 'mode', mode: 1 }, { op: 'key', key: KEY }, { op: 'host', host: '10.0.0.2', port: 18480 }] as const) {
      expect(await session.net(request)).toMatchObject({ ok: false, kind: 'transport' })
    }
    expect(wireNet(port)).toEqual([])
    await session.stop()
  })

  it('are never sent on a port that does not say it is the cable', async () => {
    const { session, port } = await ready(undefined)
    expect(await session.net({ op: 'get' })).toMatchObject({ ok: false, kind: 'transport' })
    expect(wireNet(port)).toEqual([])
    await session.stop()
  })

  it('are never sent to a dial that did not advertise net', async () => {
    const { session, port } = await ready('usb', { ...HELLO, net: undefined })
    expect(await session.net(SET)).toMatchObject({ ok: false, kind: 'unsupported' })
    expect(wireNet(port)).toEqual([])
    await session.stop()
  })

  it('say "gone" when no dial has greeted', async () => {
    const { session, port } = await connect(host(), 'usb')
    expect(await session.net({ op: 'get' })).toMatchObject({ ok: false, kind: 'gone' })
    expect(wireNet(port)).toEqual([])
    await session.stop()
  })

  it('end with "gone" when the link closes under a waiting request', async () => {
    const { session, port } = await ready('usb')
    const pending = session.net({ op: 'get' })
    await vi.waitFor(() => expect(wireNet(port)).toHaveLength(1))
    await port.close('unplugged')
    expect(await pending).toMatchObject({ ok: false, kind: 'gone' })
    await session.stop()
  })

  it('leave no value in the dial log: not the password, the key, the SSID or the address', async () => {
    const lines: string[] = []
    const rig = await connect(host({ log: (line) => lines.push(line) }), 'usb')
    rig.port.say(HELLO)
    await vi.waitFor(() => expect(rig.session.isConnected).toBe(true))
    const pending = rig.session.net(SET)
    await vi.waitFor(() => expect(wireNet(rig.port)).toHaveLength(1))
    rig.port.say({ t: 'net.state', id: wireNet(rig.port)[0].id, ok: false, err: 'pass', saved: false })
    await pending
    await rig.session.stop()
    const { readdirSync, readFileSync } = await import('node:fs')
    const written = readdirSync(rig.dir).map((name) => readFileSync(join(rig.dir, name), 'utf8')).join('\n')
    for (const where of [lines.join('\n'), written]) {
      for (const secret of ['correct horse', KEY.toString('base64'), KEY.toString('hex'), 'home-net']) expect(where).not.toContain(secret)
    }
    expect(lines.join('\n')).toContain('net.set')
  })
})

describe('telling the host that a dial can take net settings', () => {
  const handles: NetDialHandle[] = []
  const onNetCapable = vi.fn((handle: NetDialHandle) => { handles.push(handle) })

  it('happens once per link, after the greeting, for a dial on the cable that advertised net', async () => {
    onNetCapable.mockClear()
    const { session, port } = await connect(host({ onNetCapable }), 'usb')
    port.say(HELLO)
    await vi.waitFor(() => expect(onNetCapable).toHaveBeenCalledTimes(1))
    expect(onNetCapable.mock.calls[0][0].mac).toBe(MAC)
    // the keepalive greeting every 15 s does not repeat it
    port.say(HELLO)
    await new Promise((resolve) => setTimeout(resolve, 30))
    expect(onNetCapable).toHaveBeenCalledTimes(1)
    // and the handle works: its request goes down the same link
    const pending = onNetCapable.mock.calls[0][0].net({ op: 'get' })
    await vi.waitFor(() => expect(port.types()).toContain('net.get'))
    port.say({ t: 'net.state', id: port.sent.find((m) => m.t === 'net.get')!.id, ok: true, saved: false })
    expect(await pending).toEqual({ ok: true, saved: false })
    await session.stop()
  })

  it.each(['wifi', 'ble', 'tunnel'] as const)('never happens on a %s link', async (transport) => {
    onNetCapable.mockClear()
    const { session, port } = await connect(host({ onNetCapable }), transport, '192.168.1.40')
    port.say(HELLO)
    await vi.waitFor(() => expect(port.types()).toContain('welcome'))
    await new Promise((resolve) => setTimeout(resolve, 30))
    expect(onNetCapable).not.toHaveBeenCalled()
    await session.stop()
  })

  it('does not happen for a dial that did not advertise net', async () => {
    onNetCapable.mockClear()
    const { session, port } = await connect(host({ onNetCapable }), 'usb')
    port.say({ ...HELLO, net: undefined })
    await vi.waitFor(() => expect(port.types()).toContain('welcome'))
    await new Promise((resolve) => setTimeout(resolve, 30))
    expect(onNetCapable).not.toHaveBeenCalled()
    await session.stop()
  })

  it('a host that throws or rejects costs the session nothing', async () => {
    const lines: string[] = []
    const { session, port } = await connect(host({ log: (line) => lines.push(line), onNetCapable: () => { throw new Error('host broke') } }), 'usb')
    port.say(HELLO)
    await vi.waitFor(() => expect(port.types()).toContain('agents.end'))
    expect(lines.join('\n')).toContain('host broke')
    await session.stop()
    const rejected = await connect(host({ log: (line) => lines.push(line), onNetCapable: async () => { throw new Error('host rejected') } }), 'usb')
    rejected.port.say(HELLO)
    await vi.waitFor(() => expect(lines.join('\n')).toContain('host rejected'))
    await rejected.session.stop()
  })
})
