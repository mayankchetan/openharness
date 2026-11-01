// Test fixture: two cross-wired duplexes, the shape of a WebSocket stream handed to NetDials.acceptStream. Nothing listens.
import { Duplex } from 'node:stream'

/**
 * Two cross-wired duplexes: what one writes the other reads. Destroying one gives the other what was written before, then
 * the end of the stream, and the other then ends its own side and closes (as a TCP socket without half-open does).
 */
export function duplexPair(): [Duplex, Duplex] {
  const make = (other: () => Duplex): Duplex =>
    new Duplex({
      allowHalfOpen: false,
      read() {},
      write(chunk, _enc, cb) {
        const o = other()
        if (!o.destroyed) o.push(chunk)
        cb()
      },
      final(cb) {
        const o = other()
        if (!o.destroyed) o.push(null)
        cb()
      },
      destroy(error, cb) {
        const o = other()
        if (!o.destroyed) o.push(null)
        cb(error)
      },
    })
  const a: Duplex = make(() => b)
  const b: Duplex = make(() => a)
  a.on('error', () => {})
  b.on('error', () => {})
  return [a, b]
}
