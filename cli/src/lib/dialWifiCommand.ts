/**
 * `harness dial wifi set|show|forget|rotate|on|off|firewall`: giving a dial its Wi-Fi network and the key it proves to this
 * computer with, over its USB cable (docs/specs/cable-wifi-link.md section 5; task W9). The daemon does the work
 * (lib/dialNetHttp.ts, cable/netProvision.ts); this file asks, prints, and keeps secrets out of the places they leak from.
 *
 * Where the password comes from. It is TYPED, at a prompt that shows nothing (or read from a pipe, one line, when stdin is
 * not a terminal: for scripts and tests). Never from the command line (argv shows in every process list), never from the
 * environment, and never from the operating system's saved Wi-Fi profile. Any option that could carry it, and any stray word, is a
 * usage error that prints nothing of what was typed. This file reads no environment variable and starts no program: the
 * firewall command is printed for the person to run, and is not run here.
 *
 * What is printed: the network name and the address, which are not secrets. Never the password, and never the key (the daemon
 * generates it, and it never leaves the daemon and the dial).
 */
import { NET_FIELD_MESSAGES, isLanIPv4, validDial, validPass, validSsid } from './dialNetRules.js'

/** What the command needs from its surroundings. The real ones are in cli.ts; the tests bring their own. */
export interface DialWifiDeps {
  /** POST to the daemon's local API; null when the daemon cannot be reached. */
  call(path: string, body: Record<string, unknown>): Promise<{ status: number; body: Record<string, unknown> } | null>
  /** stdin is a terminal: the password is typed twice at a prompt that shows nothing. Otherwise it is the next line of stdin, once. */
  tty: boolean
  /** A question whose answer shows (the network name). From a pipe: the next line. */
  ask(label: string): Promise<string>
  /** A question whose answer does not show (the password). From a pipe: the next line. */
  askSecret(label: string): Promise<string>
  out(line: string): void
  err(line: string): void
  platform: NodeJS.Platform
  /** The listener's port (CABLE_NET_PORT): what the firewall rule is for. */
  port: number
}

const USAGE = [
  'Usage: harness dial wifi <command>',
  '  set [--dial <MAC>] [--host <IPv4>]   give a dial a Wi-Fi network. It asks for the name and the password, and the dial must be',
  '                                       plugged in by USB. --host is this computer\'s address, if it is not found by itself',
  '  show [--dial <MAC>]                  what the dials hold and this computer knows',
  '  forget [--dial <MAC>]                the dial forgets its network and key, and so does this computer',
  '  rotate [--dial <MAC>]                a new key for a dial that is set (the old one stops working)',
  '  on | off [--dial <MAC>]              whether the dial joins Wi-Fi when its USB cable is unplugged',
  '  firewall                             print the Windows firewall command for the Wi-Fi link (it is not run)',
]

/** The one command, scoped to private networks and the local subnet. Printed, never run. */
const firewallCommand = (port: number): string =>
  `netsh advfirewall firewall add rule name="Harness dial" dir=in action=allow protocol=TCP localport=${port} profile=private remoteip=LocalSubnet`

/**
 * The `--name value` / `--name=value` options in `args`, only the ones in `allowed`. Null on any other word or option, a missing
 * value, or the same option twice. A rejected token is never repeated back: it may be a password typed in the wrong place.
 */
function parseOptions(args: string[], allowed: readonly string[]): Record<string, string> | null {
  const found: Record<string, string> = {}
  for (let i = 0; i < args.length; i++) {
    const token = args[i]
    if (!token.startsWith('--')) return null
    const eq = token.indexOf('=')
    const name = eq < 0 ? token : token.slice(0, eq)
    if (!allowed.includes(name) || name in found) return null
    let value: string | undefined
    if (eq >= 0) value = token.slice(eq + 1)
    else {
      value = args[++i]
      if (value === undefined || value.startsWith('-')) return null
    }
    found[name] = value
  }
  return found
}

const stamp = (ms: unknown): string => typeof ms === 'number' ? `${new Date(ms).toISOString().slice(0, 16).replace('T', ' ')} UTC` : '?'

