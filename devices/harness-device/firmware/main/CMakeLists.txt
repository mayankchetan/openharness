# The single "main" component. ONE board, ONE renderer.
#
# The device is the round 466 dial on an ESP32-S3, and what it draws with is habitat — the
# allocation-free text compositor under ui/habitat/. There is no second renderer and no second board
# in this repository any more:
#
#   * the LVGL UI (ui/ui_screens.c, thirteen Geist faces at 4 bpp, the raster icons and the spinner)
#     is gone. Measured before it went: 3,184,848 B against habitat's 788,704 B, and ~2,032 KB of the
#     difference was the typefaces alone. Nothing about LVGL was heavy; the way it was fed was.
#   * the ESP32-P4 Pro is gone with it. That board gets its own firmware, written by the team that
#     owns it. What crossed between them was a protocol, not a build — see cable_client.c — so a Pro
#     speaking it still works with this daemon without a line of its code living here.
#
# Both deletions removed a flag as well as files: there is no DEVICE_HABITAT and no HT_FACE_PX. A
# single firmware needs neither, and a flag that is always set is a flag that will one day be wrong.

set(BOARD_SRCS
    # the portable half: the cable protocol, storage, OTA, audio
    "app_main.c"
    "ram_telemetry.c"
    "task_util.c"
    "draw_buffer.c"
    "battery.c"
    "battery_ring.c"
    "config_store.c"
    "device_mac.c"
    "cable_frame.c"
    "cable_link.c"
    "cable_xport_usb.c"
    "cable_xport_tcp.c"
    "cable_xport_tls.c"
    "net_state.c"
    "log_ring.c"
    "wifi_sta.c"
    "net_link.c"
    "cable_client.c"
    "cable_machines.c"
    "cable_scroll.c"
    "cable_json_guard.c"
    "cable_features.c"
    "adpcm_ima.c"
    "last_words.c"
    "fw_update.c"
    "pet_store.c"
    "elapsed.c"
    "audio_capture.c"
    "audio_client_habitat.c"
    "audio_probe.c"
    "voice_buffer.c"
    "ptt.c"
    "boot_key.c"
    "store_gate.c"
    # the board
    "board/board_i2c.c"
    "board/board.c"
    "board/board_table.c"
    "board/power.c"
    # the renderer: panel bring-up, touch and the compositor, all its own
    "ui/ui_perf.c"
    "ui/habitat/display_habitat.c"
    "ui/habitat/touch_habitat.c"
    "ui/habitat/ui_habitat.c"
    "ui/habitat/terminal.c"
    "ui/habitat/scroll.c"
    "ui/habitat/workspace.c"
    "ui/habitat/supertab.c"
    "ui/habitat/selection.c"
    "ui/habitat/carry.c"
    "ui/habitat/visit.c"
    "ui/habitat/form.c"
    "ui/habitat/draft.c"
    "ui/habitat/gestures.c"
    "ui/habitat/tim.c"
    "ui/habitat/character.c"
    "ui/habitat/illustrated.c"
    "ui/habitat/illustrated_cache.c"
    "ui/habitat/character_motion.c"
    "ui/habitat/character_layout.c"
    "ui/habitat/tux.c"
    "ui/habitat/focus.c"
    "ui/habitat/octopus.c"
    "ui/habitat/ascii_clip.c"
    "ui/habitat/octopus_font.c"
    "ui/habitat/command_face.c"
    "ui/habitat/fonts.c"
    "ui/habitat/lvgl_fonts.c"
    "ui/habitat/lvgl_icons.c"
    "ui/habitat/focus_marks.c"
    "ui/habitat/battery_rim.c"
    "ui/habitat/focus_faces.c"
    "ui/habitat/pets.c"
)

set(BOARD_REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    lwip
    esp_http_server
    esp_http_client
    esp_https_ota
    app_update
    esp_partition
    mbedtls
    esp_websocket_client
    esp-tls
    esp_lcd
    esp_lcd_touch
    esp_codec_dev
    esp_timer
    driver
    json
)

