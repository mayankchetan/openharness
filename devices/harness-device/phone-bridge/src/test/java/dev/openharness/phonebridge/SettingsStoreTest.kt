package dev.openharness.phonebridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Stands in for SharedPreferences. */
class FakeKv : KeyValueStore {
    val map = mutableMapOf<String, String>()
    override fun get(key: String): String? = map[key]
    override fun put(key: String, value: String?) {
        if (value == null) map.remove(key) else map[key] = value
    }
}

/** Stands in for the Keystore AES-GCM key: reversible, and the stored form is not the plain text. */
class FakeCipher(var broken: Boolean = false) : SecretCipher {
    override fun encrypt(plain: ByteArray): ByteArray = "enc:".toByteArray() + plain.reversedArray()
    override fun decrypt(blob: ByteArray): ByteArray {
        if (broken) throw java.security.GeneralSecurityException("key gone")
        require(String(blob, Charsets.ISO_8859_1).startsWith("enc:"))
        return blob.copyOfRange(4, blob.size).reversedArray()
    }
}

class SettingsStoreTest {

    private val kv = FakeKv()
    private val store = SettingsStore(kv, FakeCipher())

    @Test
    fun defaultRelayUrlIsTheDialHost() {
        assertEquals("wss://dial.mayankchetan.com/relay", SettingsStore.DEFAULT_RELAY_URL)
        val s = store.load()
        assertEquals(SettingsStore.DEFAULT_RELAY_URL, s.relayUrl)
        assertEquals("", s.clientId)
        assertFalse(s.hasToken)
    }

    @Test
    fun saveThenLoadReturnsTheSameValues() {
        val r = store.save("wss://dial.example.org/relay", "abc123.access", "s3cr3t-value")
        assertNull(r)
        val s = store.load()
        assertEquals("wss://dial.example.org/relay", s.relayUrl)
        assertEquals("abc123.access", s.clientId)
        assertEquals("s3cr3t-value", s.secret.reveal())
        assertTrue(s.hasToken)
    }

    @Test
    fun theStoredSecretIsEncryptedAndNeverPlain() {
        store.save(SettingsStore.DEFAULT_RELAY_URL, "id", "s3cr3t-value")
        assertTrue("something is stored", kv.map.isNotEmpty())
        kv.map.values.forEach { assertFalse("plain secret in storage: $it", it.contains("s3cr3t-value")) }
        assertFalse("reversed plain text in storage", kv.map.values.any { it.contains("eulav-t3rc3s") })
    }

    @Test
    fun saveWithABlankSecretKeepsTheOldOne() {
        store.save(SettingsStore.DEFAULT_RELAY_URL, "id", "first-secret")
        assertNull(store.save("wss://dial.example.org/relay", "id2", ""))
        val s = store.load()
        assertEquals("first-secret", s.secret.reveal())
        assertEquals("id2", s.clientId)
    }

    @Test
    fun clearTokenRemovesIdAndSecret() {
        store.save(SettingsStore.DEFAULT_RELAY_URL, "id", "first-secret")
        store.clearToken()
        val s = store.load()
        assertFalse(s.hasToken)
        assertEquals("", s.clientId)
        assertEquals(SettingsStore.DEFAULT_RELAY_URL, s.relayUrl)
    }

    @Test
    fun aLostKeystoreKeyReadsAsNoToken() {
        val cipher = FakeCipher()
        val st = SettingsStore(kv, cipher)
        st.save(SettingsStore.DEFAULT_RELAY_URL, "id", "first-secret")
        cipher.broken = true
        val s = st.load()
        assertFalse(s.hasToken)
        assertEquals("", s.secret.reveal())
    }

    @Test
    fun relayUrlRules() {
        val ok = listOf("wss://dial.example.org/relay", "wss://h.example.org:8443/relay")
        ok.forEach { assertNull(it, SettingsStore.validateRelayUrl(it, false)) }
        val bad = listOf(
            "", "https://x.example.org/relay", "ws://dial.example.org/relay", "wss://",
            "wss://user:pw@dial.example.org/relay", "wss://dial.example.org/relay?id=1&secret=2",
            "wss://dial.example.org/relay#frag", "not a url",
        )
        bad.forEach { assertNotNull(it, SettingsStore.validateRelayUrl(it, false)) }
    }

    @Test
    fun cleartextIsAllowedOnlyForLoopbackAndOnlyWhenAsked() {
        val u = "ws://127.0.0.1:18481/relay"
        assertNull(SettingsStore.validateRelayUrl(u, true))
        assertNotNull(SettingsStore.validateRelayUrl(u, false))
        assertNotNull(SettingsStore.validateRelayUrl("ws://192.168.1.5:18481/relay", true))
    }

