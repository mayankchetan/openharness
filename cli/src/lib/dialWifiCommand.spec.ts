import { readFileSync } from 'node:fs'
import { Readable } from 'node:stream'
import { describe, expect, it } from 'vitest'

import { dialCommand, stdinLines, type DialWifiDeps } from './dialWifiCommand.js'

const PASS = 'correct horse battery'
const MAC = 'B8:1F:3F:CA:E5:58'

type Call = { path: string; body: Record<string, unknown> }
type Reply = { status: number; body: Record<string, unknown> } | null

function rig(over: { tty?: boolean; typed?: string[]; secrets?: string[]; reply?: Reply; platform?: NodeJS.Platform } = {}) {
  const calls: Call[] = []
  const out: string[] = []
  const err: string[] = []
  const asked: string[] = []
  const askedSecret: string[] = []
  const typed = [...(over.typed ?? ['home-net'])]
  const secrets = [...(over.secrets ?? [PASS, PASS])]
  const deps: DialWifiDeps = {
    call: async (path, body) => { calls.push({ path, body }); return over.reply === undefined ? { status: 200, body: { ok: true } } : over.reply },
    tty: over.tty ?? true,
    ask: async (label) => { asked.push(label); return typed.shift() ?? '' },
    askSecret: async (label) => { askedSecret.push(label); return secrets.shift() ?? '' },
    out: (line) => out.push(line),
    err: (line) => err.push(line),
    platform: over.platform ?? 'win32',
    port: 18480,
  }
  const run = (...args: string[]) => dialCommand(args, deps)
  return { run, calls, out, err, asked, askedSecret }
}

const everything = (r: ReturnType<typeof rig>) => [...r.out, ...r.err].join('\n')