# The illustrated companions' artwork is 4.6 MB of the image, and a Focus-only build never draws it,
# so it ships only with -DDEVICE_ALL_SKINS=1 (owner, 2026-09-30). Without it the code stays and the
# pack is empty — see illustrated_cache.c.
if(DEVICE_ALL_SKINS)
    set(COMPANION_ART "../assets/companions/companion_art.pack")
else()
    set(COMPANION_ART "")
endif()

# spi_flash esp_mm: esp_private/flash_mmap.h: flash_mmap_remain() in the alive line (ram_telemetry.c); esp_private/esp_cache_private.h:
# esp_cache_get_alignment() (draw_buffer.c). esp_pm: the power-management locks and esp_pm_configure (power_sleep.c, battery sleep only).
set(BOARD_PRIV_REQUIRES spi_flash esp_mm)
if(DEVICE_BATTERY_SLEEP)
    list(APPEND BOARD_PRIV_REQUIRES esp_pm)
endif()

idf_component_register(
    SRCS ${BOARD_SRCS}
    INCLUDE_DIRS
        "."
        "ui"
        "board"
    REQUIRES ${BOARD_REQUIRES}
    PRIV_REQUIRES ${BOARD_PRIV_REQUIRES}
    EMBED_FILES ${COMPANION_ART}
)

# snprintf() is bounded (it truncates, never overflows), but -O2 enables a noisy
# format-truncation analysis that errors on safe same-size copies. Keep it as a warning, not error.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

if(DEVICE_HABITAT_ORANGE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_HABITAT_ORANGE=1)
endif()

# Host debug commands: debug.shot (a screenshot of the face) and debug.tap / press / swipe (injected touch)
# over the cable — docs/DEBUG_COMMANDS.md. ON by default in this fork. Nothing runs until the host sends
# one, but anyone with the cable can send one, so a build for a device out of your hands should say
# -DDEVICE_NO_DEBUG_COMMANDS=1: the sources, the renderer hook and the touch hook then all drop out.
if(NOT DEVICE_NO_DEBUG_COMMANDS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HARNESS_DEBUG_COMMANDS=1)
    target_sources(${COMPONENT_LIB} PRIVATE "debug_cmd.c" "debug_cmd_json.c" "cable_debug.c")
endif()

# Focus and nothing else until the companion skins are finished (owner, 2026-09-30); the host tests
# build without it and keep covering every skin. -DDEVICE_ALL_SKINS=1 lifts it for a build.
if(NOT DEVICE_ALL_SKINS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HABITAT_FOCUS_ONLY=1 HABITAT_NO_COMPANION_ART=1)
endif()

# Focus unless told otherwise — see ht_character_default().
if(DEVICE_DEFAULT_CHARACTER STREQUAL "tux")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_DEFAULT_CHARACTER_TUX=1)
elseif(DEVICE_DEFAULT_CHARACTER STREQUAL "tim")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_DEFAULT_CHARACTER_TIM=1)
elseif(DEFINED DEVICE_DEFAULT_CHARACTER AND NOT DEVICE_DEFAULT_CHARACTER STREQUAL "focus")
    message(FATAL_ERROR "DEVICE_DEFAULT_CHARACTER must be tim, tux or focus")
endif()

if(DEVICE_PERF_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_PERF_BENCH=1)
endif()

# ADPCM voice (tasks W4 and W12). The code is always built in (CABLE_VOICE_ADPCM_BUILT, cable_client.h): on a non-USB link (Wi-Fi mode) the dial
# advertises hello.voiceCodec "adpcm-v1", and when the daemon's welcome has voice.adpcm it sends each voice turn as IMA ADPCM blocks (frame type 0x07,
# adpcm_ima.c) instead of PCM. On USB it sends PCM and no voiceCodec, as before, unless -DDEVICE_VOICE_ADPCM_USB=1 (default OFF): that flag exists so
# bench gate G1 can measure the encoder (encode_us_max, WER) over the cable.
if(DEVICE_VOICE_ADPCM_USB)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_VOICE_ADPCM_USB=1)
endif()

