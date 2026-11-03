package dev.openharness.phonebridge

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.util.Base64
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** A one-connection loopback WebSocket server, just enough to see what TunnelLink puts on the wire. */
class TestWsServer : AutoCloseable {
    private val server = ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))
    private val connected = CountDownLatch(1)
    @Volatile private var socket: Socket? = null
    @Volatile var requestHeaders: Map<String, String> = emptyMap()
        private set

    /** The request line, for example `GET /relay HTTP/1.1`. */
    @Volatile var requestLine: String = ""
        private set

    val port: Int get() = server.localPort
    val url: String get() = "ws://127.0.0.1:$port/relay"

    init {
        Thread({
            try {
                val s = server.accept()
                s.soTimeout = 5000
                val input = s.getInputStream()
                val head = ByteArrayOutputStream()
                while (!head.toString(Charsets.ISO_8859_1).endsWith("\r\n\r\n")) {
                    val b = input.read()
                    if (b < 0) throw IOException("eof in handshake")
                    head.write(b)
                }
                val lines = head.toString(Charsets.ISO_8859_1).split("\r\n").filter { it.isNotEmpty() }
                requestLine = lines.first()
                requestHeaders = lines.drop(1).associate {
                    val i = it.indexOf(':')
                    it.substring(0, i).trim().lowercase() to it.substring(i + 1).trim()
                }
                val key = requestHeaders.getValue("sec-websocket-key")
                val accept = Base64.getEncoder().encodeToString(
                    MessageDigest.getInstance("SHA-1").digest((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").toByteArray())
                )
                s.getOutputStream().write(
                    ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
                        "Sec-WebSocket-Accept: $accept\r\n\r\n").toByteArray()
                )
                s.getOutputStream().flush()
                socket = s
                connected.countDown()
            } catch (_: IOException) {
            }
        }, "test-ws-server").apply { isDaemon = true }.start()
    }

    fun awaitConnected(): Boolean = connected.await(5, TimeUnit.SECONDS)

    /** Server-to-client frames are not masked. Payloads here are under 126 bytes. */
    fun send(opcode: Int, payload: ByteArray) {
        require(payload.size < 126)
        val out = socket!!.getOutputStream()
        out.write(0x80 or opcode)
        out.write(payload.size)
        out.write(payload)
        out.flush()
    }

    class Frame(val opcode: Int, val payload: ByteArray)

    /** Reads one client frame (masked). Null at end of stream, reset, or timeout. */
    fun readFrame(): Frame? {
        val input = socket!!.getInputStream()
        return try {
            val b0 = input.read()
            if (b0 < 0) return null
            val b1 = input.read()
            var len = b1 and 0x7F
            if (len == 126) len = (input.read() shl 8) or input.read()
            val mask = ByteArray(4)
            readFully(input, mask)
            val data = ByteArray(len)
            readFully(input, data)
            for (i in data.indices) data[i] = (data[i].toInt() xor mask[i % 4].toInt()).toByte()
            Frame(b0 and 0x0F, data)
        } catch (_: IOException) {
            null
        }
    }

    private fun readFully(input: InputStream, buf: ByteArray) {
        var n = 0
        while (n < buf.size) {
            val r = input.read(buf, n, buf.size - n)
            if (r < 0) throw IOException("eof")
            n += r
        }
    }

    override fun close() {
        try { socket?.close() } catch (_: IOException) {}
        try { server.close() } catch (_: IOException) {}
    }
}
