#include "last_words.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#ifdef DEVICE_BATTERY_SLEEP
#include "esp_task_wdt.h"
#endif

static const char *TAG = "boot";

#define RING_BYTES 4096
#define RING_MAGIC 0x4C575244u   // "LWRD"

// RTC_NOINIT: not touched by the bootloader or the startup code, so it holds across a panic, a watchdog
// reset or esp_restart(). A power cut clears it, and that is fine — a power cut is not a bug this can
// name. The magic and a checksum of the header guard against reading garbage on a cold boot.
typedef struct {
    uint32_t magic;
    uint32_t head;      // next write position
    uint32_t len;       // bytes in use (≤ RING_BYTES)
    uint32_t check;     // ~(head ^ len)
    char     buf[RING_BYTES];
} ring_t;

static RTC_NOINIT_ATTR ring_t s_ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_reset_reason_t s_reason;
static bool s_abnormal;

#ifdef DEVICE_BATTERY_SLEEP
// The sleep breadcrumb (last_words.h). RTC_NOINIT like the ring, believed only with the magic and a checksum over its fixed fields, used once. The
// starved text and its flag are written by the task watchdog's interrupt, so they sit outside the checksum (an interrupt can land in the middle of a
// ptt write; the worst case is a record the next boot calls unreadable).
#define CRUMB_MAGIC 0x43525542u   // "CRUB"
#define STARVED_MAX 80
typedef struct {
    uint32_t magic;
    uint32_t check;
    uint32_t seq;                 // wakes written since this record was started (this boot)
    last_words_crumb_t at;
    uint8_t  wdt_seen;            // the watchdog handler ran
    char     starved[STARVED_MAX];
} crumb_rec_t;
static RTC_NOINIT_ATTR crumb_rec_t s_crumb;
static crumb_rec_t s_crumb_prev;                      // this boot's copy of the previous boot's record, for the report
static EXT_RAM_BSS_ATTR char s_crumb_prev_starved[STARVED_MAX];
static bool s_crumb_have, s_crumb_torn;

