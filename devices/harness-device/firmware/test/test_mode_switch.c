// W14: the automatic mode switch as pure logic (mode_switch.c). Host test for the cases H1-H10 of the approved design
// (workbench briefs/research/2026-10-29-dial-mode-switch.md section 6; the bench cases T1-T11 stay in W15): the boot table, the charger, a PC
// without the daemon, enter, leave, the busy gate, the flap guard, stay_wl, the RTC record and boot_usb. An injected clock (the simulator
// below advances 100 ms per tick) and injected inputs; no IDF, no NVS, no real time.
//
//   cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=undefined,bounds -I ../main -o test_mode_switch test_mode_switch.c ../main/mode_switch.c
#include "mode_switch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define S(x) ((uint64_t)(x) * 1000u)   // seconds to ms

// ── the simulator ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// One boot of the dial. Environment fields are set by the test; tick() advances the clock by 100 ms, produces what the glue would feed
// (a welcome every `welcome_period` ms after SOF rose, a daemon byte every 5 s, the 15 s silence rule for session_up) and calls ms_step.
typedef struct {
    ms_state_t s;
    ms_out_t out;
    ms_mode_t mode;
    uint64_t t;
    // environment
    bool sof;
    bool daemon;               // alive and answering
    uint64_t daemon_dies_at;   // 0 = never
    uint64_t welcome_period;   // probe hello every 2 s
    uint64_t sof_rise;
    uint8_t wl_mode;
    bool stay_wl;
    bool asleep;
    uint32_t busy;
    uint64_t last_touch;       // when the user last touched the glass (the idle rule of the wireless -> USB gate); 0 = at boot
    // what the dial has seen
    bool have_welcome;
    uint64_t last_welcome;
    bool have_byte;
    uint64_t last_byte;
    // results
    bool rebooted;
    uint64_t reboot_t;
    ms_mode_t reboot_target;
    uint8_t reboot_flaps;
    bool reboot_boot_usb;
    unsigned notices, probes_wanted;
    bool cleared_event;
} sim_t;

static void sim_init(sim_t *m, ms_mode_t mode, uint8_t wl_mode, uint8_t flaps)
{
    memset(m, 0, sizeof *m);
    m->mode = mode;
    m->wl_mode = wl_mode;
    m->daemon = true;
    m->welcome_period = 2000;
    ms_init(&m->s, mode, flaps);
}

static void sim_set_sof(sim_t *m, bool sof)
{
    if (sof && !m->sof) m->sof_rise = m->t;
    m->sof = sof;
}

// One tick. Returns the action.
static ms_act_t tick(sim_t *m)
{
    m->t += 100;
    if (m->daemon_dies_at && m->t >= m->daemon_dies_at) m->daemon = false;
    if (m->mode != MS_MODE_USB) {
        // The probe only exists while SOF is up and the module asked for it; its first hello goes out one tick after SOF rose.
        if (m->sof && !m->stay_wl && m->daemon && (m->t - m->sof_rise - 100) % m->welcome_period == 0) {
            m->have_welcome = true;
            m->last_welcome = m->t;
        }
    } else if (m->sof && m->daemon && m->t % 5000 == 0) {
        m->have_byte = true;
        m->last_byte = m->t;
    }
    ms_in_t in;
    memset(&in, 0, sizeof in);
    in.now_ms = m->t;
    in.wl_mode = m->wl_mode;
    in.stay_wl = m->stay_wl;
    in.sof = m->sof;
    in.session_up = m->have_byte && m->t - m->last_byte <= 15000;   // the dial's 15 s silence rule
    in.have_daemon_byte = m->have_byte;
    in.last_daemon_byte_ms = m->last_byte;
    in.have_welcome = m->have_welcome;
    in.last_welcome_ms = m->last_welcome;
    in.busy = m->busy;
    in.asleep = m->asleep;
    in.idle_ms = m->t - m->last_touch > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(m->t - m->last_touch);
    ms_step(&m->s, &in, &m->out);
    if (m->out.probe_wanted) m->probes_wanted++;
    if (m->out.act == MS_ACT_NOTICE) m->notices++;
    if (m->out.flaps_cleared) m->cleared_event = true;
    if (m->out.act == MS_ACT_REBOOT && !m->rebooted) {
        m->rebooted = true;
        m->reboot_t = m->t;
        m->reboot_target = m->out.target;
        m->reboot_flaps = m->out.flaps;
        m->reboot_boot_usb = m->out.boot_usb;
    }
    return m->out.act;
}

// Run until the clock reaches t_end or the module asks for a reboot. Returns whether it did.
static bool run_to(sim_t *m, uint64_t t_end)
{
    while (m->t < t_end && !m->rebooted) tick(m);
    return m->rebooted;
}

// A wireless dial past the dwell, SOF down, ready for a scenario.
static void wireless_ready(sim_t *m, ms_mode_t mode)
{
    sim_init(m, mode, mode == MS_MODE_BLE ? MS_WL_BLE : MS_WL_WIFI, 0);
    CHECK(!run_to(m, S(70)));
}

// A USB dial past the dwell with SOF and a live daemon.
static void usb_ready(sim_t *m, uint8_t wl)
{
    sim_init(m, MS_MODE_USB, wl, 0);
    sim_set_sof(m, true);
    CHECK(!run_to(m, S(70)));
    CHECK(m->s.flaps == 0);
}

// ── H1: boot table ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static ms_boot_out_t boot(uint8_t wl, bool stay, bool bu, bool sof, uint32_t read_ms)
{
    ms_boot_in_t in = { .wl_mode = wl, .stay_wl = stay, .boot_usb = bu, .sof = sof, .sof_read_ms = read_ms };
    ms_boot_out_t out;
    memset(&out, 0xA5, sizeof out);
    ms_boot_decide(&in, &out);
    return out;
}

static void test_h1_boot_table(void)
{
    // Row 1: wl_mode unset -> USB, whatever else is set (and stay_wl cannot send it anywhere: no wireless target).
    for (int stay = 0; stay < 2; stay++)
        for (int bu = 0; bu < 2; bu++)
            for (int sof = 0; sof < 2; sof++) {
                ms_boot_out_t o = boot(MS_WL_UNSET, stay, bu, sof, 500);
                CHECK(o.mode == MS_MODE_USB);
            }
    // A stored value outside 0/1/2 is not a target either.
    CHECK(boot(3, false, false, false, 500).mode == MS_MODE_USB);
    CHECK(boot(255, true, true, true, 500).mode == MS_MODE_USB);

    // Row 2: stay_wl = 1 -> wl_mode, even with boot_usb = 1 and a cable.
    CHECK(boot(MS_WL_WIFI, true, true, true, 500).mode == MS_MODE_WIFI);
    CHECK(boot(MS_WL_BLE, true, true, true, 500).mode == MS_MODE_BLE);
    CHECK(boot(MS_WL_WIFI, true, false, false, 500).mode == MS_MODE_WIFI);

    // Row 3: boot_usb = 1 and SOF present after the delay -> USB (OTA reboot, crash, daemon-driven restart).
    CHECK(boot(MS_WL_WIFI, false, true, true, 20).mode == MS_MODE_USB);
    CHECK(boot(MS_WL_BLE, false, true, true, 500).mode == MS_MODE_USB);

    // Row 4: otherwise wl_mode. A cold start on a charger (boot_usb = 1, SOF false) and a first boot with nothing set.
    CHECK(boot(MS_WL_WIFI, false, true, false, 500).mode == MS_MODE_WIFI);
    CHECK(boot(MS_WL_BLE, false, false, true, 500).mode == MS_MODE_BLE);
    CHECK(boot(MS_WL_WIFI, false, false, false, 500).mode == MS_MODE_WIFI);

    // SOF read before the 20 ms delay is ignored: the flag starts TRUE in secondary init, so an early "true" proves nothing.
    {
        ms_boot_out_t o = boot(MS_WL_WIFI, false, true, true, 19);
        CHECK(o.mode == MS_MODE_WIFI);
        CHECK(o.sof_ignored);
        o = boot(MS_WL_WIFI, false, true, true, 0);
        CHECK(o.mode == MS_MODE_WIFI && o.sof_ignored);
        o = boot(MS_WL_WIFI, false, true, true, 20);
        CHECK(o.mode == MS_MODE_USB && !o.sof_ignored);
        // Nothing to ignore when SOF decides nothing (stay_wl, unset, boot_usb = 0).
        CHECK(!boot(MS_WL_WIFI, true, true, true, 0).sof_ignored);
        CHECK(!boot(MS_WL_UNSET, false, true, true, 0).sof_ignored);
        CHECK(!boot(MS_WL_WIFI, false, false, true, 0).sof_ignored);
        CHECK(!boot(MS_WL_WIFI, false, true, false, 0).sof_ignored);
    }

    // boot_usb is rewritten only when the choice differs from what is stored (one NVS write per mode change, none per boot).
    for (int wl = 0; wl <= 2; wl++)
        for (int stay = 0; stay < 2; stay++)
            for (int bu = 0; bu < 2; bu++)
                for (int sof = 0; sof < 2; sof++)
                    for (uint32_t rd = 0; rd <= 40; rd += 20) {
                        ms_boot_out_t o = boot((uint8_t)wl, stay, bu, sof, rd);
                        CHECK(o.boot_usb_new == (o.mode == MS_MODE_USB));
                        CHECK(o.write_boot_usb == (o.boot_usb_new != (bool)bu));
                    }
}

