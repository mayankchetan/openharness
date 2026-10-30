// The dial over Wi-Fi, daemon side (docs/specs/cable-wifi-link.md sections 4.2, 6.1 and 6.2; task W8).
//
// A dial that was given a key over USB connects here, speaks TLS-PSK (tlsPsk.ts) and then the same cable frames it
// speaks on USB. This file has three parts:
//
//   NetDials  the listener: the socket, the peer filter and the limits on peers that have not yet proved a key.
//   NetSlot   one dial's place in the fleet: it turns the connection the listener hands over into the CablePort a
//             CableSession opens, and keeps the bytes the dial sent before the session asked.
//   NetPort   that CablePort, over a TLS socket.
//
// The socket opens only when the keystore (dialKeys.ts) holds at least one dial: with no provisioned dial there is no
// open port. It closes again when the last key is removed. Nothing here is ever written to the dial's link before it has
// proved its key, so a scanner gets no banner. No mDNS: the dial stores the daemon's address when it is provisioned.
//
// Limits on unauthenticated peers (spec 4.2). They exist so that LAN noise can never lock out the dial that is provisioned:
//   - a socket that has not finished the handshake 3 s after accept is destroyed;
//   - at most 16 sockets wait in the pool, and at most 4 of them from one unknown address (a 5th connect from it is
//     destroyed at accept and counts as a failure for that address);
//   - when the pool is full a newcomer takes the place of the OLDEST socket of the unknown address that holds the MOST
//     pending sockets, so one address cannot push a slow dial out of the pool with a flood of its own. A pending socket of a
//     known address (below) is never evicted for a newcomer from an unknown one; when every waiting socket is a known
//     address's, a known newcomer evicts the oldest, and an unknown newcomer is the one refused. An eviction counts as a
//     failure for the evicted socket's address when that address held more than one pending socket, so a flooder earns
//     the ban below by its own churn;
//   - an address that authenticated in the last 7 days (the keystore's lastAddr / lastSeen) is never throttled;
//   - any other address that fails 20 handshakes within 60 s is destroyed at accept for 30 s (never more than 60 s);
//   - one authenticated socket per identity: a new one, which has proved the key, replaces the old.
// An address that holds a single pending socket is never blamed for losing it: that may be an honest dial.
import * as net from 'node:net'
import type * as tls from 'node:tls'

import type { CablePort, PortOpener } from './cableSession.js'
import type { DialKeys } from './dialKeys.js'
import { pskIdentityOf, wrapPskSocket } from './tlsPsk.js'

/** A peer that has not finished the TLS handshake this long after accept is dropped (tlsPsk.ts allows 5 s; this is stricter). */
export const NET_HANDSHAKE_DEADLINE_MS = 3_000
/** Sockets that have not yet proved a key. */
export const NET_PENDING_MAX = 16
/** Of those, from one unknown address. */
export const NET_PENDING_PER_ADDR = 4
/** Failed handshakes from one unknown address, within NET_FAIL_WINDOW_MS, that start a ban. */
export const NET_FAIL_LIMIT = 20
export const NET_FAIL_WINDOW_MS = 60_000
/** How long a ban lasts. Never more than a minute: there is no permanent or multi-minute ban. */
export const NET_BAN_MS = 30_000
/** Dials connected at once. */
export const NET_AUTH_MAX = 8
/** How long the session's opener waits for a connection before it says "none" (the session asks again every 2 s). */
export const NET_OPEN_WAIT_MS = 2_500
/** A write the far end has not taken in this long ends the link: a session awaiting it would otherwise wait for ever. */
const WRITE_TIMEOUT_MS = 10_000
/** Addresses remembered by the throttle, for failures and for bans each. */
const THROTTLE_TRACKED_MAX = 2048
/** The same failure is logged once per address per this long, so noise cannot fill the log. */
const LOG_EVERY_MS = 60_000

