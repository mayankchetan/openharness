// mode_glue_core.c (task W15): the glue between the pure mode switch (mode_switch.c) and the world, run against a SIMULATED dial: the real NVS
// accessors (config_store.c over host_nvs/), the real RTC record (mode_record.c), the real state machine, and fakes only for the hardware:
// the clock, the SOF flag, the daemon's welcomes, the UI's busy mask, and esp_restart (which here starts the next boot).
//
// The glue contract from W14, each line with its test:
//   ms_boot_decide once >= 20 ms after the reset ...... test_boot_plan, test_boot_waits_for_the_settle_time
//   ms_step every <= 250 ms ........................... the simulator steps every 250 ms
//   on REBOOT: log the reason, seal the record, store boot_usb, THEN restart ... test_reboot_sequence_order
//   the probe is on exactly while out.probe_wanted .... test_probe_edges
//   load the record with reset_is_software ............ test_power_cut_forgets_the_count
//   re-seal when flaps_cleared ........................ test_flaps_clear_after_ten_minutes
//
// And the safety rule behind all of it: THERE IS NO STORED STATE FROM WHICH A DIAL CANNOT REACH USB.
//   S1  a Wi-Fi dial, a daemon on the cable                    -> USB (the point of the task), and it stays there
//   S2  the SOF flag late after the switch's reboot            -> a detour, at most MS_FLAP_CAP reboots, never a loop
//   S3  NVS refuses the boot_usb write                         -> the same bound
//   S4  wl_mode corrupt, no credentials                         -> USB, and the dial never leaves it (wl_mode 2 is a BLE target since B8a: its tests
//       force the value, because the NVS accessors cannot store it until B6)
//   S5  stay_wl corrupt or set with no credentials              -> USB
//   S6  factory reset / net.clear                              -> USB
//   S7  the radio does not come up                             -> USB in the same boot, bounded retries
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "mode_glue_core.h"
#include "mode_record.h"
#include "mode_switch.h"
#include "nvs.h"

