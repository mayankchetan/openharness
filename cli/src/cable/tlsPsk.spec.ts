// TLS-PSK server side of the dial link (tlsPsk.ts), against a real Node client on loopback.
//
// Node runtime options confirmed by the first test: minVersion / maxVersion on tls.createServer, `ciphers` with the
// OpenSSL PSK names, and the server `pskCallback(socket, identity)`.
import { randomBytes } from 'node:crypto'
import * as net from 'node:net'
import * as tls from 'node:tls'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { pskConnect, type PskClientOptions } from './__fixtures__/pskClient.js'
import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import {
  PSK_CIPHER_LIST,
  PSK_LENGTH,
  createPskServer,
  normalizePskIdentity,
  pskIdentityOf,
  pskServerOptions,
  wrapPskSocket,
  type PskLookup,
} from './tlsPsk.js'

const ID = 'B8:1F:3F:CA:E5:58'
const KEY = randomBytes(PSK_LENGTH)

const servers: Array<{ close(): unknown }> = []
const sockets: net.Socket[] = []
afterEach(() => {
  for (const s of sockets.splice(0)) s.destroy()
  for (const s of servers.splice(0)) s.close()
  vi.restoreAllMocks()
})

interface Rig {
  port: number
  secure: tls.TLSSocket[]
  clientErrors: Error[]
  lookup: ReturnType<typeof vi.fn<PskLookup>>
}

async function rig(table: Record<string, Buffer> = { [ID]: KEY }): Promise<Rig> {
  const lookup = vi.fn<PskLookup>((identity) => table[identity])
  const secure: tls.TLSSocket[] = []
  const clientErrors: Error[] = []
  const server = createPskServer(lookup, (socket) => {
    secure.push(socket)
    sockets.push(socket)
    socket.on('error', () => {})
    socket.pipe(socket)
  })
  server.on('tlsClientError', (err) => clientErrors.push(err))
  servers.push(server)
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  return { port: (server.address() as net.AddressInfo).port, secure, clientErrors, lookup }
}

const track = (s: tls.TLSSocket) => {
  sockets.push(s)
  return s
}

/** Did the connection fail, and how many application bytes came back before it did. */
function failsWithoutData(connect: Promise<tls.TLSSocket>): Promise<{ failed: boolean; data: number }> {
  return connect.then(
    (socket) =>
      new Promise((resolve) => {
        track(socket)
        let data = 0
        socket.on('data', (b) => (data += b.length))
        socket.on('error', () => {})
        socket.write('hello')
        socket.once('close', () => resolve({ failed: true, data }))
        setTimeout(() => resolve({ failed: false, data }), 500)
      }),
    () => ({ failed: true, data: 0 }),
  )
}

describe('tlsPsk options', () => {
  it('names the Node options and the suites the dial offers', () => {
    expect(process.version).toMatch(/^v\d+\./)
    const opts = pskServerOptions(() => undefined)
    expect(opts.minVersion).toBe('TLSv1.2')
    expect(opts.maxVersion).toBe('TLSv1.2')
    expect(opts.ciphers).toBe(PSK_CIPHER_LIST)
    expect(PSK_CIPHER_LIST.split(':')).toEqual(['ECDHE-PSK-AES128-CBC-SHA256', 'ECDHE-PSK-AES256-CBC-SHA384'])
    expect(typeof opts.pskCallback).toBe('function')
    // the suites are known to the OpenSSL inside this Node
    const known = tls.getCiphers()
    expect(known).toContain('ecdhe-psk-aes128-cbc-sha256')
    expect(known).toContain('ecdhe-psk-aes256-cbc-sha384')
  })

  it('normalizes the identity to upper case', () => {
    expect(normalizePskIdentity('b8:1f:3f:ca:e5:58')).toBe(ID)
  })
})