/** Is this a private (RFC 1918), link-local or loopback address? A router port-forward shows a public source and is refused. */
export function isPrivatePeer(address: string): boolean {
  const addr = address.startsWith('::ffff:') ? address.slice(7) : address
  if (net.isIPv4(addr)) {
    const [a, b] = addr.split('.').map(Number)
    // Carrier-grade NAT (100.64.0.0/10) is deliberately refused: it is another subscriber's network, not this home's.
    return a === 10 || a === 127 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254)
  }
  if (net.isIPv6(addr)) {
    if (addr === '::1') return true
    // The first hextet as a number, so "fd::1" (0x00fd, a public-range address) is not read as the unique-local fd00::/8.
    const first = addr.split(':')[0]
    if (first === '') return false
    const h = parseInt(first, 16)
    return (h & 0xfe00) === 0xfc00 || (h & 0xffc0) === 0xfe80
  }
  return false
}

/** Failed handshakes per address, and the short bans they earn. Time comes from the caller, so tests need no waiting. */
export class AddressThrottle {
  readonly #fails = new Map<string, number[]>()
  readonly #bans = new Map<string, number>()

  constructor(private readonly now: () => number) {}

  blocked(addr: string): boolean {
    const until = this.#bans.get(addr)
    if (until === undefined) return false
    if (this.now() < until) return true
    this.#bans.delete(addr)
    return false
  }

  /** One more failed handshake from `addr`. True when this one starts a ban. */
  failed(addr: string): boolean {
    const now = this.now()
    const recent = (this.#fails.get(addr) ?? []).filter((at) => now - at < NET_FAIL_WINDOW_MS)
    recent.push(now)
    this.#fails.delete(addr)
    if (recent.length >= NET_FAIL_LIMIT) {
      this.#bans.set(addr, now + NET_BAN_MS)
      this.#trim(this.#bans)
      return true
    }
    this.#fails.set(addr, recent)
    this.#trim(this.#fails)
    return false
  }

  /** How many addresses are remembered; bounded, so a flood from many addresses costs a fixed amount of memory. */
  tracked(): number {
    return this.#fails.size + this.#bans.size
  }

  #trim(map: Map<string, unknown>): void {
    while (map.size > THROTTLE_TRACKED_MAX) map.delete(map.keys().next().value as string)
  }
}

/** A dial that proved its key: who it is, where it came from, and the TLS socket to speak cable frames over. */
export interface NetDialConnection {
  /** The dial's MAC string, upper case. */
  id: string
  /** The peer's IP address. */
  peer: string
  /** The peer's TCP port. */
  port: number
  socket: tls.TLSSocket
}

export interface NetDialsOptions {
  keys: DialKeys
  /** TCP port. 0 picks a free one (tests). */
  port: number
  /** Address to bind; all IPv4 interfaces when unset. */
  bind?: string
  /** False never opens the socket (`CABLE_NET_ENABLE=false`). */
  enabled?: boolean
  log: (line: string) => void
  /**
   * A dial finished the handshake. The socket is paused: it delivers nothing until the receiver reads it. A throw, or
   * `false`, means nobody took it: the socket is destroyed.
   */
  onDial: (connection: NetDialConnection) => void | boolean
  /** Replace the private-peer filter (tests: loopback is private, so a "public" host has to be named). */
  isPrivatePeer?: (address: string) => boolean
  /** Milliseconds clock for the 60 s / 30 s / 7 day rules. */
  now?: () => number
}

interface Pending {
  raw: net.Socket
  tls: tls.TLSSocket
  peer: string
  port: number
  timer: ReturnType<typeof setTimeout> | undefined
  /** The handshake has ended one way or the other; nothing more is counted for this socket. */
  settled: boolean
  /** The identity the peer named in its handshake, once the key lookup has run (upper case). */
  claimed?: string
  /** The key that lookup handed the handshake: a dial whose key changed since is refused when it finishes. */
  keyUsed?: Buffer
}

export class NetDials {
  readonly stats = { accepted: 0, refusedPeer: 0, throttled: 0, failed: 0, evicted: 0, evictionFailures: 0, capped: 0, revoked: 0, authenticated: 0, replaced: 0, overLimit: 0 }
  readonly #opts: NetDialsOptions
  readonly #throttle: AddressThrottle
  readonly #pending = new Set<Pending>()
  readonly #auth = new Map<string, Pending>()
  readonly #loggedAt = new Map<string, number>()
  readonly #now: () => number
  #server: net.Server | null = null
  /** The port could not be taken. One line was logged; it is not tried again until the daemon restarts. */
  #clashed = false
  #stopped = true
  #work: Promise<void> = Promise.resolve()
  #off: (() => void) | null = null

