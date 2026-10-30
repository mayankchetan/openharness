// The daemon's keystore for the dial link (dialKeys.ts): one 32-byte TLS-PSK per dial, on disk, written atomically.
import { randomBytes } from 'node:crypto'
import * as fs from 'node:fs'
import { mkdtempSync, readFileSync, readdirSync, rmSync, statSync, utimesSync, writeFileSync } from 'node:fs'
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
    const opened: Array<{ flags: unknown; mode: unknown }> = []
    const spy = {
      ...fs,
      openSync: ((path: fs.PathLike, flags: fs.OpenMode, mode?: fs.Mode | null) => {
        opened.push({ flags, mode })
        return fs.openSync(path, flags, mode)
      }) as typeof fs.openSync,
    }
    new DialKeys(file, { fs: spy }).set(A, randomBytes(32))
    expect(opened.filter((o) => o.flags === 'w').map((o) => o.mode)).toEqual([0o600])
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

describe('DialKeys: writers and failures', () => {
  const fileIn = () => join(tmp(), DIAL_KEY_FILE)
  const leftovers = (file: string) => readdirSync(join(file, '..')).filter((n) => n.endsWith('.tmp'))
  /** Move the mtime on, so two writes in one millisecond are still two writes to the stat check. */
  const bump = (file: string) => utimesSync(file, new Date(Date.now() + 5_000), new Date(Date.now() + 5_000))

  it('gives every write a temp file of its own: pid and random', () => {
    const file = fileIn()
    const names: string[] = []
    const spy = {
      ...fs,
      openSync: ((path: fs.PathLike, flags: fs.OpenMode, mode?: fs.Mode | null) => {
        if (flags === 'w') names.push(String(path))
        return fs.openSync(path, flags, mode)
      }) as typeof fs.openSync,
    }
    const one = new DialKeys(file, { fs: spy })
    const two = new DialKeys(file, { fs: spy })
    one.set(A, randomBytes(32))
    one.set(B, randomBytes(32))
    two.touch(A, '10.0.0.5')
    expect(names).toHaveLength(2)
    expect(new Set(names).size).toBe(2)
    for (const name of names) expect(name).toMatch(new RegExp(`dial-net\\.json\\.${process.pid}\\.[0-9a-f]{8}\\.tmp$`))
    expect(leftovers(file)).toEqual([])
  })

  it('reads the file again when it moved, and applies its one change on top of what is there', () => {
    const file = fileIn()
    const one = new DialKeys(file)
    const two = new DialKeys(file)
    const seen: string[] = []
    two.onChange((c) => seen.push(`${c.kind}:${c.id}`))
    one.set(A, randomBytes(32))
    bump(file)
    // `two` has never heard of A. Its write of B must not drop it.
    two.set(B, randomBytes(32))
    expect(two.ids().sort()).toEqual([A, B])
    expect(new DialKeys(file).ids().sort()).toEqual([A, B])
    expect(seen).toEqual([`set:${A}`, `set:${B}`])
  })

  it('on-disk rows win for every dial the write is not about, and a key another writer removed is gone here too', () => {
    const file = fileIn()
    const one = new DialKeys(file)
    const two = new DialKeys(file)
    one.set(A, randomBytes(32))
    one.set(B, randomBytes(32))
    two.reload()
    expect(two.ids().sort()).toEqual([A, B])
    const rotated = randomBytes(32)
    one.set(A, rotated)
    one.remove(B)
    bump(file)
    const seen: string[] = []
    two.onChange((c) => seen.push(`${c.kind}:${c.id}`))
    two.touch(A, '10.0.0.7')
    expect(two.get(A)!.equals(rotated)).toBe(true)
    expect(two.has(B)).toBe(false)
    const disk = new DialKeys(file)
    expect(disk.get(A)!.equals(rotated)).toBe(true)
    expect(disk.has(B)).toBe(false)
    expect(disk.record(A)?.lastAddr).toBe('10.0.0.7')
    expect(seen.sort()).toEqual([`remove:${B}`, `set:${A}`])
  })

  it('does not read the file again when it has not moved', () => {
    const file = fileIn()
    new DialKeys(file).set(A, randomBytes(32))
    let reads = 0
    const counting = new DialKeys(file, {
      fs: {
        ...fs,
        readFileSync: ((...args: Parameters<typeof fs.readFileSync>) => {
          reads++
          return fs.readFileSync(...args)
        }) as typeof fs.readFileSync,
      },
    })
    const before = reads
    counting.set(B, randomBytes(32))
    counting.set(A, randomBytes(32))
    expect(reads).toBe(before)
  })

  it('refuses to write while the file cannot be read, and leaves it as it was', () => {
    const file = fileIn()
    const key = randomBytes(32)
    new DialKeys(file).set(A, key)
    const before = readFileSync(file, 'utf8')
    let failing = true
    const log = vi.fn()
    const keys = new DialKeys(file, {
      log,
      fs: {
        ...fs,
        readFileSync: ((...args: Parameters<typeof fs.readFileSync>) => {
          if (failing) throw Object.assign(new Error('denied'), { code: 'EACCES' })
          return fs.readFileSync(...args)
        }) as typeof fs.readFileSync,
      },
    })
    expect(keys.readable).toBe(false)
    expect(keys.size).toBe(0)
    expect(String(log.mock.calls[0][0])).toMatch(/cannot read .* \(EACCES\)/)
    expect(() => keys.set(B, randomBytes(32))).toThrow(/cannot be read/)
    expect(() => keys.remove(A)).toThrow(/cannot be read/)
    expect(readFileSync(file, 'utf8')).toBe(before)
    expect(keys.reload()).toBe(false)
    // the file reads again: the next write loads it first, so A is kept
    failing = false
    keys.set(B, randomBytes(32))
    expect(keys.readable).toBe(true)
    expect(keys.ids().sort()).toEqual([A, B])
    expect(new DialKeys(file).get(A)!.equals(key)).toBe(true)
  })

  it('reload succeeds once the file can be read', () => {
    const file = fileIn()
    new DialKeys(file).set(A, randomBytes(32))
    let failing = true
    const keys = new DialKeys(file, {
      fs: {
        ...fs,
        readFileSync: ((...args: Parameters<typeof fs.readFileSync>) => {
          if (failing) throw Object.assign(new Error('x'), { code: 'EIO' })
          return fs.readFileSync(...args)
        }) as typeof fs.readFileSync,
      },
    })
    expect(keys.readable).toBe(false)
    failing = false
    expect(keys.reload()).toBe(true)
    expect(keys.ids()).toEqual([A])
  })

  it.each(['openSync', 'writeSync', 'fsyncSync', 'closeSync', 'chmodSync', 'renameSync'] as const)('removes its temp file when %s fails, and the store is as it was', (op) => {
    const file = fileIn()
    new DialKeys(file).set(A, randomBytes(32))
    const before = readFileSync(file, 'utf8')
    let armed = false
    const wrap = <T extends (...args: never[]) => unknown>(name: string, real: T): T =>
      ((...args: never[]) => {
        if (armed && name === op) throw new Error(`${op} failed`)
        return real(...args)
      }) as T
    const keys = new DialKeys(file, {
      fs: {
        ...fs,
        openSync: wrap('openSync', fs.openSync),
        writeSync: wrap('writeSync', fs.writeSync),
        fsyncSync: wrap('fsyncSync', fs.fsyncSync),
        closeSync: wrap('closeSync', fs.closeSync),
        chmodSync: wrap('chmodSync', fs.chmodSync),
        renameSync: wrap('renameSync', fs.renameSync),
      },
    })
    armed = true
    expect(() => keys.set(B, randomBytes(32))).toThrow(new RegExp(`${op} failed`))
    armed = false
    expect(leftovers(file)).toEqual([])
    expect(keys.has(B)).toBe(false)
    expect(readFileSync(file, 'utf8')).toBe(before)
  })

  it('flushes the folder after the rename, and carries on where the platform will not open one', () => {
    const file = fileIn()
    const dir = join(file, '..')
    const opened: string[] = []
    const make = (refuseDir: boolean) => ({
      ...fs,
      openSync: ((path: fs.PathLike, flags: fs.OpenMode, mode?: fs.Mode | null) => {
        if (String(path) === dir) {
          opened.push(`${String(flags)}`)
          if (refuseDir) throw Object.assign(new Error('EISDIR'), { code: 'EISDIR' })
        }
        return fs.openSync(path, flags, mode)
      }) as typeof fs.openSync,
    })
    const keys = new DialKeys(file, { fs: make(false) })
    keys.set(A, randomBytes(32))
    expect(opened).toEqual(['r'])
    const refusing = new DialKeys(file, { fs: make(true) })
    refusing.set(B, randomBytes(32))
    expect(opened).toHaveLength(2)
    expect(new DialKeys(file).ids().sort()).toEqual([A, B])
  })
})