void host_log_info(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void host_log_warn(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

extern void host_nvs_reset(void);
extern void host_nvs_fail_after(int mutations);
extern int host_nvs_has(const char *key);

static const uint8_t KEY[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

// ── the simulated dial ──────────────────────────────────────────────────────────────────────────────
typedef struct {
    // the outside world, set by the test
    bool cable;                 // a host is driving the USB port (SOF)
    bool daemon;                // a daemon answers on that port (welcome to every probe hello; pings in USB mode)
    bool sof_late;              // after a reboot the SOF flag takes `sof_late_ms` to appear
    uint32_t sof_late_ms;
    bool radio_fails;           // the wireless stack (Wi-Fi, BLE) does not start (net_link_start returns false)
    uint8_t wl_force;           // B8a: nonzero = the wireless target the NVS read returns (config_store cannot store wl_mode 2 before B6); 0 = ask NVS
    uint32_t busy;
    bool asleep;
    uint64_t last_touch;        // uptime of the last touch (display_idle_ms is the difference); 0 = at boot
    char focus[MS_FOCUS_MAX];   // what the carousel shows

    // the dial
    uint64_t t;                 // ms since the boot
    bool reset_sw;              // how the last reset happened (the flap record is believed after a software reset only)
    ms_reset_t reset_cls;       // the same reset as the crash record sees it (zero: power-on)
    int refuse_restarts;        // the final busy check inside the restart op finds the dial busy this many times
    int attempts;               // how many times the restart op was called (accepted or refused)
    uint64_t attempt_t[256];    // the uptime of each attempt (the first 256)
    bool reload;                // mode_glue_settings_changed(): the task re-reads wl_mode and stay_wl at its next step
    int glitch_reads;           // this many reads of wl_mode land inside a config_net_set_mode write (the marker is off): they see "no target"
    bool crash_usb;             // this boot took USB because of the panic loop
    uint8_t crashes;            // the crash count this boot started with
    ms_mode_t running;
    mg_t mg;
    bool probe_installed;
    bool notice_shown;
    char restored_focus[MS_FOCUS_MAX];
    uint64_t last_hello, last_welcome, last_daemon_byte;
    bool have_welcome, have_daemon_byte;
    uint64_t probe_since;

    // counters over the whole run
    int boots, reboots;
    ms_mode_t reboot_to[64];
    uint64_t reboot_uptime[64]; // the uptime at each reboot: no automatic switch before MS_T_DWELL_MS
    uint64_t wall;              // wall clock over the whole run, across reboots
    int probe_calls, notice_calls;
    int held_calls; uint32_t held_busy; ms_mode_t held_target; bool held_asleep;   // the held-switch log hook (mg_ops_t.held)
    char order[32];             // the reboot sequence as letters: L(og) S(eal) B(oot_usb) R(estart); r = restart refused (busy again), U = undo
    bool store_fails;
} dial_t;

static dial_t D;

// config_load_wl_effective(), or the forced value of a BLE test.
static uint8_t wl_eff(void) { return D.wl_force ? D.wl_force : config_load_wl_effective(); }

static void op_probe(void *c, bool on) { dial_t *d = c; d->probe_calls++; d->probe_installed = on; if (on) { d->have_welcome = false; d->last_hello = 0; d->probe_since = d->t; } }
static void op_notice(void *c, bool on) { dial_t *d = c; d->notice_calls++; d->notice_shown = on; }
static void op_held(void *c, ms_mode_t target, uint32_t busy, bool asleep) { dial_t *d = c; d->held_calls++; d->held_busy = busy; d->held_target = target; d->held_asleep = asleep; }
static void op_clear(void *c) { (void)c; mode_record_clear_flaps(); }
static void op_log(void *c, ms_mode_t target)
{
    dial_t *d = c;
    (void)target;
    if (strlen(d->order) + 8 >= sizeof d->order) d->order[0] = 0;   // a test that refuses the restart for ever keeps only the newest attempts
    strcat(d->order, "L");
}
static void op_seal(void *c, uint8_t flaps) { dial_t *d = c; strcat(d->order, "S"); mode_record_save(d->focus, flaps); }
static bool op_store(void *c, bool v) { dial_t *d = c; strcat(d->order, "B"); return !d->store_fails && config_save_boot_usb(v); }
// The real op re-checks the busy gate right before esp_restart() and says false when the dial turned busy meanwhile.
static bool op_restart(void *c, ms_mode_t target)
{
    dial_t *d = c;
    if (d->attempts < 256) d->attempt_t[d->attempts] = d->t;
    d->attempts++;
    if (d->refuse_restarts > 0) { d->refuse_restarts--; strcat(d->order, "r"); return false; }
    strcat(d->order, "R");
    d->reboot_to[d->reboots < 64 ? d->reboots : 63] = target;
    d->reboot_uptime[d->reboots < 64 ? d->reboots : 63] = d->t;
    d->reboots++;
    d->reset_sw = true;
    d->reset_cls = MS_RESET_HOLD;
    return true;
}
// A refused restart puts back what the dial HELD before the sequence wrote boot_usb (the core read it through load_boot_usb), not the running
// mode's value: after a radio fallback (Wi-Fi chosen, USB running) those differ.
static void op_undo(void *c, bool boot_usb_before, uint8_t flaps)
{
    dial_t *d = c;
    strcat(d->order, "U");
    config_save_boot_usb(boot_usb_before);
    mode_record_save(NULL, flaps);
}
static void op_clear_crashes(void *c) { (void)c; mode_record_crash_clear(); }
static bool op_load_boot_usb(void *c) { (void)c; return config_load_boot_usb(); }

static const mg_ops_t OPS = {&D, op_probe, op_notice, op_clear, op_log, op_seal, op_store, op_restart, op_clear_crashes, op_undo, op_load_boot_usb, op_held};

static bool sof_now(void)
{
    if (!D.cable) return false;
    if (D.sof_late && D.t < D.sof_late_ms) return false;
    return true;
}

// One boot, from the reset to the first step. Returns the mode it started in.
static ms_mode_t boot(void)
{
    D.t = 0;
    D.boots++;
    D.running = MS_MODE_USB;
    D.probe_installed = D.notice_shown = false;
    D.have_welcome = D.have_daemon_byte = false;
    D.last_welcome = D.last_daemon_byte = 0;
    D.restored_focus[0] = 0;

    mg_boot_in_t in;
    memset(&in, 0, sizeof in);
    ms_record_view_t view;
    in.record_ok = mode_record_boot(D.reset_sw, &view);
    in.record = view;
    in.crash_trip = mode_record_crash_boot(D.reset_cls, &in.crashes);   // reads the crash record; it is sealed again once the mode is known
    in.wl_effective = wl_eff();
    in.stay_wl = config_load_stay_wl();
    in.boot_usb = config_load_boot_usb();
    // The glue waits until MS_SOF_SETTLE_MS of uptime, then reads the flag once.
    D.t = MS_SOF_SETTLE_MS;
    in.sof = sof_now();
    in.sof_read_ms = (uint32_t)D.t;
    mg_boot_out_t out;
    mg_boot(&in, &out);
    assert(!out.sof_ignored);   // the simulator waits, so the reading is never too early
    if (out.write_boot_usb) assert(config_save_boot_usb(out.boot_usb_new) || D.store_fails);
    strcpy(D.restored_focus, out.focus);
    mode_record_crash_seal(out.crashes, out.mode);   // at once: a crash while the radio comes up is counted against the mode that was being started
    D.crash_usb = out.crash_usb;
    D.crashes = out.crashes;

    // A wireless mode needs its radio; if it does not start the dial runs on USB in the same boot (net_link_start returns false)
    D.running = (out.mode != MS_MODE_USB && !D.radio_fails) ? out.mode : MS_MODE_USB;
    mode_record_crash_seal(out.crashes, D.running);
    mg_init(&D.mg, D.running, out.flaps, wl_eff(), config_load_stay_wl());
    mg_note_crashes(&D.mg, out.crashes);
    D.reset_sw = false;
    D.reset_cls = MS_RESET_POWERON;
    D.order[0] = 0;
    return D.running;
}

static void put_u8(const char *key, uint8_t v);
// Run the dial for `ms` of uptime in 250 ms steps, or until it reboots. Returns true if it rebooted.
static bool run(uint64_t ms)
{
    const uint64_t end = D.t + ms;
    while (D.t < end) {
        D.t += MS_STEP_MS;
        D.wall += MS_STEP_MS;
        if (D.running == MS_MODE_USB) {
            if (D.cable && D.daemon) { D.have_daemon_byte = true; D.last_daemon_byte = D.t; }
        } else if (D.probe_installed && sof_now()) {
            // the probe says hello at once and every 2 s; a daemon answers each
            if (D.last_hello == 0 || D.t - D.last_hello >= 2000) {
                D.last_hello = D.t;
                if (D.daemon) { D.have_welcome = true; D.last_welcome = D.t; }
            }
        }
        mg_world_t w;
        memset(&w, 0, sizeof w);
        w.now_ms = D.t;
        w.sof = sof_now();
        w.session_up = D.running == MS_MODE_USB && D.cable && D.daemon;
        w.have_daemon_byte = D.have_daemon_byte;
        w.last_daemon_byte_ms = D.last_daemon_byte;
        w.have_welcome = D.have_welcome;
        w.last_welcome_ms = D.last_welcome;
        w.busy = D.busy;
        w.asleep = D.asleep;
        w.idle_ms = (uint32_t)(D.t - D.last_touch);
        const int before = D.reboots;
        // The glue task, each loop: re-read wl_mode and stay_wl when asked (mode_glue_settings_changed) or while a drop to "no target" awaits its
        // confirmation (mg_apply_settings). A read that lands inside a config_net_set_mode write sees the marker off, which reads as "no target".
        if (D.reload || D.mg.wl_pending) {
            D.reload = false;
            uint8_t wl;
            if (D.glitch_reads > 0) {
                D.glitch_reads--;
                nvs_handle_t h;
                assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK && nvs_erase_key(h, "wl_ver") == ESP_OK);   // marker_off
                wl = config_load_wl_effective();
                put_u8("wl_ver", 1);                                                                           // marker_on
            } else {
                wl = wl_eff();
            }
            mg_apply_settings(&D.mg, wl, config_load_stay_wl(), D.t);
        }
        mg_step(&D.mg, &w, &OPS);
        if (D.reboots != before) return true;
    }
    return false;
}

// Run for `wall_ms` of wall clock across reboots (each reboot starts the next boot). Returns the number of reboots.
static int run_wall(uint64_t wall_ms)
{
    const uint64_t start = D.wall;
    int reboots = 0;
    while (D.wall - start < wall_ms) {
        if (run(wall_ms - (D.wall - start))) { reboots++; boot(); }
    }
    return reboots;
}
// The bound the flap guard gives: three automatic switches in a row, then none until 10 minutes of uptime in one mode; and none before 60 s.
static void assert_bounded(int reboots, uint64_t wall_ms)
{
    assert(reboots <= (int)MS_FLAP_CAP * (int)(wall_ms / MS_FLAP_CLEAR_MS + 1));
    for (int i = 0; i < D.reboots && i < 64; i++) assert(D.reboot_uptime[i] >= MS_T_DWELL_MS);
}

static void fresh(void)
{
    host_nvs_reset();
    memset(&D, 0, sizeof D);
    strcpy(D.focus, "agent-12");
    memset(mode_record_raw_for_test(), 0, sizeof(ms_record_t));
    memset(mode_record_crash_raw_for_test(), 0, sizeof(ms_crash_t));
}
// The dial resets by itself the abnormal way (panic, watchdog, brown-out) and boots again. The flap record is not believed after one (as before);
// the crash record is.
static ms_mode_t crash_and_boot(void)
{
    D.reset_sw = false;
    D.reset_cls = MS_RESET_CRASH;
    return boot();
}
static void provision(long mode) { assert(config_net_set("home-net", "correct horse battery", "192.168.1.20", 18480, KEY, mode) == CFG_NET_OK); }
static void put_u8(const char *key, uint8_t v) { nvs_handle_t h; assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK); assert(nvs_set_u8(h, key, v) == ESP_OK); }

// ── the boot plan ───────────────────────────────────────────────────────────────────────────────────
static mg_boot_out_t plan(uint8_t wl, bool stay, bool boot_usb, bool sof, uint32_t sof_ms)
{
    mg_boot_in_t in;
    memset(&in, 0, sizeof in);
    in.wl_effective = wl;
    in.stay_wl = stay;
    in.boot_usb = boot_usb;
    in.sof = sof;
    in.sof_read_ms = sof_ms;
    mg_boot_out_t out;
    mg_boot(&in, &out);
    return out;
}

static void test_boot_plan(void)
{
    mg_boot_out_t o;
    o = plan(0, false, false, false, 50);  assert(o.mode == MS_MODE_USB);                       // wl unset: USB
    o = plan(0, true, false, false, 50);   assert(o.mode == MS_MODE_USB);                       // stay_wl with no target is nothing
    o = plan(0, false, true, true, 50);    assert(o.mode == MS_MODE_USB && !o.write_boot_usb);
    o = plan(1, false, false, false, 50);  assert(o.mode == MS_MODE_WIFI && !o.write_boot_usb);  // the ordinary Wi-Fi boot
    o = plan(1, false, true, true, 50);    assert(o.mode == MS_MODE_USB && !o.write_boot_usb);   // the switch's own reboot, cable still in
    o = plan(1, false, true, false, 50);   assert(o.mode == MS_MODE_WIFI && o.write_boot_usb && !o.boot_usb_new);   // cold start on a charger
    o = plan(1, true, true, true, 50);     assert(o.mode == MS_MODE_WIFI && o.write_boot_usb);  // stay_wl wins
    o = plan(1, false, true, true, 5);     assert(o.mode == MS_MODE_WIFI && o.sof_ignored);     // too early to believe the flag
    // B8a: Bluetooth (2) is a target like Wi-Fi: the same table, the same boot_usb bookkeeping, and nothing "unsupported" about it
    o = plan(2, false, false, false, 50);  assert(o.mode == MS_MODE_BLE && !o.unsupported && !o.write_boot_usb);   // the ordinary Bluetooth boot
    o = plan(2, false, false, true, 50);   assert(o.mode == MS_MODE_BLE && !o.unsupported && !o.write_boot_usb);   // cable in, boot_usb says Bluetooth: row 4
    o = plan(2, false, true, true, 50);    assert(o.mode == MS_MODE_USB && !o.unsupported && !o.write_boot_usb);   // the switch's own reboot, cable still in
    o = plan(2, false, true, false, 50);   assert(o.mode == MS_MODE_BLE && o.write_boot_usb && !o.boot_usb_new);   // cold start on a charger
    o = plan(2, true, false, false, 50);   assert(o.mode == MS_MODE_BLE && !o.unsupported);
    o = plan(2, true, true, true, 50);     assert(o.mode == MS_MODE_BLE && o.write_boot_usb);                     // stay_wl wins
    o = plan(2, false, true, true, 5);     assert(o.mode == MS_MODE_BLE && o.sof_ignored);                        // too early to believe the flag
    // a number that is neither 0, 1 nor 2 (a corrupt byte, a later build) has no transport: USB, and "unsupported" says so. 0 is no target, not unsupported.
    o = plan(0, false, false, false, 50);  assert(o.mode == MS_MODE_USB && !o.unsupported);
    for (unsigned v = 3; v <= 255; v++) {
        for (unsigned bits = 0; bits < 8; bits++) {
            o = plan((uint8_t)v, bits & 1, bits & 2, bits & 4, 50);
            assert(o.mode == MS_MODE_USB && o.unsupported && o.boot_usb_new);
        }
    }
    // the record rides along
    mg_boot_in_t in;
    memset(&in, 0, sizeof in);
    in.wl_effective = 1;
    in.record_ok = true;
    in.record.flaps = 2;
    strcpy(in.record.focus, "agent-9");
    mg_boot_out_t out;
    mg_boot(&in, &out);
    assert(out.flaps == 2 && strcmp(out.focus, "agent-9") == 0);
    in.record_ok = false;
    mg_boot(&in, &out);
    assert(out.flaps == 0 && out.focus[0] == 0);   // an untrusted record is zeros whatever its view says
}

// ── the reboot sequence ─────────────────────────────────────────────────────────────────────────────
static void test_reboot_sequence_order(void)
{
    // A Wi-Fi dial with a cable and a daemon, 70 s after the boot (past the dwell).
    fresh();
    provision(1);
    D.cable = D.daemon = false;
    boot();
    assert(D.running == MS_MODE_WIFI);
    assert(!run(70000));
    D.cable = D.daemon = true;
    assert(run(8000));
    assert(strcmp(D.order, "LSBR") == 0);               // log, seal, store, THEN restart
    assert(D.reboot_to[0] == MS_MODE_USB);
    assert(config_load_boot_usb());                      // stored before the restart: the next boot does not undo the switch
    ms_record_view_t v;
    assert(mode_record_boot(true, &v));
    assert(v.flaps == 1 && strcmp(v.focus, "agent-12") == 0);
    assert(!D.notice_shown);
}

// ── S1: the point of the task ───────────────────────────────────────────────────────────────────────
static void test_wifi_dial_comes_back_to_usb_and_stays(void)
{
    fresh();
    provision(1);
    boot();
    assert(D.running == MS_MODE_WIFI);
    assert(!run(120000));                                 // two minutes on Wi-Fi, nothing on the cable
    assert(!D.probe_installed);                          // no SOF: the probe costs nothing
    D.cable = D.daemon = true;                           // the user plugs the dial into a PC that runs the daemon
    assert(run(10000));                                  // within 10 s: 5 s of welcomes, plus the step
    assert(D.reboot_to[0] == MS_MODE_USB);
    // the next boot, cable still in
    assert(boot() == MS_MODE_USB);
    assert(strcmp(D.restored_focus, "agent-12") == 0);   // the carousel is put back
    assert(!run(20 * 60 * 1000));                        // twenty minutes: USB mode with a live daemon never leaves
    assert(D.reboots == 1);
    // and the count of switches in a row is cleared by the 10 minutes in one mode
    ms_record_view_t v;
    assert(mode_record_boot(true, &v));
    assert(v.flaps == 0);
}

static void test_a_daemon_that_stops_does_not_strand_a_usb_dial(void)
{
    // USB mode, cable in, daemon gone: after 60 s without a daemon byte the dial goes back to its wireless mode (and the probe brings it back
    // when the daemon returns). T_DWELL is 60 s too, so the switch is due at 60 s of uptime at the earliest.
    fresh();
    provision(1);
    D.cable = D.daemon = true;
    D.reset_sw = true;
    config_save_boot_usb(true);
    assert(boot() == MS_MODE_USB);
    assert(!run(30000));
    D.daemon = false;
    assert(!run(50000));                                  // 80 s of uptime, 50 s without a daemon byte
    assert(run(15000));                                   // 60 s without one
    assert(D.reboot_to[0] == MS_MODE_WIFI);
    assert(!config_load_boot_usb());                      // stored: the next boot does not walk back into USB
    assert(boot() == MS_MODE_WIFI);
}

// ── S2: the SOF flag is late after the reboot ───────────────────────────────────────────────────────
static void test_late_sof_is_a_bounded_detour(void)
{
    // After the switch's reboot the SOF flag is not up at the 20 ms read (it comes up 1 s later): the boot_usb row misses, the dial starts Wi-Fi, the
    // probe sees the host and switches again. A detour each time. The dwell (60 s) and the flap cap (3, then 10 minutes) make it a bounded one.
    fresh();
    provision(1);
    boot();
    D.cable = D.daemon = true;
    D.sof_late = true;
    D.sof_late_ms = 1000;
    const uint64_t wall = 30 * 60 * 1000;
    const int reboots = run_wall(wall);
    assert(reboots >= 3);                                  // it does keep trying: a late flag is not a reason to give up
    assert_bounded(reboots, wall);
}

static void test_flap_cap_stops_a_boot_loop(void)
{
    // Same cable, and the flag is never up at the read, so every boot is Wi-Fi and every Wi-Fi boot switches again. The RTC record counts across
    // the reboots: three, then it holds until 10 minutes of uptime in one mode.
    fresh();
    provision(1);
    boot();
    D.cable = D.daemon = true;
    D.sof_late = true;
    D.sof_late_ms = 1000;
    int reboots = 0;
    for (int i = 0; i < 3; i++) {
        assert(run(8 * 60 * 1000));
        reboots++;
        boot();
    }
    assert(reboots == (int)MS_FLAP_CAP);
    assert(!run(8 * 60 * 1000));                           // the fourth is held back
    assert(!run(90 * 1000));                               // 9.5 minutes of uptime: still held
    assert(run(90 * 1000));                                // past 10 minutes the count is cleared and it tries again
}

// ── S3: NVS refuses the write ───────────────────────────────────────────────────────────────────────
static void test_boot_usb_write_failure_is_bounded(void)
{
    // NVS refuses the boot_usb write: the Wi-Fi boot after each switch finds boot_usb = 0 and starts Wi-Fi again. A detour each time, and the same bound.
    fresh();
    provision(1);
    boot();
    D.cable = D.daemon = true;
    D.store_fails = true;
    const uint64_t wall = 30 * 60 * 1000;
    const int reboots = run_wall(wall);
    assert(reboots >= 3);
    assert_bounded(reboots, wall);
}

// ── S4/S5/S6: nothing stored strands a dial in a wireless mode it cannot leave, or in USB with no target ──
static void never_leaves_usb(const char *why)
{
    ms_mode_t m = boot();
    if (m != MS_MODE_USB) { fprintf(stderr, "%s: booted %s\n", why, ms_mode_name(m)); assert(0); }
    D.cable = false;                                      // cable out, no daemon, an hour: the rules that would send a USB dial to a wireless target
    const bool left = run(60 * 60 * 1000);
    if (left) { fprintf(stderr, "%s: left USB for %s\n", why, ms_mode_name(D.reboot_to[0])); assert(0); }
}

static void test_no_target_means_usb_for_good(void)
{
    // wl_mode written with no credentials
    fresh(); put_u8("wl_mode", 1);                         never_leaves_usb("wl_mode with no credentials");
    fresh(); put_u8("wl_mode", 1); put_u8("stay_wl", 1);   never_leaves_usb("wl_mode and stay_wl with no credentials");
    // a corrupt wl_mode on a whole configuration
    static const uint8_t bad[] = {2, 3, 7, 255};
    for (unsigned i = 0; i < sizeof bad; i++) {
        fresh(); provision(1); put_u8("wl_mode", bad[i]);  never_leaves_usb("corrupt wl_mode");
        fresh(); provision(1); put_u8("wl_mode", bad[i]); put_u8("stay_wl", 1); never_leaves_usb("corrupt wl_mode with stay_wl");
    }
    // wl_mode off
    fresh(); provision(0);                                  never_leaves_usb("wl_mode 0");
    fresh(); provision(0); put_u8("stay_wl", 1);            never_leaves_usb("wl_mode 0 with stay_wl");
    // the marker lost
    fresh(); provision(1); nvs_handle_t h; assert(nvs_open("pair", NVS_READWRITE, &h) == ESP_OK); nvs_erase_key(h, "wl_ver");
    never_leaves_usb("marker lost");
    // forgotten network, then a stale stay_wl cannot come back
    fresh(); provision(1); config_save_stay_wl(true); assert(config_net_clear() == CFG_NET_OK);
    never_leaves_usb("net.clear");
    provision(1);                                           // provisioned again: stay_wl is gone, so USB mode with a daemon stays USB
    assert(!config_load_stay_wl());
    // factory reset
    fresh(); provision(1); config_save_stay_wl(true); config_save_boot_usb(true); assert(config_clear_all());
    never_leaves_usb("factory reset");
}

static void test_corrupt_flags_read_as_defaults(void)
{
    // stay_wl = 7 is not "stay in Wi-Fi for ever": it reads false and the ordinary rules apply, including the probe
    fresh();
    provision(1);
    put_u8("stay_wl", 7);
    put_u8("boot_usb", 7);
    assert(boot() == MS_MODE_WIFI);
    D.cable = D.daemon = true;
    assert(run(90000));                                   // the probe was on and the dial came back to USB
    assert(D.reboot_to[0] == MS_MODE_USB);
}

// ── S7: the radio does not come up ──────────────────────────────────────────────────────────────────
static void test_radio_failure_falls_back_to_usb_in_the_same_boot(void)
{
    fresh();
    provision(1);
    D.radio_fails = true;
    assert(boot() == MS_MODE_USB);                         // net_link_start returned false: USB mode, a working link, "Not connected" if no daemon
    D.cable = D.daemon = true;
    assert(!run(30 * 60 * 1000));                          // with a daemon on the cable it stays
    // with no daemon it tries Wi-Fi again after a minute, fails again, and the cap bounds the retries
    fresh();
    provision(1);
    D.radio_fails = true;
    boot();
    D.cable = false;
    const uint64_t wall = 20 * 60 * 1000;
    const int reboots = run_wall(wall);
    assert(reboots >= 3);
    assert_bounded(reboots, wall);                         // 3 per 10 minutes at the most
}

// ── the probe, the notice, the busy gate ────────────────────────────────────────────────────────────
static void test_probe_edges(void)
{
    fresh();
    provision(1);
    boot();
    assert(D.probe_calls == 0);
    assert(!run(5000));
    D.cable = true;                                        // a host, but no daemon
    assert(!run(1000));
    assert(D.probe_installed && D.probe_calls == 1);       // installed once, not on every step
    assert(!run(60000));
    assert(D.probe_calls == 1);
    D.cable = false;
    assert(!run(1000));
    assert(!D.probe_installed && D.probe_calls == 2);      // uninstalled once when the SOF goes
    // stay_wl: the probe is never wanted
    mg_set_settings(&D.mg, 1, true);
    D.cable = true;
    assert(!run(5000));
    assert(!D.probe_installed && D.probe_calls == 2);
}

static void test_pc_without_a_daemon_does_not_switch(void)
{
    fresh();
    provision(1);
    boot();
    D.cable = true;
    D.daemon = false;
    assert(!run(60 * 60 * 1000));
    assert(D.reboots == 0);
    assert(D.probe_installed);                             // it keeps asking, at the cost of the probe's RAM, and never switches
}

static void test_charger_never_probes(void)
{
    fresh();
    provision(1);
    config_save_boot_usb(true);
    D.reset_sw = false;
    assert(boot() == MS_MODE_WIFI);                        // cold start on a charger with boot_usb stale: SOF false
    assert(!run(60 * 60 * 1000));
    assert(D.probe_calls == 0 && D.reboots == 0);
}

static void test_busy_dial_waits_and_shows_the_notice(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_VOICE;
    assert(!run(30000));                                   // due after 5 s, held by the voice turn
    assert(D.notice_shown && D.reboots == 0);
    assert(!run(30000));
    D.busy = 0;
    assert(run(MS_STEP_MS * 2));                           // released: within a step or two
    assert(!D.notice_shown);                               // the notice is gone as the switch goes
    assert(strcmp(D.order, "LSBR") == 0);
    // asleep: silent, no notice
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_READER;
    D.asleep = true;
    assert(run(10000));
    assert(D.notice_calls == 0);
    // an update or an upload blocks even a sleeping dial
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_PET_UPLOAD;
    D.asleep = true;
    assert(!run(60000));
    D.busy = 0;
    assert(run(MS_STEP_MS * 2));
}

// Review Low 3 of 2026-11-01: nothing in the log said what held a due switch (a replug that took 297 s was only "plausible" from the screen-off timer).
// The glue is told when a due switch is first held and each time the busy mask changes, once per hold: not on every 250 ms step.
static void test_a_held_switch_is_logged_once_and_on_a_change(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_VOICE;
    assert(!run(30000));                                   // due after 5 s, held by the voice turn for 25 s of steps
    assert(D.held_calls == 1 && D.held_busy == MS_BUSY_VOICE && D.held_target == MS_MODE_USB && !D.held_asleep);
    assert(!run(30000));
    assert(D.held_calls == 1);                             // the same mask: not logged again
    D.busy = MS_BUSY_VOICE | MS_BUSY_DRAFT;
    assert(!run(2000));
    assert(D.held_calls == 2 && D.held_busy == (MS_BUSY_VOICE | MS_BUSY_DRAFT));
    D.busy = MS_BUSY_DRAFT;                                // a row cleared: a change, so a line
    assert(!run(2000));
    assert(D.held_calls == 3 && D.held_busy == MS_BUSY_DRAFT);
    D.busy = 0;
    assert(run(MS_STEP_MS * 2));                           // released: the switch goes, and says nothing more
    assert(D.held_calls == 3);

    // a hold that ended (the cable was pulled) and a later one are two holds: each is logged
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_DRAFT;
    assert(!run(10000));
    assert(D.held_calls == 1);
    D.cable = false;
    assert(!run(3000));
    D.cable = D.daemon = true;
    assert(!run(10000));
    assert(D.held_calls == 2 && D.held_busy == MS_BUSY_DRAFT);

    // a sleeping dial switches silently past a soft row (nothing is held, nothing is said) but is held by an upload, and that is said
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.asleep = true;
    D.busy = MS_BUSY_READER;
    assert(run(10000));
    assert(D.held_calls == 0);
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.asleep = true;
    D.busy = MS_BUSY_PET_UPLOAD;
    assert(!run(10000));
    assert(D.held_calls == 1 && D.held_asleep && D.held_busy == MS_BUSY_PET_UPLOAD);
}

// User decision 2026-11-01: a Wi-Fi dial plugged in while awake switches to USB 30 s after the last touch, with the notice up meanwhile, instead of
// waiting for the screen to go off (297 s on the bench). The glue feeds the idle time (display_idle_ms); voice and the other strong rows still hold.
static void test_a_replug_with_a_reader_open_switches_after_30_s_of_idle(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_READER;
    D.last_touch = D.t;
    const uint64_t touched = D.t;
    assert(!run(MS_T_ENTER_MS + 5000));                    // due after the 5 s of welcomes, held by the reader
    assert(D.notice_shown && D.reboots == 0 && D.held_calls == 1 && D.held_busy == MS_BUSY_READER);
    D.last_touch = D.t;                                    // a touch restarts the 30 s
    const uint64_t second = D.t;
    assert(!run(MS_T_IDLE_MS - MS_STEP_MS));
    assert(D.notice_shown && D.reboots == 0);
    assert(run(MS_STEP_MS * 2));
    assert(D.reboot_to[0] == MS_MODE_USB && !D.notice_shown);
    assert(D.reboot_uptime[0] >= second + MS_T_IDLE_MS && D.reboot_uptime[0] <= second + MS_T_IDLE_MS + MS_STEP_MS);
    assert(D.reboot_uptime[0] > touched + MS_T_IDLE_MS);
    assert(strcmp(D.order, "LSBR") == 0 && D.held_calls == 1);

    // voice still holds, however long the glass is left alone
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_VOICE | MS_BUSY_READER;
    D.last_touch = D.t;
    assert(!run(120000));
    assert(D.notice_shown && D.reboots == 0);
    D.busy = MS_BUSY_READER;                               // the turn ended; the glass has been alone for 2 minutes
    assert(run(MS_STEP_MS * 2));
    assert(D.reboot_to[0] == MS_MODE_USB);

    // a USB dial leaving for Wi-Fi keeps the old rule: a reader holds it however long the glass is left alone
    fresh();
    provision(1);
    boot();
    D.cable = D.daemon = true;
    assert(run(70000) && D.reboot_to[0] == MS_MODE_USB);   // to USB with the cable in
    assert(boot() == MS_MODE_USB);
    assert(!run(70000));
    D.busy = MS_BUSY_READER;
    D.cable = false;                                       // the cable is pulled: a USB dial leaves for Wi-Fi after 10 s, if nothing holds it
    D.last_touch = 0;
    assert(!run(120000));
    assert(D.notice_shown && D.reboots == 1);
}

// S7 (user decision 3 of 2026-11-01): after the 30 minute power-off the next PWR-button boot must come up in Wi-Fi mode, as it does today. The power-off
// writes only its own record (power_off.c), so what the boot reads is what the running dial left: wl_mode, stay_wl and boot_usb. The power-off is a
// power cut for the RTC memory (a power-on reset), so the crash and flap records start at zero. (While the power-off runs the writers are frozen and the
// restart gate skips a due switch: test_the_restart_gate_sequence.)
static void test_the_boot_after_a_power_off_is_wifi_mode(void)
{
    // a plain Wi-Fi dial, a long idle, the power goes
    fresh();
    provision(1);
    assert(boot() == MS_MODE_WIFI && D.running == MS_MODE_WIFI);
    assert(!run(31 * 60 * 1000) && D.reboots == 0);
    D.reset_sw = false;
    D.reset_cls = MS_RESET_POWERON;                        // the PMIC cut the power: everything in RTC memory is gone
    memset(mode_record_raw_for_test(), 0, sizeof(ms_record_t)); memset(mode_record_crash_raw_for_test(), 0, sizeof(ms_crash_t));
    assert(boot() == MS_MODE_WIFI && D.running == MS_MODE_WIFI);
    assert(D.crashes == 0 && !D.crash_usb);

    // Keep Wi-Fi on: the same
    fresh();
    provision(1);
    assert(config_save_stay_wl(true));
    assert(boot() == MS_MODE_WIFI);
    assert(!run(31 * 60 * 1000));
    memset(mode_record_raw_for_test(), 0, sizeof(ms_record_t)); memset(mode_record_crash_raw_for_test(), 0, sizeof(ms_crash_t));
    D.reset_sw = false;
    D.reset_cls = MS_RESET_POWERON;
    assert(boot() == MS_MODE_WIFI);

    // a dial that went to Wi-Fi from USB (boot_usb was written 0 by that switch) and then ran 40 minutes on the battery
    fresh();
    provision(1);
    assert(config_save_boot_usb(true));                    // it had been in USB mode
    D.cable = D.daemon = true;
    assert(boot() == MS_MODE_USB);
    assert(!run(70000));
    D.cable = false;                                       // unplugged: after 10 s it leaves for Wi-Fi
    assert(run(30000) && D.reboot_to[0] == MS_MODE_WIFI);
    assert(boot() == MS_MODE_WIFI && !config_load_boot_usb());
    assert(!run(40 * 60 * 1000));
    memset(mode_record_raw_for_test(), 0, sizeof(ms_record_t)); memset(mode_record_crash_raw_for_test(), 0, sizeof(ms_crash_t));
    D.reset_sw = false;
    D.reset_cls = MS_RESET_POWERON;
    assert(boot() == MS_MODE_WIFI);                        // the PWR button, no cable: Wi-Fi, as before
}

static void test_the_notice_goes_when_the_reason_goes(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.busy = MS_BUSY_DRAFT;
    assert(!run(10000));
    assert(D.notice_shown);
    D.cable = false;                                       // the user pulled the cable: the switch is no longer due
    assert(!run(3000));
    assert(!D.notice_shown);
}

static void test_power_cut_forgets_the_count(void)
{
    fresh();
    provision(1);
    mode_record_save("agent-3", 2);
    D.reset_sw = false;                                    // the dial lost power since: the record is not believed
    boot();
    ms_record_view_t v;
    mode_record_boot(false, &v);
    assert(v.flaps == 0 && v.focus[0] == 0);
    assert(D.restored_focus[0] == 0);
}

static void test_flaps_clear_after_ten_minutes(void)
{
    fresh();
    provision(1);
    mode_record_save("", 2);
    D.reset_sw = true;
    boot();
    assert(!run(9 * 60 * 1000));
    ms_record_view_t v;
    assert(mode_record_boot(true, &v) && v.flaps == 2);
    assert(!run(2 * 60 * 1000));                           // past 10 minutes of uptime
    assert(mode_record_boot(true, &v) && v.flaps == 0);    // re-sealed at zero
}

static void test_nothing_happens_after_the_restart(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    assert(run(10000));
    const int n = D.reboots;
    const size_t len = strlen(D.order);
    mg_world_t w;
    memset(&w, 0, sizeof w);
    w.now_ms = 1000000;
    w.sof = true;
    w.have_welcome = true;
    w.last_welcome_ms = 1000000;
    for (int i = 0; i < 50; i++) mg_step(&D.mg, &w, &OPS);   // esp_restart() has not taken effect yet in a real dial: more steps may arrive
    assert(D.reboots == n && strlen(D.order) == len);
}

// ── the restart can be refused at the last moment (review finding 2) ────────────────────────────────
static void test_restart_refused_when_the_dial_turns_busy_again(void)
{
    // The glue logs, seals, stores and flushes, and that takes time (the display lock, an NVS commit, 150 ms of log flush). The user can start a
    // voice turn in that time. The restart op looks at the busy gate once more, right before esp_restart(), and says no; the core then undoes what
    // the sequence wrote (boot_usb, the raised flap count) and tries again on a later step.
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.refuse_restarts = 1000;
    assert(!run(30000));                                  // due after 5 s; every attempt finds the dial busy again
    assert(D.reboots == 0 && !D.mg.done);                 // not done: it will retry
    assert(strchr(D.order, 'r') && D.order[strlen(D.order) - 1] == 'U');
    assert(!config_load_boot_usb());                      // boot_usb is the running mode's again (Wi-Fi)
    ms_record_view_t v;
    assert(mode_record_boot(true, &v) && v.flaps == 0);   // the raised flap count was put back
    D.refuse_restarts = 0;                                // the voice turn ended
    assert(run(MG_RETRY_MAX_MS + MS_STEP_MS * 2));
    assert(D.reboots == 1 && D.order[strlen(D.order) - 1] == 'R');
    assert(strstr(D.order, "LSBR") != NULL);
    assert(config_load_boot_usb());                       // and now the switch is stored as usual
    assert(D.reboot_to[0] == MS_MODE_USB);
    // a USB dial refused the same way restores boot_usb = 1
    fresh();
    provision(1);
    config_save_boot_usb(true);
    D.cable = D.daemon = true;
    D.reset_sw = true;
    assert(boot() == MS_MODE_USB);
    assert(!run(30000));
    D.daemon = false;
    D.refuse_restarts = 1000;
    assert(!run(120000));                                 // 60 s without a daemon byte: due; refused every time
    assert(D.reboots == 0 && config_load_boot_usb());
    D.refuse_restarts = 0;
    assert(run(MG_RETRY_MAX_MS + MS_STEP_MS * 2) && D.reboot_to[0] == MS_MODE_WIFI && !config_load_boot_usb());
}

// ── W16: the Link screen's two controls reach the running glue (mode_glue_settings_changed -> mg_set_settings) ────────────────────────────
static void link_screen_changed(void) { D.reload = true; }   // the real glue re-reads on its next step (mg_apply_settings, as in run())

static void test_picking_usb_on_wifi_goes_to_usb_and_stays(void)
{
    fresh();
    provision(1);
    boot();
    assert(D.running == MS_MODE_WIFI);
    assert(!run(30000));                                  // 30 s: before the dwell; nothing on the cable
    assert(config_net_set_mode(0) == CFG_NET_OK);         // the Link screen: USB
    link_screen_changed();
    D.refuse_restarts = 1000;                             // the user is mid-action when the restart op looks again
    assert(!run(3000));
    assert(D.reboots == 0 && !D.mg.done);
    assert(strchr(D.order, 'r') && D.order[strlen(D.order) - 1] == 'U');
    assert(!config_load_boot_usb());                      // put back: the running mode is Wi-Fi
    ms_record_view_t v;
    assert(mode_record_boot(true, &v) && v.flaps == 0);
    D.refuse_restarts = 0;
    assert(run(MG_RETRY_MAX_MS + MS_STEP_MS * 2));
    assert(D.reboots == 1 && D.reboot_to[0] == MS_MODE_USB && D.reboot_uptime[0] < MS_T_DWELL_MS);   // the user's pick is not held by the dwell
    assert(strstr(D.order, "LSBR") != NULL);              // log, seal, boot_usb, restart: the one sequence
    assert(config_load_boot_usb());                       // boot_usb follows the target
    assert(boot() == MS_MODE_USB);                        // wl_mode is 0: USB, and
    assert(strcmp(D.restored_focus, "agent-12") == 0);    // the carousel comes back
    assert(D.mg.ms.flaps == 0);                           // a change by hand does not count as a flap
    assert(!run(20 * 60 * 1000));                         // it never leaves
    assert(D.reboots == 1);
    // choosing Wi-Fi again is a plain stored choice: the USB dial leaves for it as the rules say (no cable here: after T_LEAVE and the dwell)
    assert(config_net_set_mode(1) == CFG_NET_OK);
    link_screen_changed();
    assert(run(MS_T_DWELL_MS) && D.reboot_to[1] == MS_MODE_WIFI);
}

static void test_a_dial_busy_on_the_link_screen_waits_then_switches(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(5000));
    D.busy = MS_BUSY_VOICE;
    assert(config_net_set_mode(0) == CFG_NET_OK);
    link_screen_changed();
    assert(!run(20000));
    assert(D.reboots == 0 && D.notice_shown);             // "switching in a moment"
    D.busy = 0;
    assert(run(MS_STEP_MS * 2) && D.reboot_to[0] == MS_MODE_USB && !D.notice_shown);
}

