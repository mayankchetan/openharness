// The session's side of the dial's debug commands (dialDebug.ts has the layer, this has the wiring): a request
// goes out as a JSON frame, the 0x06 picture frames come back through the decoder, and none of it disturbs the
// rest of the conversation. Driven through a loopback port, as cableSession.spec.ts does.
import { mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { crc32 } from 'node:zlib'
import { describe, expect, it, vi } from 'vitest'

import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableSession, type CableHost, type CablePort } from './cableSession.js'
import { DialLog } from './dialLog.js'

class Loopback implements CablePort {
  readonly path = '/dev/loopback'
  isOpen = true
  readonly sent: Array<Record<string, unknown>> = []
  private decoder = new CableDecoder()

  constructor(private readonly onData: (chunk: Buffer) => void, private readonly onClosed: (why: string) => void) {}

  async write(bytes: Uint8Array): Promise<void> {
    this.decoder.feed(Buffer.from(bytes), (frame) => {
      if (frame.type === CableType.Json) this.sent.push(JSON.parse(Buffer.from(frame.payload).toString('utf8')) as Record<string, unknown>)
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

  /** One 0x06 frame, as the dial sends a picture chunk. */
  shotChunk(id: number, index: number, pixels: Uint8Array): void {
    const head = Buffer.alloc(4)
    head.writeUInt16LE(id, 0)
    head.writeUInt16LE(index, 2)
    this.onData(Buffer.from(encodeCableFrame(CableType.Shot, Buffer.concat([head, pixels]))))
  }

  types(): string[] { return this.sent.map((m) => m.t as string) }
}

function host(log: (line: string) => void): CableHost {
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
    sendTurn: vi.fn(), stopTurn: vi.fn(), scrolled: vi.fn(), answer: vi.fn(), focus: vi.fn(), openAgent: vi.fn(),
    forkAgent: async (id) => ({ ok: true as const, agentId: `${id}-fork` }),
    updateAgent: vi.fn(),
    listModels: async () => [],
    recentSummaries: async () => [],
    transcribe: async () => '',
    route: async () => ({ agentId: '', confidence: 0, reason: '' }),
    log,
  }
}

async function connect() {
  const lines: string[] = []
  let port!: Loopback
  const session = new CableSession(host((line) => lines.push(line)), new DialLog(mkdtempSync(join(tmpdir(), 'cable-debug-'))), async (onData, onClosed) => {
    port = new Loopback(onData, onClosed)
    return port
  })
  session.start()
  await vi.waitFor(() => expect(port).toBeDefined())
  return { session, port, lines }
}

const greet = async (port: Loopback) => {
  port.say({ t: 'hello', product: 'harness', mac: 'aa:bb', fw: '1.0.0', debug: 'v1' })
  await vi.waitFor(() => expect(port.types()).toContain('agents.end'))
}

// 6 x 3 = 18 pixels = 36 bytes, in chunks of 16 (16, 16, 4)
const W = 6, H = 3
const PIXELS = Buffer.from(Array.from({ length: W * H * 2 }, (_, i) => (i * 7 + 3) & 0xff))
const PARTS = [PIXELS.subarray(0, 16), PIXELS.subarray(16, 32), PIXELS.subarray(32)]
const begin = (id: number) => ({ t: 'debug.shot.begin', id, w: W, h: H, fmt: 'rgb565be', bytes: PIXELS.length, chunk: 16, chunks: 3 })
const end = (id: number) => ({ t: 'debug.shot.end', id, chunks: 3, crc32: crc32(PIXELS) })
const shotId = (port: Loopback) => port.sent.find((m) => m.t === 'debug.shot')!.id as number

describe('the dial\'s debug commands through a session', () => {
  it('asks for a screenshot and returns the picture the 0x06 frames make', async () => {
    const { session, port, lines } = await connect()
    try {
      await greet(port)
      const pending = session.debugShot()
      await vi.waitFor(() => expect(port.types()).toContain('debug.shot'))
      const id = shotId(port)
      port.say(begin(id))
      PARTS.forEach((part, index) => port.shotChunk(id, index, part))
      port.say(end(id))
      const result = await pending
      expect(result).toMatchObject({ ok: true, width: W, height: H })
      expect(result.ok && result.pixels.equals(PIXELS)).toBe(true)
      // A picture frame is not an unknown frame type, and a debug reply is not an unhandled message.
      expect(lines.filter((line) => /unknown frame type|unhandled message/.test(line))).toEqual([])
    } finally { await session.stop() }
  })

  it('goes on talking to the dial while the picture is on its way', async () => {
    const { session, port } = await connect()
    try {
      await greet(port)
      const pending = session.debugShot()
      await vi.waitFor(() => expect(port.types()).toContain('debug.shot'))
      const id = shotId(port)
      port.say(begin(id))
      port.shotChunk(id, 0, PARTS[0])
      // The dial's own greeting, mid-picture, is answered as ever.
      const welcomes = port.types().filter((type) => type === 'welcome').length
      port.say({ t: 'hello', product: 'harness', mac: 'aa:bb', fw: '1.0.0', debug: 'v1' })
      await vi.waitFor(() => expect(port.types().filter((type) => type === 'welcome').length).toBe(welcomes + 1))
      port.shotChunk(id, 1, PARTS[1])
      port.shotChunk(id, 2, PARTS[2])
      port.say(end(id))
      expect(await pending).toMatchObject({ ok: true })
    } finally { await session.stop() }
  })

  it('gives no picture when a chunk is lost, and the next request still works', async () => {
    const { session, port } = await connect()
    try {
      await greet(port)
      const pending = session.debugShot()
      await vi.waitFor(() => expect(port.types()).toContain('debug.shot'))
      const id = shotId(port)
      port.say(begin(id))
      port.shotChunk(id, 0, PARTS[0])
      port.shotChunk(id, 2, PARTS[2])
      port.say(end(id))
      expect(await pending).toMatchObject({ ok: false, kind: 'incomplete' })
      const again = session.debugShot()
      await vi.waitFor(() => expect(port.types().filter((type) => type === 'debug.shot')).toHaveLength(2))
      const second = port.sent.filter((m) => m.t === 'debug.shot')[1].id as number
      port.say(begin(second))
      PARTS.forEach((part, index) => port.shotChunk(second, index, part))
      port.say(end(second))
      expect(await again).toMatchObject({ ok: true })
    } finally { await session.stop() }
  })

  it('plays a touch and is done on debug.done', async () => {
    const { session, port } = await connect()
    try {
      await greet(port)
      const pending = session.debugTouch({ cmd: 'swipe', x1: 380, y1: 233, x2: 80, y2: 233, ms: 250 })
      await vi.waitFor(() => expect(port.types()).toContain('debug.swipe'))
      const sent = port.sent.find((m) => m.t === 'debug.swipe')!
      expect(sent).toMatchObject({ x1: 380, y1: 233, x2: 80, y2: 233, ms: 250 })
      port.say({ t: 'debug.done', id: sent.id, cmd: 'swipe' })
      expect(await pending).toEqual({ ok: true })
    } finally { await session.stop() }
  })

  it('refuses at once when no dial has greeted, sending nothing', async () => {
    const { session, port } = await connect()
    try {
      expect(await session.debugShot()).toMatchObject({ ok: false, kind: 'gone' })
      expect(await session.debugTouch({ cmd: 'tap', x: 1, y: 1 })).toMatchObject({ ok: false, kind: 'gone' })
      expect(port.types()).toEqual([])
    } finally { await session.stop() }
  })

  it('is cleaned up when the port closes, and when the session stops', async () => {
    const { session, port } = await connect()
    try {
      await greet(port)
      const closed = session.debugShot()
      await vi.waitFor(() => expect(port.types()).toContain('debug.shot'))
      await port.close('cable pulled')
      expect(await closed).toMatchObject({ ok: false, kind: 'gone' })
    } finally { await session.stop() }

    const second = await connect()
    try {
      await greet(second.port)
      const stopped = second.session.debugTouch({ cmd: 'tap', x: 5, y: 5 })
      await vi.waitFor(() => expect(second.port.types()).toContain('debug.tap'))
      await second.session.stop()
      expect(await stopped).toMatchObject({ ok: false, kind: 'gone' })
    } finally { await second.session.stop() }
  })
})