# VOX probe (vox_probe.c): -DDEVICE_VOX_PROBE=1, default OFF, never a daily image. 30 s after the cable session is up it runs 100 voice turns of
# 4 s each through the real voice path (capture, ring, [encoder], cable, daemon STT, reply) and logs one "VOX n=" line per turn with codec=,
# blocks=, encode_us_max=, the ring peak, stack marks and the reply latency, plus a summary every 10 turns. The words go to the first agent of the
# tab, so use a throwaway tab; -DDEVICE_VOX_REVIEW=1 finishes each turn with a review request (a draft, nothing is typed) and stops on any other
# reply (vox_gate.h). Bench method: workbench briefs/research/2026-10-28-profiling-images.md section 8.
if(DEVICE_VOX_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_VOX_PROBE=1)
    target_sources(${COMPONENT_LIB} PRIVATE "vox_probe.c")
    if(DEVICE_VOX_REVIEW)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_VOX_REVIEW=1)
    endif()
endif()

# RAM diet 2 telemetry: one RAM_REGIONS line per internal heap region (free, largest, min_free) at the boot tags
# and every 5 minutes, plus the failed-allocation hook (ram_telemetry.c). ON unless -DDEVICE_RAM_REGIONS=0.
# It reads the heap component's private region list (heap_private.h), hence the extra include directory.
if(NOT DEFINED DEVICE_RAM_REGIONS)
    set(DEVICE_RAM_REGIONS ON)
endif()
if(DEVICE_RAM_REGIONS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_RAM_REGIONS=1)
    idf_component_get_property(heap_dir heap COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${heap_dir}")
endif()

# PSRAM placement switches (RAM diet 2, round 3). Each defaults to ON in the source that owns it; pass -D<NAME>=0 to
# cmake (idf.py -D<NAME>=0 build) to put that one item back in internal RAM for an A/B build. Not a Kconfig option on purpose:
# they change what a test or a profile image measures, not what the product is.
#   HT_RENDER_DATA_PSRAM   scenes[2] and arc_caches[2] in PSRAM          (ui/habitat/terminal.h)
#   TASK_STACK_PSRAM_FLASH_OPS   habitat_actions, cable_session, voice_send stacks in PSRAM   (task_util.h)
#   HT_RIGHT_SIZE          HT_RUNS 29 and HT_CELL_COLS_MAX 272 instead of 40 and 480; on by default in the Focus-only
#                          build, off with DEVICE_ALL_SKINS (the skins draw more runs)   (ui/habitat/terminal.h)
#   HT_DRAWBUF_PSRAM       1 = one of the two panel draw buffers in PSRAM, sent by DMA from there; 0 = both internal   (ui/habitat/display_habitat.c)
#   TASK_STACK_PSRAM_READER   the cable_link reader stack in PSRAM; follows DEVICE_NO_OTA (below), an error without it   (task_util.h)
foreach(psram_switch HT_RENDER_DATA_PSRAM TASK_STACK_PSRAM_FLASH_OPS HT_RIGHT_SIZE HT_DRAWBUF_PSRAM TASK_STACK_PSRAM_READER)
    if(DEFINED ${psram_switch})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${psram_switch}=${${psram_switch}})
    endif()
endforeach()

# OTA off (RAM diet 2, plan C17; user decision 2026-10-28: dials are always updated over USB, and the daemon already runs with
# CABLE_FW_DISABLE=true). ON unless -DDEVICE_NO_OTA=0. fw_update.c keeps only fw_mark_valid() (the rollback guard: flash code, no
# RAM); the offer, slice, tick and abort code and its statics drop out, `fw.offer` is answered with `fw.error` "not supported",
# and the cable_link reader, whose only flash user was the OTA path, moves to a PSRAM stack (TASK_STACK_PSRAM_READER). The decoder
# `s_decoder` stays: it decodes every frame type. The partition table is unchanged (two app slots, otadata).
if(NOT DEFINED DEVICE_NO_OTA)
    set(DEVICE_NO_OTA ON)
endif()
if(DEVICE_NO_OTA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_NO_OTA=1)
endif()