static void test_keep_wifi_off_gives_a_wifi_dial_its_way_back_to_usb(void)
{
    // stay_wl ON: the dial runs Wi-Fi with the cable in and a live daemon, and never probes (so the cable alone cannot bring it back).
    fresh();
    provision(1);
    assert(config_save_stay_wl(true));
    D.cable = D.daemon = true;
    assert(boot() == MS_MODE_WIFI);
    assert(!run(5 * 60 * 1000));
    assert(!D.probe_installed && D.reboots == 0);
    // the Link screen turns Keep Wi-Fi off: the probe starts and the normal rule takes the dial to USB
    assert(config_save_stay_wl(false));
    link_screen_changed();
    assert(run(15000));
    assert(D.reboot_to[0] == MS_MODE_USB);
    assert(boot() == MS_MODE_USB);
    assert(!run(10 * 60 * 1000) && D.reboots == 1);       // with the daemon on the cable it stays
    // and turning it ON in USB sends a USB dial to Wi-Fi after the dwell, cable in (the 2026-10-31 decision)
    assert(config_save_stay_wl(true));
    link_screen_changed();
    assert(run(MS_T_DWELL_MS) && D.reboot_to[1] == MS_MODE_WIFI);
}

// ── W15e: review findings of the W15d re-review (2026-11-01) ────────────────────────────────────────
// 1. A refused restart is not retried at once: the delay doubles from MG_RETRY_BASE_MS to MG_RETRY_MAX_MS. A stuck NVS writer used to cost a
//    boot_usb commit, an undo commit and a probe stop and start about once a second for ever.
static void test_retry_delay_ladder(void)
{
    assert(mg_retry_delay_ms(0) == MG_RETRY_BASE_MS && mg_retry_delay_ms(1) == MG_RETRY_BASE_MS);
    assert(mg_retry_delay_ms(2) == 2 * MG_RETRY_BASE_MS && mg_retry_delay_ms(3) == 4 * MG_RETRY_BASE_MS && mg_retry_delay_ms(4) == 8 * MG_RETRY_BASE_MS);
    for (unsigned n = 5; n < 256; n++) assert(mg_retry_delay_ms((uint8_t)n) == MG_RETRY_MAX_MS);
    assert(MG_RETRY_BASE_MS >= 5000 && MG_RETRY_MAX_MS <= 300000 && MG_RETRY_MAX_MS >= MG_RETRY_BASE_MS);
}

