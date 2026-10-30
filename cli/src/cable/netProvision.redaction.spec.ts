// A whole provisioning, from the terminal to the dial and back, with every place a value could be written watched: the
// daemon's console, the host's log, the dial's log files on disk, the Wi-Fi provisioning's own log, the log of the link between the
// core and the devices, the HTTP route's log, and the CLI's own output. The password, the key (hex and base64), the SSID and the
// SSID-and-password string must be found in none of them. The key is found in exactly one file: the keystore, which is what
// it is for (docs/specs/cable-wifi-link.md section 5.3; task W9).
import { mkdtempSync, readdirSync, readFileSync, rmSync, statSync } from 'node:fs'
import type { Server } from 'node:http'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { env } from '../config/env.js'
import { createServiceLinks, type ServiceFrame } from '../core/serviceLinks.js'
import { startHookServer } from '../hookServer.js'
import { dialNetHandler } from '../lib/dialNetHttp.js'
import { dialCommand } from '../lib/dialWifiCommand.js'
import { readHookCredential } from '../lib/hookAuth.js'
import { CableDecoder, CableType, encodeCableFrame } from './cableFrame.js'
import { CableSession, type CableHost, type CablePort } from './cableSession.js'
import { DIAL_KEY_FILE, DialKeys } from './dialKeys.js'
import { DialLog } from './dialLog.js'
import { NetProvisioner } from './netProvision.js'

const MAC = 'B8:1F:3F:CA:E5:58'
const SSID = 'Redaction Net 5G'
const PASS = 'p4ss-w0rd-with-spaces ok'

const cleanup: Array<() => unknown> = []
afterEach(async () => {
  vi.restoreAllMocks()
  for (const run of cleanup.splice(0).reverse()) await run()
})

/** A dial on the cable that answers net.* as the firmware does and logs lengths only, as the firmware does. */
class FirmwareStandIn implements CablePort {
  isOpen = true
  readonly path = 'usb:redaction'
  readonly transport = 'usb' as const
  readonly received: Array<Record<string, unknown>> = []
  private decoder = new CableDecoder()
  constructor(private readonly onData: (chunk: Buffer) => void, private readonly onClosed: (why: string) => void) {}
  async write(bytes: Uint8Array): Promise<void> {
    this.decoder.feed(Buffer.from(bytes), (frame) => {
      if (frame.type !== CableType.Json) return
      const msg = JSON.parse(Buffer.from(frame.payload).toString('utf8')) as Record<string, unknown>
      this.received.push(msg)
      if (msg.t === 'net.set') {
        queueMicrotask(() => {
          this.log(`net.set ssid_len=${String(msg.ssid).length} pass_len=${String(msg.pass).length} host_len=${String(msg.host).length} key_ok=1`)
          this.say({ t: 'net.state', id: msg.id, ok: true, saved: true, ssid: msg.ssid, host: msg.host, port: msg.port, mode: msg.mode })
        })
      }
    })
  }
  async close(why = 'closed'): Promise<void> {
    if (!this.isOpen) return
    this.isOpen = false
    this.onClosed(why)
  }
  say(msg: Record<string, unknown>): void {
    this.onData(Buffer.from(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(msg), 'utf8'))))
  }
  log(line: string): void {
    this.onData(Buffer.from(encodeCableFrame(CableType.Log, Buffer.from(line, 'utf8'))))
  }
}

function host(log: (line: string) => void): CableHost {
  return {
    localMachine: () => ({ id: 'mac-local', name: 'MacBook Pro' }),
    listMachines: async () => ({ machines: [{ id: 'mac-local', name: 'MacBook Pro', state: 'ready', local: true }], source: 'backend' as const }),
    selectedMachine: () => 'mac-local',
    selectMachine: async () => ({ ok: true as const }),
    listSwarms: () => ({ selected: '', swarms: [], tiles: [] }),
    listUnread: () => [],
    selectSwarm: vi.fn(),
    appName: () => 'harness',
    voiceLang: () => 'en',
    listAgents: async () => [],
    agentTotal: () => 0,
    activeSwarm: () => 't1',
    describe: () => undefined,
    sendTurn: vi.fn(),
    stopTurn: vi.fn(),
    scrolled: vi.fn(),
    answer: vi.fn(),
    focus: vi.fn(),
    openAgent: vi.fn(),
    forkAgent: async (id) => ({ ok: true as const, agentId: `${id}-fork` }),
    updateAgent: vi.fn(),
    listModels: async () => [],
    recentSummaries: async () => [],
    transcribe: async () => '',
    route: async () => ({ agentId: '', confidence: 0, reason: '' }),
    log,
  }
}

function filesUnder(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name)
    return statSync(path).isDirectory() ? filesUnder(path) : [path]
  })
}

