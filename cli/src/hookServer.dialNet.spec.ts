// The dial Wi-Fi routes on the daemon's local server (hookServer.ts, lib/dialNetHttp.ts): the same loopback and local-credential
// rules as every other route, and a body with a password in it that must reach the devices and no log.
import type { Server } from 'node:http'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { env } from './config/env.js'
import { startHookServer, type HookServerHandlers } from './hookServer.js'
import { dialNetHandler } from './lib/dialNetHttp.js'
import { readHookCredential } from './lib/hookAuth.js'

let server: Server | null = null
afterEach(async () => {
  vi.restoreAllMocks()
  if (!server) return
  await new Promise<void>((resolve) => server!.close(() => resolve()))
  server = null
})

async function start(overrides: Partial<HookServerHandlers> = {}) {
  const started = await startHookServer(0, { onRegistered: vi.fn(), onSessionEnd: vi.fn(), ...overrides })
  server = started.server
  const credential = readHookCredential(env.ADAPTER_DATA_DIR)
  if (!credential) throw new Error('hook credential was not created')
  const base = `http://127.0.0.1:${started.port}`
  const headers = { 'x-harness-local': credential, 'x-adapter-local': '1', 'content-type': 'application/json' }
  return { base, headers }
}

const PASS = 'correct horse battery'

describe('the dial Wi-Fi routes on the local server', () => {
  it('are not routes at all without a handler: 404, as for any unknown route', async () => {
    const { base, headers } = await start()
    const unknown = await fetch(`${base}/api/dial/nothing`, { method: 'POST', headers, body: '{}' })
    const unknownBody = await unknown.text()
    for (const op of ['provision', 'status', 'forget', 'rotate', 'mode']) {
      const response = await fetch(`${base}/api/dial/net/${op}`, { method: 'POST', headers, body: '{}' })
      expect(response.status, op).toBe(404)
      expect(await response.text()).toBe(unknownBody)
    }
  })

  it('hand a request to the devices and answer with their word', async () => {
    const ask = vi.fn(async () => ({ ok: true, dial: 'B8:1F:3F:CA:E5:58', saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 }))
    const { base, headers } = await start({ onDialNet: dialNetHandler(ask) })
    const response = await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: JSON.stringify({ ssid: 'home-net', pass: PASS }) })
    expect(response.status).toBe(200)
    expect(response.headers.get('content-type')).toBe('application/json')
    expect(await response.json()).toEqual({ ok: true, dial: 'B8:1F:3F:CA:E5:58', saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 })
    expect(ask).toHaveBeenCalledWith({ op: 'provision', ssid: 'home-net', pass: PASS }, expect.any(Number))
  })

  it('answer a bad field with a 400 that names the field and not its value, before asking anyone', async () => {
    const ask = vi.fn(async () => ({ ok: true }))
    const { base, headers } = await start({ onDialNet: dialNetHandler(ask) })
    const response = await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: JSON.stringify({ ssid: 'home-net', pass: 'abc12' }) })
    expect(response.status).toBe(400)
    const text = await response.text()
    expect(JSON.parse(text)).toMatchObject({ kind: 'bad_request', field: 'pass' })
    expect(text).not.toContain('abc12')
    expect(ask).not.toHaveBeenCalled()
  })

  it('refuse a body that is not JSON, a wrong method, and a caller without the CSRF header, before asking anyone', async () => {
    const ask = vi.fn(async () => ({ ok: true }))
    const { base, headers } = await start({ onDialNet: dialNetHandler(ask) })
    expect((await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: '{"pass":"correct horse' })).status).toBe(400)
    expect((await fetch(`${base}/api/dial/net/status`, { method: 'GET', headers })).status).toBe(405)
    const { 'x-adapter-local': _csrf, ...noCsrf } = headers
    expect((await fetch(`${base}/api/dial/net/status`, { method: 'POST', headers: noCsrf, body: '{}' })).status).toBe(403)
    expect(ask).not.toHaveBeenCalled()
  })

  it('want the local credential like every other route, and a loopback Host', async () => {
    const ask = vi.fn(async () => ({ ok: true }))
    const { base, headers } = await start({ onDialNet: dialNetHandler(ask) })
    const { 'x-harness-local': _credential, ...anonymous } = headers
    const body = JSON.stringify({ ssid: 'home-net', pass: PASS })
    expect((await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers: anonymous, body })).status).toBe(401)
    expect((await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers: { ...headers, 'x-harness-local': 'wrong' }, body })).status).toBe(401)
    const http = await import('node:http')
    const status = await new Promise<number>((resolve, reject) => {
      const request = http.request(`${base}/api/dial/net/provision`, { method: 'POST', headers: { ...headers, host: 'evil.example' } }, (res) => { res.resume(); resolve(res.statusCode ?? 0) })
      request.on('error', reject)
      request.end(body)
    })
    expect(status).toBe(403)
    expect(ask).not.toHaveBeenCalled()
  })

  it('answer 502 when the handler throws, and log the route without the body or the error\'s words', async () => {
    const lines: string[] = []
    for (const method of ['log', 'warn', 'error', 'info', 'debug'] as const) vi.spyOn(console, method).mockImplementation((...args: unknown[]) => { lines.push(args.map(String).join(' ')) })
    const { base, headers } = await start({ onDialNet: async () => { throw new Error(`boom ${PASS}`) } })
    const response = await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: JSON.stringify({ ssid: 'home-net', pass: PASS }) })
    expect(response.status).toBe(502)
    expect(await response.json()).toMatchObject({ kind: 'unavailable' })
    expect(lines.join('\n')).toContain('/api/dial/net/provision failed')
    expect(lines.join('\n')).not.toContain(PASS)
  })

  it('log nothing of a good request: not the password, not the network', async () => {
    const lines: string[] = []
    for (const method of ['log', 'warn', 'error', 'info', 'debug'] as const) vi.spyOn(console, method).mockImplementation((...args: unknown[]) => { lines.push(args.map(String).join(' ')) })
    const { base, headers } = await start({ onDialNet: dialNetHandler(async () => ({ ok: true, saved: true, ssid: 'home-net' })) })
    await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: JSON.stringify({ ssid: 'home-net', pass: PASS }) })
    await fetch(`${base}/api/dial/net/provision`, { method: 'POST', headers, body: `{"ssid":"home-net","pass":"${PASS}` })
    const text = lines.join('\n')
    expect(text).not.toContain(PASS)
    expect(text).not.toContain('home-net')
  })
})