describe('harness dial wifi set', () => {
  it('asks the network name aloud and the password with no echo, twice, and posts them once', async () => {
    const r = rig({ reply: { status: 200, body: { ok: true, dial: MAC, saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 } } })
    expect(await r.run('wifi', 'set')).toBe(0)
    expect(r.asked).toHaveLength(1)
    expect(r.askedSecret).toHaveLength(2)
    expect(r.calls).toEqual([{ path: '/api/dial/net/provision', body: { ssid: 'home-net', pass: PASS } }])
    const text = everything(r)
    expect(text).toContain('home-net')
    expect(text).toContain(MAC)
    expect(text).toContain('192.168.1.20:18480')
    expect(text).not.toContain(PASS)
    expect(text).toMatch(/unplug/i)
  })

  it('sends --dial and --host, in either spelling', async () => {
    const r = rig()
    await r.run('wifi', 'set', '--dial', MAC, '--host=10.0.0.7')
    expect(r.calls[0].body).toEqual({ ssid: 'home-net', pass: PASS, dial: MAC, host: '10.0.0.7' })
  })

  it('stops when the two passwords differ, and posts nothing', async () => {
    const r = rig({ secrets: [PASS, 'something else'] })
    expect(await r.run('wifi', 'set')).toBe(1)
    expect(r.calls).toEqual([])
    expect(everything(r)).not.toContain(PASS)
    expect(everything(r)).not.toContain('something else')
  })

  it('an empty password is an open network, asked once', async () => {
    const r = rig({ secrets: [''] })
    expect(await r.run('wifi', 'set')).toBe(0)
    expect(r.askedSecret).toHaveLength(1)
    expect(r.calls[0].body).toEqual({ ssid: 'home-net', pass: '' })
  })

  it('reads the password once, from stdin, when stdin is not a terminal', async () => {
    const r = rig({ tty: false, secrets: [PASS] })
    expect(await r.run('wifi', 'set')).toBe(0)
    expect(r.askedSecret).toHaveLength(1)
    expect(r.calls[0].body).toMatchObject({ pass: PASS })
  })

  it('checks the name and the password before it asks the daemon, naming the field and never the value', async () => {
    for (const [typed, secrets, field] of [[[''], [PASS, PASS], 'ssid'], [['x'.repeat(33)], [PASS, PASS], 'ssid'], [['home-net'], ['short1', 'short1'], 'pass']] as const) {
      const r = rig({ typed: [...typed], secrets: [...secrets] })
      expect(await r.run('wifi', 'set')).toBe(1)
      expect(r.calls).toEqual([])
      expect(everything(r)).toContain(field)
      for (const value of [...typed, ...secrets]) if (value.length > 3) expect(everything(r)).not.toContain(value)
    }
  })

  it('refuses an --host that is not a private LAN address, before it asks anything', async () => {
    const r = rig()
    expect(await r.run('wifi', 'set', '--host', '8.8.8.8')).toBe(2)
    expect(r.asked).toEqual([])
    expect(r.calls).toEqual([])
  })

  it('never takes the password, the network or the key from the command line or the environment', async () => {
    const was = { a: process.env.HARNESS_WIFI_PASSWORD, b: process.env.WIFI_PASSWORD }
    process.env.HARNESS_WIFI_PASSWORD = 'from-the-environment'
    process.env.WIFI_PASSWORD = 'from-the-environment'
    try {
      for (const argv of [
        ['set', '--pass', 'hunter2hunter2'], ['set', '--password=hunter2hunter2'], ['set', '--pass=hunter2hunter2'], ['set', '-p', 'hunter2hunter2'],
        ['set', 'hunter2hunter2'], ['set', '--ssid', 'hunter2hunter2'], ['set', '--key', 'hunter2hunter2'], ['set', '--dial'], ['set', '--host', '192.168.1.5', '--', 'hunter2hunter2'],
      ]) {
        const r = rig()
        expect(await r.run('wifi', ...argv), argv.join(' ')).toBe(2)
        expect(r.calls).toEqual([])
        expect(r.asked.concat(r.askedSecret)).toEqual([])
        expect(everything(r), argv.join(' ')).not.toContain('hunter2')
      }
      const r = rig()
      await r.run('wifi', 'set')
      expect(r.calls[0].body.pass).toBe(PASS)
      expect(JSON.stringify(r.calls)).not.toContain('from-the-environment')
    } finally {
      for (const [name, value] of [['HARNESS_WIFI_PASSWORD', was.a], ['WIFI_PASSWORD', was.b]] as const) {
        if (value === undefined) delete process.env[name]
        else process.env[name] = value
      }
    }
  })

  it('reads no environment variable and starts no program at all (the source says so)', () => {
    const source = readFileSync(new URL('./dialWifiCommand.ts', import.meta.url), 'utf8')
    expect(source).not.toMatch(/process\.env/)
    expect(source).not.toMatch(/child_process|execSync|spawn|execFile/)
    expect(source).not.toMatch(/process\.argv/)
  })

  it.each([
    [{ status: 400, body: { error: 'pass must be empty (an open network) or 8 to 63 bytes with no control characters.', kind: 'bad_request', field: 'pass' } }, /pass must be/],
    [{ status: 404, body: { error: 'No dial is connected on the USB cable.', kind: 'no_dial' } }, /plug/i],
    [{ status: 409, body: { error: 'More than one dial is on the USB cable. Name one with --dial <MAC>.', kind: 'ambiguous' } }, /--dial/],
    [{ status: 502, body: { error: 'the dial refused: ssid', kind: 'device', err: 'ssid', field: undefined } }, /refused: ssid/],
    [{ status: 504, body: { error: 'the dial did not answer in 5 s', kind: 'timeout' } }, /did not answer/],
    [{ status: 409, body: { error: 'this dial\'s firmware does not keep Wi-Fi settings', kind: 'unsupported' } }, /firmware/],
    [{ status: 503, body: { error: 'The devices service did not answer. See the daemon\'s log.', kind: 'unavailable' } }, /did not answer/],
    [{ status: 404, body: { error: 'not found' } }, /not found|does not/i],
    [{ status: 200, body: { ok: false } }, /./],
  ] as Array<[Reply, RegExp]>)('says what went wrong: %j', async (reply, pattern) => {
    const r = rig({ reply })
    expect(await r.run('wifi', 'set')).toBe(1)
    expect(r.err.join('\n')).toMatch(pattern)
    expect(r.out.join('\n')).not.toMatch(/saved/i)
    expect(everything(r)).not.toContain(PASS)
  })

  it('says the daemon is not running when it cannot be reached', async () => {
    const r = rig({ reply: null })
    expect(await r.run('wifi', 'set')).toBe(1)
    expect(r.err.join('\n')).toMatch(/harness start/)
  })
})