describe('a whole provisioning, with every log watched', () => {
  it('writes the password, the key and the network to no log, console, error body or terminal line', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'net-redaction-'))
    cleanup.push(() => rmSync(dir, { recursive: true, force: true }))

    // every sink
    const sinks: Record<string, string[]> = { console: [], host: [], provisioner: [], serviceLink: [], cliOut: [], cliErr: [] }
    for (const method of ['log', 'warn', 'error', 'info', 'debug'] as const) vi.spyOn(console, method).mockImplementation((...args: unknown[]) => { sinks.console.push(args.map(String).join(' ')) })

    // the dial, its session (with its real dial log on disk) and the keystore
    const logDir = join(dir, 'logs')
    let standIn!: FirmwareStandIn
    const dialLog = new DialLog(logDir)
    const session = new CableSession(host((line) => sinks.host.push(line)), dialLog, async (onData, onClosed) => (standIn = new FirmwareStandIn(onData, onClosed)))
    session.start()
    cleanup.push(() => session.stop())
    await vi.waitFor(() => expect(standIn).toBeDefined())
    standIn.say({ t: 'hello', product: 'harness', fw: '0.0.1', proto: 3, mac: MAC, hw: 'cst9217+axp2101', net: 'v1' })
    await vi.waitFor(() => expect(session.isConnected).toBe(true))
    const keys = new DialKeys(join(dir, 'devices', DIAL_KEY_FILE))
    cleanup.push(() => keys.close())

    // the devices' side, behind a real link to the core
    const provisioner = new NetProvisioner({
      net: async (_dial, request) => ({ ...(await session.net(request)), dial: MAC }),
      keys, lan: async () => '192.168.1.20', port: 18480, log: (line) => sinks.provisioner.push(line),
    })
    const TOKEN = 'a'.repeat(48)
    const links = createServiceLinks({ token: TOKEN, owned: { devices: ['dial_net'] }, answer: () => ({}), timeoutMs: 5_000, log: (line) => sinks.serviceLink.push(line) })
    const link = links.accept('devices', TOKEN, {
      sendFrame: (frame: ServiceFrame) => {
        void provisioner.handle(frame.payload as Record<string, unknown>).then((result) => {
          link.receive({ type: `${frame.type}_result`, payload: { ...result, requestId: (frame.payload as { requestId: string }).requestId } })
        })
        return true
      },
    }, vi.fn())!

    // the core's local server, with the route in front of it
    const started = await startHookServer(0, { onRegistered: vi.fn(), onSessionEnd: vi.fn(), onDialNet: dialNetHandler((payload, waitMs) => links.call('devices', 'dial_net', payload, waitMs)) })
    const server: Server = started.server
    cleanup.push(() => new Promise<void>((resolve) => server.close(() => resolve())))
    const credential = readHookCredential(env.ADAPTER_DATA_DIR)!
    const headers = { 'x-harness-local': credential, 'x-adapter-local': '1', 'content-type': 'application/json' }

    // the terminal: the CLI, typing the password at a prompt and posting to the route
    const code = await dialCommand(['wifi', 'set'], {
      tty: true,
      call: async (path, body) => {
        const res = await fetch(`http://127.0.0.1:${started.port}${path}`, { method: 'POST', headers, body: JSON.stringify(body) })
        return { status: res.status, body: await res.json() as Record<string, unknown> }
      },
      ask: async () => SSID,
      askSecret: async () => PASS,
      out: (line) => sinks.cliOut.push(line),
      err: (line) => sinks.cliErr.push(line),
      platform: 'win32',
      port: 18480,
    })
    expect(code, sinks.cliErr.join('\n')).toBe(0)

    // it worked: the dial was told, and the daemon holds the key the dial was told
    const set = standIn.received.find((m) => m.t === 'net.set')!
    expect(set).toMatchObject({ ssid: SSID, pass: PASS, host: '192.168.1.20', port: 18480, mode: 1 })
    const key = keys.get(MAC)!
    expect(Buffer.from(set.key as string, 'base64').equals(key)).toBe(true)
    await session.stop()
    await dialLog.flush()

    // nothing is anywhere it must not be
    const secrets = [PASS, `${SSID}:${PASS}`, `${SSID}/${PASS}`, SSID, key.toString('base64'), key.toString('hex'), encodeURIComponent(PASS)]
    for (const [name, lines] of Object.entries(sinks)) {
      const text = lines.join('\n')
      for (const secret of secrets) {
        // The CLI says the network it set, which is not a secret; nothing else says it, and nothing says the rest.
        if (name === 'cliOut' && secret === SSID) continue
        expect(text, `${name} holds ${secret.length > 12 ? secret.slice(0, 6) + '...' : secret}`).not.toContain(secret)
      }
    }
    expect(sinks.cliOut.join('\n')).toContain(SSID)
    // the dial's log on disk, written by the session while all this went on
    const written = filesUnder(logDir)
    expect(written.length).toBeGreaterThan(0)
    const dialLogText = written.map((path) => readFileSync(path, 'utf8')).join('\n')
    expect(dialLogText).toContain('net.set')
    expect(dialLogText).toContain('pass_len=')
    for (const secret of secrets) expect(dialLogText, `the dial log holds ${secret.slice(0, 6)}`).not.toContain(secret)
    // the key is on disk in one file: the keystore
    const holders = filesUnder(dir).filter((path) => readFileSync(path, 'utf8').includes(key.toString('base64')))
    expect(holders.map((path) => path.split(/[\\/]/).pop())).toEqual([DIAL_KEY_FILE])
    // and the password is in no file at all
    expect(filesUnder(dir).filter((path) => readFileSync(path, 'utf8').includes(PASS))).toEqual([])
    // each sink saw something, so the checks above looked at real logs
    expect(sinks.host.length).toBeGreaterThan(0)
    expect(sinks.provisioner.length).toBeGreaterThan(0)
    expect(sinks.cliOut.length).toBeGreaterThan(0)
  })
})
