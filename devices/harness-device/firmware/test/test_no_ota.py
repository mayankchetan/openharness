"""DEVICE_NO_OTA (plan C17): the dial is updated over USB, so the OTA code and its RAM drop out of the product build.

Checks, none of which needs the board:
  1. main/CMakeLists.txt turns the switch ON unless -DDEVICE_NO_OTA=0, and TASK_STACK_PSRAM_READER (the cable_link reader
     stack in PSRAM) can only be on together with it, because esp_ota_* asserts on a PSRAM stack.
  2. fw_update.c built with -DDEVICE_NO_OTA=1 keeps fw_mark_valid() (the rollback guard, flash code only) and nothing else:
     one defined function, no static data beyond the log tag; built without the flag it still has the whole state machine.
  3. cable_client.c: every fw_update_* call sits under #ifndef DEVICE_NO_OTA, and the fw.offer branch answers
     fw.error "not supported" when the switch is on (run for real against stubs), without touching fw_update_offer.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'

# 1. the switch and the reader stack
cmake = (main / 'CMakeLists.txt').read_text(encoding='utf-8')
assert re.search(r'if\(NOT DEFINED DEVICE_NO_OTA\)\s+set\(DEVICE_NO_OTA ON\)\s+endif\(\)', cmake), 'default ON'
assert re.search(r'if\(DEVICE_NO_OTA\)\s+target_compile_definitions\(\$\{COMPONENT_LIB\} PRIVATE DEVICE_NO_OTA=1\)', cmake)
util = (main / 'task_util.h').read_text(encoding='utf-8')
assert re.search(r'#ifndef TASK_STACK_PSRAM_READER\s+#ifdef DEVICE_NO_OTA\s+#define TASK_STACK_PSRAM_READER 1\s+#else\s+#define TASK_STACK_PSRAM_READER 0',
                 util), 'reader stack in PSRAM follows the switch'
assert re.search(r'#if TASK_STACK_PSRAM_READER && !defined\(DEVICE_NO_OTA\)\s+#error', util), 'PSRAM reader without NO_OTA is an error'
link = (main / 'cable_link.c').read_text(encoding='utf-8')
assert '#include "task_util.h"' in link
block = re.search(r'#if TASK_STACK_PSRAM_READER\n(.*?)#else\n(.*?)#endif', link, re.S)
assert block and 'task_create_psram(reader_task, "cable_link"' in block[1] and 'xTaskCreate(reader_task, "cable_link"' in block[2]

# 2. fw_update.c with and without the switch
source = re.sub(r'^#include .*\n', '', (main / 'fw_update.c').read_text(encoding='utf-8'), flags=re.M)
stubs = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int esp_err_t;
typedef unsigned esp_ota_handle_t;
typedef int esp_ota_img_states_t;
typedef struct { uint32_t size; } esp_partition_t;
typedef struct { char version[32]; } esp_app_desc_t;
typedef struct { uint32_t hash; size_t bytes; } mbedtls_sha256_context;
enum { ESP_OK = 0, ESP_OTA_IMG_PENDING_VERIFY = 1 };
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
const char *esp_err_to_name(int err);
int64_t esp_timer_get_time(void);
bool audio_client_active(void);
const esp_partition_t *esp_ota_get_running_partition(void);
int esp_ota_get_state_partition(const esp_partition_t *, int *);
int esp_ota_mark_app_valid_cancel_rollback(void);
const esp_app_desc_t *esp_app_get_description(void);
const esp_partition_t *esp_ota_get_next_update_partition(const void *);
int esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
int esp_ota_write(esp_ota_handle_t, const void *, size_t);
int esp_ota_end(esp_ota_handle_t);
int esp_ota_abort(esp_ota_handle_t);
int esp_ota_set_boot_partition(const esp_partition_t *);
int esp_ota_get_partition_description(const esp_partition_t *, esp_app_desc_t *);
int esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
void esp_restart(void);
void vTaskDelay(int);
void mbedtls_sha256_init(mbedtls_sha256_context *);
void mbedtls_sha256_free(mbedtls_sha256_context *);
int mbedtls_sha256_starts(mbedtls_sha256_context *, int);
int mbedtls_sha256_update(mbedtls_sha256_context *, const unsigned char *, size_t);
int mbedtls_sha256_finish(mbedtls_sha256_context *, unsigned char *);
void cable_client_fw_accept(void);
void cable_client_fw_progress(uint32_t);
void cable_client_fw_done(void);
void cable_client_fw_error(const char *);
void ui_show_projects(void);
void ui_show_ota(void);
void ui_show_ota_progress(int);
'''
# the real header's prototypes
header = re.sub(r'^#include .*\n|^#pragma once\n', '', (main / 'fw_update.h').read_text(encoding='utf-8'), flags=re.M)


def build(flags):
    """(defined function names, bytes of .data + .bss) of fw_update.c compiled with `flags`."""
    with tempfile.TemporaryDirectory(prefix='harness-no-ota-') as folder:
        out = Path(folder)
        (out / 'fw.c').write_text(stubs + header + source, encoding='utf-8')
        # warnings from the stubbed OTA calls the real headers declare differently are not the point here
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-w', '-c', str(out / 'fw.c'), '-o', str(out / 'fw.o')] + flags, check=True)
        names = subprocess.run(['nm', '--defined-only', str(out / 'fw.o')], check=True, capture_output=True, text=True).stdout
        sections = subprocess.run(['size', '-A', str(out / 'fw.o')], check=True, capture_output=True, text=True).stdout
    functions = set()
    for line in names.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] in ('T', 't') and not parts[-1].startswith('.'):
            functions.add(parts[-1])
    data = sum(int(m[1]) for m in (line.split() for line in sections.splitlines()) if len(m) == 3 and m[0] in ('.data', '.bss') and m[1].isdigit())
    return functions, data


functions_off, data_off = build(['-DDEVICE_NO_OTA=1'])
assert functions_off == {'fw_mark_valid'}, f'NO_OTA keeps only fw_mark_valid, got {sorted(functions_off)}'
assert data_off <= 8, f'NO_OTA keeps at most the log tag pointer as data, got {data_off} B'

functions_on, data_on = build([])
assert {'fw_mark_valid', 'fw_update_offer', 'fw_update_slice', 'fw_update_tick', 'fw_update_abort', 'fw_update_active'} <= functions_on
assert data_on > 100, f'the full state machine keeps its statics, got {data_on} B'
print(f'no-ota: fw_update.c static data {data_on} B with OTA, {data_off} B without; functions {len(functions_on)} -> {len(functions_off)}')

# 3. cable_client.c
client = (main / 'cable_client.c').read_text(encoding='utf-8')
lines = client.splitlines()
guard = []   # per open #if: [kind, in OTA-only branch]; kind 'no' = #ifndef DEVICE_NO_OTA, 'yes' = #ifdef DEVICE_NO_OTA, '' = other
for number, line in enumerate(lines, 1):
    stripped = line.strip()
    if re.match(r'#\s*if', stripped):
        kind = 'no' if re.match(r'#\s*ifndef\s+DEVICE_NO_OTA\b', stripped) else 'yes' if re.match(r'#\s*ifdef\s+DEVICE_NO_OTA\b', stripped) else ''
        guard.append([kind, kind == 'no'])
    elif re.match(r'#\s*else', stripped) and guard:
        if guard[-1][0]:
            guard[-1][1] = not guard[-1][1]
    elif re.match(r'#\s*endif', stripped) and guard:
        guard.pop()
    elif re.search(r'\bfw_update_(offer|slice|tick|abort|active)\s*\(', line) and not stripped.startswith(('//', '*', '/*')):
        assert any(g[1] for g in guard), f'cable_client.c:{number} calls {stripped!r} outside the OTA-only branch of DEVICE_NO_OTA'

offer = re.search(r'    if \(strcmp\(t, "fw\.offer"\) == 0\) \{\n.*?\n        return;\n    \}\n', client, re.S)
assert offer, 'fw.offer branch'
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { int valueint; double valuedouble; int number; } cJSON;
static unsigned offers, errors;
static char last_error[64];
__attribute__((unused)) static const char *str_of(const cJSON *p, const char *key) { (void)p; return key; }
__attribute__((unused)) static bool cJSON_IsNumber(const cJSON *n) { return n && n->number; }
__attribute__((unused)) static const cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *p, const char *key) { (void)key; return p; }
__attribute__((unused)) static bool fw_update_offer(const char *v, int size, const char *sha) { (void)v; (void)size; (void)sha; offers++; return true; }
__attribute__((unused)) static void cable_client_fw_error(const char *message) { errors++; snprintf(last_error, sizeof last_error, "%s", message); }
static void handle(const cJSON *p, const char *t) {
    (void)p;
''' + offer[0] + r'''
}
int main(void) {
    cJSON p = {10, 10.0, 1};
    handle(&p, "fw.offer");
    handle(&p, "ping");
    assert(offers + errors == 1);
#ifdef DEVICE_NO_OTA
    assert(offers == 0 && errors == 1 && !strcmp(last_error, "not supported"));
    puts("fw.offer: answered fw.error 'not supported', no offer taken");
#else
    assert(offers == 1 && errors == 0);
    puts("fw.offer: handed to fw_update_offer");
#endif
    return 0;
}
'''
for flags, label in (([], 'OTA on'), (['-DDEVICE_NO_OTA=1'], 'OTA off')):
    with tempfile.TemporaryDirectory(prefix='harness-no-ota-') as folder:
        out = Path(folder)
        (out / 'offer.c').write_text(harness, encoding='utf-8')
        built = subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g'] + flags +
                               [str(out / 'offer.c'), '-o', str(out / 'offer')], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        result = subprocess.run([str(out / 'offer')], check=True, capture_output=True, text=True).stdout.strip()
        print(f'no-ota {label}: {result}')

print('no-ota: fw_update.c keeps fw_mark_valid() only (statics gone), cable_client.c guards every fw_update_* call, '
      'the cable_link reader stack follows the switch PASS')
