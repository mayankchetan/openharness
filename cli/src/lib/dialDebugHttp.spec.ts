import { describe, expect, it, vi } from 'vitest'

import { DIAL_DEBUG_WAIT_MS, dialDebugHandler, dialDebugRequest } from './dialDebugHttp.js'

const PNG = Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), Buffer.from('rest of a picture')])

const asking = (answer: Record<string, unknown> | Error) => vi.fn(async (_payload: Record<string, unknown>) => {
  if (answer instanceof Error) throw answer
  return answer
})

describe('the dial debug routes: what is asked of the devices', () => {
  it('asks for a screenshot of the first dial by default, or of the one named', async () => {
    const ask = asking({ ok: true, png: PNG.toString('base64') })
    expect(await dialDebugRequest('shot', undefined, ask)).toEqual({ status: 200, png: PNG })
    expect(await dialDebugRequest('shot', {}, ask)).toEqual({ status: 200, png: PNG })
    await dialDebugRequest('shot', { dial: ' aa:bb ' }, ask)
    expect(ask.mock.calls.map(([payload]) => payload)).toEqual([{ op: 'shot' }, { op: 'shot' }, { op: 'shot', dial: 'aa:bb' }])
  })

  it('asks for a tap, a press and a swipe with the numbers it was given, and the defaults the document uses', async () => {
    const ask = asking({ ok: true })
    expect(await dialDebugRequest('tap', { x: 233, y: 100 }, ask)).toEqual({ status: 200, body: { ok: true } })
    await dialDebugRequest('press', { x: 233, y: 233 }, ask)
    await dialDebugRequest('press', { x: 1, y: 2, ms: 5000, dial: 'serial-1' }, ask)
    await dialDebugRequest('swipe', { x1: 380, y1: 233, x2: 80, y2: 233 }, ask)
    await dialDebugRequest('swipe', { x1: 380, y1: 233, x2: 80, y2: 233, ms: 20 }, ask)
    expect(ask.mock.calls.map(([payload]) => payload)).toEqual([
      { op: 'tap', x: 233, y: 100 },
      { op: 'press', x: 233, y: 233, ms: 700 },
      { op: 'press', x: 1, y: 2, ms: 5000, dial: 'serial-1' },
      { op: 'swipe', x1: 380, y1: 233, x2: 80, y2: 233, ms: 250 },
      { op: 'swipe', x1: 380, y1: 233, x2: 80, y2: 233, ms: 20 },
    ])
  })

  it('turns away a request that is not shaped right, without asking the devices', async () => {
    const ask = asking({ ok: true })
    const bad: Array<[string, unknown]> = [
      ['tap', undefined], ['tap', null], ['tap', []], ['tap', 'x'], ['tap', {}], ['tap', { x: 1 }], ['tap', { x: 1.5, y: 2 }],
      ['tap', { x: '1', y: 2 }], ['tap', { x: -1, y: 2 }], ['tap', { x: 1, y: 466 }], ['tap', { x: 1, y: 2, dial: 7 }],
      ['press', { x: 1, y: 2, ms: 49 }], ['press', { x: 1, y: 2, ms: 5001 }], ['press', { x: 1, y: 2, ms: 'long' }],
      ['swipe', { x1: 1, y1: 2, x2: 3 }], ['swipe', { x1: 1, y1: 2, x2: 3, y2: 4, ms: 19 }], ['swipe', { x1: 1, y1: 2, x2: 3, y2: 4, ms: 3001 }],
      ['shot', 'text'], ['shot', { dial: 7 }], ['shot', { dial: 'x'.repeat(65) }],
    ]
    for (const [op, body] of bad) {
      const reply = await dialDebugRequest(op, body, ask)
      expect(reply, `${op} ${JSON.stringify(body)}`).toMatchObject({ status: 400, body: { kind: 'bad_request', error: expect.any(String) } })
    }
    expect(ask).not.toHaveBeenCalled()
  })

  it('knows no other operation', async () => {
    const ask = asking({ ok: true })
    expect(await dialDebugRequest('reset', {}, ask)).toEqual({ status: 404, body: { error: 'not found' } })
    expect(await dialDebugRequest('__proto__', {}, ask)).toEqual({ status: 404, body: { error: 'not found' } })
    expect(ask).not.toHaveBeenCalled()
  })
})

describe('the dial debug routes: what comes back', () => {
  const failure = (kind: string, extra: Record<string, unknown> = {}) => ({ ok: false, kind, message: `because ${kind}`, ...extra })
  it.each([
    ['no_dial', 404], ['busy', 409], ['timeout', 504], ['device', 502], ['incomplete', 502], ['checksum', 502], ['gone', 502],
  ])('maps %s to %i, with the message and the dial\'s reason', async (kind, status) => {
    const reply = await dialDebugRequest('tap', { x: 1, y: 2 }, asking(failure(kind, { reason: 'asleep' })))
    expect(reply).toEqual({ status, body: { error: `because ${kind}`, kind, reason: 'asleep' } })
    // and for a screenshot too, never a partial image
    expect(await dialDebugRequest('shot', {}, asking(failure(kind)))).toEqual({ status, body: { error: `because ${kind}`, kind } })
  })

  it('says 503 when the devices cannot answer, and 404 when the devices say the API is off', async () => {
    for (const error of ['SERVICE_UNAVAILABLE', 'SERVICE_FAILED', 'LOCAL_ONLY', 'OWNER_REQUIRED']) {
      expect(await dialDebugRequest('shot', {}, asking({ error }))).toMatchObject({ status: 503, body: { kind: 'unavailable' } })
    }
    expect(await dialDebugRequest('shot', {}, asking({ error: 'DISABLED' }))).toEqual({ status: 404, body: { error: 'not found' } })
    expect(await dialDebugRequest('shot', {}, asking(new Error('link broke')))).toMatchObject({ status: 503, body: { kind: 'unavailable' } })
  })

  it('never passes on an image that is not a PNG, or no image at all', async () => {
    for (const answer of [{ ok: true }, { ok: true, png: '' }, { ok: true, png: 7 }, { ok: true, png: Buffer.from('GIF89a').toString('base64') }]) {
      expect(await dialDebugRequest('shot', {}, asking(answer)), JSON.stringify(answer)).toMatchObject({ status: 502, body: { kind: 'device' } })
    }
    expect(await dialDebugRequest('tap', { x: 1, y: 2 }, asking({ nonsense: true }))).toMatchObject({ status: 502 })
  })
})

describe('the handler the daemon hands its local server', () => {
  it('is no handler at all while CABLE_DEBUG_API is off, so the routes are not there to answer', () => {
    expect(dialDebugHandler(false, asking({ ok: true }))).toBeUndefined()
  })

  it('asks the devices, waiting long enough for a picture, when it is on', async () => {
    const ask = vi.fn(async (_payload: Record<string, unknown>, _waitMs: number) => ({ ok: true }))
    const handler = dialDebugHandler(true, ask)!
    expect(await handler('tap', { x: 3, y: 4 })).toEqual({ status: 200, body: { ok: true } })
    expect(ask).toHaveBeenCalledWith({ op: 'tap', x: 3, y: 4 }, DIAL_DEBUG_WAIT_MS)
    expect(DIAL_DEBUG_WAIT_MS).toBeGreaterThan(20_000)
  })
})
