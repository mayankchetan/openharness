// The keepalive timing rule (task W13; keepalive.ts) against the table the dial's host test reads too
// (devices/harness-device/firmware/test/vectors/keepalive.txt, read in place, not copied).
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

import {
  DEFAULT_DAEMON_SILENCE_MS,
  DEFAULT_DIAL_SILENCE_MS,
  DEFAULT_PING_MS,
  KEEPALIVE_MAX_MS,
  KEEPALIVE_MIN_MS,
  daemonSilenceMs,
  dialSilenceMs,
  effectiveKeepaliveMs,
} from './keepalive.js'

const TABLE = join(dirname(fileURLToPath(import.meta.url)), '../../../devices/harness-device/firmware/test/vectors/keepalive.txt')

interface Row { name: string; raw: string; k: number; dial: number; daemon: number }

function load(): Row[] {
  return readFileSync(TABLE, 'utf8').split(/\r?\n/).filter((l) => l.startsWith('case ')).map((l) => {
    const [, name, raw, k, dial, daemon] = l.trim().split(/\s+/)
    return { name: name!, raw: raw!, k: Number(k), dial: Number(dial), daemon: Number(daemon) }
  })
}

/** What the welcome carried: no field, a field that is not a number, or a JSON number. */
const asWelcomeValue = (raw: string): unknown => (raw === 'absent' ? undefined : raw === 'nan' ? 'not a number' : Number(raw))

describe('keepalive timing', () => {
  it("keeps today's timings when keepaliveMs is absent (5 s ping, 15 s dial silence, 20 s daemon silence)", () => {
    expect(DEFAULT_PING_MS).toBe(5_000)
    expect(DEFAULT_DIAL_SILENCE_MS).toBe(15_000)
    expect(DEFAULT_DAEMON_SILENCE_MS).toBe(20_000)
    expect(effectiveKeepaliveMs(undefined)).toBe(5_000)
    expect(dialSilenceMs(effectiveKeepaliveMs(undefined))).toBe(15_000)
    expect(daemonSilenceMs(effectiveKeepaliveMs(undefined))).toBe(20_000)
  })

  it('clamps to 5 s .. 60 s', () => {
    expect([KEEPALIVE_MIN_MS, KEEPALIVE_MAX_MS]).toEqual([5_000, 60_000])
    expect(effectiveKeepaliveMs(4_999)).toBe(5_000)
    expect(effectiveKeepaliveMs(60_001)).toBe(60_000)
    expect(effectiveKeepaliveMs(Infinity)).toBe(60_000)
    expect(effectiveKeepaliveMs(-Infinity)).toBe(5_000)
    for (const junk of [NaN, null, 'x', {}, [], true]) expect(effectiveKeepaliveMs(junk)).toBe(5_000)
  })

  it("scales the dial's silence 1:3 and keeps the daemon's window the later one, for every K", () => {
    for (let k = KEEPALIVE_MIN_MS; k <= KEEPALIVE_MAX_MS; k += 250) {
      expect(effectiveKeepaliveMs(k)).toBe(k)
      expect(dialSilenceMs(k)).toBe(3 * k)
      expect(daemonSilenceMs(k) - dialSilenceMs(k)).toBe(5_000)
      expect(daemonSilenceMs(k)).toBeGreaterThan(dialSilenceMs(k))
    }
  })

  describe("the table the dial's host test reads", () => {
    const rows = load()
    it('has its rows', () => expect(rows.length).toBeGreaterThanOrEqual(12))
    it.each(rows)('$name ($raw)', (row) => {
      const k = effectiveKeepaliveMs(asWelcomeValue(row.raw))
      expect([k, dialSilenceMs(k), daemonSilenceMs(k)]).toEqual([row.k, row.dial, row.daemon])
    })
  })
})
