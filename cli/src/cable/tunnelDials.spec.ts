// The loopback relay endpoint for dials behind the phone bridge (tunnelDials.ts), against a real `ws` client and a real
// TLS-PSK client on loopback, and a real NetDials behind it. Slow cases fake setTimeout only, so the suite is fast.
import { randomBytes } from 'node:crypto'
import { mkdtempSync, rmSync } from 'node:fs'
import * as http from 'node:http'
import * as net from 'node:net'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { Duplex } from 'node:stream'
import { WebSocket } from 'ws'

import { env } from '../config/env.js'
import { pskConnect } from './__fixtures__/pskClient.js'
import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { DIAL_KEY_FILE, DialKeys } from './dialKeys.js'
import { NET_HANDSHAKE_DEADLINE_MS, NET_TUNNEL_PENDING_MAX, NetDials, type NetDialConnection } from './netDials.js'
import {
  TUNNEL_PING_MS,
  TUNNEL_PONG_MIN_MS,
  TUNNEL_PREUPGRADE_MAX,
  TUNNEL_STREAM_BUFFER_BYTES,
  TUNNEL_MAX_MESSAGE_BYTES,
  TUNNEL_PATH,
  TUNNEL_UPGRADE_DEADLINE_MS,
  TunnelDials,
  isLoopbackBind,
} from './tunnelDials.js'

const A = 'B8:1F:3F:CA:E5:58'
const B = 'B8:1F:3F:CA:E5:59'

const cleanup: Array<() => unknown> = []
afterEach(async () => {
  vi.useRealTimers()
  for (const run of cleanup.splice(0).reverse()) await run()
})

/** Poll with real macrotasks (setImmediate), so it also works while setTimeout is faked. */
async function until(check: () => boolean, what = 'condition', rounds = 20_000): Promise<void> {
  for (let i = 0; i < rounds; i++) {
    if (check()) return
    await new Promise((resolve) => setImmediate(resolve))
  }
  throw new Error(`never: ${what}`)
}

interface Rig {
  tunnel: TunnelDials
  dials: NetDials
  keys: DialKeys
  logs: string[]
  conns: NetDialConnection[]
  port: () => number
}

function rig(keyIds: Array<[string, Buffer]> = [[A, randomBytes(32)]], extra: Partial<ConstructorParameters<typeof TunnelDials>[0]> = {}): Rig {
  const dir = mkdtempSync(join(tmpdir(), 'tunnel-dials-'))
  cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
  const keys = new DialKeys(join(dir, DIAL_KEY_FILE))
  for (const [id, key] of keyIds) keys.set(id, key)
  const logs: string[] = []
  const conns: NetDialConnection[] = []
  const dials = new NetDials({
    keys,
    port: 0,
    bind: '127.0.0.1',
    enabled: false, // only the stream entry is in play: the Wi-Fi socket stays shut
    log: (line) => logs.push(line),
    onDial: (conn) => {
      conns.push(conn)
      conn.socket.on('error', () => {})
    },
  })
  dials.start()
  const tunnel = new TunnelDials({ keys, dials, port: 0, enabled: true, log: (line) => logs.push(line), ...extra })
  tunnel.start()
  cleanup.push(async () => {
    await tunnel.stop()
    await dials.stop()
  })
  return { tunnel, dials, keys, logs, conns, port: () => tunnel.address()!.port }
}

async function listening(r: Rig): Promise<Rig> {
  await r.tunnel.settled()
  expect(r.tunnel.listening).toBe(true)
  return r
}

const url = (r: Rig, path = TUNNEL_PATH) => `ws://127.0.0.1:${r.port()}${path}`

/** A `ws` client, open. */
async function wsOpen(r: Rig, options: ConstructorParameters<typeof WebSocket>[2] = {}, path = TUNNEL_PATH): Promise<WebSocket> {
  const ws = new WebSocket(url(r, path), options)
  ws.on('error', () => {})
  cleanup.push(() => ws.terminate())
  await new Promise<void>((resolve, reject) => {
    ws.once('open', () => resolve())
    ws.once('error', reject)
    ws.once('unexpected-response', (_req, res) => reject(new Error(`HTTP ${res.statusCode}`)))
  })
  return ws
}

