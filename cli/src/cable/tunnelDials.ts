// The relay endpoint of the phone bridge, daemon side (phone-bridge spec section 6, task B3b).
//
// A dial that is out of Wi-Fi range talks to its phone over Bluetooth; the phone copies the bytes into a WebSocket to
// `wss://<host>/relay`; Cloudflare Tunnel (with an Access service token) forwards that WebSocket to THIS endpoint on the
// desktop's loopback; and the stream goes to NetDials.acceptStream, the same TLS-PSK server a dial on the LAN meets. The phone
// and Cloudflare carry ciphertext and parse nothing: the dial's key `K` is known to the dial and to NetDials only.
//
// This file is a gate: it is the one place where a stranger's bytes first meet the daemon on this route. Every place that
// binds, parses or allocates is marked `GATE` below, and the limits are the named constants at the top:
//
//   GATE bind      one `server.listen` (#open), only on a loopback literal, and the bound address is read back and checked again.
//                  No env variable sets the bind address; the option exists for the tests, and a non-loopback value is refused.
//   GATE parse     Node's HTTP parser (header size capped at TUNNEL_MAX_HEADER_BYTES, so a request that is bigger is dropped by
//                  the parser) and `ws`'s frame parser (TUNNEL_MAX_MESSAGE_BYTES, no compression). Nothing here parses a body,
//                  a header value or a query: the path must equal TUNNEL_PATH exactly and no `Origin` header may be present.
//   GATE allocate  at most TUNNEL_CONNECTIONS_MAX sockets at once (`server.maxConnections`); a socket that has not finished its
//                  upgrade TUNNEL_UPGRADE_DEADLINE_MS after connect is destroyed; an upgraded stream holds at most
//                  TUNNEL_STREAM_BUFFER_BYTES unread (then the socket is paused, so a fast sender waits); an upgraded stream
//                  that has not finished the TLS-PSK handshake in NET_HANDSHAKE_DEADLINE_MS is destroyed by NetDials, which
//                  also holds at most 16 such streams (the oldest gives way) and 8 live dials.
//
// What it does not do: authenticate. Cloudflare Access is the first lock, the dial's key the second, and only the second is
// ours. A request that reaches here has therefore proved nothing; it gets a bare 404 for every refusal (no banner, no hint,
// nothing the peer chose is echoed or logged).
//
// Like the Wi-Fi socket it opens only when the keystore holds a dial, and it never opens when disabled
// (`CABLE_TUNNEL_ENABLE`, default false). It closes again when the last key is removed.
import * as http from 'node:http'
import * as net from 'node:net'
import { Duplex } from 'node:stream'
import { WebSocket, WebSocketServer } from 'ws'

import type { DialKeys } from './dialKeys.js'
import type { NetStreamInfo } from './netDials.js'

/** The one path that upgrades to a relay WebSocket. Compared as a whole string: no query, no trailing slash, no case folding. */
export const TUNNEL_PATH = '/relay'
/** A connection that has not completed its WebSocket upgrade this long after connect is destroyed. */
export const TUNNEL_UPGRADE_DEADLINE_MS = 3_000
/**
 * The largest WebSocket message, binary, in either direction the phone sends it: 64 KiB, the size of the phone pump's own queue
 * (phone-bridge spec section 6.5), so a conforming phone never sends more in one message. A TLS record is at most 16,384 bytes of
 * data plus about 330 of header, MAC and padding, so this holds four full records. A bigger message closes the connection (1009).
 */
export const TUNNEL_MAX_MESSAGE_BYTES = 64 * 1024
/** Sockets open at once, before and after the upgrade. Node closes the surplus at accept. */
export const TUNNEL_CONNECTIONS_MAX = 64
/** The HTTP parser's header budget; a bigger request is refused by the parser before any of it reaches this file. */
export const TUNNEL_MAX_HEADER_BYTES = 8 * 1024
/** Bytes an upgraded stream buffers unread before the socket is paused. */
export const TUNNEL_STREAM_BUFFER_BYTES = 64 * 1024
/** The same refusal is logged once per this long, so noise cannot fill the log. */
const LOG_EVERY_MS = 60_000

/** Is this a loopback address literal? A name (`localhost`), a wildcard and any other address are not. */
export function isLoopbackBind(address: string): boolean {
  if (net.isIPv4(address)) return address.startsWith('127.')
  return address === '::1'
}

/** What TunnelDials needs from NetDials. */
export interface StreamAcceptor {
  acceptStream(stream: Duplex, info: NetStreamInfo): void
}

