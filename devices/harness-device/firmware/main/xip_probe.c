// XiP probe: can a task with a PSRAM stack call NVS, OTA and flash-read code? DIAGNOSTIC ONLY.
//
// Built only with -DDEVICE_XIP_PROBE=1 (CMake option, off in every normal build; the file is not even
// compiled without it). It never ships. Log prefix "XIP"; end marker "XIP DONE".
//
// Why: the image runs code and rodata from PSRAM (SPIRAM_FETCH_INSTRUCTIONS + SPIRAM_RODATA), so
// MMAP_EXECUTABLES_FROM_FLASH is 0 and spi_flash_os_func_app.c:239 keeps the cache ON for a flash write unless a
// persistent mmap exists (flash_mmap_remain()). The stack assert esp_task_stack_is_sane_cache_disabled()
// (spi_flash/cache_utils.c:141) therefore fires only on the cache-disable path. This file runs the real
// operations from tasks whose stacks are in PSRAM and logs what happens.
//
// Cases (each task is created with xTaskCreate*WithCaps(..., MALLOC_CAP_SPIRAM); each logs its stack address):
//   A_core0, A_core1  1,000 x NVS blob write + commit in the test namespace "xipprobe"
//   B                 1,000 x esp_ota_write of 4 KB to the inactive OTA slot (sequential-erase mode, so every
//                     write also erases a sector). The slot content is lost; the board still boots the active slot.
//   C                 1,000 x esp_partition_read of 64 KB of the running slot + SHA-256 (as fw_update.c:160)
//   D1                NVS writer on an INTERNAL stack while a PSRAM-stack task copies the running image into the
//                     inactive slot and runs esp_ota_end (image verify: bootloader_mmap with BLOCKS_WRITE)
//   D2                the same with the NVS writer on a PSRAM stack
//   E                 D2 again, after a pause (XIP PHASE3) so the host can animate the display; the habitat
//                     frame counters are logged before and after
// flash_mmap_remain() is logged before and after every op (counted; one progress line per 100 ops).
//
// A panic or watchdog reset during a case is recorded in RTC no-init memory; the next boot logs
// "XIP PANIC-RECOVERY case=<name>" (that case FAILED), skips the probe and prints "XIP DONE".
// A reboot runs the probe again (it starts about XIP_START_DELAY_S after boot).
#include "xip_probe.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "ram_telemetry.h"
#include "ui/habitat/runtime.h"

#define XIP_START_DELAY_S 75
#define NVS_OPS 1000
#define OTA_OPS 1000
#define SHA_OPS 1000
#define CHUNK 4096

static const char *TAG = "XIP";

typedef struct {
    uint32_t magic;
    uint32_t running;
    char name[16];
} xip_rtc_t;
#define XIP_MAGIC 0x58495031u
static RTC_NOINIT_ATTR xip_rtc_t s_rtc;

static uint8_t s_buf[CHUNK];   // internal: flash read/write source and target

typedef struct {
    const char *name;
    int core;                 // tskNO_AFFINITY, 0 or 1
    bool psram_stack;
    uint32_t stack_bytes;
    TaskHandle_t waiter;
    // results
    uint32_t ops, ok, fail, remain_before, remain_after;
    esp_err_t first_err;
    int stack_in_psram;
    uint32_t max_op_us;
    uint32_t overlap_ops;     // D/E: writer ops that started while esp_ota_end ran
    int end_rc;               // D/E
    bool done;
} job_t;

static volatile int s_end_phase;    // 1 while esp_ota_end runs
static volatile int s_stop_writer;

static void mark_running(const char *name)
{
    s_rtc.magic = XIP_MAGIC;
    s_rtc.running = 1;
    snprintf(s_rtc.name, sizeof s_rtc.name, "%s", name);
}

static void mark_idle(void)
{
    s_rtc.running = 0;
}

