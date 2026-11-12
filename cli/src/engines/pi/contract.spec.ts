/**
 * The Pi extension the daemon installs runs inside Pi, not here. These tests render its text, load it, and
 * fire Pi's own events at it, with `fetch` answered the way the daemon answers a hook, to pin when it stops
 * announcing: only on a real accept (hookServer.ts answers "ignored" and "retry" with a 2xx too).
 */
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { pathToFileURL } from 'node:url'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { nativePluginSource } from '../kit/nativePlugin.js'
import { PI_EXTENSION } from './contract.js'

type Handler = (event: unknown, ctx: unknown) => Promise<void>
type Answer = Response | Error

let dir = ''
const savedPane = process.env.TMUX_PANE
const savedPty = process.env.HARNESS_PANE

beforeEach(() => {
  dir = mkdtempSync(join(tmpdir(), 'pi-ext-'))
  process.env.TMUX_PANE = '%7'
  delete process.env.HARNESS_PANE
})
afterEach(() => {
  vi.unstubAllGlobals()
  if (savedPane === undefined) delete process.env.TMUX_PANE; else process.env.TMUX_PANE = savedPane
  if (savedPty !== undefined) process.env.HARNESS_PANE = savedPty
  rmSync(dir, { recursive: true, force: true })
})

const json = (status: number, body: unknown): Response => new Response(JSON.stringify(body), { status })

/** Render, write and load the extension; hand back Pi's three events and the requests it made. */
async function extension(answers: Answer[], opts: { transcript?: boolean } = {}) {
  const credential = join(dir, 'hook-credential')
  writeFileSync(credential, 'secret-token\n')
  const source = nativePluginSource(PI_EXTENSION, {
    port: '19999', credential: JSON.stringify(credential), version: JSON.stringify('9.9.9'),
  })
  const file = join(dir, 'launcher-register.ts')
  writeFileSync(file, source)

  const requests: Array<{ url: string; init: RequestInit; body: Record<string, unknown> }> = []
  let next = 0
  vi.stubGlobal('fetch', vi.fn(async (url: string, init: RequestInit) => {
    requests.push({ url, init, body: JSON.parse(String(init.body)) })
    const answer = answers[Math.min(next++, answers.length - 1)]
    if (answer instanceof Error) throw answer
    return answer.clone()
  }))

  const transcript = join(dir, 'session.jsonl')
  if (opts.transcript) writeFileSync(transcript, '{"type":"session"}\n')
  const handlers = new Map<string, Handler>()
  const mod = await import(/* @vite-ignore */ pathToFileURL(file).href) as { default: (pi: unknown) => void }
  mod.default({ on: (name: string, handler: Handler) => { handlers.set(name, handler) } })
  const ctx = { cwd: dir, sessionManager: { getSessionFile: () => transcript, getSessionId: () => 'pi-session-1' } }
  const fire = async (...names: string[]) => { for (const name of names) await handlers.get(name)!({}, ctx) }
  return { fire, requests, transcript }
}

const ALL = ['session_start', 'turn_start', 'turn_end']

describe('the Pi extension announces only on a real accept', () => {
  it('posts the session once when the daemon accepts it and the transcript exists', async () => {
    const e = await extension([json(200, { ok: true })], { transcript: true })
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(1)
    expect(e.requests[0].url).toBe('http://127.0.0.1:19999/api/hook/session-start')
    expect((e.requests[0].init.headers as Record<string, string>)['x-harness-hook-token']).toBe('secret-token')
    expect(e.requests[0].body).toMatchObject({ engine: 'pi', pluginVersion: '9.9.9', sessionId: 'pi-session-1', transcriptPath: e.transcript, tmuxPane: '%7' })
  })

  it('announces without a path first, then once more with the path when the transcript appears', async () => {
    const e = await extension([json(200, { ok: true })])
    await e.fire('session_start')
    expect(e.requests).toHaveLength(1)
    expect(e.requests[0].body.transcriptPath).toBeUndefined()
    await e.fire('turn_start')                   // still no transcript: announced, nothing more to say
    expect(e.requests).toHaveLength(1)
    writeFileSync(e.transcript, '{"type":"session"}\n')
    await e.fire('turn_end')
    expect(e.requests).toHaveLength(2)
    expect(e.requests[1].body.transcriptPath).toBe(e.transcript)
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(2)
  })

  it('keeps trying on every event while the daemon says ignored', async () => {
    const e = await extension([json(200, { ignored: true, reason: 'not_in_terminal' })], { transcript: true })
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(3)
  })

  it('keeps trying on every event while the daemon says retry (202)', async () => {
    const e = await extension([json(202, { pending: false, retry: true })], { transcript: true })
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(3)
  })

  it('stops retrying after an ignored answer turns into an accept', async () => {
    const e = await extension([json(200, { ignored: true }), json(202, { retry: true }), json(200, { ok: true })], { transcript: true })
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(3)
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(3)
  })

  it('counts a queued admission as announced', async () => {
    const e = await extension([json(200, { pending: true })], { transcript: true })
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(1)
  })

  it('takes a 200 whose body is not JSON (an older daemon) as accepted', async () => {
    const e = await extension([new Response('OK', { status: 200 })], { transcript: true })
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(1)
  })

  it.each([500, 401])('keeps trying on every event after a %i', async (status) => {
    const e = await extension([json(status, { error: 'no' })], { transcript: true })
    await e.fire(...ALL)
    expect(e.requests).toHaveLength(3)
  })

  it('does not throw when fetch rejects, and tries again on the next event', async () => {
    const e = await extension([new Error('ECONNREFUSED')], { transcript: true })
    await expect(e.fire(...ALL)).resolves.toBeUndefined()
    expect(e.requests).toHaveLength(3)
  })
})