static void test_a_persistent_refusal_is_rate_bounded(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.refuse_restarts = 1 << 30;                          // a writer that never finishes, or a gate that never opens
    const uint64_t hour = 3600ull * 1000;
    assert(!run(hour));
    assert(D.reboots == 0 && D.attempts >= 2);
    assert(D.attempts <= (int)(hour / MG_RETRY_MAX_MS) + 6);   // before the fix: about one attempt a second, 3,600 in the hour
    // the gaps are never below the base, never shrink, and stop growing at the cap
    uint64_t prev = 0;
    for (int i = 1; i < D.attempts && i < 256; i++) {
        const uint64_t gap = D.attempt_t[i] - D.attempt_t[i - 1];
        assert(gap + MS_STEP_MS >= MG_RETRY_BASE_MS && gap + MS_STEP_MS >= prev && gap <= MG_RETRY_MAX_MS + MS_STEP_MS);
        prev = gap;
    }
    assert(D.attempt_t[1] - D.attempt_t[0] >= MG_RETRY_BASE_MS);
    assert(D.attempt_t[1] - D.attempt_t[0] <= MG_RETRY_BASE_MS + MS_STEP_MS);
    // the dial is as it was: boot_usb is the running mode's
    assert(!config_load_boot_usb());
    // the refusal ends: the switch goes at the next attempt, which is at most MG_RETRY_MAX_MS away
    D.refuse_restarts = 0;
    assert(run(MG_RETRY_MAX_MS + MS_STEP_MS * 2) && D.reboot_to[0] == MS_MODE_USB);
}

