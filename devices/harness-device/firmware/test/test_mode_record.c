// mode_record.c (task W15): the RTC record that carries the focused agent and the flap count across the software reset of a mode switch.
// The pure rules (magic, checksum, flap cap, terminated focus) are mode_switch.c's and tested in test_mode_switch.c (H9). What is tested here is
// the wrapper around the RTC_NOINIT variable:
//   - the record is trusted only after a software reset: after a power cut, a watchdog or a panic the boot sees zeros, never garbage;
//   - a boot CONSUMES the focus: the record is re-sealed with the same flap count and no focus, so a later restart that is not a mode switch
//     (a firmware update, the factory reset) does not move the carousel to an agent from an earlier switch;
//   - the flap count survives that re-seal, so a boot loop of switches keeps counting toward the cap of three;
//   - a switch seals the focus and the count in one step; a flap count of 10 minutes' uptime re-seals zero.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mode_record.h"

static void power_cut(void) { memset(mode_record_raw_for_test(), 0xA5, sizeof(ms_record_t)); }
static void garbage_with_valid_magic(void)
{
    memset(mode_record_raw_for_test(), 0, sizeof(ms_record_t));
    mode_record_raw_for_test()->magic = MS_RECORD_MAGIC;
    mode_record_raw_for_test()->flaps = 2;
}

static void test_software_reset_restores(void)
{
    mode_record_save("agent-42", 2);
    ms_record_view_t v;
    assert(mode_record_boot(true, &v));
    assert(v.flaps == 2);
    assert(strcmp(v.focus, "agent-42") == 0);
}

static void test_other_resets_see_zeros(void)
{
    mode_record_save("agent-42", 2);
    ms_record_view_t v;
    memset(&v, 0x55, sizeof v);
    assert(!mode_record_boot(false, &v));     // power-on, watchdog, panic, brown-out: anything but esp_restart()
    assert(v.flaps == 0 && v.focus[0] == 0);
    // and the record is zeroed, not just ignored: a later SOFTWARE reset must not find the old contents
    assert(!mode_record_boot(true, &v));
    assert(v.flaps == 0 && v.focus[0] == 0);
}

static void test_garbage_is_never_believed(void)
{
    power_cut();
    ms_record_view_t v;
    assert(!mode_record_boot(true, &v));      // a software reset after the RTC memory lost power: random bytes, wrong magic
    assert(v.flaps == 0 && v.focus[0] == 0);
    garbage_with_valid_magic();               // the right magic, a wrong checksum
    assert(!mode_record_boot(true, &v));
    assert(v.flaps == 0 && v.focus[0] == 0);
}

static void test_boot_consumes_the_focus_and_keeps_the_count(void)
{
    mode_record_save("agent-7", 1);
    ms_record_view_t v;
    assert(mode_record_boot(true, &v));
    assert(strcmp(v.focus, "agent-7") == 0 && v.flaps == 1);
    // a second restart that is not a mode switch (nothing sealed in between)
    assert(mode_record_boot(true, &v));
    assert(v.focus[0] == 0);                  // the focus is gone: it was restored once
    assert(v.flaps == 1);                     // the count stays, so a boot loop of switches is still counted
}

static void test_flap_count_is_held_at_the_cap(void)
{
    mode_record_save("a", 200);
    ms_record_view_t v;
    assert(mode_record_boot(true, &v));
    assert(v.flaps == MS_FLAP_CAP);
}

static void test_long_focus_is_cut_and_terminated(void)
{
    char big[200];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    mode_record_save(big, 0);
    ms_record_view_t v;
    assert(mode_record_boot(true, &v));
    assert(strlen(v.focus) == MS_FOCUS_MAX - 1);
    mode_record_save(NULL, 1);                // no focus (nothing on the carousel): an empty one
    assert(mode_record_boot(true, &v));
    assert(v.focus[0] == 0 && v.flaps == 1);
}

static void test_clearing_the_count_reseals_zero_and_no_focus(void)
{
    mode_record_save("agent-9", 3);
    mode_record_clear_flaps();                // ms_out_t.flaps_cleared: 10 minutes in one mode
    ms_record_view_t v;
    assert(mode_record_boot(true, &v));
    assert(v.flaps == 0);
    assert(v.focus[0] == 0);
}

int main(void)
{
    test_software_reset_restores();
    test_other_resets_see_zeros();
    test_garbage_is_never_believed();
    test_boot_consumes_the_focus_and_keeps_the_count();
    test_flap_count_is_held_at_the_cap();
    test_long_focus_is_cut_and_terminated();
    test_clearing_the_count_reseals_zero_and_no_focus();
    puts("ok test_mode_record");
    return 0;
}