  constructor(opts: NetDialsOptions) {
    this.#opts = opts
    this.#now = opts.now ?? Date.now
    this.#throttle = new AddressThrottle(this.#now)
  }

  /** Is the socket open? */
  get listening(): boolean {
    return this.#server !== null
  }

  /** Sockets that have not yet proved a key. */
  get pending(): number {
    return this.#pending.size
  }

  /** Dials connected now. */
  get authenticated(): number {
    return this.#auth.size
  }

  address(): net.AddressInfo | null {
    const address = this.#server?.address()
    return address && typeof address === 'object' ? address : null
  }

  /** Follow the keystore: open the socket when it has a dial, close it when it has none. */
  start(): void {
    if (!this.#stopped) return
    this.#stopped = false
    this.#clashed = false
    this.#off = this.#opts.keys.onChange((change) => {
      const live = this.#auth.get(change.id)
      if (live) this.#drop(live, change.kind === 'remove' ? 'its key was removed' : 'its key was replaced')
      // A handshake in flight with the old key must not finish into a session.
      for (const waiting of [...this.#pending]) {
        if (waiting.claimed !== change.id) continue
        this.stats.revoked++
        this.#drop(waiting, 'its key changed')
      }
      this.#reconcile()
    })
    this.#reconcile()
  }

  async stop(): Promise<void> {
    this.#stopped = true
    this.#off?.()
    this.#off = null
    this.#reconcile()
    await this.settled()
  }

  /** Resolves when the socket is open or shut as the keystore says. */
  settled(): Promise<void> {
    return this.#work
  }

