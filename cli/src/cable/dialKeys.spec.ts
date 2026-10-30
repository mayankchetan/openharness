// The daemon's keystore for the dial link (dialKeys.ts): one 32-byte TLS-PSK per dial, on disk, written atomically.
import { randomBytes } from 'node:crypto'
import * as fs from 'node:fs'
import { mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { inspect } from 'node:util'
import { afterEach, describe, expect, it, vi } from 'vitest'

import { DIAL_KEY_FILE, DialKeys, KNOWN_ADDR_MS } from './dialKeys.js'

const A = 'B8:1F:3F:CA:E5:58'
const B = 'B8:1F:3F:CA:E5:59'

const dirs: string[] = []
function tmp(): string {
  const dir = mkdtempSync(join(tmpdir(), 'dial-keys-'))
  dirs.push(dir)
  return dir
}
afterEach(() => {
  for (const d of dirs.splice(0)) rmSync(d, { recursive: true, force: true })
})

describe('DialKeys', () => {
  it('stores a key per dial, upper-cased, and gives back a copy', () => {
    const keys = new DialKeys(join(tmp(), DIAL_KEY_FILE))
    const key = randomBytes(32)
    expect(keys.size).toBe(0)
    keys.set(A.toLowerCase(), key)
    expect(keys.size).toBe(1)
    expect(keys.ids()).toEqual([A])
    expect(keys.has(A)).toBe(true)
    const got = keys.get(A.toLowerCase())!
    expect(got.equals(key)).toBe(true)
    // a caller scribbling on what it got must not change the stored key
    got.fill(0)
    expect(keys.get(A)!.equals(key)).toBe(true)
    expect(keys.get(B)).toBeUndefined()
  })

  it('refuses an identity that is not a MAC and a key that is not 32 bytes', () => {
    const keys = new DialKeys(join(tmp(), DIAL_KEY_FILE))
    expect(() => keys.set('not-a-mac', randomBytes(32))).toThrow(/identity/)
    expect(() => keys.set(A, randomBytes(31))).toThrow(/32 bytes/)
    expect(() => keys.set(A, randomBytes(33))).toThrow(/32 bytes/)
    expect(keys.size).toBe(0)
  })

  it('survives a restart', () => {
    const file = join(tmp(), 'state', DIAL_KEY_FILE)
    const key = randomBytes(32)
    const first = new DialKeys(file, { now: () => 1_000 })
    first.set(A, key)
    first.touch(A, '192.168.1.40')
    const second = new DialKeys(file)
    expect(second.get(A)!.equals(key)).toBe(true)
    expect(second.record(A)).toEqual({ createdAt: 1_000, lastSeen: 1_000, lastAddr: '192.168.1.40' })
  })

  it('remove forgets the dial, and the file with it', () => {
    const file = join(tmp(), DIAL_KEY_FILE)
    const keys = new DialKeys(file)
    keys.set(A, randomBytes(32))
    keys.set(B, randomBytes(32))
    expect(keys.remove(A)).toBe(true)
    expect(keys.remove(A)).toBe(false)
    expect(new DialKeys(file).ids()).toEqual([B])
  })

  it('set on an existing dial replaces the key and starts its record again', () => {
    let t = 10
    const keys = new DialKeys(join(tmp(), DIAL_KEY_FILE), { now: () => t })
    const first = randomBytes(32)
    keys.set(A, first)
    keys.touch(A, '10.0.0.5')
    t = 20
    const second = randomBytes(32)
    keys.set(A, second)
    expect(keys.get(A)!.equals(second)).toBe(true)
    // the new key has not been used yet: the old address says nothing about it
    expect(keys.record(A)).toEqual({ createdAt: 20 })
    expect(keys.isKnownAddr('10.0.0.5')).toBe(false)
  })

  it('knows an address that authenticated in the last 7 days, and no other', () => {
    let t = 1_000_000
    const keys = new DialKeys(join(tmp(), DIAL_KEY_FILE), { now: () => t })
    keys.set(A, randomBytes(32))
    expect(keys.isKnownAddr('10.0.0.5')).toBe(false)
    keys.touch(A, '10.0.0.5')
    expect(keys.isKnownAddr('10.0.0.5')).toBe(true)
    expect(keys.isKnownAddr('10.0.0.6')).toBe(false)
    t += KNOWN_ADDR_MS - 1
    expect(keys.isKnownAddr('10.0.0.5')).toBe(true)
    t += 1
    expect(keys.isKnownAddr('10.0.0.5')).toBe(false)
    // a touch for a dial that is not in the store is ignored
    keys.touch(B, '10.0.0.9')
    expect(keys.isKnownAddr('10.0.0.9')).toBe(false)
  })

  it('tells a listener when a key was set or removed', () => {
    const keys = new DialKeys(join(tmp(), DIAL_KEY_FILE))
    const seen: string[] = []
    const off = keys.onChange((c) => seen.push(`${c.kind}:${c.id}`))
    keys.set(A, randomBytes(32))
    keys.touch(A, '10.0.0.5')
    keys.remove(A)
    off()
    keys.set(B, randomBytes(32))
    expect(seen).toEqual([`set:${A}`, `remove:${A}`])
  })

  it('writes through a temp file and a rename: a crash before the rename leaves the old file whole', () => {
    const dir = tmp()
    const file = join(dir, DIAL_KEY_FILE)
    const key = randomBytes(32)
    const keys = new DialKeys(file)
    keys.set(A, key)
    const before = readFileSync(file, 'utf8')
    const crashing = new DialKeys(file, {
      fs: {
        ...fs,
        renameSync: () => {
          throw new Error('killed before the rename')
        },
      },
    })
    expect(() => crashing.set(B, randomBytes(32))).toThrow(/killed/)
    // memory did not move ahead of the disk
    expect(crashing.has(B)).toBe(false)
    expect(readFileSync(file, 'utf8')).toBe(before)
    expect(new DialKeys(file).ids()).toEqual([A])
    // the stale temp file does not matter, and the next write replaces it
    const ok = new DialKeys(file)
    ok.set(B, randomBytes(32))
    expect(new DialKeys(file).ids().sort()).toEqual([A, B])
    expect(readdirSync(dir).filter((n) => n.endsWith('.tmp'))).toEqual([])
  })

  it('a failed write on touch or remove does not change the store either', () => {
    const dir = tmp()
    const file = join(dir, DIAL_KEY_FILE)
    new DialKeys(file).set(A, randomBytes(32))
    const failing = new DialKeys(file, {
      fs: {
        ...fs,
        renameSync: () => {
          throw new Error('disk full')
        },
      },
    })
    expect(() => failing.remove(A)).toThrow(/disk full/)
    expect(failing.has(A)).toBe(true)
    expect(() => failing.touch(A, '10.0.0.5')).toThrow(/disk full/)
    expect(failing.isKnownAddr('10.0.0.5')).toBe(false)
  })

  it('asks for a file only its owner can read', () => {
    const dir = tmp()
    const file = join(dir, 'devices', DIAL_KEY_FILE)
    const opened: Array<{ mode: unknown }> = []
    const spy = {
      ...fs,
      openSync: ((path: fs.PathLike, flags: fs.OpenMode, mode?: fs.Mode | null) => {
        opened.push({ mode })
        return fs.openSync(path, flags, mode)
      }) as typeof fs.openSync,
    }
    new DialKeys(file, { fs: spy }).set(A, randomBytes(32))
    expect(opened.map((o) => o.mode)).toEqual([0o600])
    if (process.platform !== 'win32') {
      expect(statSync(file).mode & 0o777).toBe(0o600)
      expect(statSync(join(dir, 'devices')).mode & 0o777).toBe(0o700)
    }
  })

  it('moves an unreadable file aside and starts empty, so a restart never loses it silently', () => {
    const dir = tmp()
    const file = join(dir, DIAL_KEY_FILE)
    writeFileSync(file, '{ half a file')
    const log = vi.fn()
    const keys = new DialKeys(file, { log })
    expect(keys.size).toBe(0)
    expect(readdirSync(dir)).toContain(`${DIAL_KEY_FILE}.bad`)
    expect(log).toHaveBeenCalledTimes(1)
    expect(String(log.mock.calls[0][0])).toMatch(/unreadable/)
  })

  it('skips records with a bad identity or key instead of failing the whole file', () => {
    const dir = tmp()
    const file = join(dir, DIAL_KEY_FILE)
    const good = randomBytes(32)
    writeFileSync(
      file,
      JSON.stringify({
        version: 1,
        dials: {
          [A]: { key: good.toString('base64'), createdAt: 5 },
          [B]: { key: Buffer.alloc(5).toString('base64'), createdAt: 5 },
          'junk': { key: good.toString('base64'), createdAt: 5 },
        },
      }),
    )
    const keys = new DialKeys(file)
    expect(keys.ids()).toEqual([A])
    expect(keys.get(A)!.equals(good)).toBe(true)
  })

  it('never puts the key in a log line, an error message or the object it prints as', () => {
    const dir = tmp()
    const file = join(dir, DIAL_KEY_FILE)
    const key = randomBytes(32)
    const lines: string[] = []
    const keys = new DialKeys(file, { log: (l) => lines.push(l) })
    keys.set(A, key)
    keys.touch(A, '10.0.0.5')
    // the store when printed or serialised
    const shown = [JSON.stringify(keys), String(keys), inspect(keys, { showHidden: true, depth: 6 })].join('\n')
    // and an error out of a failed write
    const failing = new DialKeys(file, {
      log: (l) => lines.push(l),
      fs: {
        ...fs,
        renameSync: () => {
          throw new Error('rename failed')
        },
      },
    })
    let message = ''
    try {
      failing.set(B, key)
    } catch (e) {
      message = String(e)
    }
    expect(shown).not.toContain('<Buffer')
    for (const text of [...lines, shown, message]) {
      expect(text).not.toContain(key.toString('hex'))
      expect(text).not.toContain(key.toString('base64'))
    }
  })
})