    @Test
    fun aBadUrlOrBadHeaderValueIsRefusedAndNothingIsStored() {
        assertNotNull(store.save("https://x.example.org/relay", "id", "secret"))
        assertNotNull(store.save(SettingsStore.DEFAULT_RELAY_URL, "id", "bad\nsecret"))
        assertNotNull(store.save(SettingsStore.DEFAULT_RELAY_URL, "id with space", "secret"))
        assertTrue(kv.map.isEmpty())
    }

    @Test
    fun theRefusalTextNeverHoldsTheSecretOrTheUrl() {
        val msg = store.save("wss://user:topsecret@dial.example.org/relay?k=topsecret", "id", "topsecret")!!
        assertFalse(msg, msg.contains("topsecret"))
        val msg2 = store.save(SettingsStore.DEFAULT_RELAY_URL, "id", "bad\nsecret-body")!!
        assertFalse(msg2, msg2.contains("secret-body"))
    }

    @Test
    fun headersCarryTheTwoAccessFieldsOnlyWhenBothAreSet() {
        store.save(SettingsStore.DEFAULT_RELAY_URL, "the-id", "the-secret")
        assertEquals(
            mapOf("CF-Access-Client-Id" to "the-id", "CF-Access-Client-Secret" to "the-secret"),
            store.load().headers(),
        )
        store.clearToken()
        assertTrue(store.load().headers().isEmpty())
    }

    @Test
    fun aCipherThatThrowsStoresNothingAndKeepsTheOldValues() {
        val cipher = ThrowingCipher()
        val st = SettingsStore(kv, cipher)
        assertNull(st.save("wss://old.example.org/relay", "old-id", "old-secret"))
        val before = kv.map.toMap()
        cipher.failEncrypt = true
        val msg = st.save("wss://new.example.org/relay", "new-id", "new-secret")
        assertNotNull(msg)
        assertFalse(msg!!, msg.contains("new-secret"))
        assertEquals(before, kv.map.toMap())
        cipher.failEncrypt = false
        val s = st.load()
        assertEquals("old-id", s.clientId)
        assertEquals("old-secret", s.secret.reveal())
        assertEquals("wss://old.example.org/relay", s.relayUrl)
    }

    @Test
    fun aSecretWithoutAClientIdIsAnErrorAndNothingIsStored() {
        val msg = store.save(SettingsStore.DEFAULT_RELAY_URL, "", "lonely-secret")
        assertEquals("Client ID is needed with a secret", msg)
        assertTrue(kv.map.isEmpty())
    }

    @Test
    fun aBlankIdAndBlankSecretStillClearsTheToken() {
        store.save(SettingsStore.DEFAULT_RELAY_URL, "id", "first-secret")
        assertNull(store.save(SettingsStore.DEFAULT_RELAY_URL, "", ""))
        assertFalse(store.load().hasToken)
    }

    @Test
    fun theSchemeAndHostAreMatchedWithoutRegardToCase() {
        assertNull(SettingsStore.validateRelayUrl("WSS://dial.example.org/relay", false))
        assertNull(SettingsStore.validateRelayUrl("Wss://Dial.Example.org/relay", false))
        assertNull(SettingsStore.validateRelayUrl("WS://127.0.0.1:18481/relay", true))
        assertNotNull(SettingsStore.validateRelayUrl("WS://127.0.0.1:18481/relay", false))
        assertNotNull(SettingsStore.validateRelayUrl("WS://192.168.1.5/relay", true))
        assertNotNull(SettingsStore.validateRelayUrl("HTTPS://dial.example.org/relay", false))
    }

    @Test
    fun aBlobNeedsRoomForTheIvAndTheTag() {
        assertFalse(GcmBlob.isWellFormed(0))
        assertFalse(GcmBlob.isWellFormed(12))
        assertFalse(GcmBlob.isWellFormed(12 + 15))
        assertTrue(GcmBlob.isWellFormed(12 + 16))
        assertTrue(GcmBlob.isWellFormed(12 + 16 + 20))
    }
}

class ThrowingCipher(var failEncrypt: Boolean = false) : SecretCipher {
    private val inner = FakeCipher()
    override fun encrypt(plain: ByteArray): ByteArray {
        if (failEncrypt) throw java.security.GeneralSecurityException("keystore unavailable")
        return inner.encrypt(plain)
    }
    override fun decrypt(blob: ByteArray): ByteArray = inner.decrypt(blob)
}