/** The phone's pump, in a test: bytes in and out of a `ws` client as binary messages of at most 16 KiB each. */
function phonePump(ws: WebSocket): Duplex {
  const slice = 16 * 1024
  const pump = new Duplex({
    read() {},
    write(chunk: Buffer, _encoding, callback) {
      let offset = 0
      const next = (error?: Error | null): void => {
        if (error || offset >= chunk.length) return callback(error)
        const part = chunk.subarray(offset, offset + slice)
        offset += slice
        ws.send(part, { binary: true }, next)
      }
      next()
    },
    final(callback) {
      ws.close()
      callback()
    },
    destroy(error, callback) {
      ws.terminate()
      callback(error)
    },
  })
  ws.on('message', (data) => pump.push(data as Buffer))
  ws.once('close', () => pump.push(null))
  ws.on('error', (error) => pump.destroy(error))
  return pump
}

/** A PSK client over a `ws` client: the phone, in a test. */
async function dialOver(r: Rig, id: string, psk: Buffer, options: ConstructorParameters<typeof WebSocket>[2] = {}) {
  const ws = await wsOpen(r, options)
  const stream = phonePump(ws)
  stream.on('error', () => {})
  const tls = await pskConnect({ port: 0, identity: id, psk, socket: stream })
  tls.on('error', () => {})
  cleanup.push(() => tls.destroy())
  return { ws, tls }
}

/** A raw TCP client; `closed()` is true once the far end has closed it. */
function rawClient(r: Rig): { socket: net.Socket; closed: () => boolean; received: () => string } {
  const socket = net.connect({ port: r.port(), host: '127.0.0.1' })
  let closed = false
  let received = ''
  socket.on('error', () => {})
  socket.on('data', (chunk) => {
    received += chunk.toString('latin1')
  })
  socket.once('close', () => {
    closed = true
  })
  cleanup.push(() => socket.destroy())
  return { socket, closed: () => closed, received: () => received }
}

const freePort = (): Promise<number> =>
  new Promise((resolve) => {
    const probe = net.createServer()
    probe.listen(0, '127.0.0.1', () => {
      const port = (probe.address() as net.AddressInfo).port
      probe.close(() => resolve(port))
    })
  })

async function connectRefused(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const s = net.connect({ port, host: '127.0.0.1' })
    s.once('connect', () => {
      s.destroy()
      resolve(false)
    })
    s.once('error', () => resolve(true))
  })
}