static void test_the_retry_count_starts_again_when_the_switch_is_no_longer_due(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.cable = D.daemon = true;
    D.refuse_restarts = 1 << 30;
    assert(!run(10 * 60 * 1000));                         // the ladder has climbed to its cap
    assert(D.attempts >= 5);
    D.cable = false;                                      // the cable goes: the switch is no longer due
    assert(!run(5000));
    const int before = D.attempts;
    const uint64_t replugged = D.t;
    D.cable = true;                                       // it comes back: a fresh 5 s of welcomes, and the first attempt is not held by the old ladder
    assert(!run(20000));
    assert(D.attempts > before && D.attempt_t[before] - replugged < MG_RETRY_BASE_MS);
}

// 2. The undo puts back what NVS held before the sequence wrote boot_usb, not the running mode's value.
static void test_undo_after_a_radio_fallback_restores_the_old_boot_usb(void)
{
    fresh();
    provision(1);
    D.radio_fails = true;                                 // Wi-Fi is chosen at boot and the radio does not come up: USB runs
    assert(boot() == MS_MODE_USB);
    assert(!config_load_boot_usb());                      // the boot recorded the CHOICE (Wi-Fi), not the mode that runs
    D.refuse_restarts = 1000;
    assert(!run(120000));                                 // no cable: leaves for Wi-Fi after T_LEAVE and the dwell, and every attempt is refused
    assert(D.attempts >= 1 && D.reboots == 0);
    assert(strchr(D.order, 'r') && D.order[strlen(D.order) - 1] == 'U');
    assert(!config_load_boot_usb());                      // the old value; "running == USB" would have written 1 here
    D.refuse_restarts = 0;
    D.radio_fails = false;
    assert(run(MG_RETRY_MAX_MS + MS_STEP_MS * 2) && D.reboot_to[0] == MS_MODE_WIFI);
    assert(!config_load_boot_usb());
    // and with a stored 1 the undo puts back a 1
    fresh();
    provision(1);
    config_save_boot_usb(true);
    D.cable = D.daemon = true;
    D.reset_sw = true;
    assert(boot() == MS_MODE_USB);
    assert(!run(30000));
    D.daemon = false;
    D.refuse_restarts = 1000;
    assert(!run(120000));
    assert(D.attempts >= 1 && config_load_boot_usb());
}

// 3. The restart op's gate sequence, run against a fake store gate with the real gate's semantics (store_gate.c): freeze stops new writers and
//    waits for the ones in flight, a failed freeze opens the gate again by itself, thaw opens it. power_off.c freezes it for good before the PMIC
//    cuts the power (DEVICE_BATTERY_POWEROFF): a restart must neither thaw that nor race it.
typedef struct { bool frozen, writer_stuck, busy; int freezes, thaws, restarts, busy_looks; char order[16]; } fake_gate_t;
static bool fg_frozen(void *c) { return ((fake_gate_t *)c)->frozen; }
static bool fg_freeze(void *c)
{
    fake_gate_t *g = c;
    g->freezes++;
    strcat(g->order, "F");
    g->frozen = true;
    if (g->writer_stuck) { g->frozen = false; return false; }
    return true;
}
static void fg_thaw(void *c) { fake_gate_t *g = c; g->thaws++; strcat(g->order, "T"); g->frozen = false; }
static bool fg_blocked(void *c) { fake_gate_t *g = c; g->busy_looks++; strcat(g->order, "B"); return g->busy; }
static void fg_restart(void *c) { fake_gate_t *g = c; g->restarts++; strcat(g->order, "R"); }

