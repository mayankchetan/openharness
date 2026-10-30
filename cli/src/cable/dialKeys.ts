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
// Written atomically: a temp file (a name of its own: pid and random) is written and flushed, then renamed over the old
// one, and the folder is flushed where the platform lets it (Windows cannot open a folder: skipped there), so a crash
// leaves either file whole. Any failure in that sequence removes the temp file. Memory moves only after the disk has: a
// write that fails throws and the store is as it was.
//
// Writers. The devices process is the ONLY writer; the provisioning (W9) runs inside it. This is not a lock. As a guard
// against a second writer (a second daemon on the same home, a person editing the file) a write first looks at the file's
// mtime and size, and if they moved since this process last read or wrote it, it reads the file again and takes that as
// the truth, then applies its own one change on top: on-disk rows win for every dial this call is not about. Listeners are
// told of any key that changed that way. A race between two writers inside one stat tick is possible and not defended.
//
// A file that cannot be READ (a permission error, a disk error: anything but "not there") makes the store unreadable: it
// refuses to write, because a write from an empty memory would replace every key on disk with nothing. It tries the file
// again at each write, as `reload()`, and by itself every 60 s (an unref'd timer, stopped by `close()`), and writes again when
// that works; the listeners are told of every key that came back, so the Wi-Fi port can open. A file that reads but is not
// JSON is moved aside as `.bad` and the store starts empty.
//
// Leftover temp files (`dial-net.json.<pid>.<random>.tmp`, from a crash between the write and the rename) are removed: all
// of them when the store is created (no write of this process can be running then), and any older than 60 s at a later read.
//
// The key is never logged, never part of an error message, and the class keeps it in a `#private` field so that printing or
// serialising the store shows nothing. `get` hands out a copy.
import { randomBytes } from 'node:crypto'
import * as nodeFs from 'node:fs'
import { basename, dirname, join } from 'node:path'

import { PSK_LENGTH, normalizePskIdentity } from './tlsPsk.js'

export const DIAL_KEY_FILE = 'dial-net.json'

/** An address that authenticated this recently is a dial's home: the listener never throttles it (spec 4.2). */
export const KNOWN_ADDR_MS = 7 * 24 * 60 * 60 * 1000

/** A repeat connection from the same address inside this is not written to disk: `lastSeen` only has to be good to a day. */
const TOUCH_MIN_MS = 60_000

/** How often an unreadable store looks at its file again, by itself. */
const RETRY_MS = 60_000

/** A leftover temp file this old is from a write that is not running. */
const STALE_TEMP_MS = 60_000

const MAC = /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/

/** The file operations used; a test swaps one to fail a write. */
export type KeyFs = Pick<typeof nodeFs, 'mkdirSync' | 'readFileSync' | 'readdirSync' | 'statSync' | 'openSync' | 'writeSync' | 'fsyncSync' | 'closeSync' | 'renameSync' | 'unlinkSync' | 'chmodSync'>

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
  /** The person gave this dial's daemon address by hand (`--host`): a changed address of this computer is not sent to it on attach. Optional; the file stays version 1. */
  hostPinned?: boolean
}

export interface DialKeyChange {
  id: string
  kind: 'set' | 'remove'
}

interface Stored extends DialKeyRecord {
  key: Buffer
}

/** What the file looked like when it was last read or written. */
interface Stamp {
  mtimeMs: number
  size: number
}

export class DialKeys {
  #dials = new Map<string, Stored>()
  #stamp: Stamp | null = null
  #unreadable = false
  #retry: ReturnType<typeof setInterval> | null = null
  #closed = false
  readonly #listeners = new Set<(change: DialKeyChange) => void>()
  readonly #fs: KeyFs
  readonly #now: () => number
  readonly #log: (line: string) => void

  constructor(readonly file: string, options: DialKeysOptions = {}) {
    this.#fs = options.fs ?? nodeFs
    this.#now = options.now ?? Date.now
    this.#log = options.log ?? (() => {})
    this.#readFile()
    this.#sweepTemp(true)
    this.#watchUnreadable()
  }

