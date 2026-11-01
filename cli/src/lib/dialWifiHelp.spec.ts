import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

import { dialCommand, type DialWifiDeps } from './dialWifiCommand.js'

// W16: what `harness help` and `harness dial wifi` tell a person about the dial's Wi-Fi, pinned. The help block comes from the REAL
// command (tsx over cli.ts), not from a copy of its text, so a change to either side shows up here.

const CLI_ROOT = fileURLToPath(new URL('../..', import.meta.url))
const TSX = join(CLI_ROOT, 'node_modules', 'tsx', 'dist', 'cli.mjs')

function helpBlock(): string[] {
  const run = spawnSync(process.execPath, [TSX, join(CLI_ROOT, 'src', 'cli.ts'), 'help'], { encoding: 'utf8', timeout: 60_000 })
  expect(run.status).toBe(0)
  const lines = run.stdout.split(/\r?\n/)
  const start = lines.findIndex((line) => line.startsWith('  harness dial wifi '))
  expect(start).toBeGreaterThan(-1)
  const end = lines.findIndex((line, i) => i > start && line.trim() === '')
  return lines.slice(start, end)
}

function rig(reply: { status: number; body: Record<string, unknown> }) {
  const out: string[] = []
  const err: string[] = []
  const deps: DialWifiDeps = {
    call: async () => reply,
    tty: true,
    ask: async () => 'home-net',
    askSecret: async () => 'correct horse battery',
    out: (line) => out.push(line),
    err: (line) => err.push(line),
    platform: 'win32',
    port: 18480,
  }
  return { out, err, run: (...args: string[]) => dialCommand(args, deps) }
}

describe('harness help: the dial Wi-Fi entry (W16)', () => {
  it('is exactly this text', () => {
    expect(helpBlock()).toEqual([
      '  harness dial wifi <set|show|forget|rotate|on|off|firewall>',
      '                               give a plugged-in dial a Wi-Fi network and the key it proves to this computer with.',
      '                               `set` asks for the network name and the password (typed, never an argument);',
      '                               `firewall` prints the Windows firewall command, it does not run it',
      '                               The dial joins 2.4 GHz networks only. Reserve this computer\'s address in your router.',
    ])
  }, 90_000)

  it('names the same verbs as the command itself, and no verb for stay_wl', async () => {
    const r = rig({ status: 200, body: { ok: true } })
    expect(await r.run('wifi')).toBe(2)
    const usage = r.err.join('\n')
    const listed = helpBlock()[0].match(/<([^>]+)>/)![1].split('|')
    for (const verb of listed) expect(usage).toMatch(new RegExp(`^  ${verb}\\b|^  \\w+ \\| ${verb}\\b`, 'm'))
    // The only way to set stay_wl is the toggle on the dial (it can turn it off again). No verb, flag or word of it here.
    expect(`${usage}\n${helpBlock().join('\n')}`).not.toMatch(/stay|keep wi-?fi/i)
    expect(listed).toHaveLength(7)
  }, 90_000)
})

describe('harness dial wifi set: the closing advice (W16)', () => {
  it('says unplug, 2.4 GHz only, the router reservation and the firewall command, and no secret', async () => {
    const r = rig({ status: 200, body: { ok: true, dial: 'B8:1F:3F:CA:E5:58', saved: true, ssid: 'home-net', host: '192.168.1.20', port: 18480, mode: 1 } })
    expect(await r.run('wifi', 'set')).toBe(0)
    const text = r.out.join('\n')
    expect(text).toMatch(/unplug/i)
    expect(text).toMatch(/2\.4 GHz/)
    expect(text).toMatch(/DHCP reservation/)
    expect(text).toContain('harness dial wifi firewall')
    expect(text).not.toContain('correct horse battery')
  })
})