describe('the other verbs', () => {
  it('show asks the daemon and prints the keystore, the dial on the cable and this computer\'s address', async () => {
    const r = rig({ reply: { status: 200, body: {
      ok: true, lan: '192.168.1.20', port: 18480,
      keyed: [{ dial: MAC, createdAt: Date.UTC(2026, 9, 29, 12, 0, 0), lastSeen: Date.UTC(2026, 9, 30, 8, 30, 0), lastAddr: '192.168.1.40' }],
      usb: { dial: MAC, keyed: true, saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 },
    } } })
    expect(await r.run('wifi', 'show', '--dial', MAC)).toBe(0)
    expect(r.calls).toEqual([{ path: '/api/dial/net/status', body: { dial: MAC } }])
    const text = r.out.join('\n')
    for (const part of [MAC, 'home-net', '192.168.1.20:18480', '192.168.1.40', 'on']) expect(text).toContain(part)
  })

  it('show says so when nothing is set, nothing is on the cable, and why a dial could not be read', async () => {
    const r = rig({ reply: { status: 200, body: { ok: true, keyed: [], port: 18480, usbProblem: { kind: 'ambiguous', message: 'More than one dial is on the USB cable. Name one with --dial <MAC>.' } } } })
    expect(await r.run('wifi', 'show')).toBe(0)
    const text = r.out.join('\n')
    expect(text).toMatch(/no dial/i)
    expect(text).toContain('--dial')
    const unset = rig({ reply: { status: 200, body: { ok: true, keyed: [], port: 18480, usb: { dial: MAC, keyed: false, saved: false } } } })
    await unset.run('wifi', 'show')
    expect(unset.out.join('\n')).toMatch(/no Wi-Fi/i)
    const open = rig({ reply: { status: 200, body: { ok: true, keyed: [], port: 18480, usb: { dial: MAC, keyed: false, saved: true, ssid: 'x', host: '10.0.0.1', port: 18480, mode: 0 } } } })
    await open.run('wifi', 'show')
    expect(open.out.join('\n')).toMatch(/off/)
    expect(open.out.join('\n')).toMatch(/set it again/i)
  })

  it('forget says what it did on the dial and in the keystore', async () => {
    const both = rig({ reply: { status: 200, body: { ok: true, dial: MAC, cleared: true, removed: true } } })
    expect(await both.run('wifi', 'forget')).toBe(0)
    expect(both.calls).toEqual([{ path: '/api/dial/net/forget', body: {} }])
    const away = rig({ reply: { status: 200, body: { ok: true, dial: MAC, cleared: false, removed: true } } })
    await away.run('wifi', 'forget', '--dial', MAC)
    expect(away.out.join('\n')).toMatch(/not on the USB cable/i)
    expect(away.calls[0].body).toEqual({ dial: MAC })
  })

  it('rotate posts and says the dial has a new key, and which dial', async () => {
    const r = rig({ reply: { status: 200, body: { ok: true, dial: MAC, rotated: true } } })
    expect(await r.run('wifi', 'rotate')).toBe(0)
    expect(r.calls[0].path).toBe('/api/dial/net/rotate')
    expect(r.out.join('\n')).toContain(MAC)
    expect(r.out.join('\n')).toMatch(/new key/i)
  })

  it.each([['on', 1], ['off', 0]] as const)('%s sets the mode to %i', async (verb, mode) => {
    const r = rig({ reply: { status: 200, body: { ok: true, dial: MAC, mode } } })
    expect(await r.run('wifi', verb, '--dial', MAC)).toBe(0)
    expect(r.calls).toEqual([{ path: '/api/dial/net/mode', body: { dial: MAC, mode } }])
    expect(r.out.join('\n')).toMatch(mode === 1 ? /Wi-Fi/ : /USB only/)
  })

  it('firewall prints the one command, scoped to the private profile and the local subnet, and runs nothing', async () => {
    const r = rig({ platform: 'win32' })
    expect(await r.run('wifi', 'firewall')).toBe(0)
    expect(r.calls).toEqual([])
    const text = r.out.join('\n')
    expect(text).toContain('netsh advfirewall firewall add rule name="Harness dial" dir=in action=allow protocol=TCP localport=18480 profile=private remoteip=LocalSubnet')
    expect(text).toMatch(/administrator/i)
    expect(text).toMatch(/does not run/i)
  })

  it('firewall on another system says a rule is not usually needed, and still prints nothing it would run', async () => {
    const r = rig({ platform: 'darwin' })
    expect(await r.run('wifi', 'firewall')).toBe(0)
    expect(r.out.join('\n')).toMatch(/TCP 18480/)
    expect(r.out.join('\n')).not.toContain('netsh')
  })

  it('knows nothing else: usage, with exit 2', async () => {
    for (const argv of [[], ['wifi'], ['wifi', 'join'], ['wifi', 'show', 'extra'], ['wifi', 'show', '--nope'], ['bluetooth', 'set'], ['wifi', 'on', '--host', '10.0.0.1'], ['wifi', 'firewall', '--dial', MAC]]) {
      const r = rig()
      expect(await r.run(...argv), argv.join(' ')).toBe(2)
      expect(r.calls).toEqual([])
      expect(r.err.join('\n')).toMatch(/Usage: harness dial wifi/)
    }
  })

  it('refuses a --dial that is not a plausible name, without echoing it', async () => {
    const r = rig()
    expect(await r.run('wifi', 'show', '--dial', 'x'.repeat(70))).toBe(2)
    expect(everything(r)).not.toContain('xxxxxxxx')
    expect(r.calls).toEqual([])
  })
})

describe('reading lines from a pipe', () => {
  it('serves the lines of stdin one at a time, in order, over one read', async () => {
    const next = stdinLines(Readable.from(['home-net\r\ncorrect ', 'horse\nlast']))
    expect(await next()).toBe('home-net')
    expect(await next()).toBe('correct horse')
    expect(await next()).toBe('last')
    expect(await next()).toBe('')
  })

  it('serves nothing from an empty pipe', async () => {
    expect(await stdinLines(Readable.from([]))()).toBe('')
  })

  it('takes a Buffer chunk as well', async () => {
    const next = stdinLines(Readable.from([Buffer.from('a\nb\n')]))
    expect([await next(), await next(), await next()]).toEqual(['a', 'b', ''])
  })

})