static void test_the_restart_gate_sequence(void)
{
    fake_gate_t g;
    mg_gate_ops_t ops;
    // open gate, not busy: freeze, then the last look, then the restart, and nothing is thawed
    memset(&g, 0, sizeof g);
    ops = (mg_gate_ops_t){&g, fg_frozen, fg_freeze, fg_thaw, fg_blocked, fg_restart};
    assert(mg_restart_try(&ops) == MG_RESTART_GONE && !strcmp(g.order, "FBR") && g.thaws == 0 && g.frozen);
    // busy at the last look: thaw once, no restart
    memset(&g, 0, sizeof g);
    g.busy = true;
    assert(mg_restart_try(&ops) == MG_RESTART_BUSY && !strcmp(g.order, "FBT") && g.thaws == 1 && !g.frozen && g.restarts == 0);
    // a writer that will not finish: no thaw (the freeze reopened the gate itself), no look, no restart
    memset(&g, 0, sizeof g);
    g.writer_stuck = true;
    assert(mg_restart_try(&ops) == MG_RESTART_WRITER && !strcmp(g.order, "F") && g.thaws == 0 && !g.frozen && g.restarts == 0);
    // ALREADY FROZEN by the power-off sequence: the switch is skipped, the gate is left exactly as found (frozen), nothing else is touched
    memset(&g, 0, sizeof g);
    g.frozen = true;
    assert(mg_restart_try(&ops) == MG_RESTART_FROZEN && g.order[0] == 0 && g.frozen && g.freezes == 0 && g.thaws == 0 && g.busy_looks == 0 && g.restarts == 0);
    g.busy = true;                                        // busy as well: still not thawed
    assert(mg_restart_try(&ops) == MG_RESTART_FROZEN && g.frozen && g.thaws == 0);
    // missing ops never crash, and never restart
    assert(mg_restart_try(NULL) == MG_RESTART_FROZEN);
    mg_gate_ops_t none;
    memset(&none, 0, sizeof none);
    assert(mg_restart_try(&none) == MG_RESTART_FROZEN);
}

// ── W16b: review of the Link screen (2026-11-01), finding 1 ──────────────────────────────────────────
// config_net_set_mode removes the marker, commits, and writes it back. A read of wl_mode that lands in that window sees "no target". With the
// reload flag of tap 1 still set when tap 2's write begins, the 250 ms task could read inside tap 2's window, take the "user picked USB" path (which
// ignores the dwell and the flap cap) and reboot a Wi-Fi dial into USB that nobody asked for. A drop to "no target" now needs a second read at least
// MG_CONFIRM_MS later that still says so.
static void test_a_read_inside_a_write_window_does_not_send_the_dial_to_usb(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(70000));                                  // past the dwell: nothing would hold a wrong switch back
    link_screen_changed();                                // tap 1 is still waiting to be read ...
    D.glitch_reads = 1;                                   // ... and the read lands inside tap 2's write
    assert(!run(5 * 60 * 1000));
    assert(D.reboots == 0 && D.mg.wl_effective == MS_WL_WIFI && !D.mg.wl_pending);
    // two reads in a row inside windows (one step apart: less than the confirmation time): still nothing
    link_screen_changed();
    D.glitch_reads = 2;
    assert(!run(5 * 60 * 1000));
    assert(D.reboots == 0 && D.mg.wl_effective == MS_WL_WIFI && !D.mg.wl_pending);
    // and the same read at a moment when the user HAS picked USB is not lost: the second read confirms it
    assert(config_net_set_mode(0) == CFG_NET_OK);
    link_screen_changed();
    assert(!run(MS_STEP_MS));                             // the first read: seen once, not acted on
    assert(D.mg.wl_pending && D.mg.wl_effective == MS_WL_WIFI);
    assert(run(MG_CONFIRM_MS + 3 * MS_STEP_MS) && D.reboot_to[0] == MS_MODE_USB);
}

static void test_a_real_pick_of_usb_waits_only_for_its_confirmation(void)
{
    fresh();
    provision(1);
    boot();
    assert(!run(20000));                                  // before the dwell: the user's pick is not held by it
    assert(config_net_set_mode(0) == CFG_NET_OK);
    link_screen_changed();
    assert(run(MG_CONFIRM_MS + 3 * MS_STEP_MS));
    assert(D.reboot_to[0] == MS_MODE_USB && D.reboot_uptime[0] < MS_T_DWELL_MS);
    assert(D.reboot_uptime[0] >= 20000 + MG_CONFIRM_MS);  // not before the confirmation
    // a change back to Wi-Fi is never delayed: it is applied by the first read
    fresh();
    provision(1);
    assert(config_net_set_mode(0) == CFG_NET_OK);
    assert(boot() == MS_MODE_USB);
    assert(config_net_set_mode(1) == CFG_NET_OK);
    link_screen_changed();
    assert(!run(MS_STEP_MS) && D.mg.wl_effective == MS_WL_WIFI && !D.mg.wl_pending);
    // a pending drop is cancelled when the next read says Wi-Fi again
    fresh();
    provision(1);
    boot();
    assert(!run(70000));
    D.glitch_reads = 1;
    link_screen_changed();
    assert(!run(MS_STEP_MS) && D.mg.wl_pending);
    assert(!run(MS_STEP_MS) && !D.mg.wl_pending && D.mg.wl_effective == MS_WL_WIFI);
}

// ── W16c: bench finding 2026-11-01. `net.set` over the cable writes wl_mode = 1 into a dial that runs USB with no target. The glue keeps "no target:
// never leave" until it is told to read again, so the cable handler must call mode_glue_settings_changed() after its write (cable_client.c,
// net_store_task; test_mode_glue_source.py pins that). Here is the contract on the glue side, with the call and without it.
static void test_provisioning_at_runtime_needs_the_reload_and_then_leaves_on_t_leave(void)
{
    fresh();
    assert(boot() == MS_MODE_USB);                        // never provisioned: USB, no target
    assert(!run(70000));
    provision(1);                                         // net.set over the cable: the NVS now holds a whole configuration and wl_mode = 1
    D.cable = true;                                       // the cable is in while it happens
    D.daemon = true;
    assert(!run(5000));
    D.cable = D.daemon = false;                           // unplugged
    // WITHOUT the call the glue never learns: the bench saw 75 s and then a replug without a reboot; here five minutes
    assert(!run(5 * 60 * 1000) && D.reboots == 0);
    // WITH it (what net_store_task now does): it leaves for Wi-Fi when T_LEAVE has passed, and no later than that
    link_screen_changed();                                // = mode_glue_settings_changed()
    assert(run(MS_T_LEAVE_MS + MG_CONFIRM_MS + 4 * MS_STEP_MS) && D.reboot_to[0] == MS_MODE_WIFI);
    assert(D.reboot_uptime[0] >= 5 * 60 * 1000 + MS_T_LEAVE_MS);   // not before the cable had been out for T_LEAVE
}

static void test_net_clear_and_net_mode_at_runtime_are_seen_too(void)
{
    // net.mode 0 over the cable while USB runs: no target, nothing to leave to (and no reboot)
    fresh();
    provision(1);
    config_save_boot_usb(true);
    D.cable = D.daemon = true;
    D.reset_sw = true;
    assert(boot() == MS_MODE_USB);
    assert(!run(30000));
    assert(config_net_set_mode(0) == CFG_NET_OK);
    link_screen_changed();
    D.cable = D.daemon = false;
    assert(!run(5 * 60 * 1000) && D.reboots == 0);
    // net.clear: the same, and stay_wl goes with the network (config_net_clear takes it)
    assert(config_net_set_mode(1) == CFG_NET_OK && config_save_stay_wl(true));
    link_screen_changed();
    assert(config_net_clear() == CFG_NET_OK && !config_load_stay_wl());
    link_screen_changed();
    assert(!run(5 * 60 * 1000) && D.reboots == 0);
}

// ── the panic loop (user decision 2026-10-31) ───────────────────────────────────────────────────────
static void test_panic_loop_plan(void)
{
    mg_boot_in_t in;
    mg_boot_out_t o;
    memset(&in, 0, sizeof in);
    in.wl_effective = 1;
    in.crash_trip = true;
    in.crashes = 3;
    mg_boot(&in, &o);                                     // Wi-Fi would have been the choice: USB once, and boot_usb follows the choice
    assert(o.mode == MS_MODE_USB && o.crash_usb && o.write_boot_usb && o.boot_usb_new && o.crashes == 3);
    in.stay_wl = true;                                    // it beats stay_wl for this boot
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && o.crash_usb);
    in.stay_wl = false;
    in.boot_usb = true;                                   // boot_usb already says USB, no cable: row 4 would pick Wi-Fi; the trip says USB, nothing to write
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && o.crash_usb && !o.write_boot_usb);
    in.sof = true;                                        // a cable: USB anyway, so the trip changed nothing and says nothing
    in.sof_read_ms = 50;
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && !o.crash_usb);
    in.wl_effective = 0;                                  // no Wi-Fi to crash in
    in.sof = false;
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && !o.crash_usb);
    in.wl_effective = 3;                                  // a mode with no transport: USB, and not blamed on a crash
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && o.unsupported && !o.crash_usb);
    in.wl_effective = 2;                                  // B8a: Bluetooth would have been the choice, so the trip takes USB once and says so
    in.boot_usb = false;
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && !o.unsupported && o.crash_usb && o.write_boot_usb && o.boot_usb_new);
    in.stay_wl = true;                                    // it beats stay_wl for a Bluetooth dial too
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && o.crash_usb);
    in.stay_wl = false;
    in.boot_usb = true;                                   // already USB, no cable: row 4 says Bluetooth, the trip says USB, nothing to write
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && o.crash_usb && !o.write_boot_usb);
    in.sof = true;                                        // a cable: USB anyway, the trip changed nothing and says nothing
    in.sof_read_ms = 50;
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && !o.crash_usb);
    // no trip: the table decides, and the count passes through
    memset(&in, 0, sizeof in);
    in.wl_effective = 1;
    in.crashes = 2;
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_WIFI && !o.crash_usb && o.crashes == 2);
    // the flap count of the record is not touched by a trip (the cap is the cap)
    in.crash_trip = true;
    in.crashes = 3;
    in.record_ok = true;
    in.record.flaps = 2;
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && o.flaps == 2);
}

