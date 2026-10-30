// The Wi-Fi listener for dials (netDials.ts), against real TLS-PSK clients on loopback.
//
// Other "hosts" are other loopback addresses (127.0.0.2 and up): a client binds one with `localAddress`. Slow cases use
// fake timers for the 3 s handshake deadline and an injected clock for the 60 s / 30 s / 7 day rules, so the suite is fast.
import { randomBytes } from 'node:crypto'
import { mkdtempSync, rmSync } from 'node:fs'
import * as net from 'node:net'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { FakeNetDial } from './__fixtures__/fakeNetDial.js'
import { pskConnect } from './__fixtures__/pskClient.js'
import { CableType, encodeCableFrame } from './cableFrame.js'
import { DIAL_KEY_FILE, DialKeys } from './dialKeys.js'
import {
  AddressThrottle,
  NET_AUTH_MAX,
  NET_BAN_MS,
  NET_FAIL_LIMIT,
  NET_HANDSHAKE_DEADLINE_MS,
  NET_PENDING_MAX,
  NetDials,
  NetSlot,
  isPrivatePeer,
  type NetDialConnection,
  type NetDialsOptions,
} from './netDials.js'

const A = 'B8:1F:3F:CA:E5:58'
const B = 'B8:1F:3F:CA:E5:59'
const DAY = 24 * 60 * 60 * 1000

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
  dials: NetDials
  keys: DialKeys
  logs: string[]
  conns: NetDialConnection[]
  port: () => number
  clock: { t: number }
}

function rig(extra: Partial<NetDialsOptions> = {}, keyIds: Array<[string, Buffer]> = []): Rig {
  const dir = mkdtempSync(join(tmpdir(), 'net-dials-'))
  cleanup.push(() => rmSync(dir, { recursive: true, force: true }))
  const clock = { t: 1_000_000_000 }
  const keys = new DialKeys(join(dir, DIAL_KEY_FILE), { now: () => clock.t })
  for (const [id, key] of keyIds) keys.set(id, key)
  const logs: string[] = []
  const conns: NetDialConnection[] = []
  const dials = new NetDials({
    keys,
    port: 0,
    bind: '127.0.0.1',
    log: (line) => logs.push(line),
    onDial: (conn) => {
      conns.push(conn)
      conn.socket.on('error', () => {})
    },
    now: () => clock.t,
    ...extra,
  })
  cleanup.push(() => dials.stop())
  dials.start()
  return { dials, keys, logs, conns, port: () => dials.address()!.port, clock }
}

async function listening(r: Rig): Promise<Rig> {
  await r.dials.settled()
  expect(r.dials.listening).toBe(true)
  return r
}

const connect = (r: Rig, id: string, psk: Buffer, localAddress?: string) => pskConnect({ port: r.port(), identity: id, psk, localAddress })

/** A raw TCP connection that is closed at the far end when the listener destroys it. */
function rawClient(r: Rig, localAddress = '127.0.0.1'): { socket: net.Socket; closed: () => boolean } {
  const socket = net.connect({ port: r.port(), host: '127.0.0.1', localAddress })
  socket.on('error', () => {})
  let closed = false
  socket.once('close', () => {
    closed = true
  })
  cleanup.push(() => socket.destroy())
  return { socket, closed: () => closed }
}

/** A client that gets as far as the first bytes and no further: a failed handshake, fast. */
async function garbage(r: Rig, localAddress: string): Promise<void> {
  const c = rawClient(r, localAddress)
  await new Promise<void>((resolve) => c.socket.once('connect', () => resolve()))
  c.socket.write('GET / HTTP/1.1\r\nHost: x\r\n\r\n')
  await until(c.closed, 'garbage client closed')
}

