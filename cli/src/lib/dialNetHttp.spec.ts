import { describe, expect, it, vi } from 'vitest'

import { DIAL_NET_WAIT_MS, dialNetHandler, dialNetRequest } from './dialNetHttp.js'

const PASS = 'correct horse battery'
const ask = () => vi.fn<(payload: Record<string, unknown>, waitMs: number) => Promise<Record<string, unknown>>>(async () => ({ ok: true }))

describe('the dial Wi-Fi routes: what is asked', () => {
  it('provision sends the network, and only what the devices take', async () => {
    const a = ask()
    const reply = await dialNetRequest('provision', { ssid: 'home-net', pass: PASS, dial: ' B8:1F:3F:CA:E5:58 ', host: '192.168.1.20', port: 18480, mode: 0, key: 'abc', extra: 1 }, a)
    expect(reply.status).toBe(200)
    expect(a).toHaveBeenCalledWith({ op: 'provision', ssid: 'home-net', pass: PASS, dial: 'B8:1F:3F:CA:E5:58', host: '192.168.1.20', port: 18480, mode: 0 }, DIAL_NET_WAIT_MS)
  })

  it('provision needs only an ssid and a password; an open network has an empty one', async () => {
    const a = ask()
    await dialNetRequest('provision', { ssid: 'cafe', pass: '' }, a)
    expect(a).toHaveBeenCalledWith({ op: 'provision', ssid: 'cafe', pass: '' }, DIAL_NET_WAIT_MS)
  })

  it.each(['status', 'forget', 'rotate'])('%s takes no body at all, or a dial', async (op) => {
    const a = ask()
    await dialNetRequest(op, undefined, a)
    await dialNetRequest(op, { dial: 'serial-1' }, a)
    expect(a.mock.calls.map(([payload]) => payload)).toEqual([{ op }, { op, dial: 'serial-1' }])
  })

  it('mode needs 0 or 1', async () => {
    const a = ask()
    await dialNetRequest('mode', { mode: 1 }, a)
    expect(a).toHaveBeenCalledWith({ op: 'mode', mode: 1 }, DIAL_NET_WAIT_MS)
  })

  it('does not know any other route', async () => {
    const a = ask()
    expect(await dialNetRequest('reboot', {}, a)).toEqual({ status: 404, body: { error: 'not found' } })
    expect(await dialNetRequest('', {}, a)).toEqual({ status: 404, body: { error: 'not found' } })
    expect(a).not.toHaveBeenCalled()
  })
})

describe('the dial Wi-Fi routes: what is refused with a 400 before anyone is asked, and never echoed', () => {
  const secret = 'S3cretValue!!'
  const cases: Array<[string, unknown, string]> = [
    ['provision', { pass: PASS }, 'ssid'],
    ['provision', { ssid: '', pass: PASS }, 'ssid'],
    ['provision', { ssid: 'x'.repeat(33) + secret, pass: PASS }, 'ssid'],
    ['provision', { ssid: 'a\nb', pass: PASS }, 'ssid'],
    ['provision', { ssid: 'home' }, 'pass'],
    ['provision', { ssid: 'home', pass: 'short' + secret.slice(0, 1) }, 'pass'],
    ['provision', { ssid: 'home', pass: secret.repeat(6) }, 'pass'],
    ['provision', { ssid: 'home', pass: 12345678 }, 'pass'],
    ['provision', { ssid: 'home', pass: PASS, host: 'dial.local' }, 'host'],
    ['provision', { ssid: 'home', pass: PASS, host: '8.8.8.8' }, 'host'],
    ['provision', { ssid: 'home', pass: PASS, host: '127.0.0.1' }, 'host'],
    ['provision', { ssid: 'home', pass: PASS, port: 80 }, 'port'],
    ['provision', { ssid: 'home', pass: PASS, port: '18480' }, 'port'],
    ['provision', { ssid: 'home', pass: PASS, mode: 2 }, 'mode'],
    ['provision', { ssid: 'home', pass: PASS, dial: 'x'.repeat(65) }, 'dial'],
    ['provision', { ssid: 'home', pass: PASS, dial: 5 }, 'dial'],
    ['mode', {}, 'mode'],
    ['mode', { mode: 'on' }, 'mode'],
    ['status', { dial: 'a\tb' }, 'dial'],
  ]
  it.each(cases)('%s %j names the field %s', async (op, body, field) => {
    const a = ask()
    const reply = await dialNetRequest(op, body, a)
    expect(reply.status).toBe(400)
    expect(reply.body).toMatchObject({ kind: 'bad_request', field })
    const text = JSON.stringify(reply.body)
    expect(text).not.toContain(secret)
    expect(text).not.toContain(PASS)
    expect(text).not.toContain('short')
    expect(a).not.toHaveBeenCalled()
  })

  it('a body that is not an object is a 400 too', async () => {
    const a = ask()
    for (const body of [[], 'x', 7, null]) expect((await dialNetRequest('provision', body, a)).status).toBe(400)
    expect(a).not.toHaveBeenCalled()
  })
})