static void test_third_crash_in_wifi_takes_usb_once(void)
{
    fresh();
    provision(1);
    assert(boot() == MS_MODE_WIFI && D.crashes == 0 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 1 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 2 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 3 && D.crash_usb);   // the reason is logged by the glue from crash_usb
    assert(config_load_boot_usb());                       // boot_usb follows the boot choice
    // No cable, no daemon: the ordinary rule sends the USB dial back to Wi-Fi after the dwell. That is a software reset: the count is held.
    assert(!run(59000));
    assert(run(2000) && D.reboot_to[0] == MS_MODE_WIFI);
    assert(boot() == MS_MODE_WIFI && D.crashes == 3 && !D.crash_usb);   // held, and no second trip on a software reset
    assert(D.mg.ms.flaps == 1);                           // the switch is a flap as ever; the cap is the flap guard's business
    assert(!config_load_boot_usb());
    // the very next crash in Wi-Fi trips at once: the count was not cleared by the detour
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 4 && D.crash_usb);
}

static void test_trip_beats_stay_wl_and_the_dwell_still_applies(void)
{
    fresh();
    provision(1);
    config_save_stay_wl(true);
    assert(boot() == MS_MODE_WIFI);
    assert(crash_and_boot() == MS_MODE_WIFI);
    assert(crash_and_boot() == MS_MODE_WIFI);
    assert(crash_and_boot() == MS_MODE_USB && D.crash_usb);   // stay_wl does not keep a crash-looping dial in Wi-Fi
    assert(!run(59000));                                   // then stay_wl's own rule applies after the dwell: the dial may try Wi-Fi again
    assert(run(2000) && D.reboot_to[0] == MS_MODE_WIFI);
}

static void test_power_on_clears_the_crash_count(void)
{
    fresh();
    provision(1);
    boot();
    crash_and_boot();
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 2);
    D.reset_sw = false;
    D.reset_cls = MS_RESET_POWERON;                        // the battery was pulled
    assert(boot() == MS_MODE_WIFI && D.crashes == 0);
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 1);
}

static void test_ten_minutes_up_in_wifi_clears_the_crash_count(void)
{
    fresh();
    provision(1);
    boot();
    crash_and_boot();
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 2);
    assert(!run(9 * 60 * 1000));                           // 9 minutes is not enough ...
    uint8_t n;
    assert(!mode_record_crash_boot(MS_RESET_HOLD, &n) && n == 2);
    assert(!run(2 * 60 * 1000));                           // ... 11 minutes is
    assert(!mode_record_crash_boot(MS_RESET_HOLD, &n) && n == 0);
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 1);   // a crash after a good run is the first of a new row
}

static void test_time_in_usb_does_not_clear_the_crash_count(void)
{
    fresh();
    provision(1);
    boot();
    crash_and_boot();
    crash_and_boot();
    D.cable = D.daemon = true;                             // the tripped boot finds a cable with a daemon: USB, and it stays
    assert(crash_and_boot() == MS_MODE_USB && D.crash_usb);
    assert(!run(20 * 60 * 1000));
    uint8_t n;
    assert(!mode_record_crash_boot(MS_RESET_HOLD, &n) && n == 3);
}

static void test_a_crash_in_usb_mode_is_not_counted(void)
{
    fresh();
    provision(1);
    boot();
    crash_and_boot();
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 2);
    D.cable = D.daemon = true;                             // the user plugs in: USB (a software reset, the count is held)
    assert(run(70000) && D.reboot_to[0] == MS_MODE_USB);
    assert(boot() == MS_MODE_USB && D.crashes == 2);
    D.cable = false;                                       // the dial then crashes in USB mode and the cable is out when it comes back
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 2 && !D.crash_usb);   // held: a USB crash is nobody's Wi-Fi crash
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 3 && D.crash_usb);    // the next Wi-Fi crash is the third
}

static void test_a_crash_loop_in_wifi_is_slowed_to_one_attempt_per_dwell(void)
{
    // Wi-Fi mode that crashes 20 s after every start, no cable, an hour. Without the rule the dial reboots every few seconds for ever. With it:
    // three quick attempts, then USB for at least the 60 s dwell after every crash, so one Wi-Fi attempt per 80 s at the most.
    fresh();
    provision(1);
    boot();
    const uint64_t wall = 60 * 60 * 1000, start = D.wall;
    int wifi_runs = 0;
    unsigned crashes_seen = 0;
    while (D.wall - start < wall) {
        if (D.running == MS_MODE_WIFI) {
            wifi_runs++;
            if (!run(20000)) crash_and_boot(); else boot();
        } else if (run(10 * 60 * 1000)) {
            boot();
        }
        assert(D.crashes >= crashes_seen);                 // nothing in the loop clears the count
        crashes_seen = D.crashes;
    }
    assert(crashes_seen >= MS_CRASH_LIMIT);
    assert(wifi_runs <= (int)MS_CRASH_LIMIT + (int)(wall / (MS_T_DWELL_MS + 20000)) + 1);
    for (int i = 0; i < D.reboots && i < 64; i++) assert(D.reboot_uptime[i] >= MS_T_DWELL_MS);   // every USB stint lasted the dwell
}

#define SSID_FOR_TEST "home-net"
// ── B8a: Bluetooth is a wireless target like Wi-Fi (the glue only; the link itself is B4b) ─────────────────────────
// The sim forces the target (D.wl_force = 2) because config_store cannot store wl_mode 2 until B6. Every rule below is the Wi-Fi rule with the other number.

static void test_ble_target_is_kept_by_the_glue_state(void)
{
    mg_t g;
    mg_init(&g, MS_MODE_BLE, 0, 2, false);
    assert(g.wl_effective == MS_WL_BLE && g.ms.mode == MS_MODE_BLE);
    mg_init(&g, MS_MODE_USB, 0, 2, false);                 // USB running with a Bluetooth target: it is the target to leave to
    assert(g.wl_effective == MS_WL_BLE);
    mg_init(&g, MS_MODE_USB, 0, 3, false);                 // a number with no transport still reads as "no target"
    assert(g.wl_effective == MS_WL_UNSET);
    mg_init(&g, MS_MODE_USB, 0, 255, false);
    assert(g.wl_effective == MS_WL_UNSET);
    mg_set_settings(&g, 2, true);
    assert(g.wl_effective == MS_WL_BLE && g.stay_wl);
    mg_set_settings(&g, 7, true);
    assert(g.wl_effective == MS_WL_UNSET);
    // the Wi-Fi rows of mg_apply_settings are unchanged: Wi-Fi to "no target" waits for a second read
    mg_init(&g, MS_MODE_WIFI, 0, 1, false);
    mg_apply_settings(&g, 0, false, 1000);
    assert(g.wl_effective == MS_WL_WIFI && g.wl_pending);
    mg_apply_settings(&g, 0, false, 1000 + MG_CONFIRM_MS);
    assert(g.wl_effective == MS_WL_UNSET && !g.wl_pending);
}

static void test_a_drop_from_ble_to_no_target_waits_for_its_confirmation_too(void)
{
    mg_t g;
    mg_init(&g, MS_MODE_BLE, 0, 2, false);
    mg_apply_settings(&g, 0, false, 5000);                 // seen once: the read may have landed inside a write window
    assert(g.wl_effective == MS_WL_BLE && g.wl_pending);
    mg_apply_settings(&g, 0, false, 5000 + MG_CONFIRM_MS - 1);   // too soon
    assert(g.wl_effective == MS_WL_BLE && g.wl_pending);
    mg_apply_settings(&g, 0, false, 5000 + MG_CONFIRM_MS);       // a second opinion, 600 ms later
    assert(g.wl_effective == MS_WL_UNSET && !g.wl_pending);
    // a read that says Bluetooth again cancels the wait
    mg_init(&g, MS_MODE_BLE, 0, 2, false);
    mg_apply_settings(&g, 0, false, 5000);
    mg_apply_settings(&g, 2, false, 5100);
    assert(g.wl_effective == MS_WL_BLE && !g.wl_pending);
    // a change between the two wireless targets is applied at once: it is a pick, not a lost read
    mg_init(&g, MS_MODE_WIFI, 0, 1, false);
    mg_apply_settings(&g, 2, false, 100);
    assert(g.wl_effective == MS_WL_BLE && !g.wl_pending);
    mg_apply_settings(&g, 1, false, 200);
    assert(g.wl_effective == MS_WL_WIFI && !g.wl_pending);
    // a number with no transport is "no target" and is confirmed like it
    mg_init(&g, MS_MODE_BLE, 0, 2, false);
    mg_apply_settings(&g, 9, false, 100);
    assert(g.wl_effective == MS_WL_BLE && g.wl_pending);
}

static void test_a_ble_dial_goes_to_usb_and_comes_back(void)
{
    fresh();
    D.wl_force = 2;
    D.cable = D.daemon = false;
    assert(boot() == MS_MODE_BLE && D.running == MS_MODE_BLE && D.mg.wl_effective == MS_WL_BLE);
    assert(!run(70000));                                   // past the dwell, no cable: nothing to do
    assert(D.probe_calls == 0);
    D.cable = D.daemon = true;                             // the user plugs in: the probe runs, a welcome opens T_ENTER, 5 s later USB
    assert(run(8000) && D.reboot_to[0] == MS_MODE_USB && strcmp(D.order, "LSBR") == 0);
    assert(config_load_boot_usb());
    assert(boot() == MS_MODE_USB && D.mg.wl_effective == MS_WL_BLE);   // the cable is in and boot_usb says USB
    D.cable = D.daemon = false;                            // pulled: after T_LEAVE and the dwell the dial goes back to the target
    assert(!run(50000));
    assert(run(20000) && D.reboot_to[1] == MS_MODE_BLE);
    assert(!config_load_boot_usb());
    assert(boot() == MS_MODE_BLE);
    assert(D.mg.ms.flaps == 2);                            // two switches by the rule, and the cap is the shared one
}

