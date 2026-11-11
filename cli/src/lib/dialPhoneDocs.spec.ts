import { existsSync, readFileSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

import { dialCommand, type DialWifiDeps } from './dialWifiCommand.js'

// B12: the how-to for the dial's phone mode (docs/dial-phone.md) stays linked to the spec and to what the CLI, the phone app and the
// dial's screens say. Command verbs, app labels and dial lines are read from the code that has them, never typed a second time.

const REPO = resolve(fileURLToPath(new URL('../../..', import.meta.url)))
const NOTE = join(REPO, 'docs', 'dial-phone.md')
const SPEC = join(REPO, 'docs', 'specs', 'cable-ble-link.md')
const APP = join(REPO, 'devices', 'harness-device', 'phone-bridge', 'src', 'main', 'java', 'dev', 'openharness', 'phonebridge')
const UI_HABITAT_C = join(REPO, 'devices', 'harness-device', 'firmware', 'main', 'ui', 'habitat', 'ui_habitat.c')
const ENV_TS = join(REPO, 'cli', 'src', 'config', 'env.ts')
const LINK_AUTO_H = join(REPO, 'devices', 'harness-device', 'firmware', 'main', 'link_auto.h')
const NET_STATE_C = join(REPO, 'devices', 'harness-device', 'firmware', 'main', 'net_state.c')

const read = (path: string) => readFileSync(path, 'utf8').replace(/\r\n/g, '\n')

function localLinks(path: string): Array<{ target: string; file: string }> {
  const found: Array<{ target: string; file: string }> = []
  for (const match of read(path).matchAll(/\[[^\]]*\]\(([^)\s]+)\)/g)) {
    const target = match[1]
    if (/^[a-z][a-z0-9+.-]*:/i.test(target) || target.startsWith('#')) continue
    found.push({ target, file: resolve(dirname(path), target.split('#')[0]) })
  }
  return found
}

async function usage(): Promise<string> {
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
  await dialCommand(['phone'], deps)
  return out.join('\n')
}

