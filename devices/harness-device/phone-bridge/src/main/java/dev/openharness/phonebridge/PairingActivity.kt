package dev.openharness.phonebridge

import android.app.Activity
import android.os.Bundle
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView

/** Shows what the phone sees of the dial, and where pairing starts. The phone cannot start pairing itself. */
class PairingActivity : Activity() {

    private lateinit var state: TextView
    private val listener: (BridgeSnapshot) -> Unit = { s -> runOnUiThread { render(s) } }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad * 2, pad, pad)
        }
        state = TextView(this).apply { textSize = 18f }
        val hint = TextView(this).apply {
            text = PairingHint.TEXT
            textSize = 16f
            setPadding(0, pad, 0, 0)
        }
        listOf(state, hint).forEach {
            column.addView(it, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        setContentView(column)
    }

    override fun onResume() {
        super.onResume()
        BridgeState.addListener(listener)
        render(BridgeState.snapshot)
    }

    override fun onPause() {
        BridgeState.removeListener(listener)
        super.onPause()
    }

    private fun render(s: BridgeSnapshot) {
        state.text = PairingHint.stateLine(s.phase)
    }
}
