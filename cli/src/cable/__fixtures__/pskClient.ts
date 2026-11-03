// Test client for the TLS-PSK link (tlsPsk.ts). Node `tls.connect` with a client `pskCallback`, the way the dial's
// mbedTLS client behaves: it sends `identity` and proves it holds `psk`. Test code only; the daemon never dials out.
import type { Duplex } from 'node:stream'
import * as tls from 'node:tls'

export interface PskClientOptions {
  port: number
  host?: string
  /** Source address of the connection: 127.0.0.2 and up are other "hosts" on a loopback test. */
  localAddress?: string
  identity: string
  psk: Buffer
  /** OpenSSL cipher list; default is the one suite the dial offers first. */
  ciphers?: string
  minVersion?: tls.SecureVersion
  maxVersion?: tls.SecureVersion
  /** Run the handshake over this already connected socket (a dial that is slow to start TLS). */
  socket?: Duplex
  /** Offer this session for resumption. */
  session?: Buffer
  /** Leave the client without a PSK callback (offers no PSK suite). */
  noPsk?: boolean
}

/** Connect and resolve on the finished handshake; reject with the TLS error otherwise. */
export function pskConnect(opts: PskClientOptions): Promise<tls.TLSSocket> {
  return new Promise((resolve, reject) => {
    const options: tls.ConnectionOptions = {
      host: opts.host ?? '127.0.0.1',
      port: opts.port,
      ...(opts.localAddress ? { localAddress: opts.localAddress } : {}),
      minVersion: opts.minVersion ?? 'TLSv1.2',
      maxVersion: opts.maxVersion ?? 'TLSv1.2',
      // No certificate exists on a PSK link, so there is nothing for the default hostname check to look at.
      checkServerIdentity: () => undefined,
    }
    // An explicit `ciphers: undefined` keeps the Node default list (a TLS 1.3 client needs its own suites).
    if ('ciphers' in opts) options.ciphers = opts.ciphers
    else options.ciphers = 'ECDHE-PSK-AES128-CBC-SHA256'
    if (opts.socket) options.socket = opts.socket
    if (opts.session) options.session = opts.session
    if (!opts.noPsk) options.pskCallback = () => ({ psk: opts.psk, identity: opts.identity })
    const socket = tls.connect(options)
    socket.once('secureConnect', () => {
      socket.removeListener('error', reject)
      resolve(socket)
    })
    socket.once('error', reject)
  })
}
