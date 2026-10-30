// What the security review of `harness dial wifi` found (W9): strings the dial reports are printed through a filter, and a key that was
// not removed is not described as removed.
import { describe, expect, it } from 'vitest'

import { dialCommand, printable, type DialWifiDeps } from './dialWifiCommand.js'

const MAC = 'B8:1F:3F:CA:E5:58'

function run(reply: { status: number; body: Record<string, unknown> }, ...args: string[]) {
  const out: string[] = []
  const err: string[] = []
  const deps: DialWifiDeps = {
    call: async () => reply, tty: false, ask: async () => 'net', askSecret: async () => '', out: (l) => out.push(l), err: (l) => err.push(l), platform: 'win32', port: 18480,
  }
  return dialCommand(['wifi', ...args], deps).then((code) => ({ code, out: out.join('\n'), err: err.join('\n') }))
}

describe('printing what the dial reported', () => {
  it('replaces C1 controls and bidi overrides with "?" and leaves the rest', () => {
    expect(printable('a\u0080b\u009fc')).toBe('a?b?c')
    expect(printable('x‮y‪z‬‭‫')).toBe('x?y?z???')
    expect(printable('1⁦2⁧3⁨4⁩')).toBe('1?2?3?4?')
    expect(printable('café 中文 \u{1F600} ‏ ok')).toBe('café 中文 \u{1F600} ‏ ok')
    // the edges of the ranges
    expect(printable('\u007f   ⁥⁪')).toBe('\u007f   ⁥⁪')
  })

  it('applies to the network name in `set`, in `show`, and to every error', async () => {
    const evil = 'Home‮ten\u009b2J'
    const set = await run({ status: 200, body: { ok: true, dial: MAC, saved: true, ssid: evil, host: '192.168.1.20', port: 18480, mode: 1 } }, 'set')
    expect(set.out).toContain('Home?ten?2J')
    expect(set.out).not.toMatch(/[\u0080-\u009f‪-‮⁦-⁩]/)
    const show = await run({ status: 200, body: { ok: true, keyed: [], port: 18480, usb: { dial: MAC, saved: true, ssid: evil, host: '1.2.3.4', port: 18480, mode: 1 } } }, 'show')
    expect(show.out).toContain('Home?ten?2J')
    expect(show.out).not.toMatch(/[\u0080-\u009f‪-‮⁦-⁩]/)
    const failed = await run({ status: 502, body: { error: 'the dial said ‮gnorw\u0085', kind: 'device' } }, 'set')
    expect(failed.err).toContain('the dial said ?gnorw?')
    expect(failed.err).not.toMatch(/[\u0080-\u009f‪-‮⁦-⁩]/)
    const problem = await run({ status: 200, body: { ok: true, keyed: [], port: 18480, usbProblem: { kind: 'x', message: 'bad\u009d' } } }, 'show')
    expect(problem.out).toContain('bad?')
  })
})

describe('forget', () => {
  it('says the computer forgot the key only when it did', async () => {
    const both = await run({ status: 200, body: { ok: true, dial: MAC, cleared: true, removed: true } }, 'forget')
    expect(both.out).toContain('So did this computer')
    const dialOnly = await run({ status: 200, body: { ok: true, dial: MAC, cleared: true, removed: false } }, 'forget')
    expect(dialOnly.out).not.toContain('So did this computer')
    expect(dialOnly.out).toContain('forgot its Wi-Fi')
    const noKey = await run({ status: 200, body: { ok: true, dial: MAC, cleared: false, removed: false } }, 'forget')
    expect(noKey.out).not.toContain('forgot the key')
    expect(noKey.out).toContain('Nothing was changed')
    const away = await run({ status: 200, body: { ok: true, dial: MAC, cleared: false, removed: true } }, 'forget', '--dial', MAC)
    expect(away.out).toContain('This computer forgot the key')
  })

  it('prints the daemon\'s word when the key could not be removed, and exits 1', async () => {
    const answer = await run({ status: 500, body: { ok: false, kind: 'keystore', error: 'The dial forgot its Wi-Fi settings, but this computer could not remove its key. See the daemon log.' } }, 'forget')
    expect(answer.code).toBe(1)
    expect(answer.err).toContain('could not remove its key')
    expect(answer.out).not.toContain('forgot the key')
  })
})

describe('when the state of the dial is not known', () => {
  it('prints the daemon\'s note to set the dial again, as it is', async () => {
    const note = 'the dial did not answer in 5 s The dial may hold a key this computer lacks. Run `harness dial wifi set` again.'
    const answer = await run({ status: 504, body: { error: note, kind: 'timeout' } }, 'set')
    expect(answer.code).toBe(1)
    expect(answer.err).toContain('The dial may hold a key this computer lacks. Run `harness dial wifi set` again.')
  })
})

describe('show', () => {
  it('says when an address was fixed with --host', async () => {
    const answer = await run({ status: 200, body: { ok: true, port: 18480, keyed: [{ dial: MAC, createdAt: 1, hostPinned: true }] } }, 'show')
    expect(answer.out).toContain('address fixed with --host')
    const free = await run({ status: 200, body: { ok: true, port: 18480, keyed: [{ dial: MAC, createdAt: 1 }] } }, 'show')
    expect(free.out).not.toContain('address fixed')
  })
})