// ── H2: a charger or power bank ───────────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_h2_charger(void)
{
    for (int k = 0; k < 2; k++) {
        sim_t m;
        sim_init(&m, k ? MS_MODE_BLE : MS_MODE_WIFI, k ? MS_WL_BLE : MS_WL_WIFI, 0);
        // one simulated hour, SOF never rises
        bool any = false;
        while (m.t < S(3600)) {
            if (tick(&m) != MS_ACT_NONE) any = true;
        }
        CHECK(!any);
        CHECK(m.probes_wanted == 0);
        CHECK(!m.rebooted);
        CHECK(m.notices == 0);
    }
}

// ── H3: a PC without the daemon (and one whose COM port another program holds) ──────────────────────────────────────────────────────────────
static void test_h3_pc_without_daemon(void)
{
    sim_t m;
    sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 0);
    m.daemon = false;
    sim_set_sof(&m, true);
    CHECK(!run_to(&m, S(3600)));
    CHECK(m.notices == 0);
    CHECK(!m.have_welcome);
    CHECK(m.probes_wanted == 36000);   // asked for on every tick while SOF is up: the probe is the only cost
    CHECK(m.out.act == MS_ACT_NONE);
}

// ── H4: enter ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_h4_enter(void)
{
    // a) one welcome and then silence: no switch (the welcome ages past 3 s long before 5 s)
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.welcome_period = S(100000);   // exactly one welcome, at SOF rise
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(120)));
        CHECK(m.have_welcome && m.last_welcome == S(70) + 100);
    }
    // b) a welcome every 2 s: reboot_to(USB) when T_ENTER (5 s) ends, with the flap count one higher; nothing (not even a notice) before
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(80)));
        CHECK(m.reboot_target == MS_MODE_USB);
        CHECK(m.reboot_flaps == 1);
        CHECK(m.reboot_t == S(70) + 100 + S(5));   // SOF rose at +0, the first welcome (and T_ENTER) at the next tick
        CHECK(m.notices == 0);
        CHECK(strcmp(MS_REASON_SWITCH, "mode switch") == 0);
    }
    // c) BLE target mode enters USB the same way
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_BLE);
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(80)));
        CHECK(m.reboot_target == MS_MODE_USB);
    }
    // d) an SOF dip at 3 s restarts T_ENTER: no reboot 5 s after the first rise, one 5 s after the second welcome
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(3)));
        sim_set_sof(&m, false);
        tick(&m);                  // one tick without SOF
        sim_set_sof(&m, true);     // back at +3.1 s; the next welcome is 2 s after this rise
        const uint64_t rise2 = m.t;
        CHECK(!run_to(&m, S(70) + S(5) + 200));   // the old window has ended and nothing happened
        CHECK(m.out.act == MS_ACT_NONE);
        CHECK(run_to(&m, S(100)));
        CHECK(m.reboot_t == rise2 + 100 + S(5));
    }
    // e) a welcome from BEFORE the dip does not count after it (SOF must be continuous across T_ENTER)
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.welcome_period = 1000;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(4) + 500));   // 4.5 s of welcomes
        sim_set_sof(&m, false);
        tick(&m);
        m.daemon = false;          // the daemon is gone; only a stale welcome (<= 3 s old) is left
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(60)));
    }
    // e2) the same dip, but the daemon answers again 2 s after SOF is back: T_ENTER starts at that first NEW welcome. A welcome from before the dip
    //     (still under 3 s old when SOF returns) must not open the window, or the dial would enter USB 2 s early.
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.welcome_period = 1000;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(2) + 200));    // a welcome at +2.1 s, 0.1 s ago
        sim_set_sof(&m, false);
        tick(&m);
        m.daemon = false;
        sim_set_sof(&m, true);
        const uint64_t rise2 = m.t;
        CHECK(!run_to(&m, rise2 + S(2)));
        m.daemon = true;
        CHECK(run_to(&m, rise2 + S(30)));
        CHECK(m.reboot_t == rise2 + S(2) + 100 + S(5));
    }
    // f) welcome age boundary: the last welcome exactly 3.0 s old at the end of T_ENTER still counts, 3.1 s does not
    {
        // welcomes at +0 and +2 s (period 2 s) then the daemon dies: at +5.0 s the age is 3.0 s
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.daemon_dies_at = m.t + S(2) + 200;   // after the welcome at +2.1 s (the first one goes out at +0.1 s)
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(100)));
        CHECK(m.reboot_t == S(70) + 100 + S(5));
        CHECK(m.t - m.last_welcome == 3000);
        // welcomes at +0.1 s and +2.0 s (period 1.9 s) then dead: at +5.1 s the age is 3.1 s
        sim_t n;
        wireless_ready(&n, MS_MODE_WIFI);
        n.welcome_period = 1900;
        n.daemon_dies_at = n.t + S(2) + 100;
        sim_set_sof(&n, true);
        CHECK(!run_to(&n, S(100)));
        CHECK(n.last_welcome - n.sof_rise == 2000);
    }
    // g) a daemon that comes back late: the clock starts at its first welcome, not at SOF
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.daemon = false;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(30)));   // 30 s of SOF, nothing
        m.daemon = true;
        const uint64_t back = m.t;
        CHECK(run_to(&m, S(200)));
        CHECK(m.reboot_t >= back + S(5));
        CHECK(m.reboot_t <= back + S(5) + S(2) + 200);   // first welcome within one probe period of the daemon's return
    }
    // h) stale welcome from a previous plug-in is not used when the probe has just started again
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.have_welcome = true;
        m.last_welcome = S(60);   // from an earlier plug-in, long gone
        m.daemon = false;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(30)));
    }
}

