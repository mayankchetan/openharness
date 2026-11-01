// The Wi-Fi settings of a dial over its USB cable (net.set, net.get, net.clear, net.mode, net.key, net.host and the one
// answer, net.state), as the layer that asks and waits. The dial is played by these tests.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { DialNet, NET_TIMEOUT_MS, type NetFailure, type NetRequest } from './dialNet.js'

const KEY = Buffer.alloc(32, 7)
const SET: NetRequest = { op: 'set', ssid: 'home-net', pass: 'correct horse', host: '192.168.1.20', port: 18480, key: KEY, mode: 1 }

function rig(over: Partial<ConstructorParameters<typeof DialNet>[0]> = {}) {
  const sent: Array<Record<string, unknown>> = []
  const state = { onWire: true }
  const lines: string[] = []
  const net = new DialNet({ send: async (message) => { sent.push(message); return state.onWire }, log: (line) => lines.push(line), firstId: 9, ...over })
  return { net, sent, state, lines }
}
const failure = (result: unknown): NetFailure => {
  const r = result as NetFailure
  if (r.ok) throw new Error('expected a failure')
  return r
}

beforeEach(() => { vi.useFakeTimers() })
afterEach(() => { vi.useRealTimers() })

describe('the messages sent to the dial', () => {
  it.each([
    [SET, { t: 'net.set', ssid: 'home-net', pass: 'correct horse', host: '192.168.1.20', port: 18480, key: KEY.toString('base64'), mode: 1 }],
    [{ op: 'get' } as NetRequest, { t: 'net.get' }],
    [{ op: 'clear' } as NetRequest, { t: 'net.clear' }],
    [{ op: 'mode', mode: 0 } as NetRequest, { t: 'net.mode', mode: 0 }],
    [{ op: 'key', key: KEY } as NetRequest, { t: 'net.key', key: KEY.toString('base64') }],
    [{ op: 'host', host: '10.0.0.5', port: 18480 } as NetRequest, { t: 'net.host', host: '10.0.0.5', port: 18480 }],
  ])('%j goes out as one flat message with an id', async (request, wire) => {
    const { net, sent } = rig()
    const pending = net.request(request)
    await vi.advanceTimersByTimeAsync(0)
    expect(sent).toEqual([{ ...wire, id: 10 }])
    net.message({ t: 'net.state', id: 10, ok: true, saved: false })
    expect(await pending).toEqual({ ok: true, saved: false })
  })
})

describe('the answer', () => {
  it('is the dial\'s state: booleans, the SSID, the address it will join and the mode', async () => {
    const { net } = rig()
    const pending = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    expect(net.message({ t: 'net.state', id: 10, ok: true, saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 })).toBe(true)
    expect(await pending).toEqual({ ok: true, saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 })
  })

  it('names the field when the dial refuses, and nothing else it was sent', async () => {
    const { net } = rig()
    const pending = net.request(SET)
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 10, ok: false, err: 'pass', saved: false })
    const result = failure(await pending)
    expect(result).toMatchObject({ ok: false, kind: 'device', err: 'pass', saved: false })
    expect(JSON.stringify(result)).not.toContain('correct horse')
  })

  it('is read defensively: a wrong type is dropped, an error word that is not a plain word is "unknown"', async () => {
    const { net } = rig()
    const first = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 10, ok: true, saved: 'yes', ssid: 5, host: ['x'], port: 'p', mode: 7 })
    expect(await first).toEqual({ ok: true, saved: false })
    const second = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 11, ok: false, err: 'Not A Word!', saved: true, ssid: 'x'.repeat(40) })
    const refused = failure(await second)
    expect(refused).toMatchObject({ ok: false, err: 'unknown', saved: true })
    expect(refused.ssid).toBeUndefined()
  })

  it('belongs to the request with its id: a late or stray state is taken and ignored', async () => {
    const { net } = rig()
    const pending = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    expect(net.message({ t: 'net.state', id: 999, ok: true, saved: true })).toBe(true)
    expect(net.message({ t: 'net.state', ok: true, saved: true })).toBe(true)
    let settled = false
    void pending.then(() => { settled = true })
    await vi.advanceTimersByTimeAsync(0)
    expect(settled).toBe(false)
    net.message({ t: 'net.state', id: 10, ok: true, saved: false })
    expect(await pending).toEqual({ ok: true, saved: false })
  })

  it('is not for this layer when it is some other message', () => {
    const { net } = rig()
    expect(net.message({ t: 'pong' })).toBe(false)
    expect(net.message({ t: 'net.state', id: 1, ok: true })).toBe(true)
  })
})