describe('isPrivatePeer', () => {
  it.each([
    ['10.0.0.1', true],
    ['10.255.255.255', true],
    ['172.16.0.1', true],
    ['172.31.255.255', true],
    ['192.168.1.40', true],
    ['169.254.10.20', true],
    ['127.0.0.1', true],
    ['127.0.0.2', true],
    ['::ffff:192.168.1.40', true],
    ['::1', true],
    ['172.15.255.255', false],
    ['172.32.0.1', false],
    ['8.8.8.8', false],
    ['100.64.0.1', false],
    ['192.169.0.1', false],
    ['::ffff:8.8.8.8', false],
    ['2001:db8::1', false],
    ['', false],
    ['not an address', false],
  ])('%s -> %s', (addr, expected) => {
    expect(isPrivatePeer(addr)).toBe(expected)
  })
})

describe('AddressThrottle', () => {
  it('bans an address after 20 failures in 60 s, for 30 s, and not before', () => {
    let t = 0
    const throttle = new AddressThrottle(() => t)
    for (let i = 0; i < NET_FAIL_LIMIT - 1; i++) throttle.failed('10.0.0.9')
    expect(throttle.blocked('10.0.0.9')).toBe(false)
    throttle.failed('10.0.0.9')
    expect(throttle.blocked('10.0.0.9')).toBe(true)
    // only that address
    expect(throttle.blocked('10.0.0.8')).toBe(false)
    t += NET_BAN_MS - 1
    expect(throttle.blocked('10.0.0.9')).toBe(true)
    t += 1
    expect(throttle.blocked('10.0.0.9')).toBe(false)
  })

  it('never bans for longer than 60 s, however many failures follow', () => {
    let t = 0
    const throttle = new AddressThrottle(() => t)
    for (let i = 0; i < 500; i++) throttle.failed('10.0.0.9')
    expect(NET_BAN_MS).toBeLessThanOrEqual(60_000)
    t += 60_000
    expect(throttle.blocked('10.0.0.9')).toBe(false)
  })

  it('counts failures in a sliding 60 s window: slow noise never trips it', () => {
    let t = 0
    const throttle = new AddressThrottle(() => t)
    for (let i = 0; i < 200; i++) {
      throttle.failed('10.0.0.9')
      t += 3200
    }
    expect(throttle.blocked('10.0.0.9')).toBe(false)
  })

  it('stays small under a flood of different addresses', () => {
    const throttle = new AddressThrottle(() => 0)
    for (let i = 0; i < 20_000; i++) throttle.failed(`10.${(i >> 8) & 255}.${i & 255}.1`)
    expect(throttle.tracked()).toBeLessThanOrEqual(4096)
  })
})

describe('NetDials: when the port is open', () => {
  it('does not open the port with an empty keystore, opens after the first key, closes after the last', async () => {
    const r = rig()
    await r.dials.settled()
    expect(r.dials.listening).toBe(false)
    expect(r.dials.address()).toBeNull()
    r.keys.set(A, randomBytes(32))
    await r.dials.settled()
    expect(r.dials.listening).toBe(true)
    const port = r.port()
    expect(port).toBeGreaterThan(0)
    expect(r.logs.filter((l) => /listening/.test(l))).toHaveLength(1)
    // a second key leaves it open
    r.keys.set(B, randomBytes(32))
    await r.dials.settled()
    expect(r.port()).toBe(port)
    r.keys.remove(A)
    await r.dials.settled()
    expect(r.dials.listening).toBe(true)
    r.keys.remove(B)
    await r.dials.settled()
    expect(r.dials.listening).toBe(false)
    // and nothing answers there any more
    await expect(new Promise((resolve, reject) => net.connect(port, '127.0.0.1').once('connect', resolve).once('error', reject))).rejects.toMatchObject({ code: 'ECONNREFUSED' })
  })

  it('never opens when switched off, whatever the keystore holds', async () => {
    const r = rig({ enabled: false }, [[A, randomBytes(32)]])
    await r.dials.settled()
    expect(r.dials.listening).toBe(false)
    r.keys.set(B, randomBytes(32))
    await r.dials.settled()
    expect(r.dials.listening).toBe(false)
  })

  it('opens at start when the keystore already has a dial', async () => {
    const r = await listening(rig({}, [[A, randomBytes(32)]]))
    expect(r.port()).toBeGreaterThan(0)
  })

  it('on a port clash says so once and leaves the rest alone', async () => {
    const holder = net.createServer()
    await new Promise<void>((resolve) => holder.listen(0, '127.0.0.1', resolve))
    cleanup.push(() => holder.close())
    const taken = (holder.address() as net.AddressInfo).port
    const r = rig({ port: taken }, [[A, randomBytes(32)]])
    await r.dials.settled()
    expect(r.dials.listening).toBe(false)
    r.keys.set(B, randomBytes(32))
    await r.dials.settled()
    const clash = r.logs.filter((l) => /EADDRINUSE|in use/.test(l))
    expect(clash).toHaveLength(1)
    expect(clash[0]).toMatch(/USB/)
    expect(r.dials.listening).toBe(false)
  })
})

