#!/usr/bin/env bash
# Host tests for the parts of the firmware that do not need the board.
#
#   devices/harness-device/firmware/test/run.sh
#
# No ESP-IDF, no flash cycle, no cable: cable_frame.c compiles with a plain compiler on purpose, so the
# framing both halves of the link depend on can be checked in milliseconds. Run it before touching either
# half — the daemon's TypeScript decoder asserts against the same vectors, and a change here that is not
# a change there is a link that opens and then delivers nothing.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

# The vectors are generated, not written. Regenerating first means a stale file cannot pass as agreement.
python3 "$here/../scripts/gen_cable_vectors.py" --check
python3 "$here/../scripts/gen_tux_moods.py" --check
python3 "$here/../scripts/gen_pets.py" --check
python3 "$here/../scripts/gen_arc_geometry.py" --check
python3 "$here/../scripts/gen_adpcm_vectors.py" --check
python3 "$here/test_adpcm_vectors.py"
python3 "$here/../scripts/gen_ble_vectors.py" --check
python3 "$here/test_ble_vectors.py"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main" -o "$out/test_character" "$here/test_character.c" "$here/../main/pet_store.c" "$here/../main/elapsed.c" \
   "$here/../main/ui/habitat/character.c" "$here/../main/ui/habitat/illustrated.c" "$here/../main/ui/habitat/character_motion.c" \
   "$here/../main/ui/habitat/character_layout.c" "$here/../main/ui/habitat/tux.c" \
   "$here/../main/ui/habitat/focus.c" "$here/../main/ui/habitat/lvgl_fonts.c" "$here/../main/ui/habitat/lvgl_icons.c" "$here/../main/ui/habitat/focus_marks.c" "$here/../main/ui/habitat/focus_faces.c" "$here/../main/ui/habitat/pets.c" \
   "$here/../main/ui/habitat/octopus.c" "$here/../main/ui/habitat/octopus_font.c" \
   "$here/../main/ui/habitat/ascii_clip.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_character" "$here/vectors/pet_min.hpet"
# The same source again, only the custom-pet cases, under the address sanitizer: a pack read after it is freed.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${PET_SANITIZERS:-address,undefined}" \
   -I "$here/../main" -o "$out/test_character_asan" "$here/test_character.c" "$here/../main/pet_store.c" "$here/../main/elapsed.c" \
   "$here/../main/ui/habitat/character.c" "$here/../main/ui/habitat/illustrated.c" "$here/../main/ui/habitat/character_motion.c" \
   "$here/../main/ui/habitat/character_layout.c" "$here/../main/ui/habitat/tux.c" \
   "$here/../main/ui/habitat/focus.c" "$here/../main/ui/habitat/lvgl_fonts.c" "$here/../main/ui/habitat/lvgl_icons.c" "$here/../main/ui/habitat/focus_marks.c" "$here/../main/ui/habitat/focus_faces.c" "$here/../main/ui/habitat/pets.c" \
   "$here/../main/ui/habitat/octopus.c" "$here/../main/ui/habitat/octopus_font.c" \
   "$here/../main/ui/habitat/ascii_clip.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_character_asan" "$here/vectors/pet_min.hpet" --custom-only
python3 "$here/test_character_preferences.py"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main" -o "$out/test_elapsed" "$here/test_elapsed.c" "$here/../main/elapsed.c"
"$out/test_elapsed"

cc -std=c11 -Wall -Wextra -Werror -O1 \
   -o "$out/test_cable_frame" "$here/test_cable_frame.c" "$here/../main/cable_frame.c"
"$out/test_cable_frame" "$here/vectors/cable_frame.txt"
python3 "$here/test_cable_transport.py"
# The decoder over a caller buffer and the transport vtable (W5): cap 1,024 reject and resync, a wrapper transport over a loopback
# one, then cable_link.c itself over a fake transport (split reads, partial write, closed stream).
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" \
   -o "$out/test_cable_xport" "$here/test_cable_xport.c" "$here/../main/cable_frame.c"