// ── H5: leave ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_h5_leave(void)
{
    // SOF lost for 9.9 s then back: no switch. 10.0 s: switch to wl_mode.
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        sim_set_sof(&m, false);
        CHECK(!run_to(&m, m.t + 9900));
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, m.t + S(30)));
        sim_set_sof(&m, false);
        const uint64_t lost = m.t + 100;   // the first tick that sees it
        CHECK(run_to(&m, m.t + S(11)));
        CHECK(m.reboot_t == lost + S(10));
        CHECK(m.reboot_target == MS_MODE_WIFI);
        CHECK(m.reboot_flaps == 1);
    }
    // the target is the stored wireless mode (BLE too)
    {
        sim_t m;
        usb_ready(&m, MS_WL_BLE);
        sim_set_sof(&m, false);
        CHECK(run_to(&m, m.t + S(11)));
        CHECK(m.reboot_target == MS_MODE_BLE);
    }
    // SOF present, the daemon silent: 59.9 s no, 60.0 s yes, counted from the last daemon byte. (T5: gaps of 10, 30 and 70 s; the
    // daemon's first byte after the gap lands on a multiple of 5 s, so the gaps here are 10, 30, 55 (it returns) and 65, 75 (too late).)
    {
        struct { uint64_t gap_s; bool leaves; } rows[] = { { 10, false }, { 30, false }, { 55, false }, { 65, true }, { 75, true } };
        for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            sim_t m;
            usb_ready(&m, MS_WL_WIFI);
            const uint64_t last = m.last_byte;
            m.daemon = false;                         // the daemon stops: no more bytes
            const uint64_t back_at = last + S(rows[i].gap_s);
            const uint64_t until = back_at - 100 < last + S(60) - 100 ? back_at - 100 : last + S(60) - 100;
            CHECK(!run_to(&m, until));                // 59.9 s of silence at most: no switch
            if (rows[i].leaves) {
                CHECK(run_to(&m, last + S(60) + 100));
                CHECK(m.reboot_t == last + S(60));
                CHECK(m.reboot_target == MS_MODE_WIFI);
                CHECK(!m.reboot_boot_usb);
            } else {
                m.daemon = true;
                CHECK(!run_to(&m, back_at + S(130)));
                CHECK(m.have_byte && m.last_byte >= back_at);
            }
        }
    }
    // booted into USB with no session (a dock, a hub, a PC off the daemon): switch at 60 s
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_WIFI, 0);
        m.daemon = false;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(60) - 100));
        CHECK(run_to(&m, S(61)));
        CHECK(m.reboot_t == S(60));
        CHECK(m.reboot_target == MS_MODE_WIFI);
    }
    // ... and a session up before then clears it (it restarts from the last daemon byte)
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_WIFI, 0);
        m.daemon = false;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(40)));
        m.daemon = true;
        CHECK(!run_to(&m, S(110)));     // would have left at 60 s
        CHECK(m.have_byte && m.s.flaps == 0);
    }
    // wl_mode unset: a USB boot never leaves (no wireless target), however long SOF and the daemon are gone
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_UNSET, 0);
        m.daemon = false;
        CHECK(!run_to(&m, S(7200)));
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(14400)));
        CHECK(m.notices == 0);
        usb_ready(&m, 9);     // an invalid value is no target either
        sim_set_sof(&m, false);
        CHECK(!run_to(&m, m.t + S(60)));
    }
    // a short SOF flicker with the session still up keeps the old session: no switch, whatever the flicker count
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        for (int i = 0; i < 100; i++) {
            sim_set_sof(&m, false);
            tick(&m);
            sim_set_sof(&m, true);
            tick(&m);
        }
        CHECK(!m.rebooted);
        CHECK(!run_to(&m, m.t + S(120)));
    }
    // cable pulled while a live daemon keeps pinging a dead port: SOF lost decides, not the daemon timer
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        sim_set_sof(&m, false);
        m.daemon = false;
        CHECK(run_to(&m, m.t + S(11)));
        CHECK(m.reboot_t - S(70) <= S(10) + 200);
    }
}

// ── H6: the busy gate ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_h6_busy(void)
{
    const uint32_t soft[] = { MS_BUSY_VOICE, MS_BUSY_DRAFT, MS_BUSY_QUESTION, MS_BUSY_READER, MS_BUSY_PICKER, MS_BUSY_MODEL_LIST, 1u << 20 };
    const uint32_t hard[] = { MS_BUSY_FW_UPDATE, MS_BUSY_PET_UPLOAD };

    CHECK(!ms_blocked(0, false) && !ms_blocked(0, true));
    for (unsigned i = 0; i < sizeof soft / sizeof soft[0]; i++) {
        CHECK(ms_blocked(soft[i], false));
        CHECK(!ms_blocked(soft[i], true));   // asleep: a silent switch
    }
    for (unsigned i = 0; i < sizeof hard / sizeof hard[0]; i++) {
        CHECK(ms_blocked(hard[i], false));
        CHECK(ms_blocked(hard[i], true));    // an update or an upload in flight blocks even asleep
    }
    CHECK(ms_blocked(MS_BUSY_DRAFT | MS_BUSY_PET_UPLOAD, true));
    CHECK((MS_BUSY_HARD & MS_BUSY_FW_UPDATE) && (MS_BUSY_HARD & MS_BUSY_PET_UPLOAD));

    // each row that holds a wireless dial for as long as it lasts (H13: the question, reader and picker rows give way to 30 s of idle instead) blocks
    // entry and shows the notice; release switches on the next step (<= 250 ms)
    const uint32_t holding[] = { MS_BUSY_VOICE, MS_BUSY_DRAFT, MS_BUSY_MODEL_LIST, 1u << 20 };
    for (unsigned i = 0; i < sizeof holding / sizeof holding[0]; i++) {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = holding[i];
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(5) + 100));         // the timer has ended at +5.1 s
        CHECK(m.out.act == MS_ACT_NOTICE && m.out.waiting);
        CHECK(!run_to(&m, S(70) + S(60)));
        CHECK(m.out.act == MS_ACT_NOTICE);
        CHECK(m.notices > 400);
        m.busy = 0;
        const uint64_t rel = m.t;
        CHECK(run_to(&m, rel + MS_STEP_MS));
        CHECK(m.reboot_t - rel <= MS_STEP_MS);
        CHECK(m.reboot_target == MS_MODE_USB);
    }
    // leave direction with the gate
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        m.busy = MS_BUSY_VOICE;
        sim_set_sof(&m, false);
        CHECK(!run_to(&m, m.t + S(30)));
        CHECK(m.out.act == MS_ACT_NOTICE);
        m.busy = 0;
        CHECK(run_to(&m, m.t + 300));
        CHECK(m.reboot_target == MS_MODE_WIFI);
    }
    // asleep: silent switch, no notice at all
    for (unsigned i = 0; i < sizeof soft / sizeof soft[0]; i++) {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = soft[i];
        m.asleep = true;
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(80)));
        CHECK(m.notices == 0);
        CHECK(m.reboot_t == S(70) + 100 + S(5));
    }
    // firmware update or pet upload in flight: blocks even asleep, no notice on a dark screen, switches once it ends
    for (unsigned i = 0; i < sizeof hard / sizeof hard[0]; i++) {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = hard[i];
        m.asleep = true;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(60)));
        CHECK(m.notices == 0);
        CHECK(m.out.act == MS_ACT_NONE && m.out.waiting);
        m.asleep = false;
        tick(&m);
        CHECK(m.out.act == MS_ACT_NOTICE);     // awake: the user sees why
        m.busy = 0;
        CHECK(run_to(&m, m.t + 300));
    }
    // a new user action during the wait keeps the switch waiting
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = MS_BUSY_READER;
        m.last_touch = m.t;       // the user is at the glass (H13: a reader alone holds only until 30 s after the last touch)
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(10)));
        m.busy = MS_BUSY_VOICE;   // one action ends, another begins in the same step
        CHECK(!run_to(&m, m.t + S(10)));
        CHECK(m.out.act == MS_ACT_NOTICE);
    }
    // the notice goes away when the reason to switch goes away (the daemon vanished, SOF dropped) before the gate opens
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = MS_BUSY_DRAFT;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(6)));
        CHECK(m.out.act == MS_ACT_NOTICE);
        m.daemon = false;
        CHECK(!run_to(&m, m.t + S(4)));
        CHECK(m.out.act == MS_ACT_NONE && !m.out.waiting);
        m.busy = 0;
        CHECK(!run_to(&m, m.t + S(60)));
    }
    // dwell and cap are not "busy": no notice while only the dwell holds the switch back
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 0);
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(30)));
        CHECK(m.notices == 0);
    }
}

