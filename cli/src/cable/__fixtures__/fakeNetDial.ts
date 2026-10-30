// A dial on the Wi-Fi link, for tests: a TLS-PSK client (pskClient.ts) that speaks cable frames the way the firmware does.
// It sends `hello` and reads what the daemon says back. Test code only; nothing here listens.
import type * as tls from 'node:tls'

import { CableDecoder, CableType, encodeCableFrame } from '../cableFrame.js'
import { pskConnect } from './pskClient.js'

export interface FakeNetDialOptions {
  port: number
  /** The dial's MAC string, which is also its TLS-PSK identity. */
  id: string
  psk: Buffer
  host?: string
  localAddress?: string
  fw?: string
}

export class FakeNetDial {
  /** Every JSON message the daemon sent, in order. */
  readonly received: Array<Record<string, unknown>> = []
  closed = false
  private readonly decoder = new CableDecoder()

  private constructor(readonly socket: tls.TLSSocket, readonly options: FakeNetDialOptions) {
    socket.on('data', (chunk: Buffer) => {
      this.decoder.feed(chunk, (frame) => {
        if (frame.type === CableType.Json) this.received.push(JSON.parse(Buffer.from(frame.payload).toString()))
      })
    })
    socket.on('error', () => {})
    socket.once('close', () => {
      this.closed = true
    })
  }

  /** Connect and finish the TLS handshake; rejects with the TLS error when the daemon refuses it. */
  static async connect(options: FakeNetDialOptions): Promise<FakeNetDial> {
    const socket = await pskConnect({ port: options.port, identity: options.id, psk: options.psk, host: options.host, localAddress: options.localAddress })
    return new FakeNetDial(socket, options)
  }

  say(message: Record<string, unknown>): void {
    this.socket.write(encodeCableFrame(CableType.Json, Buffer.from(JSON.stringify(message))))
  }

  hello(): void {
    this.say({ t: 'hello', product: 'harness', mac: this.options.id, fw: this.options.fw ?? 'fixture', proto: 3 })
  }

  /** The first message of this type the daemon sent, once it has. */
  async next(type: string, timeoutMs = 3000): Promise<Record<string, unknown>> {
    const deadline = Date.now() + timeoutMs
    for (;;) {
      const found = this.received.find((m) => m.t === type)
      if (found) return found
      if (Date.now() > deadline) throw new Error(`no ${type} within ${timeoutMs} ms`)
      await new Promise((resolve) => setTimeout(resolve, 10))
    }
  }

  close(): void {
    this.socket.destroy()
  }
}