"$out/test_cable_xport"
python3 "$here/test_cable_xport.py"
# The TLS-PSK wrapper (B2): its source rules here (no key in logs, two ECDHE-PSK suites, TLS 1.2 pins); its behaviour in the IDF block below.
python3 "$here/test_cable_xport_tls_source.py"
# Wi-Fi mode (W11). The link state machine and its backoff ladders (pure logic); the log ring the hook queues into (four producer threads against one
# consumer; the same file passes under -fsanitize=thread); the plain-TCP inner transport over a fake socket layer; the source rules (the hook reaches no
# socket, the radio code is reachable only from net_link_start, the two stacks, the W0 radio configuration, no firmware over a radio link).
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" \
   -o "$out/test_net_state" "$here/test_net_state.c" "$here/../main/net_state.c"
"$out/test_net_state"
# B4a: the BLE link's state machine, deadlines and accept policy; the numbers are read from the shared vector file.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main"    -o "$out/test_ble_state" "$here/test_ble_state.c" "$here/../main/ble_state.c" "$here/../main/net_state.c"
"$out/test_ble_state" "$here/vectors/ble_status.txt"
python3 "$here/test_ble_state_source.py"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -pthread -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" \
   -o "$out/test_log_ring" "$here/test_log_ring.c" "$here/../main/log_ring.c"
"$out/test_log_ring"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -DCABLE_XPORT_TCP_HOST \
   -I "$here/../main" -I "$here/host_tcp" -I "$here/host_stubs" -o "$out/test_cable_xport_tcp" "$here/test_cable_xport_tcp.c" "$here/../main/cable_xport_tcp.c"
"$out/test_cable_xport_tcp"
python3 "$here/test_net_link_source.py"
# The power hooks (W13): net_power_profile calls esp_wifi_set_ps and nothing that sets listen_interval; the net task takes no PM lock of its own and
# holds PP_HOLD_LINK only over connect and handshake; welcome.keepaliveMs on the dial. The timing rule itself (clamp, 1:3, the daemon's window later)
# runs on the host against the table the daemon's spec reads too (vectors/keepalive.txt).
python3 "$here/test_wifi_power_source.py"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" \
   -o "$out/test_cable_keepalive" "$here/test_cable_keepalive.c" -lm
"$out/test_cable_keepalive" "$here/vectors/keepalive.txt"
# The automatic mode switch (W14): the boot table, enter/leave timers, the busy gate, the flap guard, stay_wl, the RTC record and boot_usb as pure
# logic with an injected clock (cases H1-H10 of the design; the bench cases T1-T11 are task W15).
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" \
   -o "$out/test_mode_switch" "$here/test_mode_switch.c" "$here/../main/mode_switch.c"
"$out/test_mode_switch"
# The glue around it (W15), pure parts. The mode switch's three NVS values and the rule that no stored value strands a dial (corrupt bytes, wl_mode
# with no credentials, wl_mode 2, a lost marker, net.clear, factory reset); the RTC record wrapper (trusted only after a software reset); the USB
# probe's hello, strict welcome test, frame cap and cadence; and the glue run against a simulated dial (boot plan, reboot sequence, the recovery
# paths to USB and the bounds on every detour).
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/host_stubs" -I "$here/host_nvs" -I "$here/../main"    -o "$out/test_mode_config" "$here/test_mode_config.c" "$here/host_nvs/host_nvs.c" "$here/../main/config_store.c"
"$out/test_mode_config"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -DMODE_RECORD_HOST -I "$here/host_stubs" -I "$here/../main"    -o "$out/test_mode_record" "$here/test_mode_record.c" "$here/../main/mode_record.c" "$here/../main/mode_switch.c"
"$out/test_mode_record"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main"    -o "$out/test_usb_probe" "$here/test_usb_probe.c" "$here/../main/usb_probe_core.c" "$here/../main/cable_frame.c"
"$out/test_usb_probe"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -DMODE_RECORD_HOST -I "$here/host_stubs" -I "$here/host_nvs" -I "$here/../main"    -o "$out/test_mode_glue" "$here/test_mode_glue.c" "$here/../main/mode_glue_core.c" "$here/../main/mode_record.c" "$here/../main/mode_switch.c"    "$here/host_nvs/host_nvs.c" "$here/../main/config_store.c"
"$out/test_mode_glue"
# The IDF side of that glue, as source rules: boot order and the radio gate, SOF settle, record trusted after a software reset only, internal task stack,
# the reboot sequence, the probe retry, no Bluetooth target, no stay_wl writer, the UI busy rows, the last_words reason, the build list.
python3 "$here/test_mode_glue_source.py"
# The Link screen (W16): the edge swipe, no session needed, NVS on the worker only, stay_wl as a toggle with one writer.
python3 "$here/test_link_screen.py"
# W15e: reset_class() compiled and called for every reason the SDK defines (needs IDF_PATH, like the other SDK checks).
python3 "$here/test_reset_class.py"
# The decoder buffer is in PSRAM (W6): only cable_link.c names it, and every consumer of a frame payload is CPU-side (no DMA).
python3 "$here/test_decoder_psram.py"
# IMA ADPCM voice blocks (frame type 0x07): encoder and decoder against the shared vectors, sanitizers on.
python3 "$here/test_adpcm_ima.py"
# The ADPCM sender (W4): wiring of the flags and fields, and the VOX probe's review gates. The sender loop itself runs in test_audio_stream.py.
python3 "$here/test_adpcm_wiring.py"
python3 "$here/test_vox_gate.py"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_cable_json_guard" "$here/test_cable_json_guard.c" "$here/../main/cable_json_guard.c"
"$out/test_cable_json_guard"