// ── H13: the wireless -> USB switch of an awake dial: soft rows give way to 30 s without a touch (user decision 2026-11-01) ──────────────────
// Before: any row held an awake dial until its screen went off (300 s on a charger; the bench measured 297 s for a replug). Now, for a switch
// from a wireless mode to USB: voice, draft, the model list, an unknown row, an update and an upload hold as before; the question, reader and
// picker rows hold only while the user is touching the glass: 30 s after the last touch (a touch restarts it) the notice is shown and the dial goes.
// USB -> wireless keeps the old rule (ms_blocked).
static void test_h13_idle_gate(void)
{
    const uint32_t idle_rows[] = { MS_BUSY_QUESTION, MS_BUSY_READER, MS_BUSY_PICKER, MS_BUSY_QUESTION | MS_BUSY_READER | MS_BUSY_PICKER };
    const uint32_t strong[] = { MS_BUSY_VOICE, MS_BUSY_DRAFT, MS_BUSY_MODEL_LIST, 1u << 20, MS_BUSY_FW_UPDATE, MS_BUSY_PET_UPLOAD,
                                MS_BUSY_VOICE | MS_BUSY_READER };
    CHECK(MS_T_IDLE_MS == 30000u);
    CHECK((MS_BUSY_IDLE_OK & (MS_BUSY_VOICE | MS_BUSY_DRAFT | MS_BUSY_MODEL_LIST | MS_BUSY_HARD)) == 0);

    // the pure gate
    for (ms_mode_t from = MS_MODE_WIFI; from <= MS_MODE_BLE; from++) {
        CHECK(!ms_gate_blocked(from, 0, false, 0) && !ms_gate_blocked(from, 0, true, 0));
        for (unsigned i = 0; i < sizeof idle_rows / sizeof idle_rows[0]; i++) {
            CHECK(ms_gate_blocked(from, idle_rows[i], false, 0));
            CHECK(ms_gate_blocked(from, idle_rows[i], false, MS_T_IDLE_MS - 1));
            CHECK(!ms_gate_blocked(from, idle_rows[i], false, MS_T_IDLE_MS));        // 30 s, not 30 s and a tick
            CHECK(!ms_gate_blocked(from, idle_rows[i], false, 4000000000u));
            CHECK(!ms_gate_blocked(from, idle_rows[i], true, 0));                    // asleep: silent, as before
        }
        for (unsigned i = 0; i < sizeof strong / sizeof strong[0]; i++) {
            CHECK(ms_gate_blocked(from, strong[i], false, 4000000000u));             // no idle time releases these
            CHECK(ms_gate_blocked(from, strong[i], false, MS_T_IDLE_MS));
        }
        CHECK(ms_gate_blocked(from, MS_BUSY_FW_UPDATE, true, 0) && ms_gate_blocked(from, MS_BUSY_PET_UPLOAD, true, 4000000000u));   // hard rows hold a dark dial
        CHECK(!ms_gate_blocked(from, MS_BUSY_VOICE, true, 0));                       // a dark dial goes past a voice row, as before
    }
    // a USB dial leaving for a wireless mode keeps the old rule: idle time never releases anything
    for (unsigned i = 0; i < sizeof idle_rows / sizeof idle_rows[0]; i++) {
        CHECK(ms_gate_blocked(MS_MODE_USB, idle_rows[i], false, 4000000000u) == ms_blocked(idle_rows[i], false));
        CHECK(ms_gate_blocked(MS_MODE_USB, idle_rows[i], true, 0) == ms_blocked(idle_rows[i], true));
    }
    CHECK(ms_gate_blocked(MS_MODE_USB, MS_BUSY_READER, false, 4000000000u));

    // the dial: a reader open, the cable plugged in as the user touches the glass
    for (unsigned i = 0; i < sizeof idle_rows / sizeof idle_rows[0]; i++) {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = idle_rows[i];
        m.last_touch = m.t;
        const uint64_t touched = m.t;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, touched + S(5) + 100));             // due: the probe has been answered for 5 s
        CHECK(m.out.act == MS_ACT_NOTICE && m.out.waiting && m.out.target == MS_MODE_USB);
        CHECK(!run_to(&m, touched + MS_T_IDLE_MS - 100));
        CHECK(m.out.act == MS_ACT_NOTICE);                    // the notice stays up while it waits
        CHECK(run_to(&m, touched + MS_T_IDLE_MS + 100));
        CHECK(m.reboot_t == touched + MS_T_IDLE_MS && m.reboot_target == MS_MODE_USB && m.reboot_boot_usb);
    }
    // a touch restarts the 30 s, however many times
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = MS_BUSY_READER;
        m.last_touch = m.t;
        sim_set_sof(&m, true);
        for (int k = 0; k < 8; k++) {                         // a touch every 20 s for almost three minutes
            CHECK(!run_to(&m, m.t + S(20)));
            m.last_touch = m.t;
        }
        CHECK(m.out.act == MS_ACT_NOTICE);
        const uint64_t last = m.last_touch;
        CHECK(run_to(&m, last + MS_T_IDLE_MS + 100));
        CHECK(m.reboot_t == last + MS_T_IDLE_MS);
    }
    // a strong row that starts during the wait takes the switch back; when it ends the idle rule decides (a voice turn bumps the touch timer, so the
    // 30 s start again from its end)
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = MS_BUSY_PICKER;
        m.last_touch = m.t;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, m.t + S(20)));
        m.busy = MS_BUSY_PICKER | MS_BUSY_VOICE;
        CHECK(!run_to(&m, m.t + S(60)));                      // far past 30 s since the touch: the voice row holds
        CHECK(m.out.act == MS_ACT_NOTICE);
        m.last_touch = m.t;                                   // the turn ended with a touch
        m.busy = MS_BUSY_PICKER;
        CHECK(!run_to(&m, m.t + MS_T_IDLE_MS - 100));
        CHECK(run_to(&m, m.t + 200));
    }
    // a dial that was idle for minutes when the switch came due goes at once (nothing to wait for), with no notice
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = MS_BUSY_READER;                              // last_touch is 0: 70 s of idle
        sim_set_sof(&m, true);
        CHECK(run_to(&m, m.t + S(6)));
        CHECK(m.reboot_t == S(70) + 100 + S(5) && m.notices == 0);
    }
    // the same rule for the pick of USB on the Link screen, which is itself a touch
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = MS_BUSY_PICKER;                              // the Link screen is on the glass
        m.last_touch = m.t;                                   // the pick
        m.wl_mode = MS_WL_UNSET;
        CHECK(!run_to(&m, m.t + MS_T_IDLE_MS - 100));
        CHECK(m.out.act == MS_ACT_NOTICE && m.out.waiting);
        CHECK(run_to(&m, m.t + 200));
        CHECK(m.reboot_target == MS_MODE_USB);
    }
    // a Wi-Fi dial leaving the cable (USB -> Wi-Fi) is not given the idle rule: a reader still holds it for as long as it is up
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        m.busy = MS_BUSY_READER;
        sim_set_sof(&m, false);
        CHECK(!run_to(&m, m.t + S(300)));
        CHECK(m.out.act == MS_ACT_NOTICE);
    }
}

// ── H7: flap guard ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// A loose cable (6 s up, 11 s down, repeating on a global clock), the whole boot chain: boot decision, mode, steps, RTC record, reboot.
typedef struct { uint64_t t_boot_global, uptime; ms_mode_t from, to; uint8_t flaps; } sw_t;

static bool cable_up(uint64_t g) { return g % S(17) < S(6); }

static unsigned loose_cable(uint64_t total, sw_t *log, unsigned cap, bool *clear_event)
{
    ms_record_t rec;
    ms_record_seal(&rec, "", 0);
    uint64_t g = 0;
    ms_mode_t mode = MS_MODE_WIFI;
    unsigned n = 0;
    uint8_t wl = MS_WL_WIFI;
    bool bu = false;   // boot_usb in NVS
    while (g < total) {
        ms_record_view_t v;
        CHECK(ms_record_load(&rec, true, &v));
        sim_t m;
        sim_init(&m, mode, wl, v.flaps);
        const uint64_t boot_g = g;
        while (g < total && !m.rebooted) {
            g += 100;
            m.t = g - boot_g - 100;               // tick() adds the 100 ms
            sim_set_sof(&m, cable_up(g));
            tick(&m);
            if (m.cleared_event) { if (clear_event) *clear_event = true; ms_record_seal(&rec, v.focus, m.out.flaps); m.cleared_event = false; }
        }
        if (!m.rebooted) break;
        if (n < cap) { log[n].t_boot_global = boot_g; log[n].uptime = m.reboot_t; log[n].from = mode; log[n].to = m.reboot_target; log[n].flaps = m.reboot_flaps; }
        n++;
        ms_record_seal(&rec, "agent-1", m.reboot_flaps);
        bu = m.reboot_boot_usb;                     // stored before the reboot
        // the boot decision, with the cable as it is 20 ms or more after the reset
        ms_boot_in_t bi = { .wl_mode = wl, .stay_wl = false, .boot_usb = bu, .sof = cable_up(g), .sof_read_ms = 500 };
        ms_boot_out_t bo;
        ms_boot_decide(&bi, &bo);
        if (bo.write_boot_usb) bu = bo.boot_usb_new;
        mode = bo.mode;
    }
    return n;
}

