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
# Run 2 (shots-58, tour at b86eec602):
# 3. the reset replaced the inbox while the inbox was still the view, so ui_notif_replace kept the card on screen ("held":
#    read on the dial, with a token) and every later item had one stray notice (inbox_empty showed 1/1). Home first.
reset = re.search(r'static void g_reset\(void\)\s*\{(.*?)\n\}', inc, re.S)[1]
assert reset.index('view(HOME)') < reset.index('g_notices(-1, 0)'), 'reset: leave the inbox before replacing the notices'
# 4. a stuck Lead shows its fixed line only through a Super Tab wait (stuck=true); the reset closes every wait it opened.
stuck = re.search(r'SETUP\(swarm_stuck\)\s*\{(.*?)\n\}', inc, re.S)[1]
assert re.search(r'ui_supertab_wait\(g_id\(3\),[^;]*true', stuck), 'swarm_stuck opens a stuck wait'
assert 'ui_supertab_wait_close(g_id(3)' in reset or 'ui_supertab_wait_close(g_id(3)' in inc
# 5. inbox rows say what they are: a question row carries a question, a failed row a failure.
notices = re.search(r'static void g_notices\(int ask, int others\)\s*\{(.*?)\n\}', inc, re.S)[1]
assert 'r->question ? (k & 1 ? 4 : 2)' in notices and 'r->failed ? (k & 1 ? 7 : 1)' in notices   # G_SUMMARY[2], [4] ask; [1], [7] fail
# 6. the tour compares what the renderer built (uim_item_scene), re-checks the model at shot_ready, and counts only a debug
# command that STARTED after shot_ready (a late shot for the previous item must not end this item's wait).
tour = re.search(r'static void tour_item\(.*?\n\}', inc, re.S)[0]
assert 'uim_item_scene(' in tour and 'MISMATCH_DRAWN' in tour and 'MISMATCH_LATE' in tour
assert 'cable_debug_commands_started()' in tour
assert 'cable_debug_commands_started' in (root / 'main/cable_debug.c').read_text()

print('ui gallery source: drag flags, gallery_ready, reset order, stuck wait, inbox summaries, drawn-scene check PASS')
