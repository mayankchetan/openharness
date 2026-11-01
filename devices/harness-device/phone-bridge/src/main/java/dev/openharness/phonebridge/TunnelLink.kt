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
 * WebSocket to the daemon relay endpoint. Binary messages only; a text message closes the tunnel.
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
        val request = Request.Builder().url(url).apply { headers.forEach { (k, v) -> addHeader(k, v) } }.build()
        val ws = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) = events.onOpen()

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) = events.onBinary(bytes.toByteArray())

            override fun onMessage(webSocket: WebSocket, text: String) {
                webSocket.close(CLOSE_UNSUPPORTED_DATA, null)
                events.onFailure(null, IOException("text message on a binary tunnel"))
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(CLOSE_NORMAL, null)
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
        const val CLOSE_UNSUPPORTED_DATA = 1003

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