describe('NetDials: who gets in', () => {
  it('hands over a dial that proves its key, with its identity and address, and records where it was seen', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const socket = await connect(r, A.toLowerCase(), key)
    cleanup.push(() => socket.destroy())
    await until(() => r.conns.length === 1, 'onDial')
    expect(r.conns[0].id).toBe(A)
    expect(r.conns[0].peer).toBe('127.0.0.1')
    expect(r.keys.record(A)?.lastAddr).toBe('127.0.0.1')
    expect(r.keys.isKnownAddr('127.0.0.1')).toBe(true)
    // and bytes go both ways
    const echoed = new Promise<string>((resolve) => socket.once('data', (b) => resolve(b.toString())))
    r.conns[0].socket.write('welcome')
    expect(await echoed).toBe('welcome')
  })

  it('destroys and counts a wrong key, an unknown identity, garbage and a plain-TCP silence', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    // wrong key
    await expect(connect(r, A, randomBytes(32))).rejects.toBeTruthy()
    // unknown identity
    await expect(connect(r, B, key)).rejects.toBeTruthy()
    // not TLS at all
    await garbage(r, '127.0.0.1')
    await until(() => r.dials.stats.failed >= 3, 'three failures counted')
    expect(r.conns).toHaveLength(0)
    expect(r.dials.stats.authenticated).toBe(0)
    expect(r.dials.pending).toBe(0)
  })

  it('drops a peer that has not finished the handshake after 3 s, not before (slow loris)', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    const r = await listening(rig({}, [[A, randomBytes(32)]]))
    const c = rawClient(r)
    await until(() => r.dials.pending === 1, 'accepted')
    await vi.advanceTimersByTimeAsync(NET_HANDSHAKE_DEADLINE_MS - 1)
    expect(c.closed()).toBe(false)
    expect(r.dials.pending).toBe(1)
    await vi.advanceTimersByTimeAsync(1)
    await until(c.closed, 'closed by the listener')
    expect(r.dials.pending).toBe(0)
    expect(r.dials.stats.failed).toBe(1)
    expect(NET_HANDSHAKE_DEADLINE_MS).toBe(3000)
  })

  it('refuses a public peer address without reading a byte', async () => {
    const key = randomBytes(32)
    const refuse = new Set(['127.0.0.9'])
    const r = await listening(rig({ isPrivatePeer: (addr) => !refuse.has(addr) }, [[A, key]]))
    await expect(connect(r, A, key, '127.0.0.9')).rejects.toBeTruthy()
    expect(r.dials.stats.refusedPeer).toBe(1)
    expect(r.conns).toHaveLength(0)
    // the same dial from a private address is fine
    const ok = await connect(r, A, key, '127.0.0.1')
    ok.destroy()
  })

  it('lets a second authenticated socket for one identity replace the first', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const first = await connect(r, A, key)
    cleanup.push(() => first.destroy())
    first.on('error', () => {})
    await until(() => r.conns.length === 1)
    const firstClosed = new Promise<void>((resolve) => first.once('close', () => resolve()))
    const second = await connect(r, A, key)
    cleanup.push(() => second.destroy())
    await until(() => r.conns.length === 2)
    await firstClosed
    expect(r.conns[0].socket.destroyed).toBe(true)
    expect(r.conns[1].socket.destroyed).toBe(false)
    expect(r.dials.stats.replaced).toBe(1)
    expect(r.dials.authenticated).toBe(1)
  })

  it('keeps at most 8 authenticated dials', async () => {
    const ids = Array.from({ length: NET_AUTH_MAX + 1 }, (_, i) => `B8:1F:3F:CA:E5:${(0x10 + i).toString(16).toUpperCase()}`)
    const keyOf = new Map(ids.map((id) => [id, randomBytes(32)] as const))
    const r = await listening(rig({}, [...keyOf]))
    const live: Array<Awaited<ReturnType<typeof connect>>> = []
    for (const id of ids.slice(0, NET_AUTH_MAX)) {
      const s = await connect(r, id, keyOf.get(id)!)
      s.on('error', () => {})
      live.push(s)
      cleanup.push(() => s.destroy())
    }
    await until(() => r.conns.length === NET_AUTH_MAX)
    const ninth = await connect(r, ids[NET_AUTH_MAX], keyOf.get(ids[NET_AUTH_MAX])!).catch(() => null)
    ninth?.on('error', () => {})
    await until(() => r.dials.stats.overLimit === 1, 'ninth refused')
    expect(r.conns).toHaveLength(NET_AUTH_MAX)
    // a dial already in is not counted twice: it reconnects fine
    const again = await connect(r, ids[0], keyOf.get(ids[0])!)
    again.on('error', () => {})
    cleanup.push(() => again.destroy())
    await until(() => r.conns.length === NET_AUTH_MAX + 1)
  })

  it('drops a dial whose key was removed or replaced', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key], [B, randomBytes(32)]]))
    const s = await connect(r, A, key)
    s.on('error', () => {})
    cleanup.push(() => s.destroy())
    await until(() => r.conns.length === 1)
    const gone = new Promise<void>((resolve) => s.once('close', () => resolve()))
    r.keys.set(A, randomBytes(32))
    await gone
    expect(r.dials.authenticated).toBe(0)
    // and the old key now fails the handshake
    await expect(connect(r, A, key)).rejects.toBeTruthy()
  })
})

