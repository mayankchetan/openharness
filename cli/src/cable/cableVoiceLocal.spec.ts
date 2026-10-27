// Voice captured on the dial, transcribed by a server the person runs (CABLE_STT_URL): an OpenAI-compatible
// /v1/audio/transcriptions endpoint such as whisper.cpp's, faster-whisper-server or LocalAI. The server is a stub on
// 127.0.0.1 with a random port, and fetch is the real one, so what is asserted is what goes over the wire.
// With CABLE_STT_URL unset nothing changes: the second half proves the account path is as it was.
import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http'
import type { AddressInfo } from 'node:net'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const mocked = vi.hoisted(() => ({
  env: {
    BACKEND_WS_URL: 'ws://127.0.0.1:1', CABLE_STT_PATH: '/api/voice/stt',
    CABLE_STT_URL: '', CABLE_STT_MODEL: 'whisper-1', CABLE_STT_API_KEY: '',
  },
}))
vi.mock('../lib/voiceRouter.js', () => ({ routeVoiceTask: vi.fn() }))
vi.mock('../config/env.js', () => ({ env: mocked.env }))

import { DaemonCableHost, multipart, wav, type CableHostWiring } from './cableHost.js'

const pcm = Buffer.from(Array.from({ length: 3_200 }, (_, i) => (i * 31) & 0xff))
const RATE = 16_000

interface Seen { method?: string; url?: string; headers: IncomingMessage['headers']; body: Buffer }
let server: Server
let seen: Seen[]
/** What the stub answers next: a status, a content type and a body; or never. */
let reply: { status: number; type: string; body: string } | 'hang'
let origin: string

beforeEach(async () => {
  seen = []
  reply = { status: 200, type: 'application/json', body: JSON.stringify({ text: 'ship the login fix' }) }
  server = createServer((req: IncomingMessage, res: ServerResponse) => {
    const parts: Buffer[] = []
    req.on('data', (chunk: Buffer) => parts.push(chunk))
    req.on('end', () => {
      seen.push({ method: req.method, url: req.url, headers: req.headers, body: Buffer.concat(parts) })
      if (reply === 'hang') return
      res.writeHead(reply.status, { 'Content-Type': reply.type })
      res.end(reply.body)
    })
  })
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve))
  origin = `http://127.0.0.1:${(server.address() as AddressInfo).port}`
  Object.assign(mocked.env, { BACKEND_WS_URL: origin.replace('http:', 'ws:'), CABLE_STT_URL: '', CABLE_STT_MODEL: 'whisper-1', CABLE_STT_API_KEY: '' })
})
afterEach(async () => {
  vi.useRealTimers()
  server.closeAllConnections()
  await new Promise<void>((resolve) => server.close(() => resolve()))
})

/** A host with no account at all unless told: the local path must not need one. */
function setup(account?: { signedIn: boolean }) {
  const log = vi.fn()
  const accessToken = vi.fn(async () => 'account-token')
  const wiring = {
    log, sessions: () => [], displayName: () => '',
    ...(account ? { accessToken, signedIn: () => account.signedIn, environment: () => 'prod' } : {}),
  } as unknown as CableHostWiring
  return { host: new DaemonCableHost(wiring), log, accessToken }
}

/** The form a request carried, read back the way a server reads it. */
async function formOf(request: Seen): Promise<FormData> {
  return new Response(request.body, { headers: { 'content-type': request.headers['content-type'] as string } }).formData()
}