# The dial's Wi-Fi settings in NVS (config_store.c config_net_*; W10): every field checked, wl_ver written last and removed first, a power cut
# between two keys, clear, factory reset, base64 key decode, and no value in the log. An in-memory NVS (host_nvs/) that can fail after N writes.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/host_stubs" -I "$here/host_nvs" -I "$here/../main" \
   -o "$out/test_net_config" "$here/test_net_config.c" "$here/host_nvs/host_nvs.c" "$here/../main/config_store.c"
"$out/test_net_config"
# B6: the phone-mode settings (key-only form, bl_ver marker, wl_mode 2)
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/host_stubs" -I "$here/host_nvs" -I "$here/../main" \
   -o "$out/test_ble_config" "$here/test_ble_config.c" "$here/host_nvs/host_nvs.c" "$here/../main/config_store.c"
"$out/test_ble_config"

cc -std=c11 -Wall -Wextra -Werror -O1 \
   -o "$out/test_cable_machines" "$here/test_cable_machines.c" "$here/../main/cable_machines.c"
"$out/test_cable_machines"

# The host's debug commands (screenshot chunking and checksum, round-face validation, gesture sampler, the
# cable-task/touch-task hand-off on real threads). The JSON half is below, with the other cJSON tests.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -pthread -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main" -o "$out/test_debug_cmd" "$here/test_debug_cmd.c" "$here/../main/debug_cmd.c" -lm
"$out/test_debug_cmd"

# Which dial this image is on, decided from who answered on the I2C bus. Two boards ship on one image;
# the table that tells them apart is arithmetic on a list of addresses, so it is proved here.
cc -std=c11 -Wall -Wextra -Werror -O1 \
   -o "$out/test_board" "$here/test_board.c" "$here/../main/board/board_table.c"
"$out/test_board"
python3 "$here/test_board_i2c.py"

# New direct compositor: fuzz partial redraws against a fresh frame under UB/bounds sanitizers.
# SANITIZERS=address,undefined also enables ASan on hosts with a compatible ASan runtime.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_terminal" "$here/test_terminal.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_terminal"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_arc_pixels" "$here/test_arc_pixels.c" \
   "$here/../main/ui/habitat/focus_faces.c" "$here/../main/ui/habitat/lvgl_fonts.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_arc_pixels"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main/ui/habitat" -o "$out/test_arc_storage" "$here/test_arc_storage.c" \
   "$here/reference79/terminal_ref.c" "$here/../main/ui/habitat/fonts.c" \
   "$here/../main/ui/habitat/focus_faces.c" "$here/../main/ui/habitat/lvgl_fonts.c"
"$out/test_arc_storage"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -DDEVICE_LAYOUT_BENCH=1 \
   -o "$out/test_arc_bounds" "$here/test_arc_bounds.c" \
   "$here/../main/ui/habitat/focus_faces.c" "$here/../main/ui/habitat/lvgl_fonts.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_arc_bounds"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main/ui/habitat" -o "$out/test_arc_ring" "$here/test_arc_ring.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_arc_ring"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main/ui/habitat" -o "$out/test_cell_zoom" "$here/test_cell_zoom.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_cell_zoom"

# Custom pets: the pack parser and store, on the daemon's own vector (cli/src/cable/pets/pack.ts). Address sanitizer
# where the host has it, so a read past a pack or a use after the dropped pack's frame is an error, not a guess.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${PET_SANITIZERS:-address,undefined}" \
   -I "$here/../main" -o "$out/test_pet_store" "$here/test_pet_store.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_pet_store" "$here/vectors/pet_min.hpet" "$here/vectors/pet_min_v2.hpet"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_voice_buffer" "$here/test_voice_buffer.c" "$here/../main/voice_buffer.c"
