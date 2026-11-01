// The keepalive timing rule of the cable link (task W13; docs/specs/cable-wifi-link.md "Keepalive"). The dial's twin is
// devices/harness-device/firmware/main/cable_keepalive.h, and both read devices/harness-device/firmware/test/vectors/keepalive.txt.
//
// Today the daemon pings every 5 s, the dial calls 15 s of silence a dead daemon, and the daemon calls 20 s of silence a dead dial.
// An optional `welcome.keepaliveMs` (K) moves the ping to K and scales both windows from that one number: the dial waits 3 x K, the
// daemon 3 x K + 5 s. With no field, K is 5 s and the three numbers are the old ones. K is clamped to 5 s .. 60 s, so the setting can
// only slow the ping down.
//
// The daemon's window stays the later one (the old 20 s against 15 s): one late message must not trip both sides at once and have
// each conclude the other left.

export const KEEPALIVE_MIN_MS = 5_000
export const KEEPALIVE_MAX_MS = 60_000
/** The ping this link has always had. */
export const DEFAULT_PING_MS = KEEPALIVE_MIN_MS
const SILENCE_FACTOR = 3
const DAEMON_EXTRA_MS = 5_000

/** K for a welcome field: a finite number, rounded and clamped; anything else (absent, text, NaN) is the default. */
export function effectiveKeepaliveMs(raw: unknown): number {
  if (typeof raw !== 'number' || Number.isNaN(raw)) return DEFAULT_PING_MS
  if (raw <= KEEPALIVE_MIN_MS) return KEEPALIVE_MIN_MS
  if (raw >= KEEPALIVE_MAX_MS) return KEEPALIVE_MAX_MS
  return Math.round(raw)
}

/** How long the dial waits in silence before it calls the daemon dead. */
export const dialSilenceMs = (keepaliveMs: number): number => SILENCE_FACTOR * keepaliveMs

/** How long the daemon waits in silence before it calls the dial dead: always later than the dial's window. */
export const daemonSilenceMs = (keepaliveMs: number): number => SILENCE_FACTOR * keepaliveMs + DAEMON_EXTRA_MS

export const DEFAULT_DIAL_SILENCE_MS = dialSilenceMs(DEFAULT_PING_MS)
export const DEFAULT_DAEMON_SILENCE_MS = daemonSilenceMs(DEFAULT_PING_MS)

/**
 * What the host's setting asks for: `undefined` (off) for 0, a negative, NaN or no setting. Anything else is clamped, so a typo in
 * the environment can slow the ping but never remove it.
 */
export function configuredKeepaliveMs(setting: number | undefined): number | undefined {
  if (setting === undefined || !Number.isFinite(setting) || setting <= 0) return undefined
  return effectiveKeepaliveMs(setting)
}
