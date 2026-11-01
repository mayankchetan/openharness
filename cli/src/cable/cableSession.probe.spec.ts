// `hello{probe:1}` (task W15; design MS section 2, "Probe constraints"). A dial in a wireless mode runs a tiny USB reader while a cable is in:
// it says hello every 2 s and looks only for a `welcome`. The daemon's whole answer is that `welcome`. It attaches no dial, pushes no roster,
// no machines, no pets, offers no firmware and runs no sync, so a PC can sit under a Wi-Fi dial for hours without touching its screen. A hello
// without the field is the session it always was. The welcome the probe gets is small on purpose: the dial's probe decoder holds one frame of
// at most 1 KB, and a welcome too big for it would be a dial that never comes back to USB.
import { mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableSession, type CableHost, type CablePort } from './cableSession.js'
import { DialLog } from './dialLog.js'

class Port implements CablePort {
  readonly path = '/dev/loopback'
  readonly transport = 'usb' as const
  isOpen = true
  closedWith: string | null = null
  readonly sent: Array<Record<string, unknown>> = []
  readonly frames: Array<{ type: number; length: number }> = []
  private decoder = new CableDecoder()
  constructor(private readonly onData: (chunk: Buffer) => void, private readonly onClosed: (why: string) => void) {}
  async write(bytes: Uint8Array): Promise<void> {
    this.decoder.feed(Buffer.from(bytes), (frame) => {
      this.frames.push({ type: frame.type, length: frame.payload.length })
      if (frame.type === CableType.Json) this.sent.push(JSON.parse(Buffer.from(frame.payload).toString('utf8')))
    })
  }
  async close(why = 'closed'): Promise<void> {
    if (!this.isOpen) return
    this.isOpen = false
    this.closedWith = why
    this.onClosed(why)
  }
  say(msg: Record<string, unknown>): void {
    this.onData(Buffer.from(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(msg), 'utf8'))))
  }
  types(): unknown[] { return this.sent.map((m) => m.t) }
}

function host(over: Partial<CableHost> = {}): CableHost {
  return {
    localMachine: () => ({ id: 'mac-local', name: 'MacBook Pro' }),
    listMachines: vi.fn(async () => ({ machines: [{ id: 'mac-local', name: 'MacBook Pro', state: 'ready' as const, local: true }], source: 'backend' as const })),
    selectedMachine: () => 'mac-local',
    selectMachine: async () => ({ ok: true as const }),
    listSwarms: () => ({ selected: '', swarms: [], tiles: [] }),
    listUnread: () => [],
    selectSwarm: vi.fn(),
    appName: () => 'harness',
    voiceLang: () => 'en',
    listAgents: vi.fn(async () => []),
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
    onDialAttached: vi.fn(),
    ...over,
  }
}

const settle = () => new Promise((r) => setTimeout(r, 0))
const HELLO = { t: 'hello', product: 'harness', fw: '0.1.0', proto: 4, mac: 'aa:bb' }
// What the firmware's probe sends (usb_probe_core.c): nothing but what the daemon needs to recognise a Harness dial.
const PROBE = { t: 'hello', product: 'harness', proto: 4, probe: 1 }

async function connect(h: CableHost) {
  const ports: Port[] = []
  const session = new CableSession(h, new DialLog(mkdtempSync(join(tmpdir(), 'cable-probe-'))), async (onData, onClosed) => {
    const port = new Port(onData, onClosed)
    ports.push(port)
    return port
  })
  session.start()
  await vi.waitFor(() => expect(ports.length).toBe(1))
  return { session, port: ports[0]!, ports }
}

