"""Two things the gallery run on the dials showed (2026-10-28), pinned as source checks; the gallery itself only builds
for the firmware.

1. setup_workspace_preview set the drag flags and then called view(HOME), whose input_cancel() clears them
   (ht_workspace_cancel_touch): the item landed on plain "home" (MISMATCH expected_view=workspace got=home).
2. `RAM tag=gallery_ready` was logged once at boot, before the daemon session, so it never reached a log. It is now
   re-logged right after the session comes up and every 30 s from the cable reader's tick.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parent / '..'
inc = (root / 'main/ui/habitat/ui_gallery.inc').read_text()
client = (root / 'main/cable_client.c').read_text()
app = (root / 'main/app_main.c').read_text()

setup = re.search(r'SETUP\(workspace_preview\)\s*\{(.*?)\n\}', inc, re.S)
assert setup, 'setup_workspace_preview'
body = setup[1]
assert body.index('view(HOME)') < body.index('workspace.touching'), 'the drag flags are set after view(HOME)'

session_up = re.search(r'static void gallery_session_up\(void\)\s*\{(.*?)\n\}', client, re.S)[1]
assert 'cable_link_set_log_framing(true)' in session_up
assert session_up.index('cable_link_set_log_framing(true)') < session_up.index('gallery_ram_report()')
tick = re.search(r'static void session_tick\(void \*ctx\)\s*\{(.*?)\n\}', client, re.S)[1]
assert 'gallery_ram_report()' in tick and '30' in tick and 'DEVICE_UI_GALLERY' in tick
assert re.search(r'void gallery_ram_report\(void\)\s*\{[^}]*ram_telemetry_checkpoint\("gallery_ready"\)', app, re.S)
assert 'GALLERY ui=%d tour_stack_b=%d' in app
print('ui gallery source: drag flags set after view(HOME); gallery_ready re-logged after the session and every 30 s PASS')
