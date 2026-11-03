package dev.openharness.phonebridge

import android.app.Activity
import android.os.Bundle
import android.text.InputType
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView

/**
 * Relay URL and the Cloudflare Access service token. The secret field is never filled in from storage: it shows only
 * whether a token is saved. Leave it empty to keep the saved secret.
 */
class SettingsActivity : Activity() {

    private lateinit var url: EditText
    private lateinit var clientId: EditText
    private lateinit var secret: EditText
    private lateinit var message: TextView
    private val store by lazy { settingsStore(this) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 2, pad, pad)
        }
        fun label(t: String) = TextView(this).apply { text = t }
        url = EditText(this).apply { inputType = InputType.TYPE_TEXT_VARIATION_URI or InputType.TYPE_CLASS_TEXT }
        clientId = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        }
        secret = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        message = TextView(this)
        val save = Button(this).apply {
            text = "Save"
            setOnClickListener { save() }
        }
        val clear = Button(this).apply {
            text = "Remove token"
            setOnClickListener {
                store.clearToken()
                show("Token removed.")
            }
        }
        val width = ViewGroup.LayoutParams.MATCH_PARENT
        val wrap = ViewGroup.LayoutParams.WRAP_CONTENT
        val views = listOf(
            label("Relay URL"), url,
            label("Cloudflare Access Client ID"), clientId,
            label("Client Secret"), secret,
            message, save, clear,
        )
        views.forEach { column.addView(it, ViewGroup.LayoutParams(width, wrap)) }
        setContentView(column)
        show(null)
    }

    private fun show(note: String?) {
        val s = store.load()
        url.setText(s.relayUrl)
        clientId.setText(s.clientId)
        secret.setText("")
        secret.hint = if (s.secret.isEmpty()) "not set" else "saved (leave empty to keep)"
        message.text = note.orEmpty()
    }

    private fun save() {
        val error = store.save(url.text.toString(), clientId.text.toString(), secret.text.toString())
        if (error != null) message.text = error else show("Saved. Restart the bridge to use it.")
    }
}
