package dev.openharness.phonebridge

import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * WebSocket to the daemon relay endpoint. Binary messages only; a text message cancels the socket.
 *
 * `headers` is where the Access service token goes (B10c). This class never logs the URL, the headers or any
 * exception message: OkHttp messages can name the host, so callers get the exception class only.
 */
class TunnelLink(
    private val url: String,
    private val headers: Map<String, String> = emptyMap(),
    private val client: OkHttpClient = defaultClient(),
) : TunnelOpener {

    override fun open(events: TunnelEvents): TunnelChannel {
        // OkHttp names the offending header value in its exception message. Rethrow with no message and no cause.
        val request = try {
            Request.Builder().url(url).apply { headers.forEach { (k, v) -> addHeader(k, v) } }.build()
        } catch (_: IllegalArgumentException) {
            throw IllegalArgumentException("invalid tunnel URL or header")
        }
        val ws = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) = events.onOpen()

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) = events.onBinary(bytes.toByteArray())

            override fun onMessage(webSocket: WebSocket, text: String) {
                // No close handshake: the stream is already wrong, so drop the socket.
                webSocket.cancel()
                events.onFailure(null, IOException("text message on a binary tunnel"))
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(closeEchoCode(code), null)
                events.onClosed(code)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) = events.onClosed(code)

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) =
                events.onFailure(response?.code, t)
        })
        return object : TunnelChannel {
            override fun send(bytes: ByteArray): Boolean = ws.send(bytes.toByteString())
            override fun queueSize(): Long = ws.queueSize()
            override fun close(code: Int) {
                ws.close(code, null)
            }
        }
    }

    companion object {
        const val CLOSE_NORMAL = 1000

        /** The link the settings describe. `urlOverride` is for tests that talk to a loopback server. */
        fun fromSettings(
            settings: TunnelSettings,
            client: OkHttpClient = defaultClient(),
            urlOverride: String? = null,
        ): TunnelLink = TunnelLink(urlOverride ?: settings.relayUrl, settings.headers(), client)

        /**
         * The code to answer a peer close with: the peer's own code when a WebSocket may carry it, else 1000.
         * An empty close frame reads as 1005 and OkHttp refuses to send that (or 1004, 1006, 1015, 1016..2999).
         */
        fun closeEchoCode(code: Int): Int =
            if (code in 1000..1003 || code in 1007..1014 || code in 3000..4999) code else CLOSE_NORMAL

        /** One client for the process. Ping every 25 s keeps the tunnel idle timeout away (rule 9). */
        fun defaultClient(): OkHttpClient = shared

        private val shared: OkHttpClient by lazy {
            OkHttpClient.Builder()
                .pingInterval(Wire.WS_PING_MS, TimeUnit.MILLISECONDS)
                .readTimeout(0, TimeUnit.MILLISECONDS)
                .retryOnConnectionFailure(false)
                .build()
        }
    }
}
