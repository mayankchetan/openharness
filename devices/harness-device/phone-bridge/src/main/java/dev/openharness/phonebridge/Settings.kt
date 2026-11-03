package dev.openharness.phonebridge

import java.net.URI
import java.net.URISyntaxException
import java.util.Base64

/** A string that never prints itself. toString() is fixed, so a log line or a data-class dump cannot leak it. */
class Secret(private val value: String) {
    fun reveal(): String = value
    fun isEmpty(): Boolean = value.isEmpty()
    override fun toString(): String = "***"
}

/** What the tunnel needs: where to connect and the Cloudflare Access service token. */
data class TunnelSettings(val relayUrl: String, val clientId: String, val secret: Secret) {
    val hasToken: Boolean get() = clientId.isNotEmpty() && !secret.isEmpty()

    /**
     * The two Access headers for the WebSocket upgrade, or none if the token is not set. This is the only place the
     * secret leaves the settings. It goes in a header, never in the URL.
     */
    fun headers(): Map<String, String> =
        if (hasToken) mapOf(HEADER_CLIENT_ID to clientId, HEADER_CLIENT_SECRET to secret.reveal()) else emptyMap()

    companion object {
        const val HEADER_CLIENT_ID = "CF-Access-Client-Id"
        const val HEADER_CLIENT_SECRET = "CF-Access-Client-Secret"
    }
}

/** Plain string storage (SharedPreferences on the phone). */
interface KeyValueStore {
    fun get(key: String): String?
    fun put(key: String, value: String?)
}

/** Encrypts the secret at rest. On the phone: AES-GCM with a key that lives in the Android Keystore. */
interface SecretCipher {
    fun encrypt(plain: ByteArray): ByteArray
    fun decrypt(blob: ByteArray): ByteArray
}

/**
 * Relay URL, Access client id (plain) and Access client secret (encrypted). This class never logs. Error texts are
 * fixed strings: they do not echo the URL or the secret back.
 */
class SettingsStore(
    private val kv: KeyValueStore,
    private val cipher: SecretCipher,
    private val defaultUrl: String = DEFAULT_RELAY_URL,
    private val allowCleartextLoopback: Boolean = false,
) {
    fun load(): TunnelSettings {
        val url = kv.get(KEY_URL)?.takeIf { it.isNotEmpty() } ?: defaultUrl
        return TunnelSettings(url, kv.get(KEY_ID).orEmpty(), Secret(readSecret()))
    }

    /**
     * Validates, then stores. Returns null on success or a fixed error text; on an error nothing is stored.
     * A blank `secret` keeps the stored one, so the screen never has to show it.
     */
    fun save(relayUrl: String, clientId: String, secret: String): String? {
        val url = relayUrl.trim()
        val id = clientId.trim()
        val sec = secret.trim()
        validateRelayUrl(url, allowCleartextLoopback)?.let { return it }
        if (id.isNotEmpty() && !isHeaderSafe(id)) return "Client ID: use visible characters only, no spaces."
        if (sec.isNotEmpty() && !isHeaderSafe(sec)) return "Client Secret: use visible characters only, no spaces."
        kv.put(KEY_URL, url)
        kv.put(KEY_ID, id.ifEmpty { null })
        if (sec.isNotEmpty()) kv.put(KEY_SECRET, Base64.getEncoder().encodeToString(cipher.encrypt(sec.toByteArray(Charsets.UTF_8))))
        if (id.isEmpty()) kv.put(KEY_SECRET, null)
        return null
    }

    fun clearToken() {
        kv.put(KEY_ID, null)
        kv.put(KEY_SECRET, null)
    }

    private fun readSecret(): String {
        val enc = kv.get(KEY_SECRET) ?: return ""
        // A key that Android dropped (lock-screen change, restore) cannot decrypt: read as no token, never crash.
        return try {
            String(cipher.decrypt(Base64.getDecoder().decode(enc)), Charsets.UTF_8)
        } catch (_: Exception) {
            ""
        }
    }

    override fun toString(): String = "SettingsStore"

    companion object {
        const val DEFAULT_RELAY_URL = "wss://dial.mayankchetan.com/relay"
        private const val KEY_URL = "relay_url"
        private const val KEY_ID = "client_id"
        private const val KEY_SECRET = "client_secret_enc"
        private const val MAX_FIELD = 512

        /** wss:// with a host, no user info, no query, no fragment. ws:// only to 127.0.0.1, and only if allowed. */
        fun validateRelayUrl(url: String, allowCleartextLoopback: Boolean): String? {
            val bad = "Relay URL: use wss://host/path with no user name, query or fragment."
            val u = try {
                URI(url)
            } catch (_: URISyntaxException) {
                return bad
            }
            val host = u.host
            if (host.isNullOrEmpty() || u.userInfo != null || u.rawQuery != null || u.rawFragment != null) return bad
            return when (u.scheme) {
                "wss" -> null
                "ws" -> if (allowCleartextLoopback && host == "127.0.0.1") null else bad
                else -> bad
            }
        }

        /** Header values must be visible ASCII: OkHttp refuses anything else with a message that includes the value. */
        fun isHeaderSafe(v: String): Boolean = v.length <= MAX_FIELD && v.all { it.code in 0x21..0x7E }
    }
}