export interface TunnelDialsOptions {
  keys: DialKeys
  dials: StreamAcceptor
  /** TCP port. 0 picks a free one (tests). */
  port: number
  /** Address to bind; `127.0.0.1` when unset. Anything that is not a loopback literal is refused (tests only: no env sets it). */
  bind?: string
  /** False never opens the socket (`CABLE_TUNNEL_ENABLE=false`, the default). */
  enabled?: boolean
  log: (line: string) => void
  now?: () => number
}

/**
 * A WebSocket as a byte stream, for `tls.TLSSocket`. Binary messages in, binary messages out; a text message is a protocol
 * violation here and closes the connection (1003). Not `createWebSocketStream` because that one hands a text message to the
 * stream as bytes.
 */
class WsStream extends Duplex {
  constructor(private readonly ws: WebSocket) {
    super({ allowHalfOpen: false, readableHighWaterMark: TUNNEL_STREAM_BUFFER_BYTES, writableHighWaterMark: TUNNEL_STREAM_BUFFER_BYTES })
    ws.on('message', (data, isBinary) => {
      if (this.destroyed) return
      if (!isBinary) {
        ws.close(1003, 'binary only')
        this.destroy()
        return
      }
      // With the default binaryType a binary message arrives as one Buffer.
      if (!this.push(data as Buffer)) ws.pause()
    })
    ws.once('close', () => {
      if (!this.destroyed) this.push(null)
    })
    ws.on('error', (error) => this.destroy(error))
  }

  override _read(): void {
    if (this.ws.isPaused) this.ws.resume()
  }

  override _write(chunk: Buffer, _encoding: BufferEncoding, callback: (error?: Error | null) => void): void {
    this.ws.send(chunk, { binary: true, compress: false }, (error) => callback(error ?? null))
  }

  override _final(callback: (error?: Error | null) => void): void {
    this.ws.close()
    callback()
  }

  override _destroy(error: Error | null, callback: (error?: Error | null) => void): void {
    if (error) {
      // `ws` has already queued its own close frame for a protocol error (1009 too big, 1002, 1007): let it leave, then
      // cut the socket if the peer does not finish the close within a second.
      const cut = setTimeout(() => this.ws.terminate(), 1_000)
      cut.unref?.()
      this.ws.once('close', () => clearTimeout(cut))
    } else {
      this.ws.terminate()
    }
    callback(error)
  }
}

export class TunnelDials {
  readonly #opts: TunnelDialsOptions
  readonly #now: () => number
  readonly #live = new Set<WebSocket>()
  readonly #deadlines = new WeakMap<net.Socket, ReturnType<typeof setTimeout>>()
  readonly #loggedAt = new Map<string, number>()
  #server: http.Server | null = null
  #wss: WebSocketServer | null = null
  /** The listener could not be opened (a clash, a bad bind). One line was logged; it is not tried again until the daemon restarts. */
  #failed = false
  #stopped = true
  #work: Promise<void> = Promise.resolve()
  #off: (() => void) | null = null

  constructor(opts: TunnelDialsOptions) {
    this.#opts = opts
    this.#now = opts.now ?? Date.now
  }

  /** Is the socket open? */
  get listening(): boolean {
    return this.#server !== null
  }

  address(): net.AddressInfo | null {
    const address = this.#server?.address()
    return address && typeof address === 'object' ? address : null
  }

