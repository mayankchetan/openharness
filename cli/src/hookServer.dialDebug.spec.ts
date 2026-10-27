// The dial debug routes on the daemon's local server (hookServer.ts): present only when the daemon is given a
// handler for them, which it is only with CABLE_DEBUG_API on (lib/dialDebugHttp.ts); loopback and the local
// credential are the server's own rules, the same as for every other route.
import type { Server } from 'node:http'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { env } from './config/env.js'
import { startHookServer, type HookServerHandlers } from './hookServer.js'
import { dialDebugHandler } from './lib/dialDebugHttp.js'
import { readHookCredential } from './lib/hookAuth.js'

let server: Server | null = null
afterEach(async () => {
  if (!server) return
  await new Promise<void>((resolve) => server!.close(() => resolve()))
  server = null
})

const PNG = Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), Buffer.from('a picture')])

async function start(overrides: Partial<HookServerHandlers> = {}) {
  const started = await startHookServer(0, { onRegistered: vi.fn(), onSessionEnd: vi.fn(), ...overrides })
  server = started.server
  const credential = readHookCredential(env.ADAPTER_DATA_DIR)
  if (!credential) throw new Error('hook credential was not created')
  const base = `http://127.0.0.1:${started.port}`
  // This OS user's process: the local credential and the CSRF header every mutating route wants.
  const headers = { 'x-harness-local': credential, 'x-adapter-local': '1', 'content-type': 'application/json' }
  return { base, headers, credential }
}

describe('the dial debug routes on the local server', () => {
  it('answers 404 exactly as it does for any unknown route when it has no handler for them (CABLE_DEBUG_API off)', async () => {
    const { base, headers } = await start({ onDialDebug: dialDebugHandler(false, vi.fn()) })
    const unknown = await fetch(`${base}/api/dial/nothing`, { method: 'POST', headers, body: '{}' })
    expect(unknown.status).toBe(404)
    const unknownBody = await unknown.text()
    for (const op of ['shot', 'tap', 'press', 'swipe']) {
      const response = await fetch(`${base}/api/dial/debug/${op}`, { method: 'POST', headers, body: JSON.stringify({ x: 1, y: 2 }) })
      expect(response.status, op).toBe(404)
      expect(response.headers.get('content-type')).toBe(unknown.headers.get('content-type'))
      expect(await response.text()).toBe(unknownBody)
    }
  })

  it('returns the PNG of a screenshot as image/png', async () => {
    const ask = vi.fn(async () => ({ ok: true, png: PNG.toString('base64') }))
    const { base, headers } = await start({ onDialDebug: dialDebugHandler(true, ask) })
    const response = await fetch(`${base}/api/dial/debug/shot`, { method: 'POST', headers, body: JSON.stringify({ dial: 'aa:bb' }) })
    expect(response.status).toBe(200)
    expect(response.headers.get('content-type')).toBe('image/png')
    expect(response.headers.get('cache-control')).toBe('no-store')
    expect(Buffer.from(await response.arrayBuffer()).equals(PNG)).toBe(true)
    expect(ask).toHaveBeenCalledWith({ op: 'shot', dial: 'aa:bb' }, expect.any(Number))
  })

  it('takes a screenshot request with no body at all', async () => {
    const ask = vi.fn(async () => ({ ok: true, png: PNG.toString('base64') }))
    const { base, headers } = await start({ onDialDebug: dialDebugHandler(true, ask) })
    const response = await fetch(`${base}/api/dial/debug/shot`, { method: 'POST', headers })
    expect(response.status).toBe(200)
    expect(ask).toHaveBeenCalledWith({ op: 'shot' }, expect.any(Number))
  })

  it('answers a touch with JSON, and a failure with its status and reason', async () => {
    const ask = vi.fn<(payload: Record<string, unknown>, waitMs: number) => Promise<Record<string, unknown>>>()
    const { base, headers } = await start({ onDialDebug: dialDebugHandler(true, ask) })
    ask.mockResolvedValueOnce({ ok: true })
    const done = await fetch(`${base}/api/dial/debug/swipe`, { method: 'POST', headers, body: JSON.stringify({ x1: 380, y1: 233, x2: 80, y2: 233, ms: 250 }) })
    expect(done.status).toBe(200)
    expect(done.headers.get('content-type')).toBe('application/json')
    expect(await done.json()).toEqual({ ok: true })
    ask.mockResolvedValueOnce({ ok: false, kind: 'device', reason: 'range', message: 'the dial refused: range' })
    const refused = await fetch(`${base}/api/dial/debug/tap`, { method: 'POST', headers, body: JSON.stringify({ x: 1, y: 1 }) })
    expect(refused.status).toBe(502)
    expect(await refused.json()).toEqual({ error: 'the dial refused: range', kind: 'device', reason: 'range' })
  })

  it('refuses a body that is not JSON, a wrong method, and a caller without the CSRF header, before asking anyone', async () => {
    const ask = vi.fn(async () => ({ ok: true }))
    const { base, headers } = await start({ onDialDebug: dialDebugHandler(true, ask) })
    expect((await fetch(`${base}/api/dial/debug/tap`, { method: 'POST', headers, body: '{nope' })).status).toBe(400)
    expect((await fetch(`${base}/api/dial/debug/tap`, { method: 'GET', headers })).status).toBe(405)
    const { 'x-adapter-local': _csrf, ...noCsrf } = headers
    expect((await fetch(`${base}/api/dial/debug/tap`, { method: 'POST', headers: noCsrf, body: JSON.stringify({ x: 1, y: 1 }) })).status).toBe(403)
    expect(ask).not.toHaveBeenCalled()
  })

  it('wants the local credential like every other route, and a loopback Host', async () => {
    const ask = vi.fn(async () => ({ ok: true }))
    const { base, headers } = await start({ onDialDebug: dialDebugHandler(true, ask) })
    const { 'x-harness-local': _credential, ...anonymous } = headers
    const body = JSON.stringify({ x: 1, y: 1 })
    expect((await fetch(`${base}/api/dial/debug/tap`, { method: 'POST', headers: anonymous, body })).status).toBe(401)
    expect((await fetch(`${base}/api/dial/debug/tap`, { method: 'POST', headers: { ...headers, 'x-harness-local': 'wrong' }, body })).status).toBe(401)
    // A page that re-points its own name at 127.0.0.1 sends its own Host: refused before any route.
    const http = await import('node:http')
    const status = await new Promise<number>((resolve, reject) => {
      const request = http.request(`${base}/api/dial/debug/tap`, { method: 'POST', headers: { ...headers, host: 'evil.example' } }, (res) => { res.resume(); resolve(res.statusCode ?? 0) })
      request.on('error', reject)
      request.end(body)
    })
    expect(status).toBe(403)
    expect(ask).not.toHaveBeenCalled()
  })

  it('answers 502 when its handler throws, rather than leaving the caller waiting', async () => {
    const { base, headers } = await start({ onDialDebug: async () => { throw new Error('boom') } })
    const response = await fetch(`${base}/api/dial/debug/shot`, { method: 'POST', headers })
    expect(response.status).toBe(502)
    expect(await response.json()).toMatchObject({ kind: 'unavailable' })
  })
})
