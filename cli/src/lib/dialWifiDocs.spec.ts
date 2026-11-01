import { existsSync, readFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

import { dialCommand, type DialWifiDeps } from './dialWifiCommand.js'

// W16: the user note for the dial's Wi-Fi (docs/dial-wifi.md) and the wire specification stay linked to each other and to what the
// product does. The firewall command and the on-glass lines are read from the code that prints them, never typed a second time.

const REPO = resolve(fileURLToPath(new URL('../../..', import.meta.url)))
const NOTE = join(REPO, 'docs', 'dial-wifi.md')
const SPEC = join(REPO, 'docs', 'specs', 'cable-wifi-link.md')
const NET_STATE_C = join(REPO, 'devices', 'harness-device', 'firmware', 'main', 'net_state.c')
const UI_HABITAT_C = join(REPO, 'devices', 'harness-device', 'firmware', 'main', 'ui', 'habitat', 'ui_habitat.c')

const read = (path: string) => readFileSync(path, 'utf8').replace(/\r\n/g, '\n')

/** Every `[text](target)` whose target is a file path (not a URL, not a bare anchor), with the file it sits in. */
function localLinks(path: string): Array<{ target: string; file: string }> {
  const found: Array<{ target: string; file: string }> = []
  for (const match of read(path).matchAll(/\[[^\]]*\]\(([^)\s]+)\)/g)) {
    const target = match[1]
    if (/^[a-z][a-z0-9+.-]*:/i.test(target) || target.startsWith('#')) continue
    found.push({ target, file: resolve(dirname(path), target.split('#')[0]) })
  }
  return found
}

async function printed(...args: string[]): Promise<string> {
  const out: string[] = []
  const deps: DialWifiDeps = {
    call: async () => ({ status: 200, body: { ok: true } }),
    tty: false,
    ask: async () => '',
    askSecret: async () => '',
    out: (line) => out.push(line),
    err: (line) => out.push(line),
    platform: 'win32',
    port: 18480,
  }
  await dialCommand(args, deps)
  return out.join('\n')
}

describe('docs for the dial on Wi-Fi (W16)', () => {
  it('every relative link in the note and the spec points at a file that exists', () => {
    for (const path of [NOTE, SPEC]) {
      for (const link of localLinks(path)) expect(existsSync(link.file), `${path} -> ${link.target}`).toBe(true)
    }
  })

  it('the note and the spec point at each other', () => {
    expect(localLinks(NOTE).map((l) => l.file)).toContain(SPEC)
    expect(read(SPEC)).toContain('docs/dial-wifi.md')
  })

  it('the note covers the router reservation, 2.4 GHz only, and the firewall command the CLI prints', async () => {
    const note = read(NOTE)
    expect(note).toMatch(/DHCP reservation/)
    expect(note).toMatch(/2\.4 GHz/)
    expect(note).toMatch(/5 GHz/)
    const command = (await printed('wifi', 'firewall')).split('\n').map((line) => line.trim()).find((line) => line.startsWith('netsh '))
    expect(command).toBeTruthy()
    expect(note).toContain(command!)
  })

  it('every `harness dial wifi <verb>` the note names is a verb of the command', async () => {
    const usage = await printed('wifi')
    const named = [...read(NOTE).matchAll(/`harness dial wifi ([a-z]+)/g)].map((m) => m[1])
    expect(named.length).toBeGreaterThan(0)
    for (const verb of new Set(named)) expect(usage, verb).toMatch(new RegExp(`\\b${verb}\\b`))
  })

  it('the lines the note says the dial shows are the lines the firmware draws', () => {
    const source = read(NET_STATE_C)
    const body = source.slice(source.indexOf('const char *net_state_caption'), source.indexOf('const char *net_state_line'))
    const captions = [...body.matchAll(/return "([^"]+)";/g)].map((m) => m[1])
    expect(captions).toHaveLength(4)
    const note = read(NOTE)
    for (const caption of captions) expect(note, caption).toContain(`| ${caption} |`)
  })

  it('the Link screen rows and state lines in the note are the ones the firmware draws', () => {
    const ui = read(UI_HABITAT_C)
    const render = ui.slice(ui.indexOf('static void render_link('), ui.indexOf('WHAT THE APP ASKED FOR'))
    const labels = [...render.matchAll(/"((?:Link|Keep Wi-Fi)[^"]*)"/g)].map((m) => m[1])
    expect(labels.sort()).toEqual(['Keep Wi-Fi: off', 'Keep Wi-Fi: on', 'Link: USB only', 'Link: Wi-Fi'])
    const note = read(NOTE)
    for (const label of labels) expect(note, label).toContain(label)
    const state = read(NET_STATE_C)
    const lines = [...state.slice(state.indexOf('const char *net_state_line')).matchAll(/return "(Wi-Fi: [^"]+)";/g)].map((m) => m[1])
    expect(lines).toHaveLength(6)
    const sentence = note.split('\n').find((line) => line.startsWith('Under the rows it shows one line of link state:'))
    expect(sentence).toBeTruthy()
    for (const line of lines) expect(sentence, line).toContain(line.replace(/^Wi-Fi: /, ''))
  })

  it('says how to leave Keep Wi-Fi, and names no internal flag, network, password or key', () => {
    const note = read(NOTE)
    expect(note).toMatch(/turn it off/i)
    expect(note).not.toMatch(/stay_wl|wl_mode/i)
    expect(note).not.toMatch(/psk|passphrase/i)
  })
})