describe('NetDials: limits on unauthenticated peers (spec 4.2)', () => {
  it('keeps at most 16 pending sockets and evicts the oldest to admit a new one', async () => {
    const r = await listening(rig({}, [[A, randomBytes(32)]]))
    const clients: Array<ReturnType<typeof rawClient>> = []
    for (let i = 0; i < NET_PENDING_MAX; i++) {
      clients.push(rawClient(r, '127.0.0.2'))
      await until(() => r.dials.pending === i + 1 || r.dials.stats.evicted > 0)
    }
    expect(r.dials.pending).toBe(NET_PENDING_MAX)
    expect(clients[0].closed()).toBe(false)
    const newest = rawClient(r, '127.0.0.2')
    await until(() => clients[0].closed(), 'the oldest evicted')
    expect(r.dials.pending).toBe(NET_PENDING_MAX)
    expect(clients[1].closed()).toBe(false)
    expect(newest.closed()).toBe(false)
    expect(r.dials.stats.evicted).toBe(1)
  })

  it('100 idle sockets from one noise host never keep a dial out', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const noise = Array.from({ length: 100 }, () => rawClient(r, '127.0.0.2'))
    await until(() => noise.filter((n) => n.closed()).length >= 100 - NET_PENDING_MAX, 'the pool churned')
    const t0 = Date.now()
    const dial = await connect(r, A, key, '127.0.0.1')
    cleanup.push(() => dial.destroy())
    dial.on('error', () => {})
    await until(() => r.conns.length === 1)
    expect(Date.now() - t0).toBeLessThan(1000)
    expect(r.dials.pending).toBeLessThanOrEqual(NET_PENDING_MAX)
  })

  it('50 failed handshakes from a noise host delay only that host, and for 30 s at most', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    for (let i = 0; i < 50; i++) await garbage(r, '127.0.0.2')
    // 20 were handshakes that failed; the rest were destroyed at accept
    expect(r.dials.stats.failed).toBe(NET_FAIL_LIMIT)
    expect(r.dials.stats.throttled).toBe(50 - NET_FAIL_LIMIT)
    expect(r.logs.some((l) => /127\.0\.0\.2/.test(l) && /throttl|refus/i.test(l))).toBe(true)
    // that host cannot even finish a handshake now, with the right key
    await expect(connect(r, A, key, '127.0.0.2')).rejects.toBeTruthy()
    // another host is untouched
    const dial = await connect(r, A, key, '127.0.0.1')
    dial.on('error', () => {})
    cleanup.push(() => dial.destroy())
    await until(() => r.conns.length === 1)
    // after the ban the noise host is looked at again
    r.clock.t += NET_BAN_MS
    const failedBefore = r.dials.stats.failed
    await garbage(r, '127.0.0.2')
    expect(r.dials.stats.failed).toBe(failedBefore + 1)
    expect(r.dials.stats.throttled).toBe(50 - NET_FAIL_LIMIT + 1)
  })

  it('never throttles an address that authenticated within 7 days, even right after 30 failures', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    r.keys.touch(A, '127.0.0.3')
    for (let i = 0; i < 30; i++) await garbage(r, '127.0.0.3')
    expect(r.dials.stats.throttled).toBe(0)
    const dial = await connect(r, A, key, '127.0.0.3')
    dial.on('error', () => {})
    cleanup.push(() => dial.destroy())
    await until(() => r.conns.length === 1)
    expect(r.conns[0].peer).toBe('127.0.0.3')
  })

  it('a known address that has been quiet for 7 days is an unknown address again', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    r.keys.touch(A, '127.0.0.3')
    r.clock.t += 7 * DAY
    for (let i = 0; i < NET_FAIL_LIMIT + 5; i++) await garbage(r, '127.0.0.3')
    expect(r.dials.stats.throttled).toBe(5)
  })

  it('a dial that reconnects 20 times while noise is at the door gets in every time, each in under 1 s', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const idle = Array.from({ length: 100 }, () => rawClient(r, '127.0.0.2'))
    for (let i = 0; i < 50; i++) await garbage(r, '127.0.0.4')
    expect(idle.length).toBe(100)
    const times: number[] = []
    for (let i = 0; i < 20; i++) {
      const t0 = Date.now()
      const dial = await connect(r, A, key, '127.0.0.1')
      dial.on('error', () => {})
      await until(() => r.conns.length === i + 1, `reconnect ${i}`)
      times.push(Date.now() - t0)
      dial.destroy()
      await until(() => r.dials.authenticated === 0, `reconnect ${i} closed`)
    }
    expect(r.conns).toHaveLength(20)
    expect(Math.max(...times)).toBeLessThan(1000)
  })
})

