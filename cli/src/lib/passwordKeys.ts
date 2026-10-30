/**
 * What one keypress does at a masked password prompt (cli.ts promptPassword). Pure, so that it can be tested without a terminal.
 *
 * A character is a code point, not a UTF-16 unit: an emoji or a letter outside the Basic Multilingual Plane arrives as one keypress
 * of two units, and counting units would drop it silently. Erasing takes the whole code point off for the same reason.
 */
export interface KeyPress {
  name?: string
  ctrl?: boolean
  meta?: boolean
}

export type KeyAction =
  | { kind: 'append'; char: string }
  | { kind: 'erase' }
  | { kind: 'submit' }
  | { kind: 'abort' }
  | { kind: 'ignore' }

export function passwordKey(str: string | undefined, key: KeyPress): KeyAction {
  if (key.ctrl && (key.name === 'c' || key.name === 'd')) return { kind: 'abort' }
  if (key.name === 'return' || key.name === 'enter') return { kind: 'submit' }
  if (key.name === 'backspace') return { kind: 'erase' }
  // Anything else that is not one printable character (arrows, tab, escape, function keys, other ctrl or meta combinations) is
  // ignored, rather than risking its raw bytes landing in the password.
  if (str && !key.ctrl && !key.meta && [...str].length === 1 && (str.codePointAt(0) ?? 0) >= 0x20) return { kind: 'append', char: str }
  return { kind: 'ignore' }
}

/** `value` without its last character (code point). */
export const eraseLast = (value: string): string => [...value].slice(0, -1).join('')