describe('TunnelDials: defaults and the bind rule', () => {
  it('is off by default and on port 18481', () => {
    // The environment of the test run does not set these (envIsolation); the defaults are the contract.
    expect(env.CABLE_TUNNEL_ENABLE).toBe(false)
    expect(env.CABLE_TUNNEL_PORT).toBe(18481)
    expect(TUNNEL_PATH).toBe('/relay')
    expect(TUNNEL_UPGRADE_DEADLINE_MS).toBe(3000)
  })

  it.each([
    ['127.0.0.1', true],
    ['127.1.2.3', true],
    ['::1', true],
    ['0.0.0.0', false],
    ['::', false],
    ['192.168.1.20', false],
    ['10.0.0.5', false],
    ['8.8.8.8', false],
    ['localhost', false],
    ['', false],
    ['127.0.0.1.evil.example', false],
    ['::ffff:192.168.1.20', false],
    ['*', false],
  ])('isLoopbackBind(%j) is %s', (address, expected) => {
    expect(isLoopbackBind(address)).toBe(expected)
  })

  it('never listens when disabled, whatever the keystore holds', async () => {
    const port = await freePort()
    const r = rig([[A, randomBytes(32)]], { port, enabled: false })
    await r.tunnel.settled()
    expect(r.tunnel.listening).toBe(false)
    expect(await connectRefused(port)).toBe(true)
  })

  it('binds 127.0.0.1 and nothing else by default', async () => {
    const r = await listening(rig())
    expect(r.tunnel.address()?.address).toBe('127.0.0.1')
  })

  it.each(['0.0.0.0', '::', '192.168.1.20', 'localhost', ''])('refuses to bind %j: no listening socket, one line says so', async (bind) => {
    const port = await freePort()
    const r = rig([[A, randomBytes(32)]], { port, bind })
    await r.tunnel.settled()
    expect(r.tunnel.listening).toBe(false)
    expect(await connectRefused(port)).toBe(true)
    expect(r.logs.filter((l) => /not a loopback address/.test(l))).toHaveLength(1)
  })

  it('opens only while the keystore holds a dial: shut with none, open after the first key, shut after the last', async () => {
    const port = await freePort()
    const r = rig([], { port })
    await r.tunnel.settled()
    expect(r.tunnel.listening).toBe(false)
    expect(await connectRefused(port)).toBe(true)
    r.keys.set(A, randomBytes(32))
    await r.tunnel.settled()
    expect(r.tunnel.listening).toBe(true)
    r.keys.remove(A)
    await r.tunnel.settled()
    expect(r.tunnel.listening).toBe(false)
    expect(await connectRefused(port)).toBe(true)
  })

  it('on a port clash says so once, throws nothing and leaves the rest alone', async () => {
    const holder = await new Promise<net.Server>((resolve) => {
      const server = net.createServer()
      server.listen(0, '127.0.0.1', () => resolve(server))
    })
    cleanup.push(() => holder.close())
    const port = (holder.address() as net.AddressInfo).port
    const r = rig([[A, randomBytes(32)]], { port })
    await r.tunnel.settled()
    r.keys.set(B, randomBytes(32))
    await r.tunnel.settled()
    expect(r.tunnel.listening).toBe(false)
    expect(r.logs.filter((l) => /in use/.test(l))).toHaveLength(1)
    expect(r.logs.join('\n')).toMatch(/USB and Wi-Fi dials are unaffected/)
  })
})

