// Interop, opt-in: the dial's TLS wrapper (firmware/main/cable_xport_tls.c) against the daemon's tlsPsk.ts.
//
// The wrapper is built for the host inside WSL, against the IDF copy of mbedTLS, by firmware/test/test_cable_xport_tls.py
// (--build-only); its --connect mode then plays the dial over a real TCP socket to a Node server on this machine. So the
// handshake is mbedTLS (client) against OpenSSL via Node (server), the pair the product runs.
//
//   OH_TLS_INTEROP_WSL=1 npx vitest run src/cable/tlsPsk.firmware.spec.ts
//
// Windows with WSL2 only; it needs the ESP-IDF tree at IDF_PATH (default /mnt/c/esp/esp-idf inside WSL) and the Windows
// firewall must let WSL reach this port. Skipped otherwise, so the default run never starts WSL.
import { execFileSync, spawn } from 'node:child_process'
import { randomBytes } from 'node:crypto'
import * as net from 'node:net'
import { resolve } from 'node:path'
import type * as tls from 'node:tls'
import { afterAll, beforeAll, describe, expect, it } from 'vitest'

import { createPskServer, PSK_LENGTH } from './tlsPsk.js'

const enabled = process.platform === 'win32' && process.env.OH_TLS_INTEROP_WSL === '1'
const IDENTITY = 'B8:1F:3F:CA:E5:58'
const KEY = randomBytes(PSK_LENGTH)

function wslPath(windowsPath: string): string {
  return windowsPath.replace(/^([A-Za-z]):/, (_m, d: string) => `/mnt/${d.toLowerCase()}`).replace(/\\/g, '/')
}

function wsl(script: string): string {
  return execFileSync('wsl', ['-e', 'bash', '-c', script], { encoding: 'utf8', timeout: 180_000 }).trim()
}

describe.skipIf(!enabled)('firmware TLS wrapper against tlsPsk.ts (WSL)', () => {
  const peer = '/tmp/tls_peer_interop'
  const secure: tls.TLSSocket[] = []
  const negotiated: Array<{ protocol: string | null; cipher: string }> = []
  let server: tls.Server
  let port = 0
  let hostIp = ''

  beforeAll(async () => {
    const tests = wslPath(resolve(__dirname, '../../../devices/harness-device/firmware/test'))
    wsl(`export IDF_PATH=\${IDF_PATH:-/mnt/c/esp/esp-idf}; cd ${tests} && python3 test_cable_xport_tls.py --build-only ${peer}`)
    hostIp = wsl("ip route | awk '/default/ {print $3}'")
    server = createPskServer((id) => (id === IDENTITY ? KEY : undefined), (socket) => {
      secure.push(socket)
      negotiated.push({ protocol: socket.getProtocol(), cipher: socket.getCipher().name })
      socket.on('error', () => {})
      socket.pipe(socket)
    })
    server.on('tlsClientError', () => {})
    await new Promise<void>((done) => server.listen(0, '0.0.0.0', done))
    port = (server.address() as net.AddressInfo).port
  }, 240_000)

  afterAll(() => {
    for (const s of secure) s.destroy()
    server?.close()
  })

  function dial(identity: string, key: Buffer, frames = 1000): Promise<{ code: number | null; out: string }> {
    return new Promise((done) => {
      const child = spawn('wsl', ['-e', peer, '--connect', hostIp, String(port), identity, key.toString('hex'), String(frames)])
      let out = ''
      child.stdout.on('data', (b) => (out += b))
      child.stderr.on('data', (b) => (out += b))
      child.on('close', (code) => done({ code, out }))
    })
  }

  it('echoes 1,000 frames through TLS 1.2 ECDHE-PSK-AES128-CBC-SHA256', async () => {
    const before = secure.length
    const res = await dial(IDENTITY, KEY)
    expect(res.out).toContain('ECHOED 1000 frames intact')
    expect(res.code).toBe(0)
    expect(secure.length).toBe(before + 1)
    expect(negotiated[before]).toEqual({ protocol: 'TLSv1.2', cipher: 'ECDHE-PSK-AES128-CBC-SHA256' })
  }, 120_000)

  it('is refused with the wrong PSK', async () => {
    const before = secure.length
    const res = await dial(IDENTITY, randomBytes(PSK_LENGTH), 1)
    expect(res.code).toBe(3)
    expect(res.out).toContain('REFUSED')
    expect(secure.length).toBe(before)
  }, 60_000)

  it('is refused with an unknown identity', async () => {
    const before = secure.length
    const res = await dial('AA:BB:CC:DD:EE:FF', KEY, 1)
    expect(res.code).toBe(3)
    expect(secure.length).toBe(before)
  }, 60_000)
})