  /** Stop looking at an unreadable file by itself. The store goes on working as it is. */
  close(): void {
    this.#closed = true
    if (this.#retry) clearInterval(this.#retry)
    this.#retry = null
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

  /** Could the file be read the last time it was tried? While it could not, nothing is written. */
  get readable(): boolean {
    return !this.#unreadable
  }

  /** Read the file again. True when that worked. Listeners hear of every key that differs from before. */
  reload(): boolean {
    this.#load()
    return !this.#unreadable
  }

  /** Give a dial a key, or replace its key (a new key starts the dial's record again). Throws when the write fails. */
  set(id: string, key: Buffer, options: { hostPinned?: boolean } = {}): void {
    if (!MAC.test(id)) throw new Error('a dial identity is its MAC string, like B8:1F:3F:CA:E5:58')
    if (key.length !== PSK_LENGTH) throw new Error(`a dial key is ${PSK_LENGTH} bytes`)
    const mac = normalizePskIdentity(id)
    this.#write((next) => {
      next.set(mac, { key: Buffer.from(key), createdAt: this.#now(), ...(options.hostPinned ? { hostPinned: true } : {}) })
    })
    this.#tell({ id: mac, kind: 'set' })
  }

  /** Forget a dial. False when it had no key. Throws when the write fails. */
  remove(id: string): boolean {
    const mac = normalizePskIdentity(id)
    this.#catchUp()
    if (!this.#dials.has(mac)) return false
    const next = new Map(this.#dials)
    next.delete(mac)
    this.#persist(next)
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
    this.#write((next) => {
      const current = next.get(mac)
      if (current) next.set(mac, { ...current, lastSeen: now, lastAddr: addr })
    })
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

  #stat(): Stamp | null {
    try {
      const stat = this.#fs.statSync(this.file)
      return { mtimeMs: stat.mtimeMs, size: stat.size }
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === 'ENOENT') return null
      throw error
    }
  }

  #changedOnDisk(): boolean {
    let now: Stamp | null
    try {
      now = this.#stat()
    } catch {
      return true
    }
    const before = this.#stamp
    return (now === null) !== (before === null) || (now !== null && before !== null && (now.mtimeMs !== before.mtimeMs || now.size !== before.size))
  }

