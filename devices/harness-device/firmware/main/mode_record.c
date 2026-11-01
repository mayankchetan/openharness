#include "mode_record.h"

#include <string.h>

#include "esp_attr.h"

static RTC_NOINIT_ATTR ms_record_t s_record;

bool mode_record_boot(bool reset_is_software, ms_record_view_t *view)
{
    ms_record_view_t local;
    ms_record_view_t *v = view ? view : &local;
    const bool ok = ms_record_load(&s_record, reset_is_software, v);   // zeros in *v unless trusted
    if (ok) ms_record_seal(&s_record, NULL, v->flaps);                 // the focus is restored once; the count stays
    else memset(&s_record, 0, sizeof s_record);                        // not trusted: zero it, so no later reset can find it again
    return ok;
}

void mode_record_save(const char *focus, uint8_t flaps) { ms_record_seal(&s_record, focus, flaps); }

void mode_record_clear_flaps(void) { ms_record_seal(&s_record, NULL, 0); }

#ifdef MODE_RECORD_HOST
ms_record_t *mode_record_raw_for_test(void) { return &s_record; }
#endif

// ── the crash record ────────────────────────────────────────────────────────────────────────────────
static RTC_NOINIT_ATTR ms_crash_t s_crash;

bool mode_record_crash_boot(ms_reset_t cls, uint8_t *crashes)
{
    ms_crash_view_t v;
    ms_crash_boot(&s_crash, cls, &v);
    if (cls == MS_RESET_POWERON) memset(&s_crash, 0, sizeof s_crash);   // whatever the memory kept across a power dip is not believed again
    if (crashes) *crashes = v.crashes;
    return v.trip;
}

void mode_record_crash_seal(uint8_t crashes, ms_mode_t running) { ms_crash_seal(&s_crash, crashes, running); }

void mode_record_crash_clear(void)
{
    // keep the mode of the run: a crash right after this still belongs to it
    const ms_mode_t mode = s_crash.mode <= MS_MODE_BLE ? (ms_mode_t)s_crash.mode : MS_MODE_USB;
    ms_crash_seal(&s_crash, 0, mode);
}

#ifdef MODE_RECORD_HOST
ms_crash_t *mode_record_crash_raw_for_test(void) { return &s_crash; }
#endif