describe('TunnelDials: a dial through the endpoint', () => {
  it('delivers 1,000 frames intact and in order through ws and the PSK handshake, as a tunnel link', async () => {
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    const { tls } = await dialOver(r, A.toLowerCase(), key)
    await until(() => r.conns.length === 1, 'onDial')
    expect(r.conns[0]).toMatchObject({ id: A, transport: 'tunnel' })
    const decoder = new CableDecoder()
    const seen: number[] = []
    r.conns[0].socket.on('data', (chunk: Buffer) => {
      decoder.feed(chunk, (frame) => {
        if (frame.type === CableType.Json) seen.push((JSON.parse(Buffer.from(frame.payload).toString()) as { n: number }).n)
      })
    })
    for (let n = 0; n < 1000; n++) tls.write(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify({ n, pad: 'x'.repeat(n % 97) }))))
    await until(() => seen.length === 1000, 'all frames', 400_000)
    expect(seen).toEqual(Array.from({ length: 1000 }, (_, n) => n))
    const echoed = new Promise<string>((resolve) => tls.once('data', (b) => resolve(b.toString())))
    r.conns[0].socket.write('welcome')
    expect(await echoed).toBe('welcome')
  })

  it('carries a 1 MB burst each way (many full TLS records) without loss', async () => {
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    const { tls } = await dialOver(r, A, key)
    await until(() => r.conns.length === 1)
    const blob = randomBytes(1024 * 1024)
    const up: Buffer[] = []
    r.conns[0].socket.on('data', (c: Buffer) => up.push(c))
    tls.write(blob)
    await until(() => up.reduce((n, c) => n + c.length, 0) >= blob.length, 'upstream 1 MB', 400_000)
    expect(Buffer.concat(up).equals(blob)).toBe(true)
    const down: Buffer[] = []
    tls.on('data', (c: Buffer) => down.push(c))
    r.conns[0].socket.write(blob)
    await until(() => down.reduce((n, c) => n + c.length, 0) >= blob.length, 'downstream 1 MB', 400_000)
    expect(Buffer.concat(down).equals(blob)).toBe(true)
  })

  it('destroys a wrong key and an unknown identity', async () => {
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    await expect(dialOver(r, A, randomBytes(32))).rejects.toBeTruthy()
    await expect(dialOver(r, B, key)).rejects.toBeTruthy()
    await until(() => r.dials.stats.failed >= 2, 'two failures counted')
    expect(r.conns).toHaveLength(0)
    expect(r.dials.pending).toBe(0)
  })

  it('100 idle upgrades never keep a real dial out: the pool gives way, oldest first', async () => {
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    const idle: WebSocket[] = []
    for (let i = 0; i < 100; i++) {
      const ws = new WebSocket(url(r))
      ws.on('error', () => {})
      cleanup.push(() => ws.terminate())
      idle.push(ws)
    }
    // Of 100 at once, those that arrive while 16 others still wait for their upgrade end the oldest of them: fewer than 100 upgrade.
    await until(() => r.dials.stats.accepted >= 16, 'upgrades accepted')
    expect(r.dials.pending).toBeLessThanOrEqual(NET_TUNNEL_PENDING_MAX)
    const { tls } = await dialOver(r, A, key)
    tls.on('error', () => {})
    await until(() => r.conns.length === 1, 'the real dial got in')
    expect(r.dials.stats.evicted).toBeGreaterThan(0)
  })

  it('stop() ends a live dial, and a removed key does too', async () => {
    const key = randomBytes(32)
    const r = await listening(rig([[A, key], [B, randomBytes(32)]]))
    const { ws } = await dialOver(r, A, key)
    await until(() => r.conns.length === 1)
    const closed = new Promise<void>((resolve) => ws.once('close', () => resolve()))
    r.keys.remove(A)
    await closed
    const { ws: ws2 } = await dialOver(r, B, r.keys.get(B)!)
    await until(() => r.conns.length === 2)
    const closed2 = new Promise<void>((resolve) => ws2.once('close', () => resolve()))
    await r.tunnel.stop()
    await closed2
    expect(r.tunnel.listening).toBe(false)
  })
})