# Sleep/wake and boot profile (sleep_profile.c): -DDEVICE_SLEEP_PROFILE=1, default OFF. 500 sleep/wake cycles 20 s after boot
# through the real sleep and touch-wake paths, with wake and sleep times, free internal memory and a summary every 50, and
# the main task's stack mark at app_ready and at the end of app_main. Needs the debug commands (the wake is an injected tap).
if(DEVICE_SLEEP_PROFILE)
    if(DEVICE_NO_DEBUG_COMMANDS OR DEVICE_UI_GALLERY OR DEVICE_XIP_PROBE OR DEVICE_PERF_BENCH OR DEVICE_LAYOUT_BENCH OR DEVICE_CREATURE_GALLERY)
        message(FATAL_ERROR "DEVICE_SLEEP_PROFILE needs the debug commands and cannot be combined with a bench, the gallery or the XiP probe")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_SLEEP_PROFILE=1)
    target_sources(${COMPONENT_LIB} PRIVATE "sleep_profile.c" "slp_stats.c")
endif()

# Battery history (battery_log.c): -DDEVICE_BATTERY_LOG=1, default OFF. While the dial runs on its battery, one sample a
# minute ({boot, uptime_s, percent}) goes into an NVS ring of 1,440 samples (24 blobs of 360 B, about 10 KB of the 24 KB NVS partition); back on
# external power with the cable session up, the new samples are logged once as "BATLOG" lines. Writes NVS: not a daily image.
if(DEVICE_BATTERY_LOG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BATTERY_LOG=1)
    target_sources(${COMPONENT_LIB} PRIVATE "battery_log.c")
endif()

# Battery sleep (power_policy.c, power_sleep.c): -DDEVICE_BATTERY_SLEEP=1, default OFF. On the CST9217 + AXP2101 dial, while the PMIC says
# "discharging", no USB host is there and the screen has been off for 3 s, the chip goes into light sleep; the PWR key wakes it (D1,
# plan section 16: on the battery a dark screen takes no touch and no BOOT; the touch level wake is compiled out, PP_TOUCH_WAKE_ON_BATTERY). External power, a host, a missing PMIC or any doubt keeps the dial exactly as it was (policy state S0). The screen policy on
# battery (dim at 30 s, off at 60 s) comes with it. Plan: workbench briefs/research/2026-10-29-battery-sleep-plan.md.
#   idf.py -DDEVICE_BATTERY_SLEEP=1 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.sleep" build
# The overlay sdkconfig.defaults.sleep (power management, tickless idle, USB-Serial-JTAG guard) is NOT loaded by default; without it this
# flag stops the configure step. Power management and tickless idle cost internal RAM in every mode, USB included (see the overlay).
if(DEVICE_BATTERY_SLEEP)
    if(NOT CONFIG_PM_ENABLE OR NOT CONFIG_FREERTOS_USE_TICKLESS_IDLE OR NOT CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION)
        message(FATAL_ERROR "DEVICE_BATTERY_SLEEP needs the power-management options of sdkconfig.defaults.sleep: pass "
                "-DSDKCONFIG_DEFAULTS=\"sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.sleep\" (and a fresh sdkconfig)")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BATTERY_SLEEP=1)
    target_sources(${COMPONENT_LIB} PRIVATE "power_policy.c" "power_sleep.c")
endif()

# PMIC power-off (power_off.c, store_gate.c): -DDEVICE_BATTERY_POWEROFF=1, default OFF, needs DEVICE_BATTERY_SLEEP=1. From S2 (battery, screen off) the dial
# switches itself off through the AXP2101 (REG 0x10 bit 0) after 30 min in S2, at 5 percent or less, or when the gauge falls faster than
# 15 percent an hour (provisional); never on external power. Only a PWR hold brings it back. It also adds the bench command debug.power.off,
# which runs the same sequence on a cable. Plan: workbench briefs/research/2026-10-29-battery-sleep-plan.md sections 3.6, 16, 16.1; bench gates P1-P6.
if(DEVICE_BATTERY_POWEROFF)
    if(NOT DEVICE_BATTERY_SLEEP)
        message(FATAL_ERROR "DEVICE_BATTERY_POWEROFF needs DEVICE_BATTERY_SLEEP=1: the triggers are the sleep policy's (state S2)")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BATTERY_POWEROFF=1)
    target_sources(${COMPONENT_LIB} PRIVATE "power_off.c")