static void test_a_ble_dial_whose_radio_does_not_start_runs_usb_and_is_bounded(void)
{
    fresh();
    D.wl_force = 2;
    D.radio_fails = true;
    assert(boot() == MS_MODE_USB);                         // the plan said Bluetooth; the radio did not come up: USB in the same boot
    int reboots = run_wall(30 * 60 * 1000);
    assert_bounded(reboots, 30 * 60 * 1000);
    assert(reboots <= (int)MS_FLAP_CAP * 4);
}

static void test_picking_usb_while_ble_runs_goes_to_usb(void)
{
    fresh();
    D.wl_force = 2;
    assert(boot() == MS_MODE_BLE);
    assert(!run(1000));
    D.wl_force = 0;                                        // no stored target any more: the user picked USB on the Link screen
    D.reload = true;
    assert(!run(MG_CONFIRM_MS - 250));                     // the drop is held until a second read agrees
    assert(run(2000) && D.reboot_to[0] == MS_MODE_USB);
    assert(boot() == MS_MODE_USB);
    assert(!run(5 * 60 * 1000) && D.reboots == 1);         // and USB with no target never leaves
}

// B6: the same rules over the REAL accessors (config_net_set_ble, config_net_set_mode(2)), no forced target: the dial boots Bluetooth only with the
// phone settings whole, and every loss of them is USB.
static void test_a_phone_provisioned_dial_boots_ble_from_nvs(void)
{
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK);
    assert(boot() == MS_MODE_USB && D.mg.wl_effective == MS_WL_UNSET);            // the key alone is no target
    assert(config_net_set_mode(2) == CFG_NET_OK && config_load_wl_effective() == 2);
    D.cable = D.daemon = false;
    assert(boot() == MS_MODE_BLE && D.running == MS_MODE_BLE && D.mg.wl_effective == MS_WL_BLE);
    assert(!run(5 * 60 * 1000) && D.reboots == 0);                                // the reload at the start of a BLE run keeps the target
    // Wi-Fi settings and a Wi-Fi pick on top: Wi-Fi boots
    assert(config_net_set(SSID_FOR_TEST, "correct horse battery", "192.168.1.20", 18480, KEY, 1) == CFG_NET_OK);
    assert(boot() == MS_MODE_WIFI);
    assert(config_net_set_mode(2) == CFG_NET_OK);
    assert(boot() == MS_MODE_BLE);
    // the user picks USB on the Link screen while Bluetooth runs: confirmed by a second read, then USB
    assert(config_net_set_mode(0) == CFG_NET_OK);
    D.reload = true;
    assert(run(5000) && D.reboot_to[D.reboots - 1] == MS_MODE_USB);
    // net.clear: nothing left, USB for good
    assert(config_net_clear() == CFG_NET_OK);
    assert(boot() == MS_MODE_USB && !run(5 * 60 * 1000));
    // a lost marker (a cut during a change): no target, USB
    fresh();
    assert(config_net_set_ble(KEY) == CFG_NET_OK && config_net_set_mode(2) == CFG_NET_OK);
    put_u8("bl_ver", 0);
    assert(boot() == MS_MODE_USB);
}

static void test_third_crash_in_ble_takes_usb_once(void)
{
    fresh();
    D.wl_force = 2;
    assert(boot() == MS_MODE_BLE && D.crashes == 0 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 1 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 2 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 3 && D.crash_usb);   // the W15d rule, now for Bluetooth
    assert(config_load_boot_usb());
    assert(!run(59000));                                   // the ordinary rule: back to the target after the dwell, a software reset holds the count
    assert(run(2000) && D.reboot_to[0] == MS_MODE_BLE);
    assert(boot() == MS_MODE_BLE && D.crashes == 3 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 4 && D.crash_usb);   // the very next crash trips at once
}

static void test_wifi_and_ble_crashes_share_one_count(void)
{
    // Two crashes of a Wi-Fi run, then the user picks Bluetooth: the next crash (it ends a Wi-Fi run) is the third.
    fresh();
    provision(1);
    assert(boot() == MS_MODE_WIFI);
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 1);
    assert(crash_and_boot() == MS_MODE_WIFI && D.crashes == 2);
    D.wl_force = 2;                                        // the stored target is now Bluetooth (a Link-screen pick would reboot; the crash does it here)
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 3 && D.crash_usb);
    // And the other way: two crashes in a Bluetooth run, then Wi-Fi again.
    fresh();
    provision(1);
    D.wl_force = 2;
    assert(boot() == MS_MODE_BLE);
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 1);
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 2);
    D.wl_force = 1;
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 3 && D.crash_usb);
    // A crash that ended a USB run is nobody's: the count is held, whichever wireless mode is the target.
    fresh();
    D.wl_force = 2;
    boot();
    crash_and_boot();
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 2);
    D.cable = D.daemon = true;
    assert(run(70000) && D.reboot_to[0] == MS_MODE_USB);
    assert(boot() == MS_MODE_USB && D.crashes == 2);
    D.cable = false;
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 2 && !D.crash_usb);
    assert(crash_and_boot() == MS_MODE_USB && D.crashes == 3 && D.crash_usb);
}

static void test_ten_minutes_up_in_ble_clears_the_crash_count(void)
{
    fresh();
    D.wl_force = 2;
    boot();
    crash_and_boot();
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 2);
    assert(!run(9 * 60 * 1000));                           // 9 minutes is not enough ...
    uint8_t n;
    assert(!mode_record_crash_boot(MS_RESET_HOLD, &n) && n == 2);
    assert(!run(2 * 60 * 1000));                           // ... 11 minutes is
    assert(!mode_record_crash_boot(MS_RESET_HOLD, &n) && n == 0);
    assert(crash_and_boot() == MS_MODE_BLE && D.crashes == 1);
}

static void test_a_crash_loop_in_ble_is_slowed_to_one_attempt_per_dwell(void)
{
    fresh();
    D.wl_force = 2;
    boot();
    const uint64_t wall = 60 * 60 * 1000, start = D.wall;
    int ble_runs = 0;
    unsigned crashes_seen = 0;
    while (D.wall - start < wall) {
        if (D.running == MS_MODE_BLE) {
            ble_runs++;
            if (!run(20000)) crash_and_boot(); else boot();
        } else if (run(10 * 60 * 1000)) {
            boot();
        }
        assert(D.crashes >= crashes_seen);
        crashes_seen = D.crashes;
    }
    assert(crashes_seen >= MS_CRASH_LIMIT);
    assert(ble_runs <= (int)MS_CRASH_LIMIT + (int)(wall / (MS_T_DWELL_MS + 20000)) + 1);
    for (int i = 0; i < D.reboots && i < 64; i++) assert(D.reboot_uptime[i] >= MS_T_DWELL_MS);
}

int main(void)
{
    test_ble_target_is_kept_by_the_glue_state();
    test_a_drop_from_ble_to_no_target_waits_for_its_confirmation_too();
    test_a_ble_dial_goes_to_usb_and_comes_back();
    test_a_ble_dial_whose_radio_does_not_start_runs_usb_and_is_bounded();
    test_picking_usb_while_ble_runs_goes_to_usb();
    test_a_phone_provisioned_dial_boots_ble_from_nvs();
    test_third_crash_in_ble_takes_usb_once();
    test_wifi_and_ble_crashes_share_one_count();
    test_ten_minutes_up_in_ble_clears_the_crash_count();
    test_a_crash_loop_in_ble_is_slowed_to_one_attempt_per_dwell();
    test_restart_refused_when_the_dial_turns_busy_again();
    test_retry_delay_ladder();
    test_a_persistent_refusal_is_rate_bounded();
    test_the_retry_count_starts_again_when_the_switch_is_no_longer_due();
    test_undo_after_a_radio_fallback_restores_the_old_boot_usb();
    test_the_restart_gate_sequence();
    test_a_read_inside_a_write_window_does_not_send_the_dial_to_usb();
    test_a_real_pick_of_usb_waits_only_for_its_confirmation();
    test_provisioning_at_runtime_needs_the_reload_and_then_leaves_on_t_leave();
    test_net_clear_and_net_mode_at_runtime_are_seen_too();
    test_picking_usb_on_wifi_goes_to_usb_and_stays();
    test_a_dial_busy_on_the_link_screen_waits_then_switches();
    test_keep_wifi_off_gives_a_wifi_dial_its_way_back_to_usb();
    test_panic_loop_plan();
    test_third_crash_in_wifi_takes_usb_once();
    test_trip_beats_stay_wl_and_the_dwell_still_applies();
    test_power_on_clears_the_crash_count();
    test_ten_minutes_up_in_wifi_clears_the_crash_count();
    test_time_in_usb_does_not_clear_the_crash_count();
    test_a_crash_in_usb_mode_is_not_counted();
    test_a_crash_loop_in_wifi_is_slowed_to_one_attempt_per_dwell();
    test_boot_plan();
    test_reboot_sequence_order();
    test_wifi_dial_comes_back_to_usb_and_stays();
    test_a_daemon_that_stops_does_not_strand_a_usb_dial();
    test_late_sof_is_a_bounded_detour();
    test_flap_cap_stops_a_boot_loop();
    test_boot_usb_write_failure_is_bounded();
    test_no_target_means_usb_for_good();
    test_corrupt_flags_read_as_defaults();
    test_radio_failure_falls_back_to_usb_in_the_same_boot();
    test_probe_edges();
    test_pc_without_a_daemon_does_not_switch();
    test_charger_never_probes();
    test_busy_dial_waits_and_shows_the_notice();
    test_the_notice_goes_when_the_reason_goes();
    test_a_held_switch_is_logged_once_and_on_a_change();
    test_the_boot_after_a_power_off_is_wifi_mode();
    test_a_replug_with_a_reader_open_switches_after_30_s_of_idle();
    test_power_cut_forgets_the_count();
    test_flaps_clear_after_ten_minutes();
    test_nothing_happens_after_the_restart();
    puts("ok test_mode_glue");
    return 0;
}