static void stack_check(job_t *j)
{
    void *sp = (void *)esp_cpu_get_sp();
    j->stack_in_psram = esp_ptr_external_ram(sp) ? 1 : 0;
    ESP_LOGI(TAG, "STACKCHECK case=%s sp=%p psram=%d want_psram=%d core=%d free_stack_hwm=%u", j->name, sp,
             j->stack_in_psram, j->psram_stack ? 1 : 0, (int)xPortGetCoreID(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void note_op(job_t *j, esp_err_t e, bool rb, bool ra, int64_t t0)
{
    j->ops++;
    if (e == ESP_OK) j->ok++;
    else {
        j->fail++;
        if (j->first_err == ESP_OK) j->first_err = e;
    }
    j->remain_before += rb ? 1 : 0;
    j->remain_after += ra ? 1 : 0;
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > j->max_op_us) j->max_op_us = us;
    if (j->ops % 100 == 0) {
        ESP_LOGI(TAG, "%s op=%u ok=%u fail=%u remain_before_true=%u remain_after_true=%u max_op_us=%u",
                 j->name, (unsigned)j->ops, (unsigned)j->ok, (unsigned)j->fail, (unsigned)j->remain_before,
                 (unsigned)j->remain_after, (unsigned)j->max_op_us);
    }
}

// ---- workers ---------------------------------------------------------------------------------------

static void nvs_ops(job_t *j, int count, bool continuous_until_stop, uint32_t pause_ms)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("xipprobe", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        j->fail++;
        j->first_err = e;
        ESP_LOGE(TAG, "%s nvs_open: %s", j->name, esp_err_to_name(e));
        return;
    }
    uint8_t blob[48];
    for (int i = 0; continuous_until_stop ? !s_stop_writer : i < count; i++) {
        for (size_t k = 0; k < sizeof blob; k++) blob[k] = (uint8_t)(i * 7 + k);   // changes every op: NVS skips equal values
        bool phase = s_end_phase;
        int64_t t0 = esp_timer_get_time();   // before the freeze-lock read: a stall behind an mmap shows up here
        bool rb = ram_flash_mmap_remain();
        e = nvs_set_blob(h, "k", blob, sizeof blob);
        if (e == ESP_OK) e = nvs_commit(h);
        bool ra = ram_flash_mmap_remain();
        if (phase || s_end_phase) j->overlap_ops++;
        note_op(j, e, rb, ra, t0);
        if (pause_ms) vTaskDelay(pdMS_TO_TICKS(pause_ms));
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

static void worker_nvs(void *arg)
{
    job_t *j = arg;
    stack_check(j);
    nvs_ops(j, NVS_OPS, false, 0);
    j->done = true;
    xTaskNotifyGive(j->waiter);
    vTaskDeleteWithCaps(NULL);
}

static void worker_nvs_concurrent(void *arg)   // runs until s_stop_writer; PSRAM or internal stack
{
    job_t *j = arg;
    stack_check(j);
    nvs_ops(j, 0, true, 5);
    j->done = true;
    xTaskNotifyGive(j->waiter);
    if (j->psram_stack) vTaskDeleteWithCaps(NULL);
    else vTaskDelete(NULL);
}

static void worker_ota_write(void *arg)
{
    job_t *j = arg;
    stack_check(j);
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t oh = 0;
    esp_err_t e = (run && target) ? esp_partition_read(run, 0, s_buf, CHUNK) : ESP_ERR_NOT_FOUND;   // valid image header 0xE9
    if (e == ESP_OK) e = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &oh);
    if (e != ESP_OK) {
        j->fail++;
        j->first_err = e;
        ESP_LOGE(TAG, "B setup: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "B target=%s offset=0x%x", target->label, (unsigned)target->address);
        for (int i = 0; i < OTA_OPS; i++) {
            int64_t t0 = esp_timer_get_time();
            bool rb = ram_flash_mmap_remain();
            e = esp_ota_write(oh, s_buf, CHUNK);
            bool ra = ram_flash_mmap_remain();
            note_op(j, e, rb, ra, t0);
            if (e != ESP_OK) break;
        }
        esp_ota_abort(oh);
    }
    j->done = true;
    xTaskNotifyGive(j->waiter);
    vTaskDeleteWithCaps(NULL);
}

static void worker_sha(void *arg)
{
    job_t *j = arg;
    stack_check(j);
    const esp_partition_t *run = esp_ota_get_running_partition();
    uint8_t first[32] = {0}, d[32];
    for (int i = 0; i < SHA_OPS && run; i++) {
        int64_t t0 = esp_timer_get_time();
        bool rb = ram_flash_mmap_remain();
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        esp_err_t e = mbedtls_sha256_starts(&ctx, 0) == 0 ? ESP_OK : ESP_FAIL;
        for (uint32_t off = 0; e == ESP_OK && off < 65536; off += CHUNK) {
            e = esp_partition_read(run, off, s_buf, CHUNK);
            if (e == ESP_OK && mbedtls_sha256_update(&ctx, s_buf, CHUNK) != 0) e = ESP_FAIL;
        }
        if (e == ESP_OK && mbedtls_sha256_finish(&ctx, d) != 0) e = ESP_FAIL;
        mbedtls_sha256_free(&ctx);
        if (e == ESP_OK) {
            if (i == 0) memcpy(first, d, 32);
            else if (memcmp(first, d, 32) != 0) e = ESP_ERR_INVALID_CRC;   // the running slot is read-only: digest must repeat
        }
        bool ra = ram_flash_mmap_remain();
        note_op(j, e, rb, ra, t0);
    }
    j->done = true;
    xTaskNotifyGive(j->waiter);
    vTaskDeleteWithCaps(NULL);
}

// D/E ota task: copy the running image to the inactive slot, then esp_ota_end (verify = bootloader_mmap with
// SPI_FLASH_MMAP_FLAG_BLOCKS_WRITE, bootloader_flash.c:69) while the NVS writer keeps committing.
static void worker_ota_end(void *arg)
{
    job_t *j = arg;
    stack_check(j);
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t oh = 0;
    esp_image_metadata_t meta;
    const esp_partition_pos_t pos = run ? (esp_partition_pos_t){.offset = run->address, .size = run->size}
                                        : (esp_partition_pos_t){0};
    esp_err_t e = (run && target) ? esp_image_get_metadata(&pos, &meta) : ESP_ERR_NOT_FOUND;
    if (e == ESP_OK) e = esp_ota_begin(target, meta.image_len, &oh);
    if (e != ESP_OK) {
        j->fail++;
        j->first_err = e;
        ESP_LOGE(TAG, "%s setup: %s", j->name, esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "%s copy image_len=%u from %s to %s", j->name, (unsigned)meta.image_len, run->label, target->label);
        for (uint32_t off = 0; off < meta.image_len && e == ESP_OK; off += CHUNK) {
            uint32_t n = meta.image_len - off < CHUNK ? meta.image_len - off : CHUNK;
            e = esp_partition_read(run, off, s_buf, n);
            if (e == ESP_OK) e = esp_ota_write(oh, s_buf, n);
        }
        if (e != ESP_OK) {
            j->fail++;
            j->first_err = e;
            esp_ota_abort(oh);
        } else {
            ESP_LOGI(TAG, "%s esp_ota_end begins (verify)", j->name);
            bool rb = ram_flash_mmap_remain();
            int64_t t0 = esp_timer_get_time();
            s_end_phase = 1;
            e = esp_ota_end(oh);
            s_end_phase = 0;
            j->end_rc = (int)e;
            ESP_LOGI(TAG, "%s esp_ota_end rc=%s took_ms=%u remain_before=%d", j->name, esp_err_to_name(e),
                     (unsigned)((esp_timer_get_time() - t0) / 1000), (int)rb);
            note_op(j, e, rb, ram_flash_mmap_remain(), t0);
        }
    }
    j->done = true;
    xTaskNotifyGive(j->waiter);
    vTaskDeleteWithCaps(NULL);
}

// ---- runner ----------------------------------------------------------------------------------------

static bool spawn(job_t *j, TaskFunction_t fn, uint32_t prio)
{
    j->waiter = xTaskGetCurrentTaskHandle();
    BaseType_t rc;
    if (j->psram_stack) {
        rc = xTaskCreatePinnedToCoreWithCaps(fn, j->name, j->stack_bytes, j, prio, NULL, j->core, MALLOC_CAP_SPIRAM);
    } else {
        rc = xTaskCreatePinnedToCore(fn, j->name, j->stack_bytes, j, prio, NULL, j->core);
    }
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "task create failed case=%s psram=%d", j->name, (int)j->psram_stack);
        j->fail++;
        j->first_err = ESP_ERR_NO_MEM;
        j->done = true;
        return false;
    }
    return true;
}

static bool wait_job(job_t *j, uint32_t timeout_ms)
{
    // Two workers can notify the controller in the D/E cases, so poll the done flag instead of trusting one take.
    for (uint32_t waited = 0; !j->done && waited < timeout_ms; waited += 100) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    if (!j->done) {
        ESP_LOGE(TAG, "TIMEOUT case=%s after %u ms (ops=%u)", j->name, (unsigned)timeout_ms, (unsigned)j->ops);
        j->fail++;
        return false;
    }
    return true;
}

static int s_pass, s_fail;

static void result(job_t *j, bool extra_pass_condition, const char *note)
{
    bool pass = j->done && j->fail == 0 && extra_pass_condition;
    pass ? s_pass++ : s_fail++;
    ESP_LOGI(TAG, "RESULT case=%s pass=%d ops=%u ok=%u fail=%u first_err=%s stack_psram=%d remain_before_true=%u "
             "remain_after_true=%u max_op_us=%u overlap_ops=%u end_rc=%d %s",
             j->name, pass ? 1 : 0, (unsigned)j->ops, (unsigned)j->ok, (unsigned)j->fail,
             j->first_err == ESP_OK ? "-" : esp_err_to_name(j->first_err), j->stack_in_psram,
             (unsigned)j->remain_before, (unsigned)j->remain_after, (unsigned)j->max_op_us,
             (unsigned)j->overlap_ops, j->end_rc, note ? note : "");
}

static void run_simple(job_t *j, TaskFunction_t fn, uint32_t timeout_ms, uint32_t expect_ops)
{
    ESP_LOGI(TAG, "CASE %s begin (stack %s, %u B)", j->name, j->psram_stack ? "PSRAM" : "internal", (unsigned)j->stack_bytes);
    mark_running(j->name);
    if (spawn(j, fn, 5)) wait_job(j, timeout_ms);
    mark_idle();
    result(j, j->ops == expect_ops && (!j->psram_stack || j->stack_in_psram), NULL);
}

static void perf_line(const char *when, const char *case_name)
{
    habitat_perf_t p;
    habitat_perf_get(&p);
    ESP_LOGI(TAG, "DISPLAY %s case=%s frames=%u frame_max_us=%u raster_max_us=%u input_max_us=%u", when, case_name,
             (unsigned)p.frames, (unsigned)p.frame_max_us, (unsigned)p.raster_max_us, (unsigned)p.input_max_us);
}

static void run_concurrent(const char *name, bool writer_psram)
{
    static job_t ota, wr;
    memset(&ota, 0, sizeof ota);
    memset(&wr, 0, sizeof wr);
    char oname[16], wname[16];
    snprintf(oname, sizeof oname, "%s_ota", name);
    snprintf(wname, sizeof wname, "%s_nvs", name);
    ota.name = oname; ota.core = tskNO_AFFINITY; ota.psram_stack = true; ota.stack_bytes = 8192;
    wr.name = wname;  wr.core = tskNO_AFFINITY;  wr.psram_stack = writer_psram; wr.stack_bytes = 6144;
    s_stop_writer = 0;
    s_end_phase = 0;
    ESP_LOGI(TAG, "CASE %s begin (ota task PSRAM stack; NVS writer %s stack)", name, writer_psram ? "PSRAM" : "internal");
    perf_line("before", name);
    mark_running(name);
    // The writer reports completion to this task as well, so give it its own waiter slot: both notify us.
    wr.waiter = xTaskGetCurrentTaskHandle();
    bool ok_w = spawn(&wr, worker_nvs_concurrent, 5);
    bool ok_o = spawn(&ota, worker_ota_end, 5);
    if (ok_o) wait_job(&ota, 240000);
    s_stop_writer = 1;
    if (ok_w) wait_job(&wr, 30000);
    mark_idle();
    perf_line("after", name);
    result(&ota, ota.end_rc == 0 && ota.stack_in_psram, "role=ota_end");
    result(&wr, wr.ops > 0 && (!writer_psram || wr.stack_in_psram), "role=nvs_writer");
}

static void controller(void *arg)
{
    (void)arg;
    esp_reset_reason_t rr = esp_reset_reason();
    bool crash = rr == ESP_RST_PANIC || rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT;
    if (rr == ESP_RST_POWERON) s_rtc.magic = 0;   // RTC no-init memory is garbage after a power cycle
    if (crash) {
        if (s_rtc.magic == XIP_MAGIC && s_rtc.running) {
            s_rtc.name[sizeof s_rtc.name - 1] = 0;
            ESP_LOGE(TAG, "PANIC-RECOVERY case=%s reset_reason=%d (that case FAILED with a panic or watchdog)", s_rtc.name, (int)rr);
            s_rtc.running = 0;
        }
        ESP_LOGW(TAG, "SKIP reset_reason=%d (crash reboot: probe not run; power-cycle or flash again to run)", (int)rr);
        ESP_LOGI(TAG, "SUMMARY pass=0 fail=0 skipped=1");
        xip_probe_banner();
        ESP_LOGI(TAG, "DONE");
        vTaskDelete(NULL);
        return;
    }
    while (esp_timer_get_time() < (int64_t)XIP_START_DELAY_S * 1000000) vTaskDelay(pdMS_TO_TICKS(1000));
    xip_probe_banner();
    ESP_LOGI(TAG, "START reset_reason=%d idf=%s delay_s=%d (display should be IDLE for phases 1 and 2)", (int)rr,
             esp_get_idf_version(), XIP_START_DELAY_S);
    ram_telemetry_checkpoint("xip_start");

    static job_t a0, a1, b, c;
    memset(&a0, 0, sizeof a0); memset(&a1, 0, sizeof a1); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    a0 = (job_t){.name = "A_core0", .core = 0, .psram_stack = true, .stack_bytes = 6144};
    a1 = (job_t){.name = "A_core1", .core = 1, .psram_stack = true, .stack_bytes = 6144};
    b  = (job_t){.name = "B", .core = tskNO_AFFINITY, .psram_stack = true, .stack_bytes = 5120};
    c  = (job_t){.name = "C", .core = tskNO_AFFINITY, .psram_stack = true, .stack_bytes = 6144};

    ESP_LOGI(TAG, "PHASE1 display idle: A (NVS), B (OTA write), C (read+SHA)");
    run_simple(&a0, worker_nvs, 120000, NVS_OPS);
    run_simple(&a1, worker_nvs, 120000, NVS_OPS);
    run_simple(&b, worker_ota_write, 300000, OTA_OPS);
    run_simple(&c, worker_sha, 300000, SHA_OPS);

    ESP_LOGI(TAG, "PHASE2 display idle: NVS write concurrent with esp_ota_end");
    run_concurrent("D1", false);
    run_concurrent("D2", true);

    ESP_LOGI(TAG, "PHASE3 animate_now wait_s=25 (start the display load now; case E repeats D2)");
    vTaskDelay(pdMS_TO_TICKS(25000));
    run_concurrent("E", true);

    ram_telemetry_checkpoint("xip_end");
    ESP_LOGI(TAG, "SUMMARY pass=%d fail=%d skipped=0", s_pass, s_fail);
    xip_probe_banner();
    ESP_LOGI(TAG, "DONE");
    vTaskDelete(NULL);
}

void xip_probe_banner(void)
{
    ESP_LOGE(TAG, "################################################################");
    ESP_LOGE(TAG, "XIP PROBE BUILD - NOT FOR PRODUCTION");
    ESP_LOGE(TAG, "diagnostic image: erases the inactive OTA slot, writes NVS 'xipprobe'");
    ESP_LOGE(TAG, "reflash a normal build when the run is done");
    ESP_LOGE(TAG, "################################################################");
}

void xip_probe_start(void)
{
    if (xTaskCreate(controller, "xip_ctl", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "controller task create failed");
        ESP_LOGI(TAG, "DONE");
    }
}