endif()

# The UI gallery (ui/habitat/ui_gallery.inc): a UI-only image that walks every screen and animation of the firmware on a
# fixture model, with the UIM line per item. Boots the display, renderer, touch, UI and the cable link only (no audio, no
# voice tasks, no ptt/beep/refresh, no OTA confirmation, no NVS writes). Implies DEVICE_UI_METRICS.
#   -DDEVICE_UI_GALLERY=1                          the tour, 20 s after boot, repeated
#   -DDEVICE_UI_GALLERY=1 -DDEVICE_UI_GALLERY_NO_DISPLAY=1   the same image without display, renderer, touch and UI
#                                                  (the baseline for the UI's internal RAM)
if(DEVICE_UI_GALLERY)
    if(DEVICE_PERF_BENCH OR DEVICE_CREATURE_GALLERY OR DEVICE_LAYOUT_BENCH OR DEVICE_OCTOPUS_BENCH OR DEVICE_XIP_PROBE)
        message(FATAL_ERROR "DEVICE_UI_GALLERY is its own image; it cannot be combined with a bench, the creature gallery or the XiP probe")
    endif()
    set(DEVICE_UI_METRICS ON)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_UI_GALLERY=1)
    if(DEVICE_UI_GALLERY_NO_DISPLAY)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_UI_GALLERY_NO_DISPLAY=1)
    endif()
elseif(DEVICE_UI_GALLERY_NO_DISPLAY)
    message(FATAL_ERROR "DEVICE_UI_GALLERY_NO_DISPLAY needs DEVICE_UI_GALLERY=1")
endif()

# Per-view UI measurements: -DDEVICE_UI_METRICS=1 adds counters on the render path (runs per scene, arc mask bytes,
# sprite columns, glyph cache hits) and one "UIM view=..." log line (tag uim) when the view changes and every 10 s
# (ui/habitat/uim.c). Off by default; not for a daily image.
if(DEVICE_UI_METRICS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_UI_METRICS=1)
    target_sources(${COMPONENT_LIB} PRIVATE "ui/habitat/uim.c")
endif()

# XiP probe (diagnostic, never part of a daily image): PSRAM-stack tasks calling NVS, OTA and flash reads.
# -DDEVICE_XIP_PROBE=1 compiles main/xip_probe.c and the guarded calls in app_main.c. It writes and erases the
# inactive OTA slot and the NVS namespace "xipprobe", so it also needs a second, explicit flag:
# -DDEVICE_XIP_PROBE_ACK=1. Without it the configure step stops. The probe image prints
# "XIP PROBE BUILD - NOT FOR PRODUCTION" at boot (xip_probe_banner).
if(DEVICE_XIP_PROBE)
    if(NOT "${DEVICE_XIP_PROBE_ACK}" STREQUAL "1")
        message(FATAL_ERROR "DEVICE_XIP_PROBE builds a diagnostic image that erases the inactive OTA slot and "
                "writes NVS 'xipprobe'; it must never ship. Add -DDEVICE_XIP_PROBE_ACK=1 to confirm.")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_XIP_PROBE=1)
    # -DDEVICE_XIP_CASES=<letters from A B C D E> picks the cases (default ACDE; B crashes on a PSRAM stack).
    if(DEVICE_XIP_CASES)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIP_CASES="${DEVICE_XIP_CASES}")
    endif()
    target_sources(${COMPONENT_LIB} PRIVATE "xip_probe.c")
endif()

# Event-tagged A/B profiling of the connected octopus. Never part of a daily image.
if(DEVICE_OCTOPUS_BENCH)
    if(NOT DEVICE_PERF_BENCH OR DEVICE_CREATURE_GALLERY)
        message(FATAL_ERROR "DEVICE_OCTOPUS_BENCH requires PERF, without the gallery")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_OCTOPUS_BENCH=1)
    target_sources(${COMPONENT_LIB} PRIVATE "ui/habitat/perf_bench.c")
endif()