"$out/test_voice_buffer"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -pthread \
   -o "$out/test_voice_buffer_threads" "$here/test_voice_buffer_threads.c" "$here/../main/voice_buffer.c"
"$out/test_voice_buffer_threads"

# task_create_psram(): the PSRAM-stack create and its internal fallback, against fake IDF create calls.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/host_stubs" -I "$here/../main" -o "$out/test_task_util" "$here/test_task_util.c" "$here/../main/task_util.c"
"$out/test_task_util"

# DEVICE_UI_METRICS: the renderer's counters (terminal.c) and the per-view log line logic (uim.c, fake IDF calls).
cc -std=c11 -Wall -Wextra -Werror -O1 -g -DDEVICE_UI_METRICS=1 -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main/ui/habitat" -o "$out/test_terminal_metrics" "$here/test_terminal_metrics.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_terminal_metrics"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -DDEVICE_UI_METRICS=1 -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/host_stubs" -I "$here/../main/ui/habitat" -o "$out/test_uim" "$here/test_uim.c" \
   "$here/../main/ui/habitat/uim.c" "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_uim"
# The same two as the default image builds them (Focus only: HT_RUNS 29, HT_CELL_COLS_MAX 272).
cc -std=c11 -Wall -Wextra -Werror -O1 -g -DDEVICE_UI_METRICS=1 -DHABITAT_FOCUS_ONLY=1 -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main/ui/habitat" -o "$out/test_terminal_metrics_focus" "$here/test_terminal_metrics.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_terminal_metrics_focus"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -DDEVICE_UI_METRICS=1 -DHABITAT_FOCUS_ONLY=1 -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/host_stubs" -I "$here/../main/ui/habitat" -o "$out/test_uim_focus" "$here/test_uim.c" \
   "$here/../main/ui/habitat/uim.c" "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_uim_focus"
# The battery cache and its 30 s schedule, the NVS sample ring (against memory), and the alive line's width.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" -o "$out/test_battery" \
   "$here/test_battery.c" "$here/../main/battery.c"
"$out/test_battery"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" -o "$out/test_battery_ring" \
   "$here/test_battery_ring.c" "$here/../main/battery_ring.c"
"$out/test_battery_ring"
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" -o "$out/test_battery_rim" \
   "$here/test_battery_rim.c" "$here/../main/battery.c" "$here/../main/ui/habitat/battery_rim.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c" -lm
"$out/test_battery_rim"
python3 "$here/test_alive_line.py"
# The battery sleep policy (DEVICE_BATTERY_SLEEP): S0/S1/S2 state machine, PMIC read schedule, poller table, battery screen levels.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" -o "$out/test_power_policy" \
   "$here/test_power_policy.c" "$here/../main/power_policy.c"
"$out/test_power_policy"
# The BOOT key state machine (tap, hold-to-talk, D1 gate) and the 5 s settings-wipe countdown.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" -o "$out/test_boot_key"    "$here/test_boot_key.c" "$here/../main/boot_key.c"
"$out/test_boot_key"
python3 "$here/test_boot_key_wiring.py"
python3 "$here/test_d1_wake_sources.py"

# The sleep/wake profile's percentiles and summary line.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -I "$here/../main" -o "$out/test_slp_stats" \
   "$here/test_slp_stats.c" "$here/../main/slp_stats.c"
"$out/test_slp_stats"