describe('tlsPsk link', () => {
  it('connects with the right PSK and echoes 1,000 cable frames intact', async () => {
    const r = await rig()
    const client = track(await pskConnect({ port: r.port, identity: ID, psk: KEY }))
    expect(client.getProtocol()).toBe('TLSv1.2')
    expect(client.getCipher().name).toBe('ECDHE-PSK-AES128-CBC-SHA256')

    const payloads: Buffer[] = []
    const sent: Uint8Array[] = []
    for (let i = 0; i < 1000; i++) {
      const payload = randomBytes(1 + ((i * 37) % 600))
      payloads.push(payload)
      sent.push(encodeCableFrame(CableType.Log, payload))
    }
    const decoder = new CableDecoder()
    const got: Uint8Array[] = []
    const done = new Promise<void>((resolve, reject) => {
      client.on('data', (chunk) => {
        decoder.feed(chunk, (f) => {
          got.push(f.payload)
          if (got.length === sent.length) resolve()
        })
      })
      client.on('error', reject)
      setTimeout(() => reject(new Error(`only ${got.length} frames echoed`)), 8000)
    })
    for (const frame of sent) client.write(frame)
    await done
    expect(decoder.corruptFrames).toBe(0)
    expect(decoder.discardedBytes).toBe(0)
    payloads.forEach((p, i) => expect(Buffer.from(got[i]).equals(p)).toBe(true))
    expect(r.lookup).toHaveBeenCalledWith(ID)
  })

  it('exposes the authenticated identity of the socket', async () => {
    const r = await rig()
    track(await pskConnect({ port: r.port, identity: ID, psk: KEY }))
    await vi.waitFor(() => expect(r.secure.length).toBe(1))
    expect(pskIdentityOf(r.secure[0])).toBe(ID)
  })

  it('matches a lower-case identity', async () => {
    const r = await rig()
    track(await pskConnect({ port: r.port, identity: ID.toLowerCase(), psk: KEY }))
    await vi.waitFor(() => expect(r.secure.length).toBe(1))
    expect(r.lookup).toHaveBeenCalledWith(ID)
    expect(pskIdentityOf(r.secure[0])).toBe(ID)
  })

  it('accepts the AES-256 suite when only that one is offered', async () => {
    const r = await rig()
    const client = track(await pskConnect({ port: r.port, identity: ID, psk: KEY, ciphers: 'ECDHE-PSK-AES256-CBC-SHA384' }))
    expect(client.getCipher().name).toBe('ECDHE-PSK-AES256-CBC-SHA384')
  })

  it('refuses a wrong PSK and delivers no application data', async () => {
    const r = await rig()
    const res = await failsWithoutData(pskConnect({ port: r.port, identity: ID, psk: randomBytes(PSK_LENGTH) }))
    expect(res).toEqual({ failed: true, data: 0 })
    expect(r.secure).toHaveLength(0)
  })

  it('refuses an unknown identity the same way, with a read-only lookup and nothing else', async () => {
    const r = await rig()
    const res = await failsWithoutData(pskConnect({ port: r.port, identity: 'aa:bb:cc:dd:ee:ff', psk: KEY }))
    expect(res).toEqual({ failed: true, data: 0 })
    expect(r.secure).toHaveLength(0)
    expect(r.lookup).toHaveBeenCalledTimes(1)
    expect(r.lookup).toHaveBeenCalledWith('AA:BB:CC:DD:EE:FF')
  })

  it('hides an unknown identity: it fails like a wrong key, not with unknown_psk_identity', async () => {
    const r = await rig()
    const message = (opts: Omit<PskClientOptions, 'port'>) =>
      pskConnect({ port: r.port, ...opts }).then(
        () => '',
        (e: Error) => String(e.message),
      )
    const unknown = await message({ identity: 'AA:BB:CC:DD:EE:FF', psk: KEY })
    const wrong = await message({ identity: ID, psk: randomBytes(PSK_LENGTH) })
    expect(unknown).not.toBe('')
    expect(unknown).not.toMatch(/unknown.psk.identity/i)
    expect(unknown).toBe(wrong)
  })

  it('treats a throwing or short-key lookup as an unknown identity', async () => {
    const lookup: PskLookup = (identity) => {
      if (identity === 'AA:AA:AA:AA:AA:01') throw new Error(`keystore exploded with ${KEY.toString('hex')}`)
      return Buffer.alloc(8, 1)
    }
    const server = createPskServer(lookup, () => {})
    servers.push(server)
    server.on('tlsClientError', () => {})
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
    const port = (server.address() as net.AddressInfo).port
    for (const identity of ['AA:AA:AA:AA:AA:01', 'AA:AA:AA:AA:AA:02']) {
      const res = await failsWithoutData(pskConnect({ port, identity, psk: Buffer.alloc(8, 1) }))
      expect(res).toEqual({ failed: true, data: 0 })
    }
  })

  it('refuses a TLS 1.3-only client', async () => {
    const r = await rig()
    const res = await failsWithoutData(
      pskConnect({ port: r.port, identity: ID, psk: KEY, minVersion: 'TLSv1.3', maxVersion: 'TLSv1.3', ciphers: undefined }),
    )
    expect(res).toEqual({ failed: true, data: 0 })
    expect(r.secure).toHaveLength(0)
  })

  it.each([
    ['a certificate suite with no PSK callback', { ciphers: 'ECDHE-RSA-AES128-GCM-SHA256', noPsk: true }],
    ['a certificate suite with a PSK callback', { ciphers: 'ECDHE-RSA-AES128-GCM-SHA256' }],
    ['plain PSK (no forward secrecy)', { ciphers: 'PSK-AES128-CBC-SHA256' }],
    ['plain PSK with GCM', { ciphers: 'PSK-AES128-GCM-SHA256' }],
    ['DHE-PSK', { ciphers: 'DHE-PSK-AES128-CBC-SHA256' }],
    ['ECDHE-PSK with ChaCha20', { ciphers: 'ECDHE-PSK-CHACHA20-POLY1305' }],
  ])('refuses %s', async (_name, extra) => {
    const r = await rig()
    const res = await failsWithoutData(pskConnect({ port: r.port, identity: ID, psk: KEY, ...extra }))
    expect(res).toEqual({ failed: true, data: 0 })
    expect(r.secure).toHaveLength(0)
  })

  it('never puts the key in an error, a log line or a server event', async () => {
    const seen: string[] = []
    for (const m of ['log', 'info', 'warn', 'error', 'debug'] as const) {
      vi.spyOn(console, m).mockImplementation((...a: unknown[]) => void seen.push(a.map(String).join(' ')))
    }
    const r = await rig()
    const wrongKey = randomBytes(PSK_LENGTH)
    const errors: string[] = []
    for (const [identity, psk] of [
      [ID, wrongKey],
      ['AA:BB:CC:DD:EE:FF', KEY],
    ] as const) {
      await pskConnect({ port: r.port, identity, psk }).catch((e: Error) => errors.push(`${e.message} ${e.stack}`))
    }
    await new Promise((resolve) => setTimeout(resolve, 100))
    const everything = [...seen, ...errors, ...r.clientErrors.map((e) => `${e.message} ${e.stack}`)].join('\n')
    for (const key of [KEY, wrongKey]) {
      expect(everything).not.toContain(key.toString('hex'))
      expect(everything).not.toContain(key.toString('base64'))
    }
    expect(r.clientErrors.length).toBeGreaterThan(0)
  })
})

