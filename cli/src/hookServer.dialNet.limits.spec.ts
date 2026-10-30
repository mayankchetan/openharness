// The dial Wi-Fi routes refuse a body over the server's limit as such, and run nothing for it (W9 review).
import type { Server } from 'node:http'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { env } from './config/env.js'
import { startHookServer } from './hookServer.js'
import { dialNetHandler } from './lib/dialNetHttp.js'
import { readHookCredential } from './lib/hookAuth.js'

let server: Server | null = null
afterEach(async () => {
  if (!server) return
  await new Promise<void>((resolve) => server!.close(() => resolve()))
  server = null
})

async function start(ask: Parameters<typeof dialNetHandler>[0]) {
  const started = await startHookServer(0, { onRegistered: vi.fn(), onSessionEnd: vi.fn(), onDialNet: dialNetHandler(ask) })
  server = started.server
  const credential = readHookCredential(env.ADAPTER_DATA_DIR)!
  return { base: `http://127.0.0.1:${started.port}`, headers: { 'x-harness-local': credential, 'x-adapter-local': '1', 'content-type': 'application/json' } }
}

describe('the size of a body on /api/dial/net/*', () => {
  it('over 256 KB is a 413, and the devices are not asked', async () => {
    const ask = vi.fn(async () => ({ ok: true }))
    const { base, headers } = await start(ask)
    const big = JSON.stringify({ ssid: 'home-net', pass: 'correct horse battery', pad: 'x'.repeat(256 * 1024 + 1) })
    const response = await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: big })
    expect(response.status).toBe(413)
    const text = await response.text()
    expect(JSON.parse(text)).toMatchObject({ kind: 'bad_request' })
    expect(text).not.toContain('correct horse')
    expect(ask).not.toHaveBeenCalled()
  })

  it('just under the limit is read as any body is', async () => {
    const ask = vi.fn(async () => ({ ok: true }))
    const { base, headers } = await start(ask)
    const pad = 'x'.repeat(200 * 1024)
    const response = await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: JSON.stringify({ ssid: 'home-net', pass: 'correct horse battery', pad }) })
    expect(response.status).toBe(200)
    expect(ask).toHaveBeenCalledWith({ op: 'provision', ssid: 'home-net', pass: 'correct horse battery' }, expect.any(Number))
  })

  it('an empty body is still an empty body, not a large one', async () => {
    const ask = vi.fn(async () => ({ ok: true, keyed: [{ dial: 'B8:1F:3F:CA:E5:58', createdAt: 1, hostPinned: true, key: 'zzz' }] }))
    const { base, headers } = await start(ask)
    const response = await fetch(`${base}/api/dial/net/status`, { method: 'POST', headers })
    expect(response.status).toBe(200)
    // and the pin is passed on, the key is not
    expect(await response.json()).toEqual({ ok: true, keyed: [{ dial: 'B8:1F:3F:CA:E5:58', createdAt: 1, hostPinned: true }] })
  })
})