describe('local transcription (CABLE_STT_URL set)', () => {
  beforeEach(() => { mocked.env.CABLE_STT_URL = `${origin}/v1/audio/transcriptions` })

  it('posts an OpenAI-style transcription request and returns the text of the reply', async () => {
    const { host } = setup()
    await expect(host.transcribe(pcm, RATE, 'en')).resolves.toBe('ship the login fix')
    expect(seen).toHaveLength(1)
    const [request] = seen
    expect(request.method).toBe('POST')
    expect(request.url).toBe('/v1/audio/transcriptions')
    expect(request.headers['content-type']).toMatch(/^multipart\/form-data; boundary=\S+$/)
    // The account's headers have no business on a server of the person's own.
    expect(request.headers.authorization).toBeUndefined()
    expect(request.headers['x-autonomous-env']).toBeUndefined()
    const form = await formOf(request)
    expect([...form.keys()].sort()).toEqual(['file', 'language', 'model', 'response_format'])
    expect(form.get('model')).toBe('whisper-1')
    expect(form.get('language')).toBe('en')
    expect(form.get('response_format')).toBe('json')
    const file = form.get('file') as File
    expect(file.name).toBe('voice.wav')
    expect(file.type).toBe('audio/wav')
    expect(Buffer.from(await file.arrayBuffer()).equals(wav(pcm, RATE))).toBe(true)
  })

  it('needs no sign-in at all, signed out or with no account wired, and never asks the account for a token', async () => {
    for (const account of [undefined, { signedIn: false }]) {
      const { host, accessToken } = setup(account)
      await expect(host.transcribe(pcm, RATE, 'en')).resolves.toBe('ship the login fix')
      expect(accessToken).not.toHaveBeenCalled()
    }
    expect(seen).toHaveLength(2)
  })

  it('sends the model and the dial\'s language as set, and leaves the language out when the dial names none', async () => {
    mocked.env.CABLE_STT_MODEL = 'Systran/faster-whisper-small'
    const { host } = setup()
    await host.transcribe(pcm, RATE, 'fr')
    await host.transcribe(pcm, RATE, '')
    const [first, second] = await Promise.all(seen.map(formOf))
    expect(first.get('model')).toBe('Systran/faster-whisper-small')
    expect(first.get('language')).toBe('fr')
    expect(second.has('language')).toBe(false)
  })

  it('sends the key as a bearer token when there is one, and never writes it down', async () => {
    mocked.env.CABLE_STT_API_KEY = 'sk-local-secret-123'
    const { host, log } = setup()
    await host.transcribe(pcm, RATE, 'en')
    expect(seen[0].headers.authorization).toBe('Bearer sk-local-secret-123')
    // and not on the failure paths either, where the log and the toast are written
    for (const failing of [{ status: 401, type: 'application/json', body: '{"error":"bad key sk-local-secret-123"}' }, { status: 500, type: 'text/plain', body: 'sk-local-secret-123' }]) {
      reply = failing
      const error = await host.transcribe(pcm, RATE, 'en').catch((e: Error) => e)
      expect((error as Error).message).not.toContain('sk-local-secret-123')
    }
    mocked.env.CABLE_STT_URL = 'http://127.0.0.1:1/closed'
    await host.transcribe(pcm, RATE, 'en').catch(() => {})
    expect(JSON.stringify(log.mock.calls)).not.toContain('sk-local-secret-123')
  })

  it('takes a plain-text reply when the server says text/plain, and trims it', async () => {
    reply = { status: 200, type: 'text/plain; charset=utf-8', body: '  open the pull request \n' }
    const { host } = setup()
    await expect(host.transcribe(pcm, RATE, 'en')).resolves.toBe('open the pull request')
  })

  it('returns silence as an empty string, which the session turns into "Didn\'t catch that"', async () => {
    reply = { status: 200, type: 'application/json', body: '{"text":""}' }
    const { host } = setup()
    await expect(host.transcribe(pcm, RATE, 'en')).resolves.toBe('')
  })

  it.each([
    ['an empty reply', { status: 200, type: 'application/json', body: '' }],
    ['an empty plain-text reply', { status: 200, type: 'text/plain', body: '  ' }],
    ['JSON with no text', { status: 200, type: 'application/json', body: '{"language":"en"}' }],
    ['JSON whose text is not a string', { status: 200, type: 'application/json', body: '{"text":7}' }],
    ['JSON that is not JSON', { status: 200, type: 'application/json', body: '{nope' }],
  ] as const)('says so when the server sends %s', async (_name, answer) => {
    reply = answer
    const { host } = setup()
    await expect(host.transcribe(pcm, RATE, 'en')).rejects.toThrow('The voice server sent no text')
  })

  it.each([
    [401, 'The voice server rejected the key'],
    [403, 'The voice server rejected the key'],
    [404, 'The voice server has no transcription endpoint'],
    [413, 'The voice server says the recording is too long'],
    [500, 'The voice server failed (HTTP 500)'],
    [503, 'The voice server failed (HTTP 503)'],
    [422, 'The voice server failed (HTTP 422)'],
  ])('turns HTTP %i into a short line for the dial, asks once, and logs the status but not the body', async (status, message) => {
    reply = { status, type: 'application/json', body: '{"error":"private detail about the audio"}' }
    const { host, log } = setup()
    await expect(host.transcribe(pcm, RATE, 'en')).rejects.toThrow(message)
    // No refresh and retry as the account path does on a 401: the key is the person's, there is nothing to refresh.
    expect(seen).toHaveLength(1)
    expect(log).toHaveBeenCalledWith(expect.stringContaining(`stt (local) HTTP ${status}`))
    expect(JSON.stringify(log.mock.calls)).not.toContain('private detail')
  })

  it('says the server cannot be reached when nothing answers, naming where it looked but not what it was sent', async () => {
    mocked.env.CABLE_STT_URL = 'http://user:pass@127.0.0.1:1/v1/audio/transcriptions'
    const { host, log } = setup()
    await expect(host.transcribe(pcm, RATE, 'en')).rejects.toThrow('Can\'t reach the voice server')
    expect(JSON.stringify(log.mock.calls)).toContain('http://127.0.0.1:1')
    expect(JSON.stringify(log.mock.calls)).not.toContain('pass')
  })

  it('refuses an address that is not an http(s) URL without trying it', async () => {
    const { host } = setup()
    for (const url of ['not a url', 'ftp://127.0.0.1/x', 'file:///etc/passwd']) {
      mocked.env.CABLE_STT_URL = url
      await expect(host.transcribe(pcm, RATE, 'en'), url).rejects.toThrow('The voice server address is not a web address')
    }
    expect(seen).toHaveLength(0)
  })

  it('gives up on a server that never answers, so the dial is not left waiting', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] })
    reply = 'hang'
    const { host } = setup()
    const pending = host.transcribe(pcm, RATE, 'en')
    const outcome = pending.catch((e: Error) => e)
    await vi.waitFor(() => expect(seen).toHaveLength(1), { timeout: 5_000 })
    await vi.advanceTimersByTimeAsync(10 * 60_000)
    expect(((await outcome) as Error).message).toBe('The voice server took too long')
  })
})