  /** Follow the keystore: open the socket when it has a dial, close it when it has none. */
  start(): void {
    if (!this.#stopped) return
    this.#stopped = false
    this.#failed = false
    this.#off = this.#opts.keys.onChange(() => this.#reconcile())
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
    const want = !this.#stopped && this.#opts.enabled === true && !this.#failed && this.#opts.keys.size > 0
    if (want && !this.#server) await this.#open()
    else if (!want && this.#server) await this.#close()
  }

  #open(): Promise<void> {
    const { port } = this.#opts
    const bind = this.#opts.bind ?? '127.0.0.1'
    // GATE bind: refused before a socket exists.
    if (!isLoopbackBind(bind)) {
      this.#failed = true
      this.#log(`cable: phone tunnel is off — bind address "${bind.replace(/[^\x20-\x7e]/g, '?').slice(0, 40)}" is not a loopback address; the relay endpoint is loopback only`)
      return Promise.resolve()
    }
    const server = http.createServer({ maxHeaderSize: TUNNEL_MAX_HEADER_BYTES, keepAlive: false }, (_req, res) => this.#plain(res))
    server.maxConnections = TUNNEL_CONNECTIONS_MAX
    server.on('connection', (socket) => this.#connected(socket))
    server.on('upgrade', (req, socket, head) => this.#upgrade(req, socket, head))
    // A request the parser rejects (too large, malformed): closed, nothing said.
    server.on('clientError', (_error, socket) => socket.destroy())
    const wss = new WebSocketServer({ noServer: true, maxPayload: TUNNEL_MAX_MESSAGE_BYTES, perMessageDeflate: false, clientTracking: false })
    return new Promise<void>((resolve) => {
      let done = false
      const failed = (error: NodeJS.ErrnoException) => {
        if (done) return
        done = true
        this.#failed = true
        const code = error.code ?? 'error'
        this.#log(code === 'EADDRINUSE' ? `cable: phone tunnel is off — port ${port} is in use (${code}); USB and Wi-Fi dials are unaffected` : `cable: phone tunnel is off — cannot listen on port ${port} (${code}); USB and Wi-Fi dials are unaffected`)
        resolve()
      }
      server.once('error', failed)
      server.listen(port, bind, () => {
        done = true
        server.off('error', failed)
        const at = server.address()
        // GATE bind, second look: what the system bound is what was asked for.
        if (!at || typeof at !== 'object' || !isLoopbackBind(at.address)) {
          this.#failed = true
          this.#log('cable: phone tunnel is off — the bound address is not loopback')
          server.close()
          resolve()
          return
        }
        server.on('error', (error: NodeJS.ErrnoException) => {
          this.#log(`cable: phone tunnel listener stopped (${error.code ?? 'error'}); USB and Wi-Fi dials are unaffected`)
          void this.#close()
        })
        this.#server = server
        this.#wss = wss
        this.#log(`cable: phone tunnel relay listening on ${at.address}:${at.port} for ${this.#opts.keys.size} provisioned dial(s)`)
        resolve()
      })
    })
  }

  async #close(): Promise<void> {
    const server = this.#server
    this.#server = null
    this.#wss = null
    for (const ws of [...this.#live]) ws.terminate()
    this.#live.clear()
    if (server) {
      // Sockets that never upgraded are ended too: `close` alone waits for them.
      server.closeAllConnections()
      await new Promise<void>((resolve) => server.close(() => resolve()))
      this.#log('cable: phone tunnel is off — no provisioned dial, or the daemon is stopping')
    }
  }

  /** GATE allocate: the 3 s upgrade deadline starts when the TCP connection is accepted. */
  #connected(socket: net.Socket): void {
    // An error on a socket that is being torn down must not become an uncaught exception.
    socket.on('error', () => {})
    const timer = setTimeout(() => socket.destroy(), TUNNEL_UPGRADE_DEADLINE_MS)
    timer.unref?.()
    this.#deadlines.set(socket, timer)
    socket.once('close', () => clearTimeout(timer))
  }

  /** A request that is not an upgrade: a bare 404 and the connection closes. Nothing of the request is read or logged. */
  #plain(res: http.ServerResponse): void {
    this.#refusedOnce('plain', 'cable: phone tunnel answered a request that is not a WebSocket upgrade with 404')
    res.writeHead(404, { Connection: 'close', 'Content-Length': '0' })
    res.end()
  }

  #refuse(socket: Duplex, reason: string): void {
    this.#refusedOnce(reason, `cable: phone tunnel refused an upgrade (${reason})`)
    if (socket.writable) socket.write('HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n')
    socket.destroy()
  }

  /** GATE parse: the whole of what is read from the request. */
  #upgrade(req: http.IncomingMessage, socket: Duplex, head: Buffer): void {
    // At runtime it is the accepted net.Socket; the type of the event is the wider Duplex.
    clearTimeout(this.#deadlines.get(socket as net.Socket))
    const wss = this.#wss
    if (!wss || this.#stopped) return this.#refuse(socket, 'closing')
    if (req.method !== 'GET' || req.url !== TUNNEL_PATH) return this.#refuse(socket, 'wrong method or path')
    // A browser sends Origin on every WebSocket; the phone app does not. A web page must not reach the handshake.
    if (req.headers.origin !== undefined) return this.#refuse(socket, 'browser origin')
    // A malformed upgrade (bad key, bad version) is answered 400 and closed by `ws` itself.
    wss.handleUpgrade(req, socket, head, (ws) => this.#relay(ws))
  }

  #relay(ws: WebSocket): void {
    this.#live.add(ws)
    ws.once('close', () => this.#live.delete(ws))
    // From here the stream is NetDials's: its pool, its 3 s handshake deadline, its key checks.
    this.#opts.dials.acceptStream(new WsStream(ws), { peer: 'tunnel', transport: 'tunnel' })
  }

  /** A refusal that noise could repeat: said once per reason per minute, with nothing the peer chose in it. */
  #refusedOnce(reason: string, line: string): void {
    const now = this.#now()
    const last = this.#loggedAt.get(reason)
    if (last !== undefined && now - last < LOG_EVERY_MS) return
    this.#loggedAt.set(reason, now)
    this.#log(line)
  }

  #log(line: string): void {
    this.#opts.log(line)
  }
}
