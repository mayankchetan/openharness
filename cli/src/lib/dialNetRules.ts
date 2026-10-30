// The rules for the Wi-Fi fields of a dial (docs/specs/cable-wifi-link.md section 5.1 step 4), in one place that the
// core's HTTP half (dialNetHttp.ts), the devices' provisioning (cable/netProvision.ts) and the CLI all read. The firmware
// applies the same rules in config_net_check (config_store.c); a change to one is a change to the other.
//
// A failed check names the FIELD and never the value: a caller's SSID or password must not come back in an error body,
// a log line or a terminal.

export const NET_SSID_MAX = 32
export const NET_PASS_MIN = 8
export const NET_PASS_MAX = 63
export const NET_PORT_MIN = 1024
export const NET_PORT_MAX = 65535
export const NET_KEY_BYTES = 32
/** The fields a caller can get wrong. */
export type NetField = 'dial' | 'ssid' | 'pass' | 'host' | 'port' | 'mode'

/** What is said of each field that fails. Fixed text: nothing in it comes from the request. */
export const NET_FIELD_MESSAGES: Record<NetField, string> = {
  dial: 'dial must be a MAC address or serial number of at most 64 characters.',
  ssid: 'ssid must be 1 to 32 bytes with no control characters.',
  pass: 'pass must be empty (an open network) or 8 to 63 bytes with no control characters.',
  host: 'host must be a dotted IPv4 address, like 192.168.1.20.',
  port: 'port must be a whole number from 1024 to 65535.',
  mode: 'mode must be 0 or 1.',
}

const CONTROL = /[\u0000-\u001f\u007f]/
const IPV4 = /^(?:0|[1-9]\d{0,2})(?:\.(?:0|[1-9]\d{0,2})){3}$/

const text = (value: unknown, min: number, max: number): value is string =>
  typeof value === 'string' && !CONTROL.test(value) && Buffer.byteLength(value, 'utf8') >= min && Buffer.byteLength(value, 'utf8') <= max

export const validSsid = (value: unknown): value is string => text(value, 1, NET_SSID_MAX)

export const validPass = (value: unknown): value is string =>
  typeof value === 'string' && (value === '' || text(value, NET_PASS_MIN, NET_PASS_MAX))

/**
 * A dotted IPv4 the daemon could be at. Not 0.0.0.0, 255.255.255.255, loopback (127/8) or multicast (224/4): none of them is a computer a
 * dial can connect to. The firmware applies the same rule (config_store.c host_unusable).
 */
export const validHost = (value: unknown): value is string => {
  if (typeof value !== 'string' || !IPV4.test(value)) return false
  const octets = value.split('.').map(Number)
  if (octets.some((octet) => octet > 255)) return false
  const [a, b, c, d] = octets
  if (a === 0 && b === 0 && c === 0 && d === 0) return false
  if (a === 255 && b === 255 && c === 255 && d === 255) return false
  return a !== 127 && !(a >= 224 && a <= 239)
}

/** A key of the right size that is not all zero: an uninitialised buffer, not a key. The firmware refuses the same (config_net_check_key). */
export const validKeyBytes = (value: Uint8Array): boolean => value.length === NET_KEY_BYTES && value.some((byte) => byte !== 0)

/** RFC 1918 only. Loopback would send the dial to itself; link-local and carrier-grade NAT are not a home network. */
export function isLanIPv4(value: string): boolean {
  if (!validHost(value)) return false
  const [a, b] = value.split('.').map(Number)
  return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168)
}

export const validPort = (value: unknown): value is number =>
  typeof value === 'number' && Number.isInteger(value) && value >= NET_PORT_MIN && value <= NET_PORT_MAX

export const validMode = (value: unknown): value is 0 | 1 => value === 0 || value === 1

export const validDial = (value: unknown): value is string =>
  typeof value === 'string' && value.length >= 1 && value.length <= 64 && !CONTROL.test(value)