describe('tlsPsk hardening', () => {
  it('rejects an identity that is not a MAC string before the lookup', async () => {
    const r = await rig()
    for (const identity of ['', 'BB', 'b8:1f:3f:ca:e5', ID + ':00', 'B8-1F-3F-CA-E5-58', 'G8:1F:3F:CA:E5:58', 'ı8:1F:3F:CA:E5:58', 'b8:1f:3f:ca:e5:58\n']) {
      const res = await failsWithoutData(pskConnect({ port: r.port, identity, psk: KEY }))
      expect(res).toEqual({ failed: true, data: 0 })
    }
    expect(r.lookup).not.toHaveBeenCalled()
    expect(r.secure).toHaveLength(0)
    expect(normalizePskIdentity('b8:1f:3f:ca:e5:58')).toBe(ID)
  })

  it('records the identity only once the handshake has finished', async () => {
    const seen: Array<string | undefined> = []
    const lookup: PskLookup = (identity) => (identity === ID ? KEY : undefined)
    const server = tls.createServer({ ...pskServerOptions(lookup) }, () => {})
    servers.push(server)
    server.on('tlsClientError', (_e, socket) => seen.push(pskIdentityOf(socket)))
    await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
    const port = (server.address() as net.AddressInfo).port
    // right identity, wrong key: the callback found the identity, the handshake failed, so no identity is recorded
    await failsWithoutData(pskConnect({ port, identity: ID, psk: randomBytes(PSK_LENGTH) }))
    await new Promise((resolve) => setTimeout(resolve, 50))
    expect(seen).toEqual([undefined])
  })

  it('sends no session ticket and does not resume a session', async () => {
    const r = await rig()
    const first = track(await pskConnect({ port: r.port, identity: ID, psk: KEY }))
    first.write('x')
    await new Promise((resolve) => first.once('data', resolve))
    await new Promise((resolve) => setTimeout(resolve, 100))
    expect(first.getTLSTicket()).toBeUndefined() // no NewSessionTicket reached the client
    expect(first.isSessionReused()).toBe(false)
    // a client that kept the session ID of the first connection finds no cache entry on the second
    const kept = first.getSession()
    const second = track(await pskConnect({ port: r.port, identity: ID, psk: KEY, session: kept }))
    expect(second.isSessionReused()).toBe(false)
    expect(r.lookup).toHaveBeenCalledTimes(2) // the PSK was checked again
  })

  it('refuses a client renegotiation', async () => {
    const r = await rig()
    const client = track(await pskConnect({ port: r.port, identity: ID, psk: KEY }))
    const outcome = await new Promise<string>((resolve) => {
      client.on('error', (e) => resolve(`error: ${e.message}`))
      client.on('close', () => resolve('closed'))
      try {
        client.renegotiate({}, (err) => resolve(err ? `refused: ${err.message}` : 'accepted'))
      } catch (e) {
        resolve(`threw: ${(e as Error).message}`)
      }
      setTimeout(() => resolve('timeout'), 2000)
    })
    expect(outcome).not.toBe('accepted')
  })

  it('drops a peer that never sends a ClientHello (server, 5 s)', async () => {
    const r = await rig()
    const raw = net.connect(r.port, '127.0.0.1')
    sockets.push(raw)
    raw.on('error', () => {})
    const t0 = Date.now()
    await new Promise<void>((resolve) => raw.once('close', () => resolve()))
    const ms = Date.now() - t0
    expect(ms).toBeGreaterThan(4500)
    expect(ms).toBeLessThan(7000)
    expect(r.clientErrors.length).toBeGreaterThan(0)
    expect(r.secure).toHaveLength(0)
  }, 15000)

  it('drops a wrapped socket that never sends a ClientHello (5 s)', async () => {
    const wrapped: tls.TLSSocket[] = []
    const tcp = net.createServer((raw) => {
      sockets.push(raw)
      const secure = wrapPskSocket(raw, () => KEY)
      secure.on('error', () => {})
      wrapped.push(secure)
    })
    servers.push(tcp)
    await new Promise<void>((resolve) => tcp.listen(0, '127.0.0.1', resolve))
    const raw = net.connect((tcp.address() as net.AddressInfo).port, '127.0.0.1')
    sockets.push(raw)
    raw.on('error', () => {})
    const t0 = Date.now()
    await new Promise<void>((resolve) => raw.once('close', () => resolve()))
    const ms = Date.now() - t0
    expect(ms).toBeGreaterThan(4500)
    expect(ms).toBeLessThan(7000)
    expect(wrapped[0].destroyed).toBe(true)
  }, 15000)
})

describe('wrapPskSocket', () => {
  it('serves TLS-PSK over an existing duplex (the tunnel case)', async () => {
    const lookup = vi.fn<PskLookup>((id) => (id === ID ? KEY : undefined))
    const tcp = net.createServer((raw) => {
      sockets.push(raw)
      const secure = wrapPskSocket(raw, lookup)
      sockets.push(secure)
      secure.on('error', () => {})
      secure.pipe(secure)
    })
    servers.push(tcp)
    await new Promise<void>((resolve) => tcp.listen(0, '127.0.0.1', resolve))
    const port = (tcp.address() as net.AddressInfo).port
    const client = track(await pskConnect({ port, identity: ID.toLowerCase(), psk: KEY }))
    const echoed = new Promise<string>((resolve) => client.once('data', (b) => resolve(b.toString())))
    client.write('ping')
    expect(await echoed).toBe('ping')
    const bad = await failsWithoutData(pskConnect({ port, identity: ID, psk: randomBytes(PSK_LENGTH) }))
    expect(bad).toEqual({ failed: true, data: 0 })
  })
})
