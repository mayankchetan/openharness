// The daemon's keystore for the dial link (docs/specs/cable-wifi-link.md section 5.4; task W8).
//
// One 32-byte TLS-PSK per dial, by the dial's MAC string, in `<HARNESS_DEVICES_DIR>/dial-net.json`
// (`~/.harness/devices/`, beside the custom pets: product-root state that `harness reset` must not lose).
//
//   { "version": 1, "dials": { "B8:1F:3F:CA:E5:58": { "key": "<base64>", "createdAt": ms, "lastSeen": ms, "lastAddr": "192.168.1.40" } } }
//
// File mode. The temp file is created with 0o600 and the directory with 0o700, which is the whole of the protection on
// POSIX. Node on Windows maps a mode to the read-only bit alone, so there it asks for nothing more than "writable by the
// owner" and the real protection is the ACL the folder inherits: `~/.harness` is under the user's profile, which the
// default Windows ACL keeps to that user, SYSTEM and Administrators. This module does not spawn `icacls`.
//
// Written atomically: a temp file is written and flushed, then renamed over the old one, so a crash leaves either file whole.
// Memory moves only after the disk has: a write that fails throws and the store is as it was.
//
// The key is never logged, never part of an error message, and the class keeps it in a `#private` field so that printing or
// serialising the store shows nothing. `get` hands out a copy.
import * as nodeFs from 'node:fs'
import { dirname } from 'node:path'

import { PSK_LENGTH, normalizePskIdentity } from './tlsPsk.js'

export const DIAL_KEY_FILE = 'dial-net.json'

/** An address that authenticated this recently is a dial's home: the listener never throttles it (spec 4.2). */
export const KNOWN_ADDR_MS = 7 * 24 * 60 * 60 * 1000

/** A repeat connection from the same address inside this is not written to disk: `lastSeen` only has to be good to a day. */
const TOUCH_MIN_MS = 60_000

const MAC = /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/

/** The file operations used; a test swaps one to fail a write. */
export type KeyFs = Pick<typeof nodeFs, 'mkdirSync' | 'readFileSync' | 'openSync' | 'writeSync' | 'fsyncSync' | 'closeSync' | 'renameSync' | 'unlinkSync' | 'chmodSync'>

export interface DialKeysOptions {
  fs?: KeyFs
  now?: () => number
  /** Where a load problem is told. Never gets a key. */
  log?: (line: string) => void
}

/** What is known about a dial besides its key. */
export interface DialKeyRecord {
  createdAt: number
  lastSeen?: number
  lastAddr?: string
}

export interface DialKeyChange {
  id: string
  kind: 'set' | 'remove'
}

interface Stored extends DialKeyRecord {
  key: Buffer
}

export class DialKeys {
  #dials = new Map<string, Stored>()
  readonly #listeners = new Set<(change: DialKeyChange) => void>()
  readonly #fs: KeyFs
  readonly #now: () => number
  readonly #log: (line: string) => void

  constructor(readonly file: string, options: DialKeysOptions = {}) {
    this.#fs = options.fs ?? nodeFs
    this.#now = options.now ?? Date.now
    this.#log = options.log ?? (() => {})
    this.#load()
  }

  /** How many dials have a key. */
  get size(): number {
    return this.#dials.size
  }