describe('TunnelDials: what the endpoint refuses', () => {
  const refusedWith = async (r: Rig, options: http.RequestOptions): Promise<{ status: number | undefined; closed: boolean }> =>
    new Promise((resolve) => {
      const req = http.request({ host: '127.0.0.1', port: r.port(), agent: false, ...options })
      req.on('response', (res) => {
        res.resume()
        res.once('close', () => resolve({ status: res.statusCode, closed: true }))
      })
      req.on('error', () => resolve({ status: undefined, closed: true }))
      req.end()
    })

  it.each([
    ['GET', '/relay'],
    ['GET', '/'],
    ['POST', '/relay'],
    ['GET', '/api/dial'],
    ['HEAD', '/relay'],
  ])('answers a plain %s %s with 404 and closes; nothing reaches NetDials', async (method, path) => {
    const r = await listening(rig())
    const out = await refusedWith(r, { method, path })
    expect(out).toEqual({ status: 404, closed: true })
    expect(r.dials.stats.accepted).toBe(0)
  })

  it.each(['/relay/', '/Relay', '/relay?x=1', '/relay%2f', '//relay', '/other', '/'])('refuses a WebSocket upgrade to %s', async (path) => {
    const r = await listening(rig())
    await expect(wsOpen(r, {}, path)).rejects.toThrow(/HTTP 404|socket hang up|ECONNRESET/)
    expect(r.dials.stats.accepted).toBe(0)
  })

  it('refuses a request that carries an Origin header (a web page, not the phone app)', async () => {
    const r = await listening(rig())
    await expect(wsOpen(r, { headers: { Origin: 'https://evil.example' } })).rejects.toThrow(/HTTP 404|socket hang up|ECONNRESET/)
    expect(r.dials.stats.accepted).toBe(0)
  })

  it('refuses a request whose headers are over the size limit', async () => {
    const r = await listening(rig())
    await expect(wsOpen(r, { headers: { 'X-Pad': 'a'.repeat(20_000) } })).rejects.toBeTruthy()
    expect(r.dials.stats.accepted).toBe(0)
  })

  it('drops a connection that has not finished its upgrade request 3 s after connect, not before', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    const r = await listening(rig())
    const c = rawClient(r)
    await new Promise<void>((resolve) => c.socket.once('connect', () => resolve()))
    c.socket.write('GET /relay HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n') // the blank line never comes
    await vi.advanceTimersByTimeAsync(TUNNEL_UPGRADE_DEADLINE_MS - 1)
    expect(c.closed()).toBe(false)
    await vi.advanceTimersByTimeAsync(1)
    await until(c.closed, 'closed by the endpoint')
    expect(TUNNEL_UPGRADE_DEADLINE_MS).toBe(3000)
  })

  it('drops a connection that sends nothing at all after 3 s', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    const r = await listening(rig())
    const c = rawClient(r)
    await new Promise<void>((resolve) => c.socket.once('connect', () => resolve()))
    await vi.advanceTimersByTimeAsync(TUNNEL_UPGRADE_DEADLINE_MS)
    await until(c.closed, 'closed by the endpoint')
  })

  it('refuses an upgraded connection that has not begun the TLS handshake within the 3 s of NetDials', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    const r = await listening(rig())
    const ws = new WebSocket(url(r))
    ws.on('error', () => {})
    cleanup.push(() => ws.terminate())
    const closed = new Promise<void>((resolve) => ws.once('close', () => resolve()))
    await until(() => ws.readyState === WebSocket.OPEN, 'upgraded')
    await vi.advanceTimersByTimeAsync(NET_HANDSHAKE_DEADLINE_MS)
    await closed
    expect(r.dials.stats.failed).toBe(1)
  })

  it('closes on a message over the size limit (code 1009), and the dial never connects', async () => {
    const r = await listening(rig())
    const ws = await wsOpen(r)
    const code = new Promise<number>((resolve) => ws.once('close', (c) => resolve(c)))
    ws.send(Buffer.alloc(TUNNEL_MAX_MESSAGE_BYTES + 1, 7), { binary: true })
    expect(await code).toBe(1009)
    expect(r.conns).toHaveLength(0)
  })

  it('accepts a message exactly at the size limit', async () => {
    const r = await listening(rig())
    const ws = await wsOpen(r)
    let closed = false
    ws.once('close', () => {
      closed = true
    })
    ws.send(Buffer.alloc(TUNNEL_MAX_MESSAGE_BYTES, 7), { binary: true }) // not a TLS hello: NetDials ends it, not the size rule
    await until(() => r.dials.stats.failed >= 1, 'handshake failure, not a size close')
    await until(() => closed, 'closed after the failed handshake')
  })

  it('closes on a text message before the handshake (code 1003)', async () => {
    const r = await listening(rig())
    const ws = await wsOpen(r)
    const code = new Promise<number>((resolve) => ws.once('close', (c) => resolve(c)))
    ws.send('hello')
    expect(await code).toBe(1003)
  })

  it('closes a live dial on a text message in the middle of a binary stream, and the link ends', async () => {
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    const { ws } = await dialOver(r, A, key)
    await until(() => r.conns.length === 1)
    const code = new Promise<number>((resolve) => ws.once('close', (c) => resolve(c)))
    const linkClosed = new Promise<void>((resolve) => r.conns[0].socket.once('close', () => resolve()))
    ws.send('text in a binary stream')
    expect(await code).toBe(1003)
    await linkClosed
  })

  it(`holds at most ${TUNNEL_PREUPGRADE_MAX} connections that have not upgraded; the oldest gives way (review B3 medium 1)`, async () => {
    const r = await listening(rig())
    const clients = Array.from({ length: 40 }, () => rawClient(r))
    await until(() => clients.filter((c) => c.closed()).length >= 40 - TUNNEL_PREUPGRADE_MAX, 'the surplus was closed')
    await new Promise((resolve) => setTimeout(resolve, 50))
    expect(clients.filter((c) => c.closed()).length).toBe(40 - TUNNEL_PREUPGRADE_MAX)
    const last = rawClient(r)
    await until(() => clients.filter((c) => c.closed()).length === 40 - TUNNEL_PREUPGRADE_MAX + 1, 'one more gave way')
    expect(last.closed()).toBe(false)
    expect(TUNNEL_PREUPGRADE_MAX).toBe(16)
  })

  it('a real upgrade still gets in after a flood of 100 silent connections', async () => {
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    for (let i = 0; i < 100; i++) rawClient(r)
    await new Promise((resolve) => setTimeout(resolve, 50))
    const { tls } = await dialOver(r, A, key)
    tls.on('error', () => {})
    await until(() => r.conns.length === 1, 'the real dial got in')
  })
})