/** What to do about a failure, by the kind the daemon names. */
const HINTS: Record<string, string> = {
  no_dial: 'Plug the dial in with its USB cable and try again.',
  unsupported: 'Update the dial\'s firmware first.',
  timeout: 'Unplug the dial, plug it in again and try again.',
  transport: 'Plug the dial in with its USB cable and try again.',
  busy: 'Wait a moment and try again.',
}

export async function dialCommand(args: string[], deps: DialWifiDeps): Promise<number> {
  const usage = (): number => { for (const line of USAGE) deps.err(line); return 2 }
  if (args[0] !== 'wifi') return usage()
  const verb = args[1]
  const rest = args.slice(2)

  if (verb === 'firewall') {
    if (rest.length) return usage()
    if (deps.platform !== 'win32') {
      deps.out(`  A firewall rule is not usually needed here. If a firewall blocks the dial, allow inbound TCP ${deps.port} from your local network.`)
      return 0
    }
    deps.out('')
    deps.out('  Windows Firewall blocks the dial\'s Wi-Fi connection until it is allowed. Run this once, in a terminal opened as an')
    deps.out('  administrator (it allows inbound TCP on the Wi-Fi port from your own network only, on Private networks):')
    deps.out('')
    deps.out(`    ${firewallCommand(deps.port)}`)
    deps.out('')
    deps.out('  Harness does not run it for you and changes no firewall setting itself. Your home network must be set to "Private" in Windows.')
    deps.out('')
    return 0
  }

  const verbs = ['set', 'show', 'forget', 'rotate', 'on', 'off']
  if (!verb || !verbs.includes(verb)) return usage()
  const options = parseOptions(rest, verb === 'set' ? ['--dial', '--host'] : ['--dial'])
  if (!options) return usage()
  const body: Record<string, unknown> = {}
  if (options['--dial'] !== undefined) {
    if (!validDial(options['--dial'])) { deps.err(`  ✗ ${NET_FIELD_MESSAGES.dial}`); return 2 }
    body.dial = options['--dial']
  }

  if (verb === 'set') {
    if (options['--host'] !== undefined) {
      if (!isLanIPv4(options['--host'])) { deps.err('  ✗ --host must be a private LAN IPv4 address (10.x, 172.16-31.x or 192.168.x).'); return 2 }
      body.host = options['--host']
    }
    deps.out('')
    deps.out('  The dial must be plugged in with its USB cable. The network name and the password are asked here and never stored on this computer.')
    const ssid = await deps.ask('  Wi-Fi network name: ')
    if (!validSsid(ssid)) { deps.err(`  ✗ ${NET_FIELD_MESSAGES.ssid}`); return 1 }
    const pass = await deps.askSecret('  Wi-Fi password (empty for an open network): ')
    if (deps.tty && pass !== '' && await deps.askSecret('  Type the password again: ') !== pass) {
      deps.err('  ✗ The two passwords did not match. Nothing was changed.')
      return 1
    }
    if (!validPass(pass)) { deps.err(`  ✗ ${NET_FIELD_MESSAGES.pass}`); return 1 }
    body.ssid = ssid
    body.pass = pass
  }
  if (verb === 'on' || verb === 'off') body.mode = verb === 'on' ? 1 : 0

  const path = verb === 'show' ? 'status' : verb === 'on' || verb === 'off' ? 'mode' : verb === 'set' ? 'provision' : verb
  const reply = await deps.call(`/api/dial/net/${path}`, body)
  if (!reply) { deps.err('  ✗ The Harness daemon is not running. Start it with: harness start'); return 1 }
  if (reply.status !== 200 || reply.body.ok !== true) {
    const kind = typeof reply.body.kind === 'string' ? reply.body.kind : ''
    const error = typeof reply.body.error === 'string' ? reply.body.error
      : reply.status === 404 ? 'This daemon does not support Wi-Fi dials (update it).' : 'The daemon sent back something unexpected.'
    deps.err(`  ✗ ${error}`)
    if (HINTS[kind]) deps.err(`    ${HINTS[kind]}`)
    return 1
  }
  print(verb, reply.body, deps)
  return 0
}

