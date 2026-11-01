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
//   S4  wl_mode corrupt, wl_mode 2 (Bluetooth), no credentials -> USB, and the dial never leaves it
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
    bool radio_fails;           // the Wi-Fi stack does not start (net_link_start returns false)
    uint32_t busy;
    bool asleep;
    char focus[MS_FOCUS_MAX];   // what the carousel shows

    // the dial
    uint64_t t;                 // ms since the boot
    bool reset_sw;              // how the last reset happened (the flap record is believed after a software reset only)
    ms_reset_t reset_cls;       // the same reset as the crash record sees it (zero: power-on)
    int refuse_restarts;        // the final busy check inside the restart op finds the dial busy this many times
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
    char order[32];             // the reboot sequence as letters: L(og) S(eal) B(oot_usb) R(estart); r = restart refused (busy again), U = undo
    bool store_fails;
} dial_t;

static dial_t D;

static void op_probe(void *c, bool on) { dial_t *d = c; d->probe_calls++; d->probe_installed = on; if (on) { d->have_welcome = false; d->last_hello = 0; d->probe_since = d->t; } }
static void op_notice(void *c, bool on) { dial_t *d = c; d->notice_calls++; d->notice_shown = on; }
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
    if (d->refuse_restarts > 0) { d->refuse_restarts--; strcat(d->order, "r"); return false; }
    strcat(d->order, "R");
    d->reboot_to[d->reboots < 64 ? d->reboots : 63] = target;
    d->reboot_uptime[d->reboots < 64 ? d->reboots : 63] = d->t;
    d->reboots++;
    d->reset_sw = true;
    d->reset_cls = MS_RESET_HOLD;
    return true;
}
static void op_undo(void *c, ms_mode_t running, uint8_t flaps)
{
    dial_t *d = c;
    strcat(d->order, "U");
    config_save_boot_usb(running == MS_MODE_USB);
    mode_record_save(NULL, flaps);
}
static void op_clear_crashes(void *c) { (void)c; mode_record_crash_clear(); }

static const mg_ops_t OPS = {&D, op_probe, op_notice, op_clear, op_log, op_seal, op_store, op_restart, op_clear_crashes, op_undo};

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
    in.wl_effective = config_load_wl_effective();
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

    // Wi-Fi mode needs the radio; if it does not start the dial runs on USB in the same boot (net_link_start returns false)
    D.running = (out.mode == MS_MODE_WIFI && !D.radio_fails) ? MS_MODE_WIFI : MS_MODE_USB;
    mode_record_crash_seal(out.crashes, D.running);
    mg_init(&D.mg, D.running, out.flaps, config_load_wl_effective(), config_load_stay_wl());
    mg_note_crashes(&D.mg, out.crashes);
    D.reset_sw = false;
    D.reset_cls = MS_RESET_POWERON;
    D.order[0] = 0;
    return D.running;
}

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
        const int before = D.reboots;
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
    // a target that is neither: Bluetooth has no transport yet. Never a mode the dial cannot start.
    o = plan(2, false, false, false, 50);  assert(o.mode == MS_MODE_USB && o.unsupported);
    o = plan(2, true, false, true, 50);    assert(o.mode == MS_MODE_USB && o.unsupported);
    o = plan(255, false, true, true, 50);  assert(o.mode == MS_MODE_USB && o.unsupported);
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
    assert(run(MS_STEP_MS * 2));
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
    assert(run(MS_STEP_MS * 2) && D.reboot_to[0] == MS_MODE_WIFI && !config_load_boot_usb());
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
    in.wl_effective = 2;                                  // a mode with no transport: USB, and not blamed on a crash
    mg_boot(&in, &o);
    assert(o.mode == MS_MODE_USB && o.unsupported && !o.crash_usb);
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

int main(void)
{
    test_restart_refused_when_the_dial_turns_busy_again();
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
    test_power_cut_forgets_the_count();
    test_flaps_clear_after_ten_minutes();
    test_nothing_happens_after_the_restart();
    puts("ok test_mode_glue");
    return 0;
}