static void test_h7_flap(void)
{
    sw_t log[64];
    bool cleared = false;
    // The first 9 minutes: at most 3 switches, none within the dwell of its boot, and then silence until a mode has run 10 min.
    const unsigned n9 = loose_cable(S(9 * 60), log, 64, &cleared);
    printf("  loose cable, 9 min: %u reboots", n9);
    for (unsigned i = 0; i < n9 && i < 64; i++) printf(" [boot at %llu s, switch after %llu s uptime, %s->%s, count %u]", (unsigned long long)(log[i].t_boot_global / 1000), (unsigned long long)(log[i].uptime / 1000), ms_mode_name(log[i].from), ms_mode_name(log[i].to), log[i].flaps);
    printf("\n");
    CHECK(n9 == 3);
    for (unsigned i = 0; i < n9 && i < 64; i++) {
        CHECK(log[i].uptime >= MS_T_DWELL_MS);
        CHECK(log[i].flaps == i + 1);
    }
    CHECK(!cleared);
    // Over two hours: the count clears after 10 min in one mode, so there are more switches, but never more than 3 in a row, and a fourth
    // only after a boot has run 10 min.
    cleared = false;
    const unsigned n = loose_cable(S(2 * 3600), log, 64, &cleared);
    CHECK(n > 3 && n < 64);
    CHECK(cleared);
    for (unsigned i = 0; i < n; i++) {
        CHECK(log[i].uptime >= MS_T_DWELL_MS);
        CHECK(log[i].flaps >= 1 && log[i].flaps <= MS_FLAP_CAP);
        if (log[i].flaps == 1 && i > 0) CHECK(log[i].uptime >= MS_FLAP_CLEAR_MS || log[i - 1].flaps == MS_FLAP_CAP);
        if (i > 0 && log[i - 1].flaps == MS_FLAP_CAP) CHECK(log[i].uptime >= MS_FLAP_CLEAR_MS);   // the pause after the cap
    }
    // cap reached: even a perfect condition does not switch before 10 min of uptime
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, MS_FLAP_CAP);
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, MS_FLAP_CLEAR_MS - 100));
        CHECK(m.notices == 0 && !m.cleared_event);
        CHECK(run_to(&m, MS_FLAP_CLEAR_MS + 1000));     // the count clears at 10 min, then the switch is allowed
        CHECK(m.cleared_event);
        CHECK(m.reboot_flaps == 1);
        CHECK(m.reboot_t >= MS_FLAP_CLEAR_MS);
    }
    // the clear is an event once, with the count at zero in the output
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 2);
        unsigned events = 0;
        while (m.t < MS_FLAP_CLEAR_MS + S(60)) {
            tick(&m);
            if (m.out.flaps_cleared) { events++; CHECK(m.out.flaps == 0); CHECK(m.t == MS_FLAP_CLEAR_MS); }
        }
        CHECK(events == 1);
        CHECK(m.s.flaps == 0);
        // no event when there was nothing to clear
        sim_t z;
        sim_init(&z, MS_MODE_WIFI, MS_WL_WIFI, 0);
        while (z.t < MS_FLAP_CLEAR_MS + S(60)) { tick(&z); CHECK(!z.out.flaps_cleared); }
    }
    // dwell: with every condition met from t = 0, the switch is at 60 s and not before (both directions)
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 0);
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(70)));
        CHECK(m.reboot_t == MS_T_DWELL_MS);
        sim_t u;
        sim_init(&u, MS_MODE_USB, MS_WL_WIFI, 0);
        u.daemon = false;
        CHECK(run_to(&u, S(70)));      // no SOF from the start: T_LEAVE is long over at 10 s
        CHECK(u.reboot_t == MS_T_DWELL_MS);
    }
    // a manual change clears the count
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, MS_FLAP_CAP);
        ms_note_manual_change(&m.s);
        CHECK(m.s.flaps == 0);
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(80)));
    }
    // flaps passed in above the cap count as the cap (a corrupt but checksummed value cannot lift it)
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 200);
        CHECK(m.s.flaps == MS_FLAP_CAP);
    }
}

// ── H8: stay_wl ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_h8_stay_wl(void)
{
    // wireless: never probes, never enters
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 0);
        m.stay_wl = true;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(3600)));
        CHECK(m.probes_wanted == 0 && !m.have_welcome && m.notices == 0);
        // switched off: it probes at once and enters 5 s after the first welcome
        m.stay_wl = false;
        const uint64_t off = m.t;
        CHECK(run_to(&m, off + S(30)));
        CHECK(m.reboot_target == MS_MODE_USB);
        CHECK(m.reboot_t >= off + S(5));
    }
    // switched on while a T_ENTER runs: it is dropped
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70) + S(3)));
        m.stay_wl = true;
        CHECK(!run_to(&m, S(70) + S(120)));
        CHECK(m.out.act == MS_ACT_NONE && !m.out.probe_wanted);
    }
    // USB: stay_wl leaves at once, cable in with a live daemon (W14, user decision 2026-10-31); the dwell (passed here) and the gate still apply
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        m.stay_wl = true;
        CHECK(run_to(&m, m.t + 300));        // SOF present, daemon alive: leaves anyway
        CHECK(m.reboot_target == MS_MODE_WIFI);
        CHECK(!m.reboot_boot_usb);
    }
    // USB: the loss of SOF leaves at once too
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        m.stay_wl = true;
        sim_set_sof(&m, false);
        CHECK(run_to(&m, m.t + 300));
        CHECK(m.reboot_target == MS_MODE_WIFI);
    }
    // USB, stay_wl with the cable in and a live daemon: leaves for wl_mode at exactly T_DWELL (60 s uptime), not at the daemon timer
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_WIFI, 0);
        m.stay_wl = true;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(60) - 100));     // dwell holds it
        CHECK(run_to(&m, S(61)));
        CHECK(m.reboot_t == S(60));
        CHECK(m.reboot_target == MS_MODE_WIFI);
        CHECK(m.reboot_flaps == 1);
        CHECK(!m.reboot_boot_usb);           // the next boot is wireless (row 2), not USB
        CHECK(m.have_byte && m.s.flaps == 0 && m.notices == 0);   // the daemon was alive all along
    }
    // USB, stay_wl, cable in and live daemon, busy: the notice shows and the switch waits for the gate
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_BLE, 0);
        m.stay_wl = true;
        m.busy = MS_BUSY_DRAFT;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(70)));
        CHECK(m.out.act == MS_ACT_NOTICE && m.out.waiting);
        CHECK(m.notices > 0);
        m.busy = 0;
        CHECK(run_to(&m, m.t + MS_STEP_MS));
        CHECK(m.reboot_target == MS_MODE_BLE && m.reboot_t - S(70) <= MS_STEP_MS);
    }
    // USB, stay_wl, cable in and live daemon, asleep: switches at 60 s with no notice (the busy gate lets a dark dial through)
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_WIFI, 0);
        m.stay_wl = true;
        m.busy = MS_BUSY_VOICE;
        m.asleep = true;
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(70)));
        CHECK(m.reboot_t == S(60) && m.reboot_target == MS_MODE_WIFI);
        CHECK(m.notices == 0);
    }
    // USB, stay_wl, cable in, live daemon, wl_mode unset or invalid: no target, so it stays in USB
    {
        const uint8_t none[] = { MS_WL_UNSET, 3, 255 };
        for (unsigned i = 0; i < sizeof none / sizeof none[0]; i++) {
            sim_t m;
            sim_init(&m, MS_MODE_USB, none[i], 0);
            m.stay_wl = true;
            sim_set_sof(&m, true);
            CHECK(!run_to(&m, S(600)));
            CHECK(m.out.act == MS_ACT_NONE && !m.out.waiting && m.notices == 0);
        }
    }
    // USB, stay_wl turned OFF at 30 s with the cable in and a live daemon: no switch (the daemon keeps the session; the normal rules apply)
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_WIFI, 0);
        m.stay_wl = true;
        sim_set_sof(&m, true);
        CHECK(!run_to(&m, S(30)));
        m.stay_wl = false;
        CHECK(!run_to(&m, S(300)));
        CHECK(!m.rebooted && m.out.act == MS_ACT_NONE);
    }
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        m.stay_wl = true;
        m.busy = MS_BUSY_READER;
        sim_set_sof(&m, false);
        CHECK(!run_to(&m, m.t + S(5)));
        CHECK(m.out.act == MS_ACT_NOTICE);   // the gate still applies
        m.busy = 0;
        CHECK(run_to(&m, m.t + 300));
    }
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_WIFI, 0);
        m.stay_wl = true;
        CHECK(!run_to(&m, S(60) - 100));     // the dwell still applies
        CHECK(run_to(&m, S(61)));
        CHECK(m.reboot_t == MS_T_DWELL_MS);
    }
    // USB, stay_wl off: 10 s as before
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        sim_set_sof(&m, false);
        CHECK(!run_to(&m, m.t + 9900));
    }
    // stay_wl: the daemon timer does not matter in USB; SOF present and the daemon silent still leaves at once (dwell passed)
    {
        sim_t m;
        usb_ready(&m, MS_WL_WIFI);
        m.stay_wl = true;
        m.daemon = false;
        CHECK(run_to(&m, m.t + 300));
        CHECK(m.reboot_target == MS_MODE_WIFI);
    }
    // stay_wl with an unset wl_mode: nothing to stay on, nothing to leave to
    {
        sim_t m;
        sim_init(&m, MS_MODE_USB, MS_WL_UNSET, 0);
        m.stay_wl = true;
        CHECK(!run_to(&m, S(3600)));
    }
}