describe('TunnelDials: a silent peer is cut off, and a ping flood is not echoed (review B3 low 2 and 3)', () => {
  const fakeTimers = () => vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval'] })

  it('pings an authenticated link every TUNNEL_PING_MS and ends one that answers nothing, and the dial link ends with it', async () => {
    fakeTimers()
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    const { ws } = await dialOver(r, A, key, { autoPong: false })
    await until(() => r.conns.length === 1)
    let pings = 0
    ws.on('ping', () => pings++)
    const linkClosed = new Promise<void>((resolve) => r.conns[0].socket.once('close', () => resolve()))
    await vi.advanceTimersByTimeAsync(TUNNEL_PING_MS)
    await until(() => pings === 1, 'the first ping')
    expect(ws.readyState).toBe(WebSocket.OPEN)
    await vi.advanceTimersByTimeAsync(TUNNEL_PING_MS)
    await linkClosed
    expect(r.dials.authenticated).toBe(0)
    expect(TUNNEL_PING_MS).toBe(20_000)
  })

  it('keeps a link whose peer answers pings, over five intervals', async () => {
    fakeTimers()
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    const { ws } = await dialOver(r, A, key)
    await until(() => r.conns.length === 1)
    let pings = 0
    ws.on('ping', () => pings++)
    for (let i = 0; i < 5; i++) {
      await vi.advanceTimersByTimeAsync(TUNNEL_PING_MS)
      await until(() => pings === i + 1, `ping ${i + 1}`)
      await new Promise((resolve) => setImmediate(resolve))
    }
    expect(ws.readyState).toBe(WebSocket.OPEN)
    expect(r.conns[0].socket.destroyed).toBe(false)
  })

  it('counts any data from the peer as a sign of life, even with pongs off', async () => {
    fakeTimers()
    const key = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    const { ws, tls } = await dialOver(r, A, key, { autoPong: false })
    await until(() => r.conns.length === 1)
    let got = 0
    r.conns[0].socket.on('data', (c: Buffer) => {
      got += c.length
    })
    for (let i = 0; i < 4; i++) {
      await vi.advanceTimersByTimeAsync(TUNNEL_PING_MS)
      tls.write('x')
      await until(() => got === i + 1, `data ${i + 1}`)
    }
    expect(ws.readyState).toBe(WebSocket.OPEN)
  })

  it('answers a flood of 1,000 pings with one pong a second, and stays open', async () => {
    const clock = { t: 1_000_000 }
    const r = await listening(rig([[A, randomBytes(32)]], { now: () => clock.t }))
    const ws = await wsOpen(r)
    let pongs = 0
    ws.on('pong', () => pongs++)
    for (let i = 0; i < 1000; i++) ws.ping()
    await new Promise((resolve) => setTimeout(resolve, 150))
    expect(pongs).toBe(1)
    expect(ws.readyState).toBe(WebSocket.OPEN)
    clock.t += TUNNEL_PONG_MIN_MS
    ws.ping()
    await until(() => pongs === 2, 'a ping a second later is answered')
    expect(TUNNEL_PONG_MIN_MS).toBe(1000)
  })
})