describe('the dial Wi-Fi routes: what is answered', () => {
  const answer = (over: Record<string, unknown>) => async () => over

  it('a success is passed on, field by field; the password and the key are never among them', async () => {
    const reply = await dialNetRequest('provision', { ssid: 'home', pass: PASS }, answer({
      ok: true, dial: 'B8:1F:3F:CA:E5:58', saved: true, ssid: 'home', host: '192.168.1.20', port: 18480, mode: 1,
      pass: PASS, key: 'a2V5', nested: { pass: PASS },
    }))
    expect(reply).toEqual({ status: 200, body: { ok: true, dial: 'B8:1F:3F:CA:E5:58', saved: true, ssid: 'home', host: '192.168.1.20', port: 18480, mode: 1 } })
  })

  it('a status keeps its lists and its nested dial, and drops what it does not know', async () => {
    const reply = await dialNetRequest('status', {}, answer({
      ok: true, lan: '192.168.1.20', port: 18480,
      keyed: [{ dial: 'B8:1F:3F:CA:E5:58', createdAt: 1, lastSeen: 2, lastAddr: '192.168.1.40', key: 'zzz' }],
      usb: { dial: 'B8:1F:3F:CA:E5:58', saved: true, ssid: 'home', keyed: true, pass: PASS },
      usbProblem: { kind: 'ambiguous', message: 'two dials' },
    }))
    expect(reply.status).toBe(200)
    expect(reply.body).toEqual({
      ok: true, lan: '192.168.1.20', port: 18480,
      keyed: [{ dial: 'B8:1F:3F:CA:E5:58', createdAt: 1, lastSeen: 2, lastAddr: '192.168.1.40' }],
      usb: { dial: 'B8:1F:3F:CA:E5:58', saved: true, ssid: 'home', keyed: true },
      usbProblem: { kind: 'ambiguous', message: 'two dials' },
    })
  })

  it.each([
    ['bad_request', 400], ['no_dial', 404], ['ambiguous', 409], ['busy', 409], ['transport', 409], ['unsupported', 409],
    ['timeout', 504], ['gone', 502], ['device', 502], ['keystore', 500], ['something-new', 502],
  ])('a failure of kind %s is a %i with its message, and the field or the dial\'s word for it', async (kind, status) => {
    const reply = await dialNetRequest('provision', { ssid: 'home', pass: PASS }, answer({ ok: false, kind, message: 'the dial refused: pass', err: 'pass', field: 'pass', pass: PASS }))
    expect(reply.status).toBe(status)
    expect(reply.body).toEqual({ error: 'the dial refused: pass', kind: typeof kind === 'string' && kind !== 'something-new' ? kind : 'device', err: 'pass', field: 'pass' })
  })

  it('a failure with a message that is not a short string, an err that is not a word, or a field that is not one, is cleaned', async () => {
    const reply = await dialNetRequest('provision', { ssid: 'home', pass: PASS }, answer({ ok: false, kind: 'device', message: 7, err: 'Not A Word', field: 'key' }))
    expect(reply.body).toEqual({ error: 'The dial did not do it.', kind: 'device' })
    const long = await dialNetRequest('provision', { ssid: 'home', pass: PASS }, answer({ ok: false, kind: 'device', message: 'x'.repeat(500) }))
    expect(((long.body as { error: string }).error).length).toBeLessThanOrEqual(300)
  })

  it('the devices not answering is a 503; the route not being there is a 404', async () => {
    const unavailable = { status: 503, body: { error: 'The devices service did not answer. See the daemon\'s log.', kind: 'unavailable' } }
    expect(await dialNetRequest('status', {}, async () => { throw new Error('closed') })).toEqual(unavailable)
    expect(await dialNetRequest('status', {}, answer({ error: 'SERVICE_UNAVAILABLE' }))).toEqual(unavailable)
    expect(await dialNetRequest('status', {}, answer({ error: 'OWNER_REQUIRED' }))).toEqual(unavailable)
    expect(await dialNetRequest('status', {}, answer({ error: 'DISABLED' }))).toEqual({ status: 404, body: { error: 'not found' } })
  })

  it('an answer that is neither ok nor a failure is a 502', async () => {
    expect((await dialNetRequest('status', {}, answer({}))).status).toBe(502)
  })

  it('the handler is the same function with the asker bound', async () => {
    const a = ask()
    const handler = dialNetHandler(a)
    expect((await handler('status', undefined)).status).toBe(200)
    expect(a).toHaveBeenCalledWith({ op: 'status' }, DIAL_NET_WAIT_MS)
  })
})