static uint32_t crumb_check(const crumb_rec_t *r)
{
    uint32_t h = 2166136261u ^ r->magic;   // FNV-1a over the sequence number and the record
    const uint8_t *p = (const uint8_t *)&r->seq;
    for (size_t i = 0; i < sizeof r->seq + sizeof r->at; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

void last_words_crumb(const last_words_crumb_t *c)
{
    if (!c) return;
    if (s_crumb.magic != CRUMB_MAGIC) { memset(&s_crumb, 0, sizeof s_crumb); s_crumb.magic = CRUMB_MAGIC; }
    s_crumb.seq++;
    s_crumb.at = *c;
    s_crumb.check = crumb_check(&s_crumb);
}

// Which entries starved the task watchdog, as text, into the RTC record: the IDF prints them with ESP_EARLY_LOGE, to a console that may be the very
// USB port that is dead. Runs in the watchdog's interrupt just before the panic: no log, no lock, no allocation.
typedef struct { char *dst; size_t cap; size_t len; bool first; } starved_sink_t;
static void starved_add(void *opaque, const char *msg)
{
    starved_sink_t *s = opaque;
    if (s->first) { s->first = false; return; }   // the first message is the IDF's caption
    for (const char *p = msg; *p && s->len + 1 < s->cap; p++) s->dst[s->len++] = (*p == '\n') ? ' ' : *p;
    s->dst[s->len] = '\0';
}

void esp_task_wdt_isr_user_handler(void)
{
    if (s_crumb.magic != CRUMB_MAGIC) { memset(&s_crumb, 0, sizeof s_crumb); s_crumb.magic = CRUMB_MAGIC; s_crumb.check = crumb_check(&s_crumb); }
    starved_sink_t sink = {.dst = s_crumb.starved, .cap = sizeof s_crumb.starved, .len = 0, .first = true};
    s_crumb.starved[0] = '\0';
    (void)esp_task_wdt_print_triggered_tasks(starved_add, &sink, NULL);
    s_crumb.wdt_seen = 1;
}
#endif

// The reason a software reset was asked for (last_words_note_reason). Same memory, same rule as the ring: it survives esp_restart(), a power cut
// leaves it as anything, so it is believed only after a software reset and only with the right magic and checksum, and it is used once.
#define NOTE_MAGIC 0x4E4F5445u   // "NOTE"
#define NOTE_MAX 24
typedef struct {
    uint32_t magic;
    uint32_t check;
    char     text[NOTE_MAX];
} note_t;
static RTC_NOINIT_ATTR note_t s_note;
static char s_note_text[NOTE_MAX];   // this boot's copy of the previous boot's note; empty for any other kind of reset

static uint32_t note_check(const note_t *n)
{
    uint32_t h = 2166136261u ^ n->magic;   // FNV-1a over the text
    for (size_t i = 0; i < NOTE_MAX; i++) h = (h ^ (uint8_t)n->text[i]) * 16777619u;
    return h;
}

void last_words_note_reason(const char *reason)
{
    if (!reason) return;
    note_t n = {.magic = NOTE_MAGIC};
    snprintf(n.text, sizeof n.text, "%s", reason);
    n.check = note_check(&n);
    s_note = n;
}
// The previous boot's lines, copied out before this boot starts overwriting the ring. Static rather than
// heap: 4 KB of BSS is cheaper than a failed allocation at the one moment it would matter.
static EXT_RAM_BSS_ATTR char s_previous[RING_BYTES + 1];   // PSRAM: boot-time copy, task context only
static size_t s_previous_len;
static bool s_reported;

static bool ring_valid(void)
{
    return s_ring.magic == RING_MAGIC && s_ring.len <= RING_BYTES && s_ring.head < RING_BYTES
        && s_ring.check == ~(s_ring.head ^ s_ring.len);
}

static void ring_seal(void) { s_ring.check = ~(s_ring.head ^ s_ring.len); }

static const char *reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (esp_restart)";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
    }
}

void last_words_boot(void)
{
    s_reason = esp_reset_reason();
    s_note_text[0] = '\0';
    if (s_reason == ESP_RST_SW && s_note.magic == NOTE_MAGIC && s_note.check == note_check(&s_note) && memchr(s_note.text, '\0', NOTE_MAX)) {
        memcpy(s_note_text, s_note.text, NOTE_MAX);
    }
    s_note.magic = 0;   // used once, whatever kind of reset this was
#ifdef DEVICE_BATTERY_SLEEP
    s_crumb_have = s_crumb_torn = false;
    if (s_reason == ESP_RST_PANIC || s_reason == ESP_RST_INT_WDT || s_reason == ESP_RST_TASK_WDT || s_reason == ESP_RST_WDT || s_reason == ESP_RST_BROWNOUT ||
        s_reason == ESP_RST_UNKNOWN) {
        if (s_crumb.magic == CRUMB_MAGIC) {
            if (s_crumb.check == crumb_check(&s_crumb)) {
                s_crumb_prev = s_crumb;
                s_crumb_have = true;
                s_crumb_prev.starved[STARVED_MAX - 1] = '\0';
                memcpy(s_crumb_prev_starved, s_crumb_prev.starved, STARVED_MAX);
            } else {
                s_crumb_torn = true;
            }
        }
    }
    s_crumb.magic = 0;   // used once, whatever kind of reset this was
#endif
    s_abnormal = s_reason == ESP_RST_PANIC || s_reason == ESP_RST_INT_WDT || s_reason == ESP_RST_TASK_WDT
              || s_reason == ESP_RST_WDT || s_reason == ESP_RST_BROWNOUT || s_reason == ESP_RST_UNKNOWN;
    if (s_abnormal && ring_valid() && s_ring.len) {
        // Unroll the ring into reading order: oldest byte first.
        uint32_t start = (s_ring.head + RING_BYTES - s_ring.len) % RING_BYTES;
        for (uint32_t i = 0; i < s_ring.len; i++) s_previous[i] = s_ring.buf[(start + i) % RING_BYTES];
        s_previous_len = s_ring.len;
        s_previous[s_previous_len] = '\0';
    }
    s_ring.magic = RING_MAGIC;
    s_ring.head = 0;
    s_ring.len = 0;
    ring_seal();
}

