import { describe, expect, it } from 'vitest'

import { eraseLast, passwordKey } from './passwordKeys.js'

describe('a keypress at the masked password prompt', () => {
  it('appends one printable character', () => {
    expect(passwordKey('a', { name: 'a' })).toEqual({ kind: 'append', char: 'a' })
    expect(passwordKey(' ', { name: 'space' })).toEqual({ kind: 'append', char: ' ' })
    expect(passwordKey('é', {})).toEqual({ kind: 'append', char: 'é' })
  })

  it('appends a character outside the Basic Multilingual Plane, which arrives as two UTF-16 units', () => {
    const emoji = '\u{1F600}'
    expect(emoji.length).toBe(2)
    expect(passwordKey(emoji, {})).toEqual({ kind: 'append', char: emoji })
    expect(passwordKey('\u{20BB7}', {})).toEqual({ kind: 'append', char: '\u{20BB7}' })
  })

  it('builds a password with an emoji in it, and erases the emoji whole', () => {
    let value = ''
    for (const char of ['p', 'a', '\u{1F600}', 's']) {
      const action = passwordKey(char, {})
      if (action.kind === 'append') value += action.char
    }
    expect(value).toBe('pa\u{1F600}s')
    expect(eraseLast(value)).toBe('pa\u{1F600}')
    expect(eraseLast(eraseLast(value))).toBe('pa')
    expect(eraseLast('')).toBe('')
  })

  it('ignores what is not one printable character', () => {
    for (const [str, key] of [
      [undefined, { name: 'up' }], ['', {}], ['ab', {}], ['\t', { name: 'tab' }], ['\u001b', { name: 'escape' }],
      ['a', { ctrl: true, name: 'a' }], ['a', { meta: true, name: 'a' }], ['\u{1F600}\u{1F600}', {}],
    ] as const) expect(passwordKey(str, key)).toEqual({ kind: 'ignore' })
  })

  it('submits, erases and aborts', () => {
    expect(passwordKey('\r', { name: 'return' })).toEqual({ kind: 'submit' })
    expect(passwordKey('\n', { name: 'enter' })).toEqual({ kind: 'submit' })
    expect(passwordKey('\b', { name: 'backspace' })).toEqual({ kind: 'erase' })
    expect(passwordKey('c', { ctrl: true, name: 'c' })).toEqual({ kind: 'abort' })
    expect(passwordKey('d', { ctrl: true, name: 'd' })).toEqual({ kind: 'abort' })
  })
})
