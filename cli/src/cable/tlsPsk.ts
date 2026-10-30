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
// No resumption, no renegotiation. A session ticket or a session-ID resume would skip the PSK check on a later
// connection, and a renegotiation would run a second handshake mid-stream; neither has a use on this link, so tickets
// and renegotiation are switched off and the session cache never gets an entry.
//
// Unknown identity. Answering "unknown_psk_identity" would tell a scanner which MACs are provisioned. The callback
// returns 32 random bytes instead, so an unknown identity fails exactly like a wrong key (decrypt_error after the
// client Finished). A lookup that throws, returns a key of the wrong length, or an identity that is not a MAC string
// is treated the same way. The error is swallowed on purpose: nothing from the keystore, and never the key, may reach a
// log or an exception here.
import { constants, randomBytes } from 'node:crypto'
import type { Duplex } from 'node:stream'
import * as tls from 'node:tls'

/** PSK length in bytes: one random key per dial. */
export const PSK_LENGTH = 32

/** A peer that has not finished the handshake by now is dropped (the 3-5 s unauthenticated-peer limit of the Wi-Fi spec). */
export const PSK_HANDSHAKE_TIMEOUT_MS = 5000

/** The OpenSSL names of TLS-ECDHE-PSK-WITH-AES-128-CBC-SHA256 (0xC037) and -AES-256-CBC-SHA384 (0xC038), in that order. */
export const PSK_CIPHER_LIST = 'ECDHE-PSK-AES128-CBC-SHA256:ECDHE-PSK-AES256-CBC-SHA384'

/** Key for an identity, or undefined when none is provisioned. Must be a pure read: this module calls it once per handshake. */
export type PskLookup = (identity: string) => Buffer | undefined

const MAC_IDENTITY = /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/

// identity seen by the callback, not yet proven: committed to `identities` only when the handshake finishes
const pending = new WeakMap<object, string>()
const identities = new WeakMap<object, string>()

/** The dial MAC string is upper case on the wire; a client that sends it in lower case is the same dial. ASCII fold only. */
export function normalizePskIdentity(identity: string): string {
  return identity.replace(/[a-f]/g, (c) => c.toUpperCase())
}

/**
 * The identity a socket authenticated as, or undefined. Set when the handshake has finished, so a socket whose key was
 * wrong never has one.
 */
export function pskIdentityOf(socket: tls.TLSSocket): string | undefined {
  return identities.get(socket)
}

function commitIdentity(socket: tls.TLSSocket): void {
  const id = pending.get(socket)
  if (id !== undefined && !identities.has(socket)) identities.set(socket, id)
  pending.delete(socket)
}

function keyFor(socket: tls.TLSSocket, identity: string, lookup: PskLookup): Buffer {
  if (!MAC_IDENTITY.test(identity)) return randomBytes(PSK_LENGTH)
  const id = normalizePskIdentity(identity)
  let key: Buffer | undefined
  try {
    key = lookup(id)
  } catch {
    key = undefined
  }
  if (!key || key.length !== PSK_LENGTH) return randomBytes(PSK_LENGTH)
  if (!identities.has(socket)) pending.set(socket, id)
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
    // No session tickets (they would resume without the PSK) and no renegotiation.
    secureOptions: constants.SSL_OP_NO_TICKET | constants.SSL_OP_NO_RENEGOTIATION,
    pskCallback: (socket, identity) => keyFor(socket, identity, lookup),
  }
}

/** A TLS server on the dial link. `onSecure` gets each socket whose handshake finished (so the key was right). */
export function createPskServer(lookup: PskLookup, onSecure: (socket: tls.TLSSocket) => void): tls.Server {
  const server = tls.createServer({ ...pskServerOptions(lookup), handshakeTimeout: PSK_HANDSHAKE_TIMEOUT_MS }, (socket) => {
    commitIdentity(socket)
    onSecure(socket)
  })
  // Resumption by session ID is off too: the cache is never filled, and a resume request finds nothing.
  // A listener on 'tlsClientError' (ours, or the caller's) stops Node from closing the socket itself: close it here, so a
  // handshake timeout or a bad ClientHello always ends the connection.
  server.on('tlsClientError', (_err, socket) => socket.destroy())
  server.on('newSession', (_id, _data, done) => done())
  server.on('resumeSession', (_id, done) => done(null, null))
  return server
}

/**
 * Serve TLS-PSK on a stream that already exists: a net.Socket accepted by the caller, or the duplex of a WebSocket
 * tunnel. The returned socket emits 'secure' when the handshake finishes and 'error' when it fails; one that has not
 * finished within PSK_HANDSHAKE_TIMEOUT_MS is destroyed (tls.Server does that for its own sockets; here it is a timer).
 */
export function wrapPskSocket(stream: Duplex, lookup: PskLookup): tls.TLSSocket {
  const socket = new tls.TLSSocket(stream, { ...pskServerOptions(lookup), isServer: true })
  const timer = setTimeout(() => socket.destroy(new Error('TLS-PSK handshake timeout')), PSK_HANDSHAKE_TIMEOUT_MS)
  timer.unref()
  const stop = () => clearTimeout(timer)
  socket.once('secure', () => {
    stop()
    commitIdentity(socket)
  })
  socket.once('close', stop)
  return socket
}