describe('the dial phone routes (B6)', () => {
  it('phone-set sends only the dial and a mode of 0 or 2; a key or any other field in the body is dropped', async () => {
    const a = ask()
    await dialNetRequest('phone-set', undefined, a)
    await dialNetRequest('phone-set', { dial: ' B8:1F:3F:CA:E5:58 ', mode: 0, key: 'abc', ssid: 'x', pass: PASS, extra: 1 }, a)
    await dialNetRequest('phone-set', { mode: 2 }, a)
    expect(a.mock.calls.map(([payload]) => payload)).toEqual([
      { op: 'phone_set' },
      { op: 'phone_set', dial: 'B8:1F:3F:CA:E5:58', mode: 0 },
      { op: 'phone_set', mode: 2 },
    ])
    expect(a.mock.calls[0][1]).toBe(DIAL_NET_WAIT_MS)
  })

  it('phone-mode needs 0 or 2, and phone-forget takes a dial or nothing', async () => {
    const a = ask()
    await dialNetRequest('phone-mode', { mode: 2 }, a)
    await dialNetRequest('phone-mode', { mode: 0, dial: 'serial-1' }, a)
    await dialNetRequest('phone-forget', undefined, a)
    await dialNetRequest('phone-forget', { dial: 'serial-1', mode: 2 }, a)
    expect(a.mock.calls.map(([payload]) => payload)).toEqual([
      { op: 'phone_mode', mode: 2 },
      { op: 'phone_mode', dial: 'serial-1', mode: 0 },
      { op: 'phone_forget' },
      { op: 'phone_forget', dial: 'serial-1' },
    ])
  })

  it.each([
    ['phone-set', { mode: 1 }, 'mode'],
    ['phone-set', { mode: '2' }, 'mode'],
    ['phone-set', { dial: 'x'.repeat(65) }, 'dial'],
    ['phone-mode', {}, 'mode'],
    ['phone-mode', { mode: 1 }, 'mode'],
    ['phone-mode', { mode: 3 }, 'mode'],
    ['phone-forget', { dial: 'a\nb' }, 'dial'],
  ])('%s %j is a 400 for %s before anyone is asked', async (op, body, field) => {
    const a = ask()
    const reply = await dialNetRequest(op, body, a)
    expect(reply.status).toBe(400)
    expect(reply.body).toMatchObject({ kind: 'bad_request', field })
    expect(a).not.toHaveBeenCalled()
  })

  it('the underscore names of the payload are not routes', async () => {
    const a = ask()
    for (const op of ['phone_set', 'phone_mode', 'phone_forget', 'phone']) expect(await dialNetRequest(op, {}, a)).toEqual({ status: 404, body: { error: 'not found' } })
    expect(a).not.toHaveBeenCalled()
  })

  it('an answer carries ble and the mode, and still no key', async () => {
    const reply = await dialNetRequest('phone-set', {}, async () => ({ ok: true, dial: 'B8:1F:3F:CA:E5:58', ble: true, mode: 2, key: 'AAAA', pass: 'x' }))
    expect(reply).toEqual({ status: 200, body: { ok: true, dial: 'B8:1F:3F:CA:E5:58', ble: true, mode: 2 } })
    const status = await dialNetRequest('status', {}, async () => ({ ok: true, usb: { dial: 'B8:1F:3F:CA:E5:58', saved: false, ble: true, mode: 2, keyed: true } }))
    expect(status.body).toMatchObject({ usb: { ble: true, mode: 2, keyed: true } })
  })

  it('a failure keeps its kind, and the 502 / 409 mapping is the Wi-Fi one', async () => {
    const reply = await dialNetRequest('phone-set', {}, async () => ({ ok: false, kind: 'unsupported', message: 'this dial\'s firmware has no phone mode' }))
    expect(reply.status).toBe(409)
    expect(reply.body).toMatchObject({ kind: 'unsupported' })
  })
})
