// `harness dial phone set|show|forget|on|off` (task B6, lib/dialWifiCommand.ts): giving a plugged-in dial a key for the phone mode over its
// USB cable. No password is asked: the daemon makes the key, files it for the tunnel endpoint and never says it.
import { spawnSync } from 'node:child_process'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

import { dialCommand, type DialWifiDeps } from './dialWifiCommand.js'

const MAC = 'B8:1F:3F:CA:E5:58'

type Reply = { status: number; body: Record<string, unknown> } | null

function rig(reply: Reply = { status: 200, body: { ok: true } }) {
  const calls: Array<{ path: string; body: Record<string, unknown> }> = []
  const out: string[] = []
  const err: string[] = []
  let asked = 0
  const deps: DialWifiDeps = {
    call: async (path, body) => { calls.push({ path, body }); return reply },
    tty: true,
    ask: async () => { asked++; return '' },
    askSecret: async () => { asked++; return '' },
    out: (line) => out.push(line),
    err: (line) => err.push(line),
    platform: 'win32',
    port: 18480,
  }
  return { calls, out, err, asked: () => asked, run: (...args: string[]) => dialCommand(args, deps), text: () => [...out, ...err].join('\n') }
}

describe('harness dial phone set', () => {
  it('asks nothing, posts one request and says what the person does next, without a key', async () => {
    const r = rig({ status: 200, body: { ok: true, dial: MAC, ble: true, mode: 2 } })
    expect(await r.run('phone', 'set')).toBe(0)
    expect(r.asked()).toBe(0)
    expect(r.calls).toEqual([{ path: '/api/dial/net/phone-set', body: {} }])
    const text = r.text()
    expect(text).toContain(MAC)
    expect(text).toMatch(/phone/i)
    expect(text).toMatch(/unplug/i)
    expect(text).toContain('CABLE_TUNNEL_ENABLE')
    expect(text).not.toMatch(/key\s*[:=]/i)
  })

  it('sends --dial in either spelling', async () => {
    const a = rig()
    await a.run('phone', 'set', '--dial', MAC)
    const b = rig()
    await b.run('phone', 'set', `--dial=${MAC}`)
    expect(a.calls[0].body).toEqual({ dial: MAC })
    expect(b.calls[0].body).toEqual({ dial: MAC })
  })

  it('tells a mode-2 failure after the key was filed what to run', async () => {
    const r = rig({ status: 502, body: { error: 'the dial refused: feature The key is stored. Run `harness dial phone on` to turn the phone mode on.', kind: 'device', err: 'feature' } })
    expect(await r.run('phone', 'set')).toBe(1)
    expect(r.err.join('\n')).toContain('harness dial phone on')
  })
})

describe('harness dial phone on | off', () => {
  it('posts mode 2 for on and mode 0 for off, and says which', async () => {
    const on = rig({ status: 200, body: { ok: true, dial: MAC, mode: 2 } })
    expect(await on.run('phone', 'on')).toBe(0)
    expect(on.calls).toEqual([{ path: '/api/dial/net/phone-mode', body: { mode: 2 } }])
    expect(on.out.join('\n')).toMatch(/phone mode is on/i)
    const off = rig({ status: 200, body: { ok: true, dial: MAC, mode: 0 } })
    expect(await off.run('phone', 'off', '--dial', MAC)).toBe(0)
    expect(off.calls).toEqual([{ path: '/api/dial/net/phone-mode', body: { dial: MAC, mode: 0 } }])
    expect(off.out.join('\n')).toMatch(/phone mode is off/i)
  })
})

describe('harness dial phone forget', () => {
  it('posts one request and says the dial and this computer forgot the key', async () => {
    const r = rig({ status: 200, body: { ok: true, dial: MAC, cleared: true, removed: true } })
    expect(await r.run('phone', 'forget')).toBe(0)
    expect(r.calls).toEqual([{ path: '/api/dial/net/phone-forget', body: {} }])
    expect(r.out.join('\n')).toMatch(/forgot/i)
    expect(r.out.join('\n')).toContain(MAC)
  })

  it('prints the refusal when the dial also holds Wi-Fi settings, with the exit code 1', async () => {
    const r = rig({ status: 400, body: { error: 'This dial also holds Wi-Fi settings, and they share the phone key: clearing would take both. Run `harness dial wifi forget` to clear both.', kind: 'bad_request' } })
    expect(await r.run('phone', 'forget')).toBe(1)
    expect(r.err.join('\n')).toContain('harness dial wifi forget')
  })
})

