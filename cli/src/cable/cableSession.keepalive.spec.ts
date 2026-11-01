// A session with and without welcome.keepaliveMs (task W13; keepalive.ts). Absent, the timings are the ones this protocol has
// always had. Present, and only for a dial that said `hello.keepalive = "v1"`, the daemon pings every K and gives a quiet dial
// 3 x K + 5 s, and the welcome tells the dial K so that it waits 3 x K for the daemon: both windows come from one number.
import { mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableSession, type CableHost, type CablePort } from './cableSession.js'
import { DialLog } from './dialLog.js'
import { daemonSilenceMs, dialSilenceMs } from './keepalive.js'

class Port implements CablePort {
  readonly path = '/dev/loopback'
  readonly transport = 'usb' as const
  isOpen = true
  closedWith: string | null = null
  readonly sent: Array<Record<string, unknown>> = []
  private decoder = new CableDecoder()
  constructor(private readonly onData: (chunk: Buffer) => void, private readonly onClosed: (why: string) => void) {}
  async write(bytes: Uint8Array): Promise<void> {
    this.decoder.feed(Buffer.from(bytes), (frame) => {
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
  pings(): number { return this.sent.filter((m) => m.t === 'ping').length }
  welcomes(): Array<Record<string, unknown>> { return this.sent.filter((m) => m.t === 'welcome') }
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

const settle = () => new Promise((r) => setTimeout(r, 0))
const HELLO = { t: 'hello', product: 'harness', fw: '0.1.0', proto: 4, mac: 'aa:bb' }

async function connect(h: CableHost) {
  const ports: Port[] = []
  const session = new CableSession(h, new DialLog(mkdtempSync(join(tmpdir(), 'cable-ka-'))), async (onData, onClosed) => {
    const port = new Port(onData, onClosed)
    ports.push(port)
    return port
  })
  session.start()
  await vi.waitFor(() => expect(ports.length).toBe(1))
  return { session, port: ports[0]!, ports }
}

/** Advance one simulated second at a time; the seconds (counted from the call) at which a ping went out. */
async function pingSeconds(port: Port, seconds: number): Promise<number[]> {
  const at: number[] = []
  let seen = port.pings()
  for (let s = 1; s <= seconds; s++) {
    vi.advanceTimersByTime(1_000)
    await settle()
    if (port.pings() > seen) { at.push(s); seen = port.pings() }
  }
  return at
}

const gaps = (seconds: number[]): number[] => seconds.slice(1).map((s, i) => s - seconds[i]!)

/** Advance until the daemon closes the port; the second (counted from the call) at which it did, or null if still open after `max`. */
async function silenceSecond(port: Port, max: number): Promise<number | null> {
  for (let s = 1; s <= max; s++) {
    vi.advanceTimersByTime(1_000)
    await settle()
    if (!port.isOpen) return s
  }
  return null
}

describe('welcome.keepaliveMs', () => {
  afterEach(() => vi.useRealTimers())

  const fake = () => vi.useFakeTimers({ toFake: ['Date', 'setInterval', 'clearInterval'] })

  it('absent: no field in the welcome, a ping every 5 s', async () => {
    fake()
    const { session, port } = await connect(host())
    port.say({ ...HELLO, keepalive: 'v1' })   // even a dial that can take it gets nothing when the daemon is not configured
    await settle()
    expect(port.welcomes()).toHaveLength(1)
    expect(port.welcomes()[0]).not.toHaveProperty('keepaliveMs')
    const at = await pingSeconds(port, 19)
    expect(at.length).toBeGreaterThanOrEqual(3)
    expect(gaps(at)).toEqual(Array(at.length - 1).fill(5))
    await session.stop()
  })

  it('absent: a dial that goes quiet is dropped on the first tick past 20 s (today)', async () => {
    fake()
    const { session, port } = await connect(host())
    port.say({ ...HELLO, keepalive: 'v1' })
    await settle()
    expect(await silenceSecond(port, 40)).toBe(21)
    expect(port.closedWith).toBe('silence')
    await session.stop()
  })

  it('configured and the dial said keepalive v1: the welcome carries K and the ping is every K', async () => {
    fake()
    const { session, port } = await connect(host({ keepaliveMs: () => 10_000 }))
    port.say({ ...HELLO, keepalive: 'v1' })
    await settle()
    expect(port.welcomes()[0]).toMatchObject({ keepaliveMs: 10_000 })
    const at = await pingSeconds(port, 34)   // inside the 35 s window
    expect(at.length).toBeGreaterThanOrEqual(3)
    expect(gaps(at)).toEqual(Array(at.length - 1).fill(10))
    await session.stop()
  })

  it('configured: a quiet dial is dropped on the first tick past 3K + 5 s, not before', async () => {
    fake()
    const { session, port } = await connect(host({ keepaliveMs: () => 10_000 }))
    port.say({ ...HELLO, keepalive: 'v1' })
    await settle()
    expect(daemonSilenceMs(10_000)).toBe(35_000)
    expect(await silenceSecond(port, 60)).toBe(36)
    expect(port.closedWith).toBe('silence')
    await session.stop()
  })

  it('clamps what the host asks for: 1 s becomes 5 s, 5 min becomes 60 s', async () => {
    fake()
    for (const [asked, sent] of [[1_000, 5_000], [300_000, 60_000]] as const) {
      const { session, port } = await connect(host({ keepaliveMs: () => asked }))
      port.say({ ...HELLO, keepalive: 'v1' })
      await settle()
      expect(port.welcomes()[0]).toMatchObject({ keepaliveMs: sent })
      await session.stop()
    }
  })

  it('never sends it to a dial that did not say keepalive v1 (an older firmware would call the daemon dead after 15 s)', async () => {
    fake()
    const { session, port } = await connect(host({ keepaliveMs: () => 30_000 }))
    port.say(HELLO)
    await settle()
    expect(port.welcomes()[0]).not.toHaveProperty('keepaliveMs')
    const at = await pingSeconds(port, 19)   // and the ping stays at today's 5 s
    expect(gaps(at)).toEqual(Array(at.length - 1).fill(5))
    expect(await silenceSecond(port, 40)).toBe(2)   // 19 s spent above, closed on the first tick past 20 s
    await session.stop()
  })

  it('an unset host (0, NaN, undefined, negative) means off', async () => {
    fake()
    for (const unset of [0, NaN, undefined, -1]) {
      const { session, port } = await connect(host({ keepaliveMs: () => unset }))
      port.say({ ...HELLO, keepalive: 'v1' })
      await settle()
      expect(port.welcomes()[0]).not.toHaveProperty('keepaliveMs')
      await session.stop()
    }
  })

  it("the dial's window (3K) is shorter than the daemon's (3K + 5 s) for every K the daemon can send", async () => {
    fake()
    for (const asked of [5_000, 7_500, 20_000, 60_000]) {
      const { session, port } = await connect(host({ keepaliveMs: () => asked }))
      port.say({ ...HELLO, keepalive: 'v1' })
      await settle()
      const k = port.welcomes()[0]!.keepaliveMs as number
      expect(k).toBe(asked)
      expect(dialSilenceMs(k)).toBeLessThan(daemonSilenceMs(k))
      await session.stop()
    }
  })

  it("a new link starts from today's timings again (nothing of the last welcome outlives the port)", async () => {
    fake()
    const { session, port, ports } = await connect(host({ keepaliveMs: () => 20_000 }))
    port.say({ ...HELLO, keepalive: 'v1' })
    await settle()
    expect(port.welcomes()[0]).toMatchObject({ keepaliveMs: 20_000 })
    await port.close('unplugged')
    // The daemon opens the port again on a later tick; this dial does not say keepalive v1.
    for (let s = 0; s < 10 && ports.length < 2; s++) { vi.advanceTimersByTime(1_000); await settle() }
    expect(ports).toHaveLength(2)
    const again = ports[1]!
    again.say(HELLO)
    await settle()
    expect(again.welcomes()[0]).not.toHaveProperty('keepaliveMs')
    const at = await pingSeconds(again, 19)
    expect(gaps(at)).toEqual(Array(at.length - 1).fill(5))
    await session.stop()
  })
})