  ids(): string[] {
    return [...this.#dials.keys()]
  }

  has(id: string): boolean {
    return this.#dials.has(normalizePskIdentity(id))
  }

  /** The key of a dial, as a copy, or undefined. */
  get(id: string): Buffer | undefined {
    const found = this.#dials.get(normalizePskIdentity(id))
    return found ? Buffer.from(found.key) : undefined
  }

  /** Everything about a dial but its key. */
  record(id: string): DialKeyRecord | undefined {
    const found = this.#dials.get(normalizePskIdentity(id))
    if (!found) return undefined
    const { key: _key, ...record } = found
    return record
  }

  /** Give a dial a key, or replace its key (a new key starts the dial's record again). Throws when the write fails. */
  set(id: string, key: Buffer): void {
    if (!MAC.test(id)) throw new Error('a dial identity is its MAC string, like B8:1F:3F:CA:E5:58')
    if (key.length !== PSK_LENGTH) throw new Error(`a dial key is ${PSK_LENGTH} bytes`)
    const mac = normalizePskIdentity(id)
    const next = new Map(this.#dials)
    next.set(mac, { key: Buffer.from(key), createdAt: this.#now() })
    this.#commit(next)
    this.#tell({ id: mac, kind: 'set' })
  }

  /** Forget a dial. False when it had no key. Throws when the write fails. */
  remove(id: string): boolean {
    const mac = normalizePskIdentity(id)
    if (!this.#dials.has(mac)) return false
    const next = new Map(this.#dials)
    next.delete(mac)
    this.#commit(next)
    this.#tell({ id: mac, kind: 'remove' })
    return true
  }

  /** The dial authenticated from `addr` just now. Ignored for a dial with no key. Throws when the write fails. */
  touch(id: string, addr: string): void {
    const mac = normalizePskIdentity(id)
    const found = this.#dials.get(mac)
    if (!found) return
    const now = this.#now()
    if (found.lastAddr === addr && found.lastSeen !== undefined && now - found.lastSeen < TOUCH_MIN_MS) return
    const next = new Map(this.#dials)
    next.set(mac, { ...found, lastSeen: now, lastAddr: addr })
    this.#commit(next)
  }

  /** Did any dial authenticate from this address in the last 7 days? */
  isKnownAddr(addr: string): boolean {
    const now = this.#now()
    for (const dial of this.#dials.values()) {
      if (dial.lastAddr === addr && dial.lastSeen !== undefined && now - dial.lastSeen < KNOWN_ADDR_MS) return true
    }
    return false
  }

  /** Be told when a key is set or removed (not on a touch). Returns the function that stops it. */
  onChange(listener: (change: DialKeyChange) => void): () => void {
    this.#listeners.add(listener)
    return () => void this.#listeners.delete(listener)
  }

  #tell(change: DialKeyChange): void {
    for (const listener of [...this.#listeners]) {
      try {
        listener(change)
      } catch {
        // a listener's trouble is not the store's
      }
    }
  }

  #commit(next: Map<string, Stored>): void {
    const dials: Record<string, { key: string } & DialKeyRecord> = {}
    for (const [id, dial] of next) {
      dials[id] = { key: dial.key.toString('base64'), createdAt: dial.createdAt, ...(dial.lastSeen !== undefined ? { lastSeen: dial.lastSeen } : {}), ...(dial.lastAddr ? { lastAddr: dial.lastAddr } : {}) }
    }
    const text = JSON.stringify({ version: 1, dials }, null, 2) + '\n'
    const fs = this.#fs
    fs.mkdirSync(dirname(this.file), { recursive: true, mode: 0o700 })
    const temp = `${this.file}.tmp`
    // The temp file name is fixed: this process is its only writer, and a leftover from a crash is simply written over.
    try {
      fs.unlinkSync(temp)
    } catch {
      // none
    }
    const fd = fs.openSync(temp, 'w', 0o600)
    try {
      fs.writeSync(fd, text)
      fs.fsyncSync(fd)
    } finally {
      fs.closeSync(fd)
    }
    try {
      fs.chmodSync(temp, 0o600)
      fs.renameSync(temp, this.file)
    } catch (error) {
      try {
        fs.unlinkSync(temp)
      } catch {
        // none
      }
      throw error
    }
    this.#dials = next
  }

  #load(): void {
    let text: string
    try {
      text = this.#fs.readFileSync(this.file, 'utf8')
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') this.#log(`dial keys: cannot read ${this.file}: ${(error as NodeJS.ErrnoException).code ?? 'error'}`)
      return
    }
    let parsed: unknown
    try {
      parsed = JSON.parse(text)
    } catch {
      parsed = undefined
    }
    const dials = (parsed as { version?: unknown; dials?: unknown } | undefined)?.dials
    if (!dials || typeof dials !== 'object' || Array.isArray(dials)) {
      // Moved aside, not overwritten: whatever is in it stays for the person to look at.
      try {
        this.#fs.renameSync(this.file, `${this.file}.bad`)
      } catch {
        // best effort
      }
      this.#log(`dial keys: ${this.file} is unreadable — moved to ${DIAL_KEY_FILE}.bad, no Wi-Fi dial has a key until it is provisioned again`)
      return
    }
    for (const [id, raw] of Object.entries(dials)) {
      const row = raw as Partial<Record<string, unknown>> | null
      if (!MAC.test(id) || !row || typeof row.key !== 'string') continue
      const key = Buffer.from(row.key, 'base64')
      if (key.length !== PSK_LENGTH) continue
      this.#dials.set(normalizePskIdentity(id), {
        key,
        createdAt: typeof row.createdAt === 'number' ? row.createdAt : 0,
        ...(typeof row.lastSeen === 'number' ? { lastSeen: row.lastSeen } : {}),
        ...(typeof row.lastAddr === 'string' ? { lastAddr: row.lastAddr } : {}),
      })
    }
  }
}