  /**
   * Make memory equal the file before a write, if the file moved since this process last saw it. Throws when the file
   * cannot be read: nothing is written over what could not be looked at.
   */
  #catchUp(): void {
    if (this.#unreadable) this.#load()
    if (this.#unreadable) throw new Error('the dial keystore cannot be read, so it is not written')
    if (this.#changedOnDisk()) {
      this.#load()
      if (this.#unreadable) throw new Error('the dial keystore cannot be read, so it is not written')
    }
  }

  /** Read the file, then tell the listeners of every key that is not what it was (another writer's change, or a recovery). */
  #load(): void {
    const before = this.#dials
    this.#readFile()
    this.#sweepTemp(false)
    this.#watchUnreadable()
    for (const id of new Set([...before.keys(), ...this.#dials.keys()])) {
      const was = before.get(id)
      const is = this.#dials.get(id)
      if (!is) this.#tell({ id, kind: 'remove' })
      else if (!was || !was.key.equals(is.key)) this.#tell({ id, kind: 'set' })
    }
  }

  /** While the file cannot be read, look again every minute; stop when it can. */
  #watchUnreadable(): void {
    if (this.#unreadable && !this.#retry && !this.#closed) {
      this.#retry = setInterval(() => this.#load(), RETRY_MS)
      this.#retry.unref?.()
    } else if ((!this.#unreadable || this.#closed) && this.#retry) {
      clearInterval(this.#retry)
      this.#retry = null
    }
  }

  /** Remove temp files a crashed write left behind: all of them (at start) or those older than a minute. */
  #sweepTemp(all: boolean): void {
    const fs = this.#fs
    const dir = dirname(this.file)
    const prefix = `${basename(this.file)}.`
    try {
      for (const name of fs.readdirSync(dir)) {
        if (!name.startsWith(prefix) || !name.endsWith('.tmp')) continue
        const path = join(dir, name)
        try {
          if (all || Date.now() - fs.statSync(path).mtimeMs > STALE_TEMP_MS) fs.unlinkSync(path)
        } catch {
          // gone already, or not ours to remove
        }
      }
    } catch {
      // no folder yet
    }
  }

  /** One change: look at the file first, apply the change to a copy, write it, and only then keep the copy. */
  #write(apply: (next: Map<string, Stored>) => void): void {
    this.#catchUp()
    const next = new Map(this.#dials)
    apply(next)
    this.#persist(next)
  }

  #persist(next: Map<string, Stored>): void {
    const dials: Record<string, { key: string } & DialKeyRecord> = {}
    for (const [id, dial] of next) {
      dials[id] = { key: dial.key.toString('base64'), createdAt: dial.createdAt, ...(dial.lastSeen !== undefined ? { lastSeen: dial.lastSeen } : {}), ...(dial.lastAddr ? { lastAddr: dial.lastAddr } : {}), ...(dial.hostPinned ? { hostPinned: true } : {}) }
    }
    const text = JSON.stringify({ version: 1, dials }, null, 2) + '\n'
    const fs = this.#fs
    const dir = dirname(this.file)
    // A name of its own, so a second writer's temp file is never this one's.
    const temp = `${this.file}.${process.pid}.${randomBytes(4).toString('hex')}.tmp`
    let fd: number | undefined
    try {
      fs.mkdirSync(dir, { recursive: true, mode: 0o700 })
      fd = fs.openSync(temp, 'w', 0o600)
      fs.writeSync(fd, text)
      fs.fsyncSync(fd)
      const done = fd
      fd = undefined
      fs.closeSync(done)
      fs.chmodSync(temp, 0o600)
      fs.renameSync(temp, this.file)
    } catch (error) {
      if (fd !== undefined) {
        try {
          fs.closeSync(fd)
        } catch {
          // none
        }
      }
      try {
        fs.unlinkSync(temp)
      } catch {
        // none
      }
      throw error
    }
    // Durability of the rename itself. A folder cannot be opened on Windows; the rename is already done either way.
    try {
      const dirFd = fs.openSync(dir, 'r')
      try {
        fs.fsyncSync(dirFd)
      } finally {
        fs.closeSync(dirFd)
      }
    } catch {
      // skipped where the platform does not allow it
    }
    this.#dials = next
    try {
      this.#stamp = this.#stat()
    } catch {
      this.#stamp = null
    }
  }

  #readFile(): void {
    let text: string
    let stamp: Stamp | null
    try {
      stamp = this.#stat()
      text = this.#fs.readFileSync(this.file, 'utf8')
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code
      if (code === 'ENOENT') {
        this.#dials = new Map()
        this.#stamp = null
        this.#unreadable = false
        return
      }
      if (!this.#unreadable) this.#log(`dial keys: cannot read ${this.file} (${code ?? 'error'}) — no key is written until it can be`)
      this.#unreadable = true
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
      this.#dials = new Map()
      this.#stamp = null
      this.#unreadable = false
      return
    }
    const loaded = new Map<string, Stored>()
    for (const [id, raw] of Object.entries(dials)) {
      const row = raw as Partial<Record<string, unknown>> | null
      if (!MAC.test(id) || !row || typeof row.key !== 'string') continue
      const key = Buffer.from(row.key, 'base64')
      if (key.length !== PSK_LENGTH) continue
      loaded.set(normalizePskIdentity(id), {
        key,
        createdAt: typeof row.createdAt === 'number' ? row.createdAt : 0,
        ...(typeof row.lastSeen === 'number' ? { lastSeen: row.lastSeen } : {}),
        ...(typeof row.lastAddr === 'string' ? { lastAddr: row.lastAddr } : {}),
        ...(row.hostPinned === true ? { hostPinned: true } : {}),
      })
    }
    this.#dials = loaded
    this.#stamp = stamp
    this.#unreadable = false
  }
}