describe('TunnelDials: the limits that each guard sets', () => {
  it('does not negotiate compression, even when the client asks for it', async () => {
    const r = await listening(rig())
    const ws = await wsOpen(r, { perMessageDeflate: true })
    expect(ws.extensions).toBe('')
  })

  it('closes on a message sent as fragments that add up to more than the limit (1009)', async () => {
    const r = await listening(rig())
    const ws = await wsOpen(r)
    const code = new Promise<number>((resolve) => ws.once('close', (c) => resolve(c)))
    const part = Buffer.alloc(30 * 1024, 7)
    ws.send(part, { binary: true, fin: false })
    ws.send(part, { binary: true, fin: false })
    ws.send(part, { binary: true, fin: true })
    expect(await code).toBe(1009)
  })

  it('stops reading a peer that sends faster than the stream is read: the buffer stays under 4 x 64 KiB, and nothing is lost when reading resumes', async () => {
    const held: Duplex[] = []
    const r = await listening(
      rig([[A, randomBytes(32)]], {
        dials: {
          acceptStream: (stream) => {
            stream.on('error', () => {})
            held.push(stream)
          },
        },
      }),
    )
    const ws = await wsOpen(r)
    await until(() => held.length === 1, 'the stream reached the acceptor')
    const total = 1024
    const message = Buffer.alloc(16 * 1024, 5)
    for (let i = 0; i < total; i++) ws.send(message, { binary: true })
    await new Promise((resolve) => setTimeout(resolve, 400))
    expect(held[0].readableLength).toBeLessThan(4 * TUNNEL_STREAM_BUFFER_BYTES)
    let got = 0
    held[0].on('data', (c: Buffer) => {
      got += c.length
    })
    await until(() => got === total * message.length, 'every byte arrived once reading resumed', 2_000_000)
  })

  it('binds ::1 when asked to (skipped where IPv6 loopback is missing)', async (ctx) => {
    const ok = await new Promise<boolean>((resolve) => {
      const probe = net.createServer()
      probe.once('error', () => resolve(false))
      probe.listen(0, '::1', () => probe.close(() => resolve(true)))
    })
    if (!ok) return ctx.skip()
    const r = await listening(rig([[A, randomBytes(32)]], { bind: '::1' }))
    expect(r.tunnel.address()?.address).toBe('::1')
  })

  it('refuses to stay open when the system reports a bound address that is not loopback', async () => {
    const port = await freePort()
    const spy = vi.spyOn(net.Server.prototype, 'address').mockReturnValue({ address: '0.0.0.0', family: 'IPv4', port })
    const r = rig([[A, randomBytes(32)]], { port })
    await r.tunnel.settled()
    spy.mockRestore()
    expect(r.tunnel.listening).toBe(false)
    expect(r.logs.filter((l) => /bound address is not loopback/.test(l))).toHaveLength(1)
    expect(await connectRefused(port)).toBe(true)
  })

  it('sends the 1003 close frame to a peer that sent text, every time (review B3 low 4)', async () => {
    const r = await listening(rig())
    for (let i = 0; i < 25; i++) {
      const ws = await wsOpen(r)
      const code = new Promise<number>((resolve) => ws.once('close', (c) => resolve(c)))
      ws.send('text')
      expect(await code).toBe(1003)
    }
  })
})

describe('TunnelDials: nothing secret in the log', () => {
  it('no line holds a key, a header value or a path the peer chose', async () => {
    const key = randomBytes(32)
    const other = randomBytes(32)
    const r = await listening(rig([[A, key]]))
    await dialOver(r, A, key)
    await expect(dialOver(r, A, other)).rejects.toBeTruthy()
    await expect(wsOpen(r, { headers: { Origin: 'https://secret-origin.example', 'X-Token': 'token-value-1234' } }, '/secret-path-xyz')).rejects.toBeTruthy()
    await r.tunnel.stop()
    expect(r.logs.length).toBeGreaterThan(1)
    for (const line of r.logs) {
      for (const secret of [key, other]) {
        expect(line).not.toContain(secret.toString('hex'))
        expect(line).not.toContain(secret.toString('base64'))
      }
      expect(line).not.toMatch(/secret-origin|token-value|secret-path/)
    }
  })
})