describe('docs for the dial on the phone (B12)', () => {
  it('every relative link points at a file that exists, and the note and the spec point at each other', () => {
    expect(existsSync(NOTE), NOTE).toBe(true)
    for (const link of localLinks(NOTE)) expect(existsSync(link.file), `${NOTE} -> ${link.target}`).toBe(true)
    expect(localLinks(NOTE).map((l) => l.file)).toContain(SPEC)
    expect(read(SPEC)).toContain('docs/dial-phone.md')
  })

  it('points at the Cloudflare runbook in the workbench repo by path, as plain text', () => {
    expect(read(NOTE)).toContain('briefs/research/2026-11-01-cloudflare-runbook.md')
  })

  it('every `harness dial phone <verb>` the note names is a verb of the command', async () => {
    const text = await usage()
    const named = [...read(NOTE).matchAll(/`harness dial phone ([a-z]+)/g)].map((m) => m[1])
    expect(new Set(named)).toContain('set')
    for (const verb of new Set(named)) expect(text, verb).toMatch(new RegExp(`\\b${verb}\\b`))
  })

  it('names the switch that turns the tunnel endpoint on, and it is a real setting', () => {
    expect(read(ENV_TS)).toContain('CABLE_TUNNEL_ENABLE')
    expect(read(NOTE)).toContain('CABLE_TUNNEL_ENABLE=true')
  })

  it('uses the words the phone app shows: the buttons, the three settings fields, the pairing line', () => {
    const note = read(NOTE)
    const main = read(join(APP, 'MainActivity.kt'))
    for (const label of ['Start bridge', 'Settings', 'Pairing']) {
      expect(main, label).toContain(`text = "${label}"`)
      expect(note, label).toContain(label)
    }
    const settings = read(join(APP, 'SettingsActivity.kt'))
    for (const label of ['Relay URL', 'Cloudflare Access Client ID', 'Client Secret', 'Save']) {
      expect(settings, label).toContain(`"${label}"`)
      expect(note, label).toContain(label)
    }
    const hint = read(join(APP, 'PairingHint.kt'))
    expect(hint).toContain('Settings > Phone > Pair')
    expect(note).toContain('Settings > Phone > Pair')
  })

  it('uses the lines the dial draws: Phone code, Same on the phone?, [yes], Waiting for phone', () => {
    const ui = read(UI_HABITAT_C)
    const note = read(NOTE)
    for (const line of ['Phone code', 'Same on the phone?', 'Waiting for phone']) {
      expect(ui, line).toContain(`"${line}"`)
      expect(note, line).toContain(line)
    }
    expect(ui).toContain('"[yes]"')
    expect(ui).toContain('"[pair]"')
  })

  it('names no key, and says the Access secret is typed on the phone only', () => {
    const note = read(NOTE)
    expect(note).not.toMatch(/psk|passphrase/i)
    expect(note).toMatch(/never (put|type|paste)s? it (in|on|into)/i)
  })
})

describe('docs for the auto mode, Wi-Fi first then the phone (wifi-first work)', () => {
  const section = () => {
    const note = read(NOTE)
    const start = note.indexOf('## Wi-Fi first, then the phone (auto mode)')
    expect(start).toBeGreaterThan(-1)
    return note.slice(start, note.indexOf('\n## ', start + 5))
  }
  const constant = (name: string): number => {
    const found = read(LINK_AUTO_H).match(new RegExp(`#define\\s+${name}\\s+(\\d+)u`))
    expect(found, name).toBeTruthy()
    return Number(found![1])
  }

  it('names `harness dial net auto`, and it is a verb of the command', async () => {
    const out: string[] = []
    const deps: DialWifiDeps = { call: async () => ({ status: 200, body: { ok: true } }), tty: false, ask: async () => '', askSecret: async () => '', out: (l) => out.push(l), err: (l) => out.push(l), platform: 'win32', port: 18480 }
    await dialCommand(['net'], deps)
    expect(section()).toContain('`harness dial net auto`')
    expect(out.join('\n')).toMatch(/\bauto\b/)
  })

  it('says the numbers the firmware uses, read from link_auto.h', () => {
    const text = section().replace(/\s+/g, ' ')
    const seconds = (ms: number) => `${ms / 1000} seconds`
    expect(text).toContain(seconds(constant('LA_TRY_MS')))                       // 30 seconds without a connection: the phone
    const waits = read(LINK_AUTO_H).match(/#define\s+LA_DROP_WAIT_LIST\s+([\d\s,u]+)/)![1].split(',').map((x) => parseInt(x, 10))
    expect(waits).toEqual([30000, 60000, 120000])
    for (const ms of waits) expect(text).toContain(seconds(ms))                  // 30, 60 and 120 seconds after a drop of the phone link
    expect(constant('LA_NOAP_SCANS')).toBe(2)
    expect(text).toMatch(/not found twice in a row/)
    expect(constant('LA_WIFI_LOST_MS')).toBe(120000)
    expect(constant('LA_NEVER_MS')).toBe(120000)
    expect(text).toContain('2 minutes')
    expect(text).toMatch(new RegExp(`at most ${constant('LA_RETRY_MAX')} returns to Wi-Fi`))
    expect(text).toContain(`${constant('LA_STABLE_MS') / 60000} minutes`)
  })

  it('uses the lines the dial draws, read from net_state.c', () => {
    const text = section()
    const state = read(NET_STATE_C)
    for (const line of ['Using phone', 'Link: Auto - Wi-Fi', 'Link: Auto - Phone', 'Plug in USB']) {
      expect(state, line).toContain(`"${line}"`)
      expect(text, line).toContain(`"${line}"`)
    }
  })

  it('says to pair first, and that the dial cannot be checked for a pairing', () => {
    const text = section().replace(/\s+/g, ' ')
    expect(text).toMatch(/pair it first|Set up the phone and pair it first/i)
    expect(text).toContain('harness dial phone on')
    expect(text).toMatch(/cannot say how many phones/)
  })

  it('says voice starts with the BOOT button only, and says nothing about tapping to talk', () => {
    for (const path of [NOTE, SPEC, join(REPO, 'docs', 'dial-wifi.md')]) expect(read(path), path).not.toMatch(/tap(ping)?\s+(the\s+\w+\s+)?to\s+(talk|speak|record)/i)
    expect(section()).toContain('BOOT button')
  })

  it('the spec has the auto section, and no longer says the home hint returns the dial to Wi-Fi', () => {
    const spec = read(SPEC)
    expect(spec).toContain('## 9. Auto mode (Wi-Fi first, then the phone)')
    expect(spec).not.toMatch(/home hint true for 10 s\s+returns the dial to Wi-Fi/)
    expect(spec).toMatch(/No firmware code reads it for the mode switch/)
    for (const name of ['LA_TRY_MS', 'LA_NOAP_SCANS', 'LA_WIFI_LOST_MS', 'LA_NEVER_MS', 'LA_RETRY_MAX', 'LA_EDGE_CAP', 'LA_STABLE_MS', 'LA_DROP_WAIT_LIST']) {
      expect(spec, name).toContain(name)
      expect(read(LINK_AUTO_H), name).toContain(`#define ${name}`)
    }
  })
})