# PROD build override: `idf.py -DDEVICE_FORCE_PROD=1 build` (into a separate build dir) forces the
# production backend and ignores any local provisioned_config.h — no need to move the header, so the LOCAL
# and PROD build dirs both stay warm (no CMake-reconfigure churn). See app_main.c's DEVICE_FORCE_PROD.
if(DEVICE_FORCE_PROD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_FORCE_PROD=1)
endif()

# LOCAL BACKEND override: `idf.py -DDEVICE_BACKEND_URL="ws://<laptop-ip>:8085" build` points the device
# at a backend on your machine. The URL is otherwise hardcoded to production AND re-forced over whatever
# NVS holds on every boot (app_main.c), so it is not a setting the device can be talked into — a local
# build is the only way.
if(DEVICE_BACKEND_URL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_PROD_BACKEND_URL="${DEVICE_BACKEND_URL}")
endif()

# BRING-UP PROBE: `idf.py -DBOARD_PROBE=1 build` (separate build dir recommended). app_main runs
# board/board_probe.c before the display comes up and prints what is on the I2C bus, which reset pins
# do what, and what each touch driver reads idle. Never set for a release image.
if(BOARD_PROBE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_PROBE=1)
    target_sources(${COMPONENT_LIB} PRIVATE "board/board_probe.c")
endif()

if(DEVICE_TRANSPORT_BENCH)
    if(DEVICE_CREATURE_GALLERY OR DEVICE_LAYOUT_BENCH)
        message(FATAL_ERROR "DEVICE_TRANSPORT_BENCH requires the normal renderer")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_TRANSPORT_BENCH=1)
    target_sources(${COMPONENT_LIB} PRIVATE "ui/habitat/transport_bench.c"
        "../test/reference91/cable_frame_ref.c")
endif()

# Opt-in, local-only ASCII visual study. Omitted entirely from normal firmware.
if(DEVICE_CREATURE_GALLERY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_CREATURE_GALLERY=1)
    target_sources(${COMPONENT_LIB} PRIVATE
        "ui/habitat/creature_gallery.c"
        "ui/habitat/creature_font.c")
endif()

# One-boot layout CPU profiling. Never enabled in the daily device image.
if(DEVICE_LAYOUT_BENCH)
    if(DEVICE_CREATURE_GALLERY OR DEVICE_OCTOPUS_BENCH)
        message(FATAL_ERROR "DEVICE_LAYOUT_BENCH requires the connected UI only")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_LAYOUT_BENCH=1)
    target_sources(${COMPONENT_LIB} PRIVATE "ui/habitat/layout_bench.c" "ui/habitat/scroll_bench.c")
    if(DEVICE_LAYOUT_BASELINE STREQUAL "82")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_LAYOUT_BASELINE82=1)
    elseif(DEVICE_LAYOUT_BASELINE STREQUAL "79")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_LAYOUT_BASELINE79=1)
        target_sources(${COMPONENT_LIB} PRIVATE "../test/reference79/terminal_ref.c")
    else()
        target_sources(${COMPONENT_LIB} PRIVATE
            "../test/reference48/terminal_ref.c" "../test/reference48/octopus_ref.c")
    endif()
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../test" "${CMAKE_CURRENT_LIST_DIR}/ui/habitat")
endif()

# One-shot fault injection, omitted from daily builds. A retained marker makes
# the reboot after the injected stall resume normal operation instead of looping.
if(DEVICE_RENDER_FAULT)
    if(DEVICE_CREATURE_GALLERY OR DEVICE_LAYOUT_BENCH OR DEVICE_OCTOPUS_BENCH)
        message(FATAL_ERROR "DEVICE_RENDER_FAULT requires the normal renderer")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_RENDER_FAULT=1)
endif()

if(DEVICE_RENDER_STRESS)
    if(DEVICE_CREATURE_GALLERY OR DEVICE_LAYOUT_BENCH OR DEVICE_OCTOPUS_BENCH OR DEVICE_RENDER_FAULT)
        message(FATAL_ERROR "DEVICE_RENDER_STRESS requires the normal renderer without fault injection")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_RENDER_STRESS=1)
endif()