// ── H9: the RTC record ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_h9_record(void)
{
    _Static_assert(sizeof(ms_record_t) == 60, "the record has no implicit padding");
    ms_record_t r;
    ms_record_view_t v;

    ms_record_seal(&r, "agent-0123456789abcdef", 2);
    memset(&v, 0x5A, sizeof v);
    CHECK(ms_record_load(&r, true, &v));
    CHECK(v.flaps == 2 && strcmp(v.focus, "agent-0123456789abcdef") == 0);

    // not after a power cut, a watchdog, a panic or a button reset: only a software reset is trusted; the view is zero then
    memset(&v, 0x5A, sizeof v);
    CHECK(!ms_record_load(&r, false, &v));
    CHECK(v.flaps == 0 && v.focus[0] == '\0');
    for (size_t i = 0; i < sizeof v.focus; i++) CHECK(v.focus[i] == 0);

    // bad magic
    ms_record_t b = r;
    b.magic ^= 1u;
    memset(&v, 0x5A, sizeof v);
    CHECK(!ms_record_load(&b, true, &v) && v.flaps == 0 && v.focus[0] == 0);
    // bad checksum
    b = r;
    b.check ^= 0x80000000u;
    CHECK(!ms_record_load(&b, true, &v) && v.flaps == 0);
    CHECK(ms_record_crc(&r) == r.check);
    // every single bit of the 60 bytes flipped: all rejected (the cold-boot garbage case; CRC-32 sees every one-bit error)
    for (size_t byte = 0; byte < sizeof r; byte++)
        for (int bit = 0; bit < 8; bit++) {
            b = r;
            ((unsigned char *)&b)[byte] ^= (unsigned char)(1u << bit);
            memset(&v, 0x5A, sizeof v);
            CHECK(!ms_record_load(&b, true, &v));
            CHECK(v.flaps == 0 && v.focus[0] == 0);
        }
    // all zero and all ones (SRAM after a power cut is either, or random)
    memset(&b, 0, sizeof b);
    CHECK(!ms_record_load(&b, true, &v));
    memset(&b, 0xFF, sizeof b);
    CHECK(!ms_record_load(&b, true, &v));
    // a sealed record with a flap count above the cap, or a focus id with no terminator, is not believed either
    ms_record_seal(&b, "x", 1);
    b.flaps = MS_FLAP_CAP + 1;
    b.check = ms_record_crc(&b);   // a valid checksum over a count nobody can have written
    CHECK(!ms_record_load(&b, true, &v) && v.flaps == 0);
    b = r;
    memset(b.focus, 'a', sizeof b.focus);
    b.check = ms_record_crc(&b);   // a valid checksum over an unterminated string
    CHECK(!ms_record_load(&b, true, &v));
    // NULL view or record: safe
    CHECK(!ms_record_load(NULL, true, &v));
    CHECK(ms_record_load(&r, true, NULL));    // validity alone, no view wanted

    // the focus id: empty, exactly the longest that fits, and one too long (cut, still terminated)
    ms_record_seal(&r, "", 0);
    CHECK(ms_record_load(&r, true, &v) && v.focus[0] == 0 && v.flaps == 0);
    ms_record_seal(&r, NULL, 3);
    CHECK(ms_record_load(&r, true, &v) && v.focus[0] == 0 && v.flaps == 3);
    char id[80];
    memset(id, 'k', sizeof id);
    id[MS_FOCUS_MAX - 1] = '\0';        // 47 characters: fits
    ms_record_seal(&r, id, 1);
    CHECK(ms_record_load(&r, true, &v) && strlen(v.focus) == MS_FOCUS_MAX - 1);
    id[MS_FOCUS_MAX - 1] = 'k';
    id[79] = '\0';                      // 79 characters: cut to 47
    ms_record_seal(&r, id, 1);
    CHECK(ms_record_load(&r, true, &v) && strlen(v.focus) == MS_FOCUS_MAX - 1 && v.focus[MS_FOCUS_MAX - 1] == 0);
    ms_record_seal(&r, "a", 250);       // sealing a count above the cap stores the cap
    CHECK(ms_record_load(&r, true, &v) && v.flaps == MS_FLAP_CAP);

    // the record travels the whole path: a switch writes (focus, count), the next boot reads them and the module starts with that count
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 0);
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(80)));
        ms_record_seal(&r, "focus-7", m.out.flaps);
        CHECK(ms_record_load(&r, true, &v) && v.flaps == 1 && strcmp(v.focus, "focus-7") == 0);
        sim_t n;
        sim_init(&n, MS_MODE_USB, MS_WL_WIFI, v.flaps);
        CHECK(n.s.flaps == 1);
        // a panic (non-software reset) boots with zero, not with the stale count
        CHECK(!ms_record_load(&r, false, &v));
        sim_init(&n, MS_MODE_USB, MS_WL_WIFI, v.flaps);
        CHECK(n.s.flaps == 0);
    }
}