  #reconcile(): void {
    this.#work = this.#work.then(() => this.#sync()).catch(() => {})
  }

  async #sync(): Promise<void> {
    const want = !this.#stopped && this.#opts.enabled !== false && !this.#clashed && this.#opts.keys.size > 0
    if (want && !this.#server) await this.#open()
    else if (!want && this.#server) await this.#close()
  }

  #open(): Promise<void> {
    const { port, bind } = this.#opts
    const server = net.createServer((raw) => this.#accept(raw))
    return new Promise<void>((resolve) => {
      // Said once: the listener is not tried again until the daemon restarts.
      let done = false
      const failed = (error: NodeJS.ErrnoException) => {
        if (done) return
        done = true
        this.#clashed = true
        const code = error.code ?? 'error'
        this.#log(code === 'EADDRINUSE' ? `cable: Wi-Fi dials are off — port ${port} is in use (${code}); USB dials are unaffected` : `cable: Wi-Fi dials are off — cannot listen on port ${port} (${code}); USB dials are unaffected`)
        resolve()
      }
      server.once('error', failed)
      const opened = () => {
        done = true
        server.off('error', failed)
        // A fault after it opened ends the listener the same way, and says so once.
        server.on('error', (error: NodeJS.ErrnoException) => {
          this.#log(`cable: Wi-Fi listener stopped (${error.code ?? 'error'}); USB dials are unaffected`)
          void this.#close()
        })
        this.#server = server
        const at = this.address()
        this.#log(`cable: Wi-Fi dials listening on ${at?.address ?? bind}:${at?.port ?? port} for ${this.#opts.keys.size} provisioned dial(s)`)
        resolve()
      }
      // A bad port or address is a synchronous throw from listen, not an 'error' event.
      try {
        server.listen(port, bind ?? '0.0.0.0', opened)
      } catch (error) {
        failed(error as NodeJS.ErrnoException)
      }
    })
  }

  async #close(): Promise<void> {
    const server = this.#server
    this.#server = null
    for (const entry of [...this.#pending]) this.#drop(entry, 'listener closing')
    for (const entry of [...this.#auth.values()]) this.#drop(entry, 'listener closing')
    if (server) {
      await new Promise<void>((resolve) => server.close(() => resolve()))
      this.#log('cable: Wi-Fi dials are off — no provisioned dial, so no open port')
    }
  }

  #accept(raw: net.Socket): void {
    this.stats.accepted++
    // An error on a socket that is being torn down must not become an uncaught exception.
    raw.on('error', () => {})
    const peer = (raw.remoteAddress ?? '').replace(/^::ffff:/, '')
    if (!(this.#opts.isPrivatePeer ?? isPrivatePeer)(peer)) {
      this.stats.refusedPeer++
      this.#logSome(`peer:${peer}`, `cable: Wi-Fi peer ${peer || '?'} refused — not a private address`)
      raw.destroy()
      return
    }
    if (!this.#opts.keys.isKnownAddr(peer) && this.#throttle.blocked(peer)) {
      this.stats.throttled++
      raw.destroy()
      return
    }
    const known = this.#opts.keys.isKnownAddr(peer)
    // One unknown address cannot hold more than its share of the pool.
    if (!known && this.#pendingFrom(peer) >= NET_PENDING_PER_ADDR) {
      this.stats.capped++
      this.#noteFailure(peer)
      raw.destroy()
      return
    }
    if (this.#pending.size >= NET_PENDING_MAX) {
      const victim = this.#victim(known)
      if (!victim) {
        // Only known dials wait, and the newcomer is not one: it is the one turned away.
        this.stats.capped++
        this.#noteFailure(peer)
        raw.destroy()
        return
      }
      this.#evict(victim)
    }
    raw.setNoDelay(true)
    let entry!: Pending
    // The lookup runs during the handshake: it records which identity the peer named and the key it was given.
    const secure = wrapPskSocket(raw, (identity) => {
      entry.claimed = identity.toUpperCase()
      const key = this.#opts.keys.get(identity)
      entry.keyUsed = key ? Buffer.from(key) : undefined
      return key
    })
    entry = { raw, tls: secure, peer, port: raw.remotePort ?? 0, timer: undefined, settled: false }
    this.#pending.add(entry)
    secure.on('error', (error) => this.#failed(entry, error.message))
    secure.once('secure', () => this.#secured(entry))
    secure.once('close', () => this.#closed(entry))
    entry.timer = setTimeout(() => this.#failed(entry, 'no handshake within the deadline'), NET_HANDSHAKE_DEADLINE_MS)
  }

  /** The handshake did not finish. Counted once per socket, against its address. */
  #failed(entry: Pending, reason: string): void {
    if (entry.settled) return
    this.#settle(entry)
    this.stats.failed++
    this.#logSome(`fail:${entry.peer}`, `cable: Wi-Fi handshake from ${entry.peer} failed (${reason.split('\n')[0].slice(0, 120)})`)
    this.#noteFailure(entry.peer)
    entry.tls.destroy()
    entry.raw.destroy()
  }

  /** One failure against an address; the one that starts a ban says so. */
  #noteFailure(peer: string): void {
    if (this.#throttle.failed(peer)) {
      this.#log(`cable: Wi-Fi peer ${peer} failed ${NET_FAIL_LIMIT} handshakes in ${NET_FAIL_WINDOW_MS / 1000} s — refusing it for ${NET_BAN_MS / 1000} s${this.#opts.keys.isKnownAddr(peer) ? ' (a known dial address is not refused)' : ''}`)
    }
  }

  #pendingFrom(peer: string): number {
    let count = 0
    for (const entry of this.#pending) if (entry.peer === peer) count++
    return count
  }

  /**
   * Whose socket makes room for a newcomer: the oldest of the unknown address holding the most (ties: the address whose
   * socket is oldest). Known addresses' sockets are not offered, except to a known newcomer when nothing else waits.
   */
  #victim(newcomerKnown: boolean): Pending | undefined {
    const byAddr = new Map<string, Pending[]>()
    for (const entry of this.#pending) {
      if (this.#opts.keys.isKnownAddr(entry.peer)) continue
      const list = byAddr.get(entry.peer) ?? []
      list.push(entry)
      byAddr.set(entry.peer, list)
    }
    let best: Pending[] | undefined
    for (const list of byAddr.values()) if (!best || list.length > best.length) best = list
    if (best) return best[0]
    return newcomerKnown ? (this.#pending.values().next().value as Pending) : undefined
  }

  #evict(victim: Pending): void {
    const crowded = this.#pendingFrom(victim.peer) > 1
    this.stats.evicted++
    this.#drop(victim, 'pool full')
    if (crowded) {
      this.stats.evictionFailures++
      this.#noteFailure(victim.peer)
    }
  }

  #closed(entry: Pending): void {
    if (!entry.settled) {
      this.#failed(entry, 'closed before the handshake finished')
      return
    }
    for (const [id, live] of this.#auth) {
      if (live !== entry) continue
      this.#auth.delete(id)
      this.#log(`cable: Wi-Fi dial ${id} at ${entry.peer} disconnected`)
    }
  }

  #secured(entry: Pending): void {
    if (entry.settled) return
    this.#settle(entry)
    const id = pskIdentityOf(entry.tls)
    if (!id) {
      this.stats.failed++
      entry.tls.destroy()
      return
    }
    // The key may have been removed or replaced while the handshake ran: a revoked dial does not get in on it.
    const current = this.#opts.keys.get(id)
    const used = entry.keyUsed
    entry.keyUsed = undefined
    const revoked = !current || !used || !current.equals(used)
    current?.fill(0)
    used?.fill(0)
    if (revoked) {
      this.stats.revoked++
      this.#log(`cable: Wi-Fi dial ${id} from ${entry.peer} refused — its key was removed or replaced during the handshake`)
      entry.tls.destroy()
      entry.raw.destroy()
      return
    }
    if (!this.#auth.has(id) && this.#auth.size >= NET_AUTH_MAX) {
      this.stats.overLimit++
      this.#log(`cable: Wi-Fi dial ${id} from ${entry.peer} refused — ${NET_AUTH_MAX} dials are connected`)
      entry.tls.destroy()
      return
    }
    const old = this.#auth.get(id)
    if (old) {
      this.stats.replaced++
      this.#drop(old, 'replaced by a new connection')
    }
    this.#auth.set(id, entry)
    this.stats.authenticated++
    try {
      this.#opts.keys.touch(id, entry.peer)
    } catch (error) {
      this.#log(`cable: Wi-Fi dial ${id}: cannot record its address (${(error as NodeJS.ErrnoException).code ?? 'write failed'})`)
    }
    this.#log(`cable: Wi-Fi dial ${id} authenticated from ${entry.peer}`)
    let taken: void | boolean
    try {
      taken = this.#opts.onDial({ id, peer: entry.peer, port: entry.port, socket: entry.tls })
    } catch (error) {
      taken = false
      this.#log(`cable: Wi-Fi dial ${id}: nobody could take the link (${error instanceof Error ? error.message.slice(0, 120) : 'error'})`)
    }
    if (taken === false) this.#drop(entry, 'not taken')
  }

  #settle(entry: Pending): void {
    entry.settled = true
    clearTimeout(entry.timer)
    this.#pending.delete(entry)
  }

  /** End a socket for a reason of ours: not a failed handshake, so nothing is counted against its address. */
  #drop(entry: Pending, _why: string): void {
    this.#settle(entry)
    for (const [id, live] of this.#auth) if (live === entry) this.#auth.delete(id)
    entry.tls.destroy()
    entry.raw.destroy()
  }

  #log(line: string): void {
    this.#opts.log(line)
  }

  /** A line that noise could repeat: said once per `key` per minute. */
  #logSome(key: string, line: string): void {
    const now = this.#now()
    const last = this.#loggedAt.get(key)
    if (last !== undefined && now - last < LOG_EVERY_MS) return
    this.#loggedAt.delete(key)
    this.#loggedAt.set(key, now)
    while (this.#loggedAt.size > THROTTLE_TRACKED_MAX) this.#loggedAt.delete(this.#loggedAt.keys().next().value as string)
    this.#opts.log(line)
  }
}

/** A dial's cable link over its TLS socket, as the session sees a port. */
export class NetPort implements CablePort {
  readonly transport = 'wifi' as const
  readonly path: string
  readonly peer: string
  isOpen = true
  #finished = false

  constructor(
    private readonly connection: NetDialConnection,
    private readonly onData: (chunk: Buffer) => void,
    private readonly onClosed: (why: string) => void,
  ) {
    this.peer = connection.peer
    this.path = `wifi:${connection.peer}:${connection.port}`
    const socket = connection.socket
    socket.once('close', () => this.#finish('the dial closed the link'))
    socket.on('error', (error: NodeJS.ErrnoException) => void this.close(error.code ?? error.message))
    // The first read waits one macrotask: the session stores this port as its link in the microtasks after the opener
    // returns, and a `hello` handled before that could not be answered. What the dial sent meanwhile stays in the stream.
    setImmediate(() => {
      if (this.#finished) return
      socket.on('data', (chunk: Buffer) => {
        if (this.#finished) return
        try {
          this.onData(chunk)
        } catch (error) {
          void this.close(String((error as Error).message ?? error))
        }
      })
    })
  }

  write(bytes: Uint8Array): Promise<void> {
    if (!this.isOpen) return Promise.reject(new Error('the Wi-Fi link is closed'))
    return new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        reject(new Error('write timed out'))
        void this.close('write timed out')
      }, WRITE_TIMEOUT_MS)
      timer.unref?.()
      this.connection.socket.write(bytes, (error) => {
        clearTimeout(timer)
        if (error) reject(error)
        else resolve()
      })
    })
  }

  close(why = 'closed'): Promise<void> {
    this.#finish(why)
    return Promise.resolve()
  }

  #finish(why: string): void {
    if (this.#finished) return
    this.#finished = true
    this.isOpen = false
    this.connection.socket.destroy()
    this.onClosed(why)
  }
}