void last_words_add(const char *line, size_t len)
{
    if (!len) return;
    portENTER_CRITICAL(&s_mux);
    if (!ring_valid()) { s_ring.magic = RING_MAGIC; s_ring.head = 0; s_ring.len = 0; }
    for (size_t i = 0; i < len; i++) {
        s_ring.buf[s_ring.head] = line[i];
        s_ring.head = (s_ring.head + 1) % RING_BYTES;
    }
    s_ring.buf[s_ring.head] = '\n';
    s_ring.head = (s_ring.head + 1) % RING_BYTES;
    s_ring.len += len + 1;
    if (s_ring.len > RING_BYTES) s_ring.len = RING_BYTES;
    ring_seal();
    portEXIT_CRITICAL(&s_mux);
}

#define LW_HOOKS 4
static void (*s_hooks[LW_HOOKS])(void);

void last_words_on_report(void (*fn)(void))
{
    if (!fn) return;
    portENTER_CRITICAL(&s_mux);
    int free_slot = -1;
    for (int i = 0; i < LW_HOOKS; i++) {
        if (s_hooks[i] == fn) { free_slot = -2; break; }
        if (!s_hooks[i] && free_slot == -1) free_slot = i;
    }
    if (free_slot >= 0) s_hooks[free_slot] = fn;
    portEXIT_CRITICAL(&s_mux);
}

void last_words_report(void)
{
    if (s_reported) return;
    s_reported = true;
    if (s_abnormal) ESP_LOGW(TAG, "reset reason: %s", reason_name(s_reason));
    else            ESP_LOGI(TAG, "reset reason: %s%s%s", reason_name(s_reason), s_note_text[0] ? ": " : "", s_note_text);
#ifdef DEVICE_BATTERY_SLEEP
    if (s_crumb_torn) ESP_LOGW(TAG, "last sleep breadcrumb unreadable (torn write)");
    if (s_crumb_have) {
        const last_words_crumb_t *a = &s_crumb_prev.at;
        ESP_LOGW(TAG, "last sleep breadcrumb: wake #%u at %u ms: want S%u applied S%u in_s2=%u sleeps=%u holds=0x%x render_stage=%u",
                 (unsigned)s_crumb_prev.seq, (unsigned)a->tick_ms, (unsigned)a->want, (unsigned)a->applied, (unsigned)a->in_s2, (unsigned)a->sleeps,
                 (unsigned)a->holds, (unsigned)a->stage);
        if (s_crumb_prev.wdt_seen) ESP_LOGW(TAG, "starved at the watchdog: %s", s_crumb_prev_starved[0] ? s_crumb_prev_starved : "no entry");
    }
    s_crumb_have = s_crumb_torn = false;
#endif
    for (int i = 0; i < LW_HOOKS; i++)
        if (s_hooks[i]) s_hooks[i]();   // the lines that were printed at boot, before a daemon could hear them
    if (!s_previous_len) return;
    ESP_LOGW(TAG, "last %u bytes of log before that reset follow", (unsigned)s_previous_len);
    // The first line is very likely a fragment (the ring wrapped through it); everything after is whole.
    char *p = s_previous;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p) ESP_LOGW(TAG, "last: %s", p);
        p = nl ? nl + 1 : NULL;
    }
    ESP_LOGW(TAG, "end of last words");
    s_previous_len = 0;
}