// ── H10: boot_usb follows the boot choice ─────────────────────────────────────────────────────────────────────────────────────────────────
static void test_h10_boot_usb(void)
{
    bool bu = false;   // the NVS value
    unsigned writes = 0;
    ms_boot_out_t o;

    // 1. first boot, wl_mode unset: USB, boot_usb written 1
    o = boot(MS_WL_UNSET, false, bu, true, 500);
    CHECK(o.mode == MS_MODE_USB && o.write_boot_usb && o.boot_usb_new);
    bu = o.boot_usb_new; writes++;
    // 2. the user picks Wi-Fi in Settings (wl_mode = 1) and the dial crashes with the cable in: USB again, no new write
    o = boot(MS_WL_WIFI, false, bu, true, 500);
    CHECK(o.mode == MS_MODE_USB && !o.write_boot_usb);
    // 3. a crash with no cable (SOF false): Wi-Fi, boot_usb written 0
    o = boot(MS_WL_WIFI, false, bu, false, 500);
    CHECK(o.mode == MS_MODE_WIFI && o.write_boot_usb && !o.boot_usb_new);
    bu = o.boot_usb_new; writes++;
    // 4. another crash, now with the cable in: boot_usb is 0, so Wi-Fi; the probe handles the plug-in. No write.
    o = boot(MS_WL_WIFI, false, bu, true, 500);
    CHECK(o.mode == MS_MODE_WIFI && !o.write_boot_usb);
    // 5. an automatic switch to USB: the step that asks for the reboot also says which boot_usb to store (ms_out_t.boot_usb), so the very next
    //    boot takes row 3 and comes up in USB. Without it boot_usb would still be 0 here and the dial would boot Wi-Fi again, see the welcome
    //    and switch again (a loop only the flap cap ends). The write is the same one the boot would make, so there is no second write.
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        sim_set_sof(&m, true);
        CHECK(run_to(&m, S(80)));
        CHECK(m.reboot_target == MS_MODE_USB && m.reboot_boot_usb);
        bu = m.reboot_boot_usb; writes++;
        o = boot(MS_WL_WIFI, false, bu, true, 500);
        CHECK(o.mode == MS_MODE_USB && !o.write_boot_usb);
        // and one that finds no SOF 20 ms after the reset (the cable moved) is the documented detour: wireless, then the probe tries again
        o = boot(MS_WL_WIFI, false, bu, false, 500);
        CHECK(o.mode == MS_MODE_WIFI && o.write_boot_usb && !o.boot_usb_new);
        // leaving USB (cable pulled, or the daemon gone) stores 0, so a boot with SOF present does not go straight back to USB
        sim_t u;
        usb_ready(&u, MS_WL_WIFI);
        u.daemon = false;
        CHECK(run_to(&u, u.last_byte + S(61)));
        CHECK(u.reboot_target == MS_MODE_WIFI && !u.reboot_boot_usb);
        o = boot(MS_WL_WIFI, false, u.reboot_boot_usb, true, 500);
        CHECK(o.mode == MS_MODE_WIFI);
    }
    // Count: exactly one write per change over a long random walk.
    unsigned changes = 0;
    bool cur = false;
    unsigned seed = 12345;
    for (int i = 0; i < 2000; i++) {
        seed = seed * 1103515245u + 12345u;
        const uint8_t wl = (uint8_t)((seed >> 16) % 3);
        const bool sof = (seed >> 20) & 1u, stay = (seed >> 21) & 1u;
        o = boot(wl, stay, cur, sof, 500);
        if (o.write_boot_usb) { CHECK(o.boot_usb_new != cur); cur = o.boot_usb_new; changes++; }
        else CHECK(o.boot_usb_new == cur);
    }
    CHECK(changes > 0 && changes < 2000);
    (void)writes;
}

// ── robustness and the rest ───────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void test_robustness(void)
{
    ms_out_t out;
    ms_in_t in;
    memset(&in, 0, sizeof in);
    // NULL arguments do nothing and do not crash
    ms_step(NULL, &in, &out);
    ms_init(NULL, MS_MODE_USB, 0);
    ms_note_manual_change(NULL);
    ms_boot_decide(NULL, NULL);
    ms_state_t s;
    ms_init(&s, MS_MODE_WIFI, 0);
    ms_step(&s, NULL, &out);
    ms_step(&s, &in, NULL);

    // a running mode outside the three never switches
    ms_init(&s, (ms_mode_t)7, 0);
    in.sof = true; in.wl_mode = MS_WL_WIFI;
    for (uint64_t t = 0; t < S(200); t += 100) {
        in.now_ms = t;
        ms_step(&s, &in, &out);
        CHECK(out.act == MS_ACT_NONE && !out.probe_wanted);
    }

    // the clock going backwards never fires anything and never goes below the earlier reading
    sim_t m;
    sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 0);
    sim_set_sof(&m, true);
    run_to(&m, S(30));
    ms_in_t back = { .now_ms = 10, .wl_mode = MS_WL_WIFI, .sof = true, .have_welcome = true, .last_welcome_ms = 10 };
    ms_step(&m.s, &back, &out);
    CHECK(out.act == MS_ACT_NONE);

    // the output is fully written on every call
    memset(&out, 0xA5, sizeof out);
    ms_init(&s, MS_MODE_WIFI, 0);
    memset(&in, 0, sizeof in);
    in.wl_mode = MS_WL_WIFI;   // (a wireless mode with no target would now be the user pick of USB: H12)
    ms_step(&s, &in, &out);
    CHECK(out.act == MS_ACT_NONE && !out.waiting && !out.probe_wanted && !out.flaps_cleared && out.flaps == 0);

    // enum names exist for logs
    CHECK(strcmp(ms_mode_name(MS_MODE_USB), "usb") == 0 && strcmp(ms_mode_name(MS_MODE_WIFI), "wifi") == 0 && strcmp(ms_mode_name(MS_MODE_BLE), "ble") == 0);
    CHECK(strcmp(ms_mode_name((ms_mode_t)9), "?") == 0);

    // constants are the design's
    CHECK(MS_SOF_SETTLE_MS == 20 && MS_T_ENTER_MS == 5000 && MS_WELCOME_AGE_MS == 3000 && MS_T_LEAVE_MS == 10000);
    CHECK(MS_T_LEAVE_DAEMON_MS == 60000 && MS_T_DWELL_MS == 60000 && MS_FLAP_CAP == 3 && MS_FLAP_CLEAR_MS == 600000 && MS_STEP_MS == 250);

    // 49 days of uptime and more: no 32-bit wrap in the timers
    {
        sim_t w;
        sim_init(&w, MS_MODE_WIFI, MS_WL_WIFI, 0);
        w.t = (uint64_t)5000000000ull;
        w.s.last_now = w.t;
        sim_set_sof(&w, true);
        CHECK(run_to(&w, w.t + S(20)));
        CHECK(w.reboot_target == MS_MODE_USB);
    }
}

// ── H11: the panic loop (user decision 2026-10-31) ─────────────────────────────────────────────────────────────────────────────────────────
// A second RTC record counts abnormal resets that ended a run in Wi-Fi mode. It survives every reset but a power-on, and the boot that sees the
// third abnormal reset in a row takes USB once. Only a power-on, or 10 minutes of uptime in Wi-Fi (the glue, test_mode_glue.c), clears it; a
// software reset (the switch back to Wi-Fi) holds it, so a crash loop cannot go on at the speed of a reboot.
static ms_crash_t crash_rec(uint8_t crashes, ms_mode_t mode)
{
    ms_crash_t r;
    ms_crash_seal(&r, crashes, mode);
    return r;
}

static ms_crash_view_t crash_boot(const ms_crash_t *r, ms_reset_t cls)
{
    ms_crash_view_t v;
    memset(&v, 0x5A, sizeof v);
    ms_crash_boot(r, cls, &v);
    return v;
}