describe('harness dial phone show', () => {
  it('asks the status route and reads the phone part of it', async () => {
    const r = rig({ status: 200, body: { ok: true, lan: '192.168.1.20', port: 18480, keyed: [{ dial: MAC, createdAt: 1_700_000_000_000 }], usb: { dial: MAC, keyed: true, saved: false, ble: true, mode: 2 } } })
    expect(await r.run('phone', 'show')).toBe(0)
    expect(r.calls).toEqual([{ path: '/api/dial/net/status', body: {} }])
    const text = r.out.join('\n')
    expect(text).toContain(MAC)
    expect(text).toMatch(/phone settings\s+held/i)
    expect(text).toMatch(/phone mode\s+on/i)
    expect(text).toMatch(/key on this computer\s+yes/i)
  })

  it('says when the dial holds no phone settings, and when this computer lacks the key', async () => {
    const none = rig({ status: 200, body: { ok: true, port: 18480, keyed: [], usb: { dial: MAC, keyed: false, saved: false, ble: false } } })
    await none.run('phone', 'show')
    expect(none.out.join('\n')).toContain('harness dial phone set')
    const lacks = rig({ status: 200, body: { ok: true, port: 18480, keyed: [], usb: { dial: MAC, keyed: false, saved: false, ble: true, mode: 0 } } })
    await lacks.run('phone', 'show')
    expect(lacks.out.join('\n')).toMatch(/key on this computer\s+no/i)
    expect(lacks.out.join('\n')).toMatch(/phone mode\s+off/i)
  })

  it('says when the dial is in Wi-Fi mode, and when none is on the cable', async () => {
    const wifi = rig({ status: 200, body: { ok: true, port: 18480, keyed: [], usb: { dial: MAC, keyed: true, saved: true, ssid: 'home-net', ble: true, mode: 1 } } })
    await wifi.run('phone', 'show')
    expect(wifi.out.join('\n')).toMatch(/wi-?fi mode/i)
    const nothing = rig({ status: 200, body: { ok: true, port: 18480, keyed: [], usbProblem: { kind: 'no_dial', message: 'No dial is connected on the USB cable.' } } })
    await nothing.run('phone', 'show')
    expect(nothing.out.join('\n')).toContain('No dial is connected on the USB cable.')
  })

  it('lists the keys held on this computer by dial, never the key', async () => {
    const r = rig({ status: 200, body: { ok: true, port: 18480, keyed: [{ dial: MAC, createdAt: 1_700_000_000_000, lastSeen: 1_700_000_100_000, lastAddr: '192.168.1.40' }], usb: { dial: MAC, keyed: true, saved: false, ble: true, mode: 2 } } })
    await r.run('phone', 'show')
    expect(r.out.join('\n')).toMatch(new RegExp(`Key held\\s+${MAC}`))
  })
})

describe('harness dial phone: refusals and failures', () => {
  it('knows nothing else: usage for the phone, with exit 2 and no request', async () => {
    for (const argv of [['phone'], ['phone', 'join'], ['phone', 'set', 'extra'], ['phone', 'set', '--host', '10.0.0.1'], ['phone', 'show', '--nope'], ['phone', 'on', '--dial'], ['phone', 'set', '--dial', MAC, '--dial', MAC]]) {
      const r = rig()
      expect(await r.run(...argv), argv.join(' ')).toBe(2)
      expect(r.calls).toEqual([])
      expect(r.err.join('\n')).toMatch(/Usage: harness dial phone/)
    }
  })

  it('the bare command lists both families', async () => {
    const r = rig()
    expect(await r.run()).toBe(2)
    expect(r.err.join('\n')).toMatch(/Usage: harness dial wifi/)
    expect(r.err.join('\n')).toMatch(/harness dial phone/)
  })

  it('refuses a --dial that is not a plausible name, without echoing it', async () => {
    const r = rig()
    expect(await r.run('phone', 'set', '--dial', 'a\nb' + 'S3cret')).toBe(2)
    expect(r.calls).toEqual([])
    expect(r.text()).not.toContain('S3cret')
  })

  it('says the daemon is not running', async () => {
    const r = rig(null)
    expect(await r.run('phone', 'set')).toBe(1)
    expect(r.err.join('\n')).toMatch(/daemon is not running/)
  })

  it('gives a hint for the failures a person can act on', async () => {
    const cases: Array<[Record<string, unknown>, RegExp]> = [
      [{ error: 'no dial', kind: 'no_dial' }, /Plug the dial in with its USB cable/],
      [{ error: 'old', kind: 'unsupported' }, /Update the dial's firmware/],
      [{ error: 'refused', kind: 'device', err: 'feature' }, /no phone mode/i],
    ]
    for (const [body, hint] of cases) {
      const r = rig({ status: 502, body })
      expect(await r.run('phone', 'set')).toBe(1)
      expect(r.err.join('\n')).toMatch(hint)
    }
  })

  it('prints nothing the dial or the daemon sent as a control character', async () => {
    const r = rig({ status: 200, body: { ok: true, dial: 'B8:1F:3F:CA:E5:58\u009b31m', ble: true, mode: 2 } })
    await r.run('phone', 'set')
    expect(r.text()).not.toContain('\u009b')
  })
})

describe('harness help: the dial phone entry', () => {
  const CLI_ROOT = fileURLToPath(new URL('../..', import.meta.url))
  const TSX = join(CLI_ROOT, 'node_modules', 'tsx', 'dist', 'cli.mjs')

  it('is exactly this text, and sits before the Wi-Fi entry so the Wi-Fi block is unchanged', () => {
    const run = spawnSync(process.execPath, [TSX, join(CLI_ROOT, 'src', 'cli.ts'), 'help'], { encoding: 'utf8', timeout: 60_000 })
    expect(run.status).toBe(0)
    const lines = run.stdout.split(/\r?\n/)
    const start = lines.findIndex((line) => line.startsWith('  harness dial phone '))
    expect(start).toBeGreaterThan(-1)
    expect(lines.slice(start, start + 4)).toEqual([
      '  harness dial phone <set|show|forget|on|off>',
      '                               give a plugged-in dial a key for the phone mode: Bluetooth to the Harness phone app, then a tunnel to',
      '                               this computer. No password is asked. The tunnel endpoint stays off until CABLE_TUNNEL_ENABLE=true',
      '  harness dial wifi <set|show|forget|rotate|on|off|firewall>',
    ])
  }, 90_000)
})
