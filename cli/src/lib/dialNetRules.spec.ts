import { describe, expect, it } from 'vitest'

import { NET_FIELD_MESSAGES, isLanIPv4, validDial, validHost, validMode, validPass, validPort, validSsid } from './dialNetRules.js'

describe('the rules for a dial\'s Wi-Fi fields (the same ones the firmware applies in config_net_check)', () => {
  it('an SSID is 1 to 32 bytes with no control characters', () => {
    expect(validSsid('home')).toBe(true)
    expect(validSsid('a'.repeat(32))).toBe(true)
    expect(validSsid('a'.repeat(33))).toBe(false)
    expect(validSsid('')).toBe(false)
    // bytes, not characters: each of these is 2 bytes, so 16 fit and 17 do not
    expect(validSsid('é'.repeat(16))).toBe(true)
    expect(validSsid('é'.repeat(17))).toBe(false)
    expect(validSsid('a\nb')).toBe(false)
    expect(validSsid('a\u0000b')).toBe(false)
    expect(validSsid(42)).toBe(false)
    expect(validSsid(undefined)).toBe(false)
  })

  it('a password is empty (an open network) or 8 to 63 bytes with no control characters', () => {
    expect(validPass('')).toBe(true)
    expect(validPass('12345678')).toBe(true)
    expect(validPass('x'.repeat(63))).toBe(true)
    expect(validPass('x'.repeat(64))).toBe(false)
    expect(validPass('1234567')).toBe(false)
    expect(validPass('1234567\u007f')).toBe(false)
    expect(validPass('pass word ok')).toBe(true)
    expect(validPass('tab\there')).toBe(false)
    expect(validPass(null)).toBe(false)
    expect(validPass(12345678)).toBe(false)
  })

  it('a host is a dotted IPv4 address, strictly', () => {
    for (const ok of ['192.168.1.20', '10.0.0.1', '172.16.5.5', '1.2.3.4', '0.0.0.1']) expect(validHost(ok), ok).toBe(true)
    for (const bad of ['', 'host.local', '192.168.1', '192.168.1.256', '192.168.01.1', '192.168.1.1.1', ' 192.168.1.1', '1.2.3.4 ', '::1', '1.2.3.-4', 'a.b.c.d', '0x7f.0.0.1']) expect(validHost(bad), bad).toBe(false)
    expect(validHost(1234)).toBe(false)
  })

  it('only a private LAN address (RFC 1918) can be given to a dial as the daemon\'s address', () => {
    for (const ok of ['10.1.2.3', '172.16.0.1', '172.31.255.254', '192.168.0.9']) expect(isLanIPv4(ok), ok).toBe(true)
    for (const no of ['8.8.8.8', '127.0.0.1', '169.254.1.1', '172.15.0.1', '172.32.0.1', '100.64.0.1', '192.169.0.1', '0.0.0.0', '255.255.255.255', 'x']) expect(isLanIPv4(no), no).toBe(false)
  })

  it('a port is a whole number from 1024 to 65535', () => {
    expect(validPort(18480)).toBe(true)
    expect(validPort(1024)).toBe(true)
    expect(validPort(65535)).toBe(true)
    for (const bad of [1023, 65536, 0, -1, 1.5, NaN, '18480', null]) expect(validPort(bad), String(bad)).toBe(false)
  })

  it('a mode is 0 or 1', () => {
    expect(validMode(0)).toBe(true)
    expect(validMode(1)).toBe(true)
    for (const bad of [2, -1, '1', true, null, 0.5]) expect(validMode(bad), String(bad)).toBe(false)
  })

  it('a dial is named by a MAC or a serial of at most 64 characters with no control characters', () => {
    expect(validDial('B8:1F:3F:CA:E5:58')).toBe(true)
    expect(validDial('serial-1')).toBe(true)
    expect(validDial('')).toBe(false)
    expect(validDial('x'.repeat(65))).toBe(false)
    expect(validDial('a\nb')).toBe(false)
    expect(validDial(5)).toBe(false)
  })

  it('every field has a fixed message that names the field and nothing a caller sent', () => {
    for (const [field, message] of Object.entries(NET_FIELD_MESSAGES)) {
      expect(message.startsWith(field), field).toBe(true)
      expect(message).not.toMatch(/\$\{/)
    }
  })
})