static void test_h11_panic_loop(void)
{
    ms_crash_view_t v;
    ms_crash_t r;

    CHECK(MS_CRASH_LIMIT == 3 && MS_CRASH_CLEAR_MS == MS_FLAP_CLEAR_MS);
    CHECK(MS_RESET_POWERON == 0);   // a zeroed variable is a power-on

    // the count climbs one per abnormal reset that ended a Wi-Fi run, and trips at the third
    r = crash_rec(0, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 1 && !v.trip);
    r = crash_rec(1, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 2 && !v.trip);
    r = crash_rec(2, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 3 && v.trip);
    // past the limit every further crash trips again at once (the USB boot and the switch back do not clear the count)
    r = crash_rec(3, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 4 && v.trip);
    // a crash that ended a USB run is not a Wi-Fi crash: the count is held and nothing trips
    r = crash_rec(2, MS_MODE_USB);  v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 2 && !v.trip);
    r = crash_rec(3, MS_MODE_USB);  v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 3 && !v.trip);
    r = crash_rec(2, MS_MODE_BLE);  v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 2 && !v.trip);
    // a software reset (the mode switch, an update) or a deep-sleep wake holds the count and never trips
    r = crash_rec(2, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_HOLD);  CHECK(v.crashes == 2 && !v.trip);
    r = crash_rec(3, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_HOLD);  CHECK(v.crashes == 3 && !v.trip);
    // a power-on clears it, whatever the record says
    r = crash_rec(3, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_POWERON); CHECK(v.crashes == 0 && !v.trip);
    // saturation: a byte cannot wrap round to zero and forget a crash loop
    r = crash_rec(250, MS_MODE_WIFI); v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 250 && v.trip);
    r = crash_rec(255, MS_MODE_WIFI); CHECK(r.crashes == 250);

    // a record that cannot be believed is a count of zero: nothing to count, nothing trips
    memset(&r, 0, sizeof r);                         v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 0 && !v.trip);
    memset(&r, 0xA5, sizeof r);                      v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 0 && !v.trip);
    r = crash_rec(2, MS_MODE_WIFI); r.crashes = 9;   v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 0 && !v.trip);   // checksum wrong
    r = crash_rec(2, MS_MODE_WIFI); r.mode = MS_MODE_USB; v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 0 && !v.trip);
    r = crash_rec(2, MS_MODE_WIFI); r.magic ^= 1;    v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 0 && !v.trip);
    // the right checksum over absurd contents (a later build, a stray write that was sealed): refused
    r = crash_rec(2, MS_MODE_WIFI); r.mode = 7;      r.check = ms_crash_crc(&r); v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 0 && !v.trip);
    r = crash_rec(2, MS_MODE_WIFI); r.crashes = 251; r.check = ms_crash_crc(&r); v = crash_boot(&r, MS_RESET_CRASH); CHECK(v.crashes == 0 && !v.trip);
    // every byte of the record is covered by the checksum
    r = crash_rec(2, MS_MODE_WIFI);
    for (size_t i = 0; i < sizeof r; i++) {
        ms_crash_t q = r;
        ((unsigned char *)&q)[i] ^= 0x01;
        CHECK(ms_crash_crc(&q) != q.check);
    }
    // a class number outside the enum is held, never a trip
    r = crash_rec(2, MS_MODE_WIFI); v = crash_boot(&r, (ms_reset_t)9); CHECK(v.crashes == 2 && !v.trip);

    // NULL arguments
    ms_crash_boot(NULL, MS_RESET_CRASH, NULL);
    ms_crash_seal(NULL, 1, MS_MODE_WIFI);
    memset(&v, 0x5A, sizeof v);
    ms_crash_boot(NULL, MS_RESET_CRASH, &v);
    CHECK(v.crashes == 0 && !v.trip);
    CHECK(ms_crash_crc(NULL) == 0);
}

// ── H12: the user picks USB while the dial runs wireless (user decision 2026-11-01, task W16) ────────────────────────────────────────────────
// wl_mode (as the switch sees it) turning unset while a wireless mode runs means the user picked USB on the Link screen: the dial reboots into USB
// through the same busy gate as any automatic switch. It is the user's own act, so the dwell and the flap cap do not hold it, it does not count as a
// flap (the count restarts from zero), and it needs no cable, no probe and no daemon. USB mode with no target is unchanged: it never leaves.
static void test_h12_pick_usb_while_wireless(void)
{
    // Wi-Fi, past the dwell, nothing on the cable; the user picks USB at 100 s: reboot to USB at the next step, boot_usb stored for USB
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        CHECK(!run_to(&m, S(100)));
        m.wl_mode = MS_WL_UNSET;
        CHECK(run_to(&m, S(100) + MS_STEP_MS));
        CHECK(m.reboot_target == MS_MODE_USB && m.reboot_boot_usb && m.reboot_flaps == 0);
        CHECK(m.notices == 0);
    }
    // the same pick before the dwell: the user's act is not held back (an automatic switch would be)
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, 0);
        CHECK(!run_to(&m, S(5)));
        m.wl_mode = MS_WL_UNSET;
        CHECK(run_to(&m, S(5) + MS_STEP_MS));
        CHECK(m.reboot_target == MS_MODE_USB && m.reboot_t < MS_T_DWELL_MS);
    }
    // the flap cap does not hold it, and the new count is zero, not cap + 1
    {
        sim_t m;
        sim_init(&m, MS_MODE_WIFI, MS_WL_WIFI, MS_FLAP_CAP);
        CHECK(!run_to(&m, S(20)));
        m.wl_mode = MS_WL_UNSET;
        CHECK(run_to(&m, S(20) + MS_STEP_MS));
        CHECK(m.reboot_target == MS_MODE_USB && m.reboot_flaps == 0);
    }
    // a corrupt wl_mode byte, or one the switch cannot target, is the same as unset (the accessor maps them to 0 before the switch sees them)
    {
        const uint8_t none[] = { 0, 3, 255 };
        for (unsigned i = 0; i < sizeof none / sizeof none[0]; i++) {
            sim_t m;
            sim_init(&m, MS_MODE_WIFI, none[i], 0);
            CHECK(run_to(&m, S(1)));
            CHECK(m.reboot_target == MS_MODE_USB);
        }
    }
    // stay_wl does not keep it on Wi-Fi: the user just said USB
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.stay_wl = true;
        m.wl_mode = MS_WL_UNSET;
        CHECK(run_to(&m, m.t + S(1)));
        CHECK(m.reboot_target == MS_MODE_USB);
    }
    // the busy gate: an awake dial in a voice turn shows the notice and waits; it goes when the turn ends
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.busy = MS_BUSY_VOICE;
        m.wl_mode = MS_WL_UNSET;
        CHECK(!run_to(&m, m.t + S(30)));
        CHECK(m.out.act == MS_ACT_NOTICE && m.out.waiting && m.out.target == MS_MODE_USB && m.notices > 0);
        m.busy = 0;
        CHECK(run_to(&m, m.t + MS_STEP_MS));
        CHECK(m.reboot_target == MS_MODE_USB);
    }
    // asleep: silent, and an update or an upload in flight still holds it
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        m.asleep = true;
        m.busy = MS_BUSY_FW_UPDATE;
        m.wl_mode = MS_WL_UNSET;
        CHECK(!run_to(&m, m.t + S(10)));
        CHECK(m.out.act == MS_ACT_NONE && m.out.waiting && m.notices == 0);
        m.busy = MS_BUSY_READER;   // a sleeping dial does not wait for a reader
        CHECK(run_to(&m, m.t + MS_STEP_MS));
        CHECK(m.reboot_target == MS_MODE_USB && m.notices == 0);
    }
    // the probe is not needed: no SOF, no welcome, and the rule still fires; with the cable in and a live daemon it fires once, to USB
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        sim_set_sof(&m, true);
        m.wl_mode = MS_WL_UNSET;
        CHECK(run_to(&m, m.t + MS_STEP_MS));
        CHECK(m.reboot_target == MS_MODE_USB);
    }
    // Wi-Fi with a target never fires it: no cable, a hundred minutes
    {
        sim_t m;
        wireless_ready(&m, MS_MODE_WIFI);
        CHECK(!run_to(&m, S(6000)));
        CHECK(m.out.act == MS_ACT_NONE && m.notices == 0);
    }
    // USB with no target still never leaves (the old rule), pick or no pick
    {
        sim_t m;
        usb_ready(&m, MS_WL_UNSET);
        CHECK(!run_to(&m, m.t + S(600)));
        CHECK(m.out.act == MS_ACT_NONE);
    }
}

int main(void)
{
    test_h1_boot_table();
    test_h2_charger();
    test_h3_pc_without_daemon();
    test_h4_enter();
    test_h5_leave();
    test_h6_busy();
    test_h7_flap();
    test_h8_stay_wl();
    test_h9_record();
    test_h10_boot_usb();
    test_h11_panic_loop();
    test_h12_pick_usb_while_wireless();
    test_h13_idle_gate();
    test_robustness();
    printf("test_mode_switch: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