describe('transcription with CABLE_STT_URL unset (the account path, as it was)', () => {
  it('still refuses while signed out, and sends nothing anywhere', async () => {
    for (const unset of ['', undefined]) {
      mocked.env.CABLE_STT_URL = unset as unknown as string
      for (const account of [undefined, { signedIn: false }]) {
        const { host } = setup(account)
        await expect(host.transcribe(pcm, RATE, 'en')).rejects.toThrow('Sign in on your computer to use voice')
      }
    }
    expect(seen).toHaveLength(0)
  })

  it('posts to the backend with the account\'s token and environment, the lang in the query and the WAV alone in the body', async () => {
    reply = { status: 200, type: 'application/json', body: JSON.stringify({ success: true, data: { transcript: 'from the account' } }) }
    const { host, accessToken } = setup({ signedIn: true })
    await expect(host.transcribe(pcm, RATE, 'en')).resolves.toBe('from the account')
    expect(accessToken).toHaveBeenCalledTimes(1)
    const [request] = seen
    expect(request.url).toBe('/api/voice/stt?lang=en')
    expect(request.headers.authorization).toBe('Bearer account-token')
    expect(request.headers['x-autonomous-env']).toBe('prod')
    const boundary = /boundary=(\S+)$/.exec(request.headers['content-type'] as string)![1]
    expect(request.body.equals(multipart(wav(pcm, RATE), 'voice.wav', 'audio/wav', boundary))).toBe(true)
  })

  it('still refreshes once on a 401 and gives the account\'s sign-in line', async () => {
    reply = { status: 401, type: 'application/json', body: JSON.stringify({ success: false, error: { code: 'UNAUTHORIZED' } }) }
    const { host, accessToken } = setup({ signedIn: true })
    await expect(host.transcribe(pcm, RATE, 'en')).rejects.toThrow('Sign in again on your computer to use voice')
    expect(seen).toHaveLength(2)
    expect(accessToken).toHaveBeenCalledTimes(2)
  })
})

describe('the multipart helper', () => {
  it('writes the one file part exactly as it always did when given no fields', () => {
    const body = multipart(Buffer.from('AUDIO'), 'voice.wav', 'audio/wav', 'B')
    expect(body.toString('utf8')).toBe(
      '--B\r\nContent-Disposition: form-data; name="file"; filename="voice.wav"\r\nContent-Type: audio/wav\r\n\r\nAUDIO\r\n--B--\r\n')
  })

  it('writes text fields before the file when given them', () => {
    const body = multipart(Buffer.from('AUDIO'), 'voice.wav', 'audio/wav', 'B', { model: 'whisper-1', language: 'en' })
    expect(body.toString('utf8')).toBe(
      '--B\r\nContent-Disposition: form-data; name="model"\r\n\r\nwhisper-1\r\n'
      + '--B\r\nContent-Disposition: form-data; name="language"\r\n\r\nen\r\n'
      + '--B\r\nContent-Disposition: form-data; name="file"; filename="voice.wav"\r\nContent-Type: audio/wav\r\n\r\nAUDIO\r\n--B--\r\n')
  })
})