describe('NetDials: the key stays out of the log', () => {
  it('no line contains the key, in hex or base64, through successes and failures', async () => {
    const key = randomBytes(32)
    const other = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const ok = await connect(r, A, key)
    ok.on('error', () => {})
    cleanup.push(() => ok.destroy())
    await until(() => r.conns.length === 1)
    await expect(connect(r, A, other, '127.0.0.5')).rejects.toBeTruthy()
    await garbage(r, '127.0.0.6')
    for (let i = 0; i < NET_FAIL_LIMIT + 2; i++) await garbage(r, '127.0.0.7')
    r.keys.set(B, other)
    r.keys.remove(B)
    await r.dials.settled()
    expect(r.logs.length).toBeGreaterThan(3)
    for (const line of r.logs) {
      for (const secret of [key, other]) {
        expect(line).not.toContain(secret.toString('hex'))
        expect(line).not.toContain(secret.toString('base64'))
      }
    }
  })
})

describe('NetSlot: one dial, one session', () => {
  async function pair(r: Rig, id: string, key: Buffer): Promise<{ client: FakeNetDial; conn: NetDialConnection }> {
    const client = await FakeNetDial.connect({ port: r.port(), id, psk: key })
    cleanup.push(() => client.close())
    await until(() => r.conns.length > 0)
    return { client, conn: r.conns[r.conns.length - 1] }
  }

  it('gives the session the connection, and the bytes the dial sent before the session asked', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const { client, conn } = await pair(r, A, key)
    client.hello()
    await new Promise((resolve) => setTimeout(resolve, 100))
    const slot = new NetSlot(A)
    slot.offer(conn)
    const chunks: Buffer[] = []
    const port = await slot.opener((c) => chunks.push(c), () => {})
    expect(port).not.toBeNull()
    expect(port!.transport).toBe('wifi')
    expect(port!.peer).toBe('127.0.0.1')
    expect(port!.path).toMatch(/^wifi:127\.0\.0\.1:\d+$/)
    await until(() => chunks.length > 0, 'the hello reaches the session')
    expect(Buffer.concat(chunks).includes(Buffer.from('"t":"hello"'))).toBe(true)
    // and writing goes out to the dial
    await port!.write(encodeCableFrame(CableType.Json, Buffer.from('{"t":"welcome"}')))
    expect((await client.next('welcome')).t).toBe('welcome')
  })

  it('waits up to 2.5 s for a connection and then answers null', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    const slot = new NetSlot(A)
    const result = slot.opener(() => {}, () => {})
    let settled = false
    void result.then(() => {
      settled = true
    })
    await vi.advanceTimersByTimeAsync(2499)
    expect(settled).toBe(false)
    await vi.advanceTimersByTimeAsync(1)
    expect(await result).toBeNull()
  })

  it('hands a connection that arrives while the session waits straight over', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const slot = new NetSlot(A)
    const waiting = slot.opener(() => {}, () => {})
    const { conn } = await pair(r, A, key)
    slot.offer(conn)
    const port = await waiting
    expect(port?.isOpen).toBe(true)
  })

  it('the newest connection wins when two wait', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const slot = new NetSlot(A)
    const first = await pair(r, A, key)
    slot.offer(first.conn)
    const second = await pair(r, A, key)
    slot.offer(second.conn)
    const port = await slot.opener(() => {}, () => {})
    expect(port?.isOpen).toBe(true)
    expect(first.conn.socket.destroyed).toBe(true)
  })

  it('reports close once, from either side, and refuses writes after it', async () => {
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const { client, conn } = await pair(r, A, key)
    const slot = new NetSlot(A)
    slot.offer(conn)
    const closed: string[] = []
    const port = (await slot.opener(() => {}, (why) => closed.push(why)))!
    await port.close('by the session')
    await port.close('again')
    expect(closed).toEqual(['by the session'])
    expect(port.isOpen).toBe(false)
    await expect(port.write(Buffer.from('x'))).rejects.toThrow()
    await until(() => client.closed, 'the dial sees the close')

    const { conn: conn2, client: client2 } = await pair(r, A, key)
    slot.offer(conn2)
    const closed2: string[] = []
    const port2 = (await slot.opener(() => {}, (why) => closed2.push(why)))!
    client2.close()
    await until(() => closed2.length === 1, 'far end closed')
    expect(port2.isOpen).toBe(false)
    expect(closed2[0]).toMatch(/closed|end/)
  })

  it('skips a connection that died while it waited', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    const key = randomBytes(32)
    const r = await listening(rig({}, [[A, key]]))
    const { conn, client } = await pair(r, A, key)
    const slot = new NetSlot(A)
    slot.offer(conn)
    client.close()
    await until(() => conn.socket.destroyed, 'closed')
    const result = slot.opener(() => {}, () => {})
    await vi.advanceTimersByTimeAsync(2500)
    expect(await result).toBeNull()
  })

  it('close() on the slot ends a waiting opener and the live port', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    const slot = new NetSlot(A)
    const waiting = slot.opener(() => {}, () => {})
    slot.close('stopping')
    expect(await waiting).toBeNull()
  })
})
