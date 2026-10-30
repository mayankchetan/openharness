// The daemon end of the dial's TLS-PSK link (docs/specs/cable-wifi-link.md section 4.2; task W7 re-cut as B2).
//
// The dial is the TLS client. It sends its MAC string as the PSK identity and proves it holds the 32-byte key.
// This module only builds the server side: the options, the identity and key lookup, and a wrapper for a socket
// that already exists. It knows no keystore (W8 injects one), opens no port and logs nothing.
//
// Suites. TLS 1.2 only, and only the two ECDHE-PSK CBC suites the dial offers (mbedTLS has no ECDHE-PSK GCM suite;
// interop result 0xC037 in the W0 report). No plain-PSK suite: it has no forward secrecy. No certificate suite: the
// link has no certificate. TLS 1.3 is refused by the version pin. Node leaves PSK suites off unless `ciphers` names
// them, so the list below is also what turns them on.
//
// Unknown identity. Answering "unknown_psk_identity" would tell a scanner which MACs are provisioned. The callback
// returns 32 random bytes instead, so an unknown identity fails exactly like a wrong key (decrypt_error after the
// client Finished). A lookup that throws, or returns a key of the wrong length, is treated the same way. The error
// is swallowed on purpose: nothing from the keystore, and never the key, may reach a log or an exception here.
import { randomBytes } from 'node:crypto'
import type { Duplex } from 'node:stream'
import * as tls from 'node:tls'

/** PSK length in bytes: one random key per dial. */
export const PSK_LENGTH = 32

/** The OpenSSL names of TLS-ECDHE-PSK-WITH-AES-128-CBC-SHA256 (0xC037) and -AES-256-CBC-SHA384 (0xC038), in that order. */
export const PSK_CIPHER_LIST = 'ECDHE-PSK-AES128-CBC-SHA256:ECDHE-PSK-AES256-CBC-SHA384'

/** Key for an identity, or undefined when none is provisioned. Must be a pure read: this module calls it once per handshake. */
export type PskLookup = (identity: string) => Buffer | undefined

const identities = new WeakMap<object, string>()

/** The dial MAC string is upper case on the wire; a client that sends it in lower case is the same dial. */
export function normalizePskIdentity(identity: string): string {
  return identity.toUpperCase()
}

/**
 * The identity a socket authenticated as. Meaningful once the handshake has finished: the callback records the
 * identity when the key is found, and a wrong key still fails the handshake before any data flows.
 */
export function pskIdentityOf(socket: tls.TLSSocket): string | undefined {
  return identities.get(socket)
}

function keyFor(socket: tls.TLSSocket, identity: string, lookup: PskLookup): Buffer {
  const id = normalizePskIdentity(identity)
  let key: Buffer | undefined
  try {
    key = lookup(id)
  } catch {
    key = undefined
  }
  if (!key || key.length !== PSK_LENGTH) return randomBytes(PSK_LENGTH)
  identities.set(socket, id)
  return key
}

/** `tls.createServer` / `new tls.TLSSocket` options for the dial link. */
export function pskServerOptions(lookup: PskLookup): tls.TlsOptions {
  return {
    ciphers: PSK_CIPHER_LIST,
    minVersion: 'TLSv1.2',
    maxVersion: 'TLSv1.2',
    // The suite list wins over the client's order, so the dial's first choice is always the one used.
    honorCipherOrder: true,
    pskCallback: (socket, identity) => keyFor(socket, identity, lookup),
  }
}

/** A TLS server on the dial link. `onSecure` gets each socket whose handshake finished (so the key was right). */
export function createPskServer(lookup: PskLookup, onSecure: (socket: tls.TLSSocket) => void): tls.Server {
  return tls.createServer(pskServerOptions(lookup), onSecure)
}

/**
 * Serve TLS-PSK on a stream that already exists: a net.Socket accepted by the caller, or the duplex of a WebSocket
 * tunnel. The returned socket emits 'secure' when the handshake finishes and 'error' when it fails.
 */
export function wrapPskSocket(stream: Duplex, lookup: PskLookup): tls.TLSSocket {
  return new tls.TLSSocket(stream, { ...pskServerOptions(lookup), isServer: true })
}