# The UI gallery's tour table: names, holds, and that every view_t value and home face has an item.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -I "$here/../main/ui/habitat" -o "$out/test_ui_gallery_items" "$here/test_ui_gallery_items.c"
"$out/test_ui_gallery_items"
python3 "$here/test_ui_gallery_source.py"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_scroll" "$here/test_scroll.c" "$here/../main/ui/habitat/scroll.c" -lm
"$out/test_scroll"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_gestures" "$here/test_gestures.c" "$here/../main/ui/habitat/gestures.c"
"$out/test_gestures"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_workspace" "$here/test_workspace.c" "$here/../main/ui/habitat/workspace.c"
"$out/test_workspace"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_supertab" "$here/test_supertab.c" "$here/../main/ui/habitat/supertab.c"
"$out/test_supertab"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_tim" "$here/test_tim.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_tim"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_octopus" "$here/test_octopus.c" "$here/../main/ui/habitat/octopus.c" \
   "$here/../main/ui/habitat/octopus_font.c" "$here/../main/ui/habitat/ascii_clip.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_octopus"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/../main/ui/habitat" -I "$here" -o "$out/test_renderer_reference" \
   "$here/test_renderer_reference.c" "$here/reference48/terminal_ref.c" "$here/reference48/octopus_ref.c" \
   "$here/../main/ui/habitat/octopus.c" "$here/../main/ui/habitat/ascii_clip.c" \
   "$here/../main/ui/habitat/octopus_font.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_renderer_reference"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -DDEVICE_LAYOUT_BENCH=1 \
   -o "$out/test_ascii_scene" "$here/test_ascii_scene.c" "$here/../main/ui/habitat/octopus.c" \
   "$here/../main/ui/habitat/octopus_font.c" "$here/../main/ui/habitat/ascii_clip.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_ascii_scene"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_glyph_cache" "$here/test_glyph_cache.c" "$here/../main/ui/habitat/octopus.c" \
   "$here/../main/ui/habitat/octopus_font.c" "$here/../main/ui/habitat/ascii_clip.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_glyph_cache"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -DDEVICE_LAYOUT_BENCH=1 -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_raster_ascii" "$here/test_raster_ascii.c" "$here/../main/ui/habitat/octopus.c" \
   "$here/../main/ui/habitat/octopus_font.c" "$here/../main/ui/habitat/ascii_clip.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_raster_ascii"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -DDEVICE_LAYOUT_BENCH=1 -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_damage_delta" "$here/test_damage_delta.c" "$here/../main/ui/habitat/octopus.c" \
   "$here/../main/ui/habitat/octopus_font.c" "$here/../main/ui/habitat/ascii_clip.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_damage_delta"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -DDEVICE_LAYOUT_BENCH=1 -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_damage_bands" "$here/test_damage_bands.c" "$here/../main/ui/habitat/octopus.c" \
   "$here/../main/ui/habitat/octopus_font.c" "$here/../main/ui/habitat/ascii_clip.c" "$here/../main/ui/habitat/tim.c" "$here/../main/ui/habitat/character_motion.c" "$here/../main/ui/habitat/character_layout.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_damage_bands"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -DDEVICE_LAYOUT_BENCH=1 -o "$out/test_ascii_clip" "$here/test_ascii_clip.c" "$here/../main/ui/habitat/ascii_clip.c"
"$out/test_ascii_clip"
python3 "$here/test_octopus_art.py"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_creature_gallery" "$here/test_creature_gallery.c" \
   "$here/../main/ui/habitat/creature_gallery.c" "$here/../main/ui/habitat/creature_font.c" \
   "$here/../main/ui/habitat/terminal.c" "$here/../main/ui/habitat/fonts.c"
"$out/test_creature_gallery"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_selection" "$here/test_selection.c" "$here/../main/ui/habitat/selection.c"
"$out/test_selection"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_carry" "$here/test_carry.c" "$here/../main/ui/habitat/carry.c"
"$out/test_carry"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_visit" "$here/test_visit.c" "$here/../main/ui/habitat/visit.c"
"$out/test_visit"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_form" "$here/test_form.c" "$here/../main/ui/habitat/form.c"
"$out/test_form"

cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -o "$out/test_draft" "$here/test_draft.c" "$here/../main/ui/habitat/draft.c"
"$out/test_draft"

python3 "$here/test_search_ui.py"
python3 "$here/test_draft_ui.py"
python3 "$here/test_question_ui.py"
python3 "$here/test_voice_ui.py"
python3 "$here/test_touch_ui.py"
python3 "$here/test_brightness_ui.py"
# The preferences the desktop app now owns, driven through the device's own read/apply/worker path.
python3 "$here/test_device_settings.py"
python3 "$here/test_touch_driver.py"
python3 "$here/test_touch_inject.py"
python3 "$here/test_render_guard.py"
# PSRAM placement of the scene buffers and arc caches (HT_RENDER_DATA_PSRAM), read from the preprocessed source.
python3 "$here/test_render_data_psram.py"
# Which tasks have a PSRAM stack (task_create_psram under its TASK_STACK_PSRAM_* macro) and which keep an internal one.
python3 "$here/test_psram_task_sites.py"
# HT_RUNS / HT_CELL_COLS_MAX right-sized from the gallery tour, against the built-in sprite frames and the pet packs.
python3 "$here/test_render_limits.py"
# The panel draw buffer in PSRAM: the allocation (fake IDF calls) and its wiring in display_habitat.c.
cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
   -I "$here/host_stubs" -I "$here/../main" -o "$out/test_draw_buffer" "$here/test_draw_buffer.c" "$here/../main/draw_buffer.c"