/**
 * One dial's place in the fleet. The listener `offer`s each connection it authenticates; the dial's session calls
 * `opener` like it would open a serial port. Bytes that arrive before the session asks wait in the socket, so the first
 * `hello` is not lost. A newer connection replaces an older one nobody has taken yet.
 */
export class NetSlot {
  #waiting: NetDialConnection | null = null
  #current: NetPort | null = null
  #taker: ((connection: NetDialConnection | null) => void) | null = null
  #closed = false

  constructor(readonly id: string) {}

  /** The open link, if any. */
  get port(): NetPort | null {
    return this.#current?.isOpen ? this.#current : null
  }

  offer(connection: NetDialConnection): void {
    if (this.#closed) {
      connection.socket.destroy()
      return
    }
    const taker = this.#taker
    if (taker) {
      this.#taker = null
      taker(connection)
      return
    }
    if (this.#waiting && this.#waiting !== connection) this.#waiting.socket.destroy()
    this.#waiting = connection
  }

  /** The PortOpener of this dial's session: the next connection, waiting up to 2.5 s for one, else null. */
  readonly opener: PortOpener = (onData, onClosed) => {
    const adopt = (connection: NetDialConnection | null): CablePort | null => {
      if (!connection || connection.socket.destroyed || this.#closed) return null
      const port = new NetPort(connection, onData, onClosed)
      this.#current = port
      return port
    }
    const ready = this.#waiting
    this.#waiting = null
    if (ready && !ready.socket.destroyed) return Promise.resolve(adopt(ready))
    this.#taker?.(null)
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.#taker = null
        resolve(null)
      }, NET_OPEN_WAIT_MS)
      timer.unref?.()
      this.#taker = (connection) => {
        clearTimeout(timer)
        resolve(adopt(connection))
      }
    })
  }

  /** End the open link and any connection not yet taken, but go on accepting new ones (USB took over; the dial may return). */
  drop(why: string): void {
    this.#waiting?.socket.destroy()
    this.#waiting = null
    void this.#current?.close(why)
  }

  /** The dial is gone for good (its key was removed, or the daemon is stopping). */
  close(why: string): void {
    this.#closed = true
    this.#taker?.(null)
    this.#taker = null
    this.drop(why)
  }
}