describe('what goes wrong', () => {
  it('a dial that does not answer in 5 s is a timeout, and the next request is free to go', async () => {
    expect(NET_TIMEOUT_MS).toBe(5_000)
    const { net, lines } = rig()
    const pending = net.request(SET)
    await vi.advanceTimersByTimeAsync(NET_TIMEOUT_MS)
    expect(failure(await pending)).toMatchObject({ ok: false, kind: 'timeout' })
    const again = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 11, ok: true, saved: false })
    expect(await again).toMatchObject({ ok: true })
    expect(lines.join('\n')).not.toContain('correct horse')
  })

  it('a second request while one waits is turned away before it is sent', async () => {
    const { net, sent } = rig()
    void net.request({ op: 'get' })
    const second = await net.request({ op: 'clear' })
    expect(failure(second).kind).toBe('busy')
    await vi.advanceTimersByTimeAsync(0)
    expect(sent.map((m) => m.t)).toEqual(['net.get'])
  })

  it('a dial that is not on the wire is "gone" at once', async () => {
    const { net, state } = rig()
    state.onWire = false
    const pending = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    expect(failure(await pending).kind).toBe('gone')
  })

  it('a send that throws is "gone" too', async () => {
    const net = new DialNet({ send: async () => { throw new Error('boom') }, log: () => {} })
    const pending = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    expect(failure(await pending).kind).toBe('gone')
  })

  it('abort ends the wait with the reason', async () => {
    const { net } = rig()
    const pending = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    net.abort('the dial went away')
    expect(failure(await pending)).toMatchObject({ kind: 'gone', message: 'the dial went away' })
    net.abort('nothing waits now')
  })

  it('a key that is not 32 bytes is refused here, before anything is sent', async () => {
    const { net, sent } = rig()
    expect(failure(await net.request({ op: 'key', key: Buffer.alloc(31) })).kind).toBe('bad_request')
    expect(failure(await net.request({ ...SET, key: Buffer.alloc(33) } as NetRequest)).kind).toBe('bad_request')
    expect(sent).toEqual([])
  })

  it('the log lines carry the kind and the op, never a value', async () => {
    const { net, lines } = rig()
    const pending = net.request(SET)
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 10, ok: false, err: 'host', saved: false })
    await pending
    const text = lines.join('\n')
    expect(text).toContain('net.set')
    for (const secret of ['correct horse', 'home-net', '192.168.1.20', KEY.toString('base64'), KEY.toString('hex')]) expect(text).not.toContain(secret)
  })
})

describe('the phone-mode forms (B6)', () => {
  it('the key-only set goes out with a key and no other field', async () => {
    const { net, sent } = rig()
    const pending = net.request({ op: 'set_ble', key: KEY })
    await vi.advanceTimersByTimeAsync(0)
    expect(sent).toEqual([{ t: 'net.set', key: KEY.toString('base64'), id: 10 }])
    expect(Object.keys(sent[0]).sort()).toEqual(['id', 'key', 't'])
    net.message({ t: 'net.state', id: 10, ok: true, saved: false, ble: true, mode: 0 })
    expect(await pending).toEqual({ ok: true, saved: false, ble: true, mode: 0 })
  })

  it('net.mode accepts 2 (the phone) and refuses 3', async () => {
    const { net, sent } = rig()
    const pending = net.request({ op: 'mode', mode: 2 })
    await vi.advanceTimersByTimeAsync(0)
    expect(sent).toEqual([{ t: 'net.mode', mode: 2, id: 10 }])
    net.message({ t: 'net.state', id: 10, ok: true, saved: false, ble: true, mode: 2 })
    expect(await pending).toEqual({ ok: true, saved: false, ble: true, mode: 2 })
    expect(failure(await net.request({ op: 'mode', mode: 3 } as unknown as NetRequest))).toMatchObject({ kind: 'bad_request' })
    expect(sent).toHaveLength(1)
  })

  it('reads ble only when it is true, and a mode only when it is 0, 1 or 2', async () => {
    const { net } = rig()
    let pending = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 10, ok: true, saved: false, ble: 'yes', mode: 3 })
    expect(await pending).toEqual({ ok: true, saved: false, ble: false })
    pending = net.request({ op: 'get' })
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 11, ok: true, saved: true, ssid: 'x', ble: true, mode: 1 })
    expect(await pending).toEqual({ ok: true, saved: true, ssid: 'x', ble: true, mode: 1 })
  })

  it('refuses an all-zero or short key for the key-only set before sending, and never logs the key', async () => {
    const { net, sent, lines } = rig()
    expect(failure(await net.request({ op: 'set_ble', key: Buffer.alloc(32) }))).toMatchObject({ kind: 'bad_request' })
    expect(failure(await net.request({ op: 'set_ble', key: Buffer.alloc(16, 1) }))).toMatchObject({ kind: 'bad_request' })
    expect(sent).toEqual([])
    const pending = net.request({ op: 'set_ble', key: KEY })
    await vi.advanceTimersByTimeAsync(0)
    net.message({ t: 'net.state', id: 10, ok: true, saved: false, ble: true })
    await pending
    for (const line of lines) {
      expect(line).not.toContain(KEY.toString('base64'))
      expect(line).not.toContain(KEY.toString('hex'))
    }
    expect(lines.some((l) => /net\.set_ble 10 done/.test(l))).toBe(true)
  })
})