describe('hello{probe:1}', () => {
  afterEach(() => vi.useRealTimers())
  const fake = () => vi.useFakeTimers({ toFake: ['Date', 'setInterval', 'clearInterval'] })

  it('is answered with one welcome and nothing else: no attach, no roster, no machines, no pets, no firmware offer', async () => {
    fake()
    const firmwareFor = vi.fn(async () => ({ version: '9.9.9', image: Buffer.alloc(4096), sha256: 'x'.repeat(64) }))
    const pets = vi.fn(() => null)
    const h = host({ firmwareFor, pets })
    const { session, port } = await connect(h)
    port.say({ ...PROBE, fw: '0.0.1', hw: 'round', pets: 2, petIds: [] })   // even a probe that names a fw and pets is offered neither
    await settle()
    expect(port.types()).toEqual(['welcome'])
    expect(port.sent[0]).toMatchObject({ t: 'welcome', probe: 1 })
    expect(h.onDialAttached).not.toHaveBeenCalled()
    // Ticks after it (the ping, the syncs a greeted dial gets) must not push either: only pings may follow.
    for (let i = 0; i < 40; i++) {
      vi.advanceTimersByTime(1_000)
      port.say(PROBE)   // the probe says hello every 2 s; saying it every second only tightens the test
      await settle()
    }
    expect(port.types().filter((t) => t !== 'ping' && t !== 'welcome')).toEqual([])
    expect(h.listAgents).not.toHaveBeenCalled()
    expect(h.listMachines).not.toHaveBeenCalled()
    expect(firmwareFor).not.toHaveBeenCalled()
    expect(pets).not.toHaveBeenCalled()
    expect(port.frames.filter((f) => f.type !== CableType.Json)).toEqual([])   // no PET slices, no FW slices
    await session.stop()
  })

  it('keeps the port open while the probe keeps saying hello, and drops it on silence like any link', async () => {
    fake()
    const { session, port } = await connect(host())
    for (let i = 0; i < 30; i++) {   // 60 s of a probe
      port.say(PROBE)
      vi.advanceTimersByTime(2_000)
      await settle()
    }
    expect(port.isOpen).toBe(true)
    vi.advanceTimersByTime(25_000)
    await settle()
    vi.advanceTimersByTime(1_000)
    await settle()
    expect(port.closedWith).toBe('silence')
    await session.stop()
  })

  it('the welcome fits the dial\'s probe frame cap with room to spare, whatever the machine is called', async () => {
    fake()
    const longName = 'A very long machine name '.repeat(8)
    const { session, port } = await connect(host({ localMachine: () => ({ id: 'm'.repeat(64), name: longName }), appName: () => 'harness-with-a-long-app-name' }))
    port.say({ ...PROBE, mac: 'aa:bb:cc:dd:ee:ff', fw: '9.9.9-long' })
    await settle()
    const welcome = port.frames.find((f) => f.type === CableType.Json)
    expect(welcome).toBeDefined()
    expect(welcome!.length).toBeLessThanOrEqual(256)   // MS: the probe's frame cap is 1 KB; the welcome is a quarter of it at most
    await session.stop()
  })

  it('does not count as a greeting: a real hello that follows is greeted in full (attach, state push)', async () => {
    fake()
    const h = host()
    const { session, port } = await connect(h)
    port.say(PROBE)
    await settle()
    expect(h.onDialAttached).not.toHaveBeenCalled()
    port.say(HELLO)
    await settle()
    await settle()
    expect(h.onDialAttached).toHaveBeenCalledTimes(1)
    expect(port.types().filter((t) => t === 'welcome')).toHaveLength(2)
    expect(port.sent.filter((m) => m.t === 'welcome').at(-1)).not.toHaveProperty('probe')
    expect(port.types()).toContain('agents.begin')
    await session.stop()
  })

  it('a hello without the field behaves as before: one welcome, the state push, the dial attached', async () => {
    fake()
    const h = host()
    const { session, port } = await connect(h)
    port.say(HELLO)
    await settle()
    await settle()
    expect(h.onDialAttached).toHaveBeenCalledTimes(1)
    expect(port.sent[0]).toMatchObject({ t: 'welcome', proto: expect.any(Number), machine: { id: 'mac-local' } })
    expect(port.sent[0]).not.toHaveProperty('probe')
    expect(port.types()).toContain('agents.begin')
    await session.stop()
  })

  it.each([[true], ['1'], [2], [0], [null], [{}]])('only the number 1 is a probe: probe=%j is an ordinary hello', async (value) => {
    fake()
    const h = host()
    const { session, port } = await connect(h)
    port.say({ ...HELLO, probe: value })
    await settle()
    await settle()
    expect(h.onDialAttached).toHaveBeenCalledTimes(1)
    expect(port.sent[0]).not.toHaveProperty('probe')
    await session.stop()
  })

  it('a probe from another product still gets nothing and the port is released', async () => {
    fake()
    const { session, port } = await connect(host())
    port.say({ ...PROBE, product: 'something-else' })
    await settle()
    expect(port.sent).toEqual([])
    expect(port.isOpen).toBe(false)
    await session.stop()
  })

  it('a probe with no product at all gets nothing (a positive match, and absence means no)', async () => {
    fake()
    const { session, port } = await connect(host())
    port.say({ t: 'hello', proto: 4, probe: 1 })
    await settle()
    expect(port.sent).toEqual([])
    await session.stop()
  })

  it('logs the first probe welcome of a plug-in, once, and again for the next plug-in (review Low 3)', async () => {
    fake()
    const lines: string[] = []
    const { session, port, ports } = await connect(host({ log: (line) => lines.push(line) }))
    const probeLines = () => lines.filter((l) => /probing this cable/.test(l))
    for (let i = 0; i < 10; i++) {
      port.say(PROBE)
      vi.advanceTimersByTime(2_000)
      await settle()
    }
    expect(probeLines()).toHaveLength(1)
    expect(probeLines()[0]).not.toMatch(/aa:bb/i)   // no MAC, no key: the line names the cable's port only
    // an ordinary hello is not a probe and logs nothing of this kind
    port.say(HELLO)
    await settle()
    expect(probeLines()).toHaveLength(1)
    // unplugged and plugged in again: a new port, a new first line
    await port.close('unplugged')
    vi.advanceTimersByTime(10_000)
    await vi.waitFor(() => expect(ports.length).toBeGreaterThan(1))
    await settle()   // the session assigns the new port after its open resolves
    await settle()
    ports[ports.length - 1]!.say(PROBE)
    await settle()
    expect(probeLines()).toHaveLength(2)
    await session.stop()
  })
})