"$out/test_draw_buffer"
python3 "$here/test_drawbuf_psram.py"
python3 "$here/test_display_power.py"
python3 "$here/test_machine_ui.py"
python3 "$here/test_reload_threads.py"
python3 "$here/test_agent_snapshot.py"
python3 "$here/test_cable_session.py"
python3 "$here/test_cable_idle.py"
python3 "$here/test_fw_update.py"
python3 "$here/test_no_ota.py"
python3 "$here/test_sleep_overlay.py"
python3 "$here/test_sleep_pins.py"
python3 "$here/test_quiet_pollers.py"
python3 "$here/test_power_sleep_glue.py"
python3 "$here/test_touch_sleep.py"
python3 "$here/test_power_off.py"
python3 "$here/test_power_off_wiring.py"
python3 "$here/test_cable_logging.py"
python3 "$here/test_audio_notify.py"
python3 "$here/test_audio_init.py"
python3 "$here/test_audio_stream.py"
python3 "$here/test_audio_threads.py"
python3 "$here/test_audio_idle.py"

# These exercise the exact cJSON version linked into the board, when its SDK is
# available. The rest of the suite remains independent of ESP-IDF.
if [[ -n "${IDF_PATH:-}" ]]; then
    python3 "$here/test_cable_crc.py"
    cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
        -I "$here/../main" -I "$IDF_PATH/components/json/cJSON" \
        -o "$out/test_cable_features" "$here/test_cable_features.c" \
        "$here/../main/cable_features.c" "$IDF_PATH/components/json/cJSON/cJSON.c"
    "$out/test_cable_features"
    cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" \
        -I "$here/../main" -I "$IDF_PATH/components/json/cJSON" \
        -o "$out/test_debug_cmd_json" "$here/test_debug_cmd_json.c" \
        "$here/../main/debug_cmd.c" "$here/../main/debug_cmd_json.c" "$IDF_PATH/components/json/cJSON/cJSON.c" -lm
    "$out/test_debug_cmd_json"
    # the same with the bench command of the PMIC power-off compiled in (DEVICE_BATTERY_POWEROFF)
    cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -g -fsanitize="${SANITIZERS:-undefined,bounds}" -DDEVICE_BATTERY_POWEROFF=1         -I "$here/../main" -I "$IDF_PATH/components/json/cJSON"         -o "$out/test_debug_cmd_json_poff" "$here/test_debug_cmd_json.c"         "$here/../main/debug_cmd.c" "$here/../main/debug_cmd_json.c" "$IDF_PATH/components/json/cJSON/cJSON.c" -lm
    "$out/test_debug_cmd_json_poff"
    cc -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -g -pthread -fsanitize="${SANITIZERS:-undefined,bounds}" \
        -I "$here/../main" -I "$IDF_PATH/components/json/cJSON" \
        -o "$out/test_cable_scroll" "$here/test_cable_scroll.c" \
        "$here/../main/cable_scroll.c" "$IDF_PATH/components/json/cJSON/cJSON.c"
    "$out/test_cable_scroll"
    python3 "$here/test_cable_json_parse.py"
    python3 "$here/test_companion_protocol.py"
    python3 "$here/test_cable_identity.py"
    python3 "$here/test_cable_supertab.py"
    python3 "$here/test_cable_models.py"
    python3 "$here/test_cable_outbound.py"
    # net.set / get / clear / mode / key / host (cable_client.c handle_net, W10): USB only, behind the feature, field names and never values.
    python3 "$here/test_cable_net_set.py"
    # cable_xport_tls.c against the IDF copy of mbedTLS built for the host: handshake, echo, refusals, concurrency, deadlines (about 20 s)
    python3 "$here/test_cable_xport_tls.py"
    # which mbedTLS errors mean "the daemon refused us" (net_hs_class.h), against the IDF headers
    python3 "$here/test_net_hs_class.py"
fi

python3 "$here/test_companions.py"