/** What each verb says when it worked. */
function print(verb: string, answer: Record<string, unknown>, deps: DialWifiDeps): void {
  const dial = typeof answer.dial === 'string' ? answer.dial : 'the dial'
  const out = deps.out
  switch (verb) {
    case 'set':
      out('')
      out(`  ✓ Wi-Fi saved on the dial ${dial}`)
      out(`    network   ${String(answer.ssid)}`)
      out(`    daemon    ${String(answer.host)}:${String(answer.port)}   (this computer)`)
      out('')
      out('  Unplug the USB cable: after about 10 seconds the dial joins the network and connects here.')
      out('  This computer\'s address must stay the same: reserve it in your router (a DHCP reservation).')
      out('  On Windows, allow the connection through the firewall once:  harness dial wifi firewall')
      out('')
      return
    case 'show': {
      out('')
      if (typeof answer.lan === 'string') out(`  This computer   ${answer.lan}:${String(answer.port)}`)
      else out(`  This computer   no private network address found (listener port ${String(answer.port)})`)
      const usb = answer.usb as Record<string, unknown> | undefined
      if (usb) {
        out(`  On the cable    ${String(usb.dial ?? 'a dial')}`)
        if (usb.saved === true) {
          out(`    network       ${String(usb.ssid ?? '?')}`)
          out(`    daemon        ${String(usb.host ?? '?')}:${String(usb.port ?? '?')}`)
          out(`    Wi-Fi         ${usb.mode === 1 ? 'on (joins when the cable is unplugged)' : 'off (USB only)'}`)
          if (usb.keyed === false) out('    This computer holds no key for it. Set it again with `harness dial wifi set`.')
        } else out('    No Wi-Fi settings. Run `harness dial wifi set`.')
      } else if (answer.usbProblem && typeof (answer.usbProblem as { message?: unknown }).message === 'string') {
        out(`  On the cable    ${(answer.usbProblem as { message: string }).message}`)
      } else out('  On the cable    no dial')
      const keyed = Array.isArray(answer.keyed) ? answer.keyed as Array<Record<string, unknown>> : []
      if (!keyed.length) out('  No dial has a key on this computer yet. Run: harness dial wifi set')
      for (const row of keyed) {
        out(`  Key held        ${String(row.dial)}   set ${stamp(row.createdAt)}${row.lastSeen === undefined ? '' : `   last seen ${stamp(row.lastSeen)} from ${String(row.lastAddr ?? '?')}`}`)
      }
      out('')
      return
    }
    case 'forget':
      out('')
      if (answer.cleared === true) out(`  ✓ The dial ${dial} forgot its Wi-Fi network and key.${answer.removed === true ? ' So did this computer.' : ''}`)
      else out(`  ✓ This computer forgot the key of ${dial}. The dial was not on the USB cable, so it still holds its own settings: it cannot connect any more. Clear it with the cable plugged in.`)
      out('')
      return
    case 'rotate':
      out('')
      out(`  ✓ The dial ${dial} has a new key. The old one no longer works.`)
      out('')
      return
    default:
      out('')
      out(answer.mode === 1
        ? `  ✓ Wi-Fi is on for ${dial}: it joins its network when the USB cable is unplugged.`
        : `  ✓ Wi-Fi is off for ${dial}: it starts on USB only.`)
      out('')
  }
}

/**
 * The lines of a pipe, one per call, from one read of it. Reading each line with its own reader would lose the second line to
 * the first reader's buffer. An empty string once the lines have run out.
 */
export function stdinLines(input: AsyncIterable<string | Buffer>): () => Promise<string> {
  let lines: string[] | null = null
  const load = async (): Promise<string[]> => {
    const chunks: Buffer[] = []
    for await (const chunk of input) chunks.push(typeof chunk === 'string' ? Buffer.from(chunk, 'utf8') : chunk)
    const parts = Buffer.concat(chunks).toString('utf8').split(/\r?\n/)
    if (parts.at(-1) === '') parts.pop()
    return parts
  }
  return async () => (lines ??= await load()).shift() ?? ''
}
