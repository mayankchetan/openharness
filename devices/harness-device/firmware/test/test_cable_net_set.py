"""net.set and the other Wi-Fi settings messages (cable_client.c handle_net; docs/specs/cable-wifi-link.md section 3).

Two halves. Source checks, in the style of test_cable_outbound.py: no value is logged, a request is refused off the cable
before any setting is touched, the JSON guard stands in front of the parser. Then the real handler: its functions are cut out of
cable_client.c and run against the real cJSON, the real config_store.c and an in-memory NVS (test/host_nvs), with the link
swapped for a fake. Requires IDF_PATH (cJSON), like test_cable_outbound.py.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
MAIN = HERE / '../main'
CLIENT = (MAIN / 'cable_client.c').read_text()
STORE = (MAIN / 'config_store.c').read_text()
LINK = (MAIN / 'cable_link.c').read_text()
JSON_DIR = Path(os.environ['IDF_PATH']) / 'components/json/cJSON'


def function(name, source=CLIENT):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\r?\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'


# ── source checks ────────────────────────────────────────────────────────────────────────────────────

handle_net = function('handle_net')
net_reply = function('net_reply')
net_block = CLIENT[CLIENT.index("THE DIAL'S WI-FI SETTINGS"):CLIENT.index('static void handle_message(')]

net_store = function('net_store_task')

# 1. Off the cable, nothing is touched: the transport check is the first thing handle_net does, before any config_net_ call.
first_call = re.search(r'config_net_(?!err_t)\w+\(', handle_net).start()
assert 'cable_link_transport() != CABLE_XPORT_USB' in handle_net
assert handle_net.index('cable_link_transport() != CABLE_XPORT_USB') < first_call
assert handle_net.index('cable_client_supports(CABLE_FEATURE_NET)') < first_call

# 1b. THE READER WRITES NO NVS. The cable_link reader's stack is in PSRAM and must not run a flash write (power_off_request follows the
# same rule). handle_net only reads and checks: it calls no nvs_ function and no config_net_ writer, and hands every write to the
# net_store task, which has an internal stack (xTaskCreate, not task_create_psram).
assert not re.search(r'\bnvs_\w+', net_block)
allowed_in_handle_net = {'config_net_key_decode', 'config_net_check', 'config_net_check_host', 'config_net_check_mode', 'config_net_check_key', 'config_net_err_name', 'config_net_err_t'}
assert set(re.findall(r'\bconfig_net_\w+', handle_net)) <= allowed_in_handle_net, set(re.findall(r'\bconfig_net_\w+', handle_net))
writers = {'config_net_set', 'config_net_set_ble', 'config_net_set_key', 'config_net_set_host', 'config_net_set_mode', 'config_net_clear'}
for name in writers:
    assert not re.search(r'\b' + name + r'\(', handle_net), name
    assert re.search(r'\b' + name + r'\(', net_store), name
# the writers are called from net_store_task and from nowhere else in cable_client.c
for name in writers:
    assert len(re.findall(r'\b' + name + r'\(', CLIENT)) == 1, name
assert re.search(r'xTaskCreate\(net_store_task, "net_store", NET_STORE_STACK, NULL, \d, NULL\) == pdPASS', handle_net)
net_code = re.sub(r'//[^\n]*', '', net_block)
assert 'task_create_psram' not in net_code and 'WithCaps' not in net_code
assert re.search(r'#define NET_STORE_STACK 4096\b', net_block)
# one request at a time, claimed before the slot is touched; released only by the task (or the failed start)
assert handle_net.index('atomic_exchange(&s_net_busy, true)') < handle_net.index('s_net_req.id = id')
assert net_store.rindex('atomic_store(&s_net_busy, false)') > net_store.index('net_reply(')
# the slot is overwritten before the reply goes out and the task ends itself
assert net_store.index('net_zero(&s_net_req') < net_store.index('net_reply(') < net_store.index('vTaskDelete(NULL)')
# the stack high-water mark is logged once per run
assert net_store.count('uxTaskGetStackHighWaterMark(NULL)') == 1

# 2. handle_net is reached from one place, handle_message, which only ever sees a frame that passed the JSON guard.
assert len(re.findall(r'\bhandle_net\(', CLIENT)) == 2   # the definition and the one call
assert re.search(r'strncmp\(t, "net\.", 4\) == 0\) \{ handle_net\(t, p\); return; \}', CLIENT)
assert CLIENT.index('cable_json_guard(payload, payload_len)') < CLIENT.index('handle_message(root);')
assert len(re.findall(r'\bhandle_message\(', CLIENT)) == 2   # the definition and the one call

# 3. The reply holds booleans, the SSID, the address and the mode: no field named pass or key, and the full configuration is never read for it.
assert not re.search(r'msg_\w+\(&root, "(pass|key)"', net_reply)
assert 'config_net_load' not in net_block
assert 'config_net_info' in net_reply

# 4. No log line of the handler or of the Wi-Fi part of the config store can carry a value: the handler logs nothing itself, and each line of
# the store prints lengths and fixed words only, never the SSID, the password, the address or the key.
def log_calls(text):
    return re.findall(r'ESP_LOG[A-Z]\((.*?)\);', text, re.S)


wifi_store = STORE[STORE.index('--- Wi-Fi settings'):STORE.index('bool config_clear_all(void)')]
net_logs = log_calls(net_block)
assert len(net_logs) == 1 and 'stack high-water' in net_logs[0] and '%s' not in net_logs[0], 'only net_store logs, once, a number'
logs = log_calls(wifi_store)
assert len(logs) >= 5
for call in logs:
    parts = re.match(r'\s*TAG,\s*"((?:[^"\\]|\\.)*)"(.*)$', call, re.S)
    fmt, args = parts.group(1), parts.group(2)
    fixed_words = len(re.findall(r'config_net_err_name\(|ok \? "', args))
    args = re.sub(r'\(unsigned\)bounded_len\([^()]*\)', '', args)
    args = re.sub(r'config_net_err_name\([^()]*\)', '', args)
    args = re.sub(r'ok \? "[^"]*" : "[^"]*"', '', args)
    assert not re.search(r'\b(ssid|pass|host|key|port)\b', args), call
    assert fmt.count('%s') == fixed_words, call

# 5. The strings of the request are overwritten once used.
# every way out of handle_net passes the label where the secrets of the request are overwritten: no return statement at all
assert not re.search(r'\breturn\b', re.sub(r'//[^\n]*', '', handle_net))
done = handle_net[handle_net.index('done:'):]
assert 'net_scrub(pass)' in done and 'net_scrub(key_text)' in done
assert handle_net.index('char *pass = (char *)net_text(p, "pass")') < handle_net.index('goto done')   # found before the first way out
# the decoder's own copy of a net.* frame is overwritten once the frame has been handled, whatever became of it
frame = CLIENT[CLIENT.index('static void on_frame('):CLIENT.index('// ── session task')]
assert 'memcmp(payload + i, "\\"net.", 5)' in frame
assert frame.index('handle_message(root)') < frame.index('cable_link_scrub_payload(payload, payload_len)')
assert len(re.findall(r'\breturn;', frame[frame.index('carries_secret = false'):])) == 0, 'no way out of the JSON path skips the scrub'
assert 'net_zero(key, sizeof key)' in handle_net   # the reader's own copy of the key

# 6. The hello offers net on the cable only, and ble (an image with the BLE link) on the cable only.
assert re.search(r'if \(cable_link_transport\(\) == CABLE_XPORT_USB\) msg_string\(&root, "net", "v1"\);', CLIENT)
assert re.search(r'#ifdef DEVICE_BLE_LINK\n[^\n]*\n\s*if \(cable_link_transport\(\) == CABLE_XPORT_USB\) msg_string\(&root, "ble", "v1"\);\n#endif', CLIENT)

# 7. B6: the phone-mode forms (key-only net.set, net.mode 2) need the image AND the daemon's "ble"; the refusal comes before any write.
assert re.search(r'static bool ble_provisioning_allowed\(void\)\s*\{\s*return cable_ble_link_built\(\) && cable_client_supports\(CABLE_FEATURE_BLE\);\s*\}', CLIENT)
assert 'feature_refused = !ble_provisioning_allowed();' in handle_net and 'feature_refused = err == CFG_NET_OK && mode == 2 && !ble_provisioning_allowed();' in handle_net
assert handle_net.index('feature_refused ? "feature"') > handle_net.index('feature_refused = !ble_provisioning_allowed()')
assert handle_net.index('feature_refused ? "feature"') < handle_net.index('atomic_exchange(&s_net_busy, true)'), 'refused before the line is claimed'
assert "case 'b': err = config_net_set_ble(r->key); break;" in net_store
assert not re.search(r'msg_\w+\(&root, "(key|pass)"', net_reply) and 'msg_bool(&root, "ble", info.ble)' in net_reply

# ── the real handler ─────────────────────────────────────────────────────────────────────────────────

code = r'''
#include "cJSON.h"
#include "cable_features.h"
#include "cable_frame.h"
#include "cable_json_guard.h"
#include "cable_xport.h"
#include "config_store.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint s_features;
static uint32_t s_unknown;
static cable_xport_kind_t s_transport = CABLE_XPORT_USB;
static char wire[4096];
static unsigned sends;
static char s_log[16384];

cable_xport_kind_t cable_link_transport(void) { return s_transport; }
static bool cable_link_send(uint8_t type, const uint8_t *bytes, size_t n)
{
    assert(type == CABLE_TYPE_JSON && n < sizeof wire);
    memcpy(wire, bytes, n);
    wire[n] = 0;
    sends++;
    return true;
}
static void capture(const char *tag, const char *fmt, va_list ap)
{
    size_t used = strlen(s_log);
    used += (size_t)snprintf(s_log + used, sizeof s_log - used, "%s: ", tag);
    if (used < sizeof s_log) vsnprintf(s_log + used, sizeof s_log - used, fmt, ap);
    strncat(s_log, "\n", sizeof s_log - strlen(s_log) - 1);
}
void host_log_info(const char *tag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); capture(tag, fmt, ap); va_end(ap); }
void host_log_warn(const char *tag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); capture(tag, fmt, ap); va_end(ap); }

extern void host_nvs_reset(void);
extern void host_nvs_fail_after(int mutations);
extern int host_nvs_count(void);
extern int host_nvs_has(const char *key);
extern const void *host_nvs_raw(const char *key);
extern int host_nvs_mutations(void);

#define EXT_RAM_BSS_ATTR
#define pdPASS ((BaseType_t)1)
#define ESP_LOGI(tag, ...) host_log_info(tag, __VA_ARGS__)
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
static const char *TAG = "cable_client";
// The mode switch (mode_glue.c), reduced to the one call the net handler makes: it must come after the write and before the reply.
static unsigned glue_told, glue_sends_at_told;
static void mode_glue_settings_changed(void) { glue_told++; glue_sends_at_told = (unsigned)sends; }
// FreeRTOS, reduced to what net_store needs: a task runs inline when it is created (the default), or is held until the test runs it.
static unsigned tasks_created, task_stack, task_prio;
static char task_name[16];
static bool defer_tasks, fail_create, task_ended;
static TaskFunction_t pending_fn;
static BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, unsigned stack, void *arg, unsigned prio, TaskHandle_t *handle)
{
    (void)arg; (void)handle;
    if (fail_create) return 0;
    tasks_created++;
    snprintf(task_name, sizeof task_name, "%s", name);
    task_stack = stack;
    task_prio = prio;
    if (defer_tasks) { assert(!pending_fn); pending_fn = fn; return pdPASS; }
    fn(NULL);
    return pdPASS;
}
static void vTaskDelete(void *handle) { assert(!handle); task_ended = true; }
static unsigned uxTaskGetStackHighWaterMark(void *handle) { assert(!handle); return 1234; }
static void run_pending_task(void)
{
    assert(pending_fn);
    TaskFunction_t fn = pending_fn;
    pending_fn = NULL;
    fn(NULL);
}
'''
link_scrub = function('cable_link_scrub_payload', LINK)
code += 'static uint8_t s_dec_buf[CABLE_MAX_FRAME];\n' + link_scrub
code += re.search(r'#define NET_STORE_STACK[^\n]*\n', CLIENT)[0]
code += re.search(r'typedef struct \{[^}]*\} net_request_t;\n', CLIENT)[0]
code += re.search(r'static EXT_RAM_BSS_ATTR net_request_t s_net_req;[^\n]*\n', CLIENT)[0]
code += re.search(r'static atomic_bool\s+s_net_busy;\n', CLIENT)[0]
for name in ['cable_client_supports', 'ble_provisioning_allowed', 'send_json', 'msg', 'msg_check', 'msg_string', 'msg_number', 'msg_bool',
             'net_zero', 'net_scrub', 'net_text', 'net_whole', 'net_reply', 'net_store_task', 'handle_net']:
    code += function(name)
code += r'''
#define SSID "home-net"
#define PASS "correct horse battery"
#define HOST "192.168.1.20"
#define KEY_B64 "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
#define KEY2_B64 "CQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQk="
static const char *SET = "{\"t\":\"net.set\",\"id\":7,\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}";

static cJSON *reply;
/** Run one message through the handler, as handle_message does, and parse the single reply. */
static cJSON *ask_keep(const char *json, cJSON **keep)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON_Delete(reply);
    reply = NULL;
    sends = 0;
    handle_net(cJSON_GetObjectItemCaseSensitive(root, "t")->valuestring, root);
    assert(sends == 1);
    reply = cJSON_Parse(wire);
    assert(reply && cJSON_IsObject(reply));
    if (keep) *keep = root; else cJSON_Delete(root);
    return reply;
}
static cJSON *ask(const char *json) { return ask_keep(json, NULL); }
static bool ok_of(const cJSON *r) { const cJSON *o = cJSON_GetObjectItemCaseSensitive(r, "ok"); assert(cJSON_IsBool(o)); return cJSON_IsTrue(o); }
static const char *err_of(const cJSON *r) { const cJSON *e = cJSON_GetObjectItemCaseSensitive(r, "err"); return cJSON_IsString(e) ? e->valuestring : NULL; }
static bool saved_of(const cJSON *r) { const cJSON *s = cJSON_GetObjectItemCaseSensitive(r, "saved"); assert(cJSON_IsBool(s)); return cJSON_IsTrue(s); }
static void refused(const char *json, const char *err)
{
    int before = host_nvs_count();
    cJSON *r = ask(json);
    if (ok_of(r) || !err_of(r) || strcmp(err_of(r), err)) { fprintf(stderr, "wanted err %s for %s, got %s\n", err, json, wire); assert(0); }
    assert(host_nvs_count() == before);
}
static void fresh(void)
{
    host_nvs_reset();
    atomic_store(&s_features, CABLE_FEATURE_NET);
    s_transport = CABLE_XPORT_USB;
    s_log[0] = 0;
    defer_tasks = fail_create = task_ended = false;
    pending_fn = NULL;
    tasks_created = 0;
    memset(&s_net_req, 0, sizeof s_net_req);
    atomic_store(&s_net_busy, false);
}
static int wl_held(void)
{
    static const char *k[] = {"wl_ssid", "wl_pass", "wl_host", "wl_port", "wl_key", "wl_ver", "wl_mode"};
    int n = 0;
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) n += host_nvs_has(k[i]);
    return n;
}

static void test_set_and_state(void)
{
    fresh();
    cJSON *root = NULL;
    cJSON *r = ask_keep(SET, &root);
    assert(ok_of(r) && saved_of(r) && !err_of(r));
    assert(cJSON_GetObjectItemCaseSensitive(r, "id")->valueint == 7);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(r, "ssid")->valuestring, SSID));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(r, "host")->valuestring, HOST));
    assert(cJSON_GetObjectItemCaseSensitive(r, "port")->valueint == 18480);
    assert(cJSON_GetObjectItemCaseSensitive(r, "mode")->valueint == 1);
    // the reply holds no password and no key, under any name, and the text of it holds neither value
    assert(!cJSON_GetObjectItemCaseSensitive(r, "pass") && !cJSON_GetObjectItemCaseSensitive(r, "key"));
    assert(!strstr(wire, "correct horse") && !strstr(wire, KEY_B64));
    // the strings of the request were overwritten where they sit
    assert(cJSON_GetObjectItemCaseSensitive(root, "pass")->valuestring[0] == 0);
    assert(cJSON_GetObjectItemCaseSensitive(root, "key")->valuestring[0] == 0);
    cJSON_Delete(root);
    // it was stored, whole
    config_net_t n;
    assert(config_net_load(&n) && !strcmp(n.pass, PASS) && n.key[1] == 1 && n.key[31] == 31 && n.mode == 1);
    // net.get says the same without an id when none was asked with
    r = ask("{\"t\":\"net.get\"}");
    assert(ok_of(r) && saved_of(r) && !cJSON_GetObjectItemCaseSensitive(r, "id"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(r, "ssid")->valuestring, SSID));
}

static void test_get_when_nothing_is_set(void)
{
    fresh();
    cJSON *r = ask("{\"t\":\"net.get\",\"id\":65535}");
    assert(ok_of(r) && !saved_of(r) && !err_of(r));
    assert(cJSON_GetObjectItemCaseSensitive(r, "id")->valueint == 65535);
    assert(!cJSON_GetObjectItemCaseSensitive(r, "ssid") && !cJSON_GetObjectItemCaseSensitive(r, "host") && !cJSON_GetObjectItemCaseSensitive(r, "mode"));
    // an id that is not a whole number from 1 to 65535 is not echoed
    const char *not_ids[] = {"0", "65536", "1.5", "-1", "\"7\"", "null", "true"};
    for (unsigned i = 0; i < sizeof not_ids / sizeof not_ids[0]; i++) {
        char text[64];
        snprintf(text, sizeof text, "{\"t\":\"net.get\",\"id\":%s}", not_ids[i]);
        r = ask(text);
        assert(ok_of(r) && !cJSON_GetObjectItemCaseSensitive(r, "id"));
    }
}

static void test_a_refusal_names_the_field(void)
{
    fresh();
    refused("{\"t\":\"net.set\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "ssid");
    refused("{\"t\":\"net.set\",\"ssid\":5,\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "ssid");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "pass");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"short\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "pass");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"nope\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "host");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":7,\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "host");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"key\":\"" KEY_B64 "\",\"mode\":1}", "port");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480.5,\"key\":\"" KEY_B64 "\",\"mode\":1}", "port");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":\"18480\",\"key\":\"" KEY_B64 "\",\"mode\":1}", "port");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":80,\"key\":\"" KEY_B64 "\",\"mode\":1}", "port");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"not base64\",\"mode\":1}", "key");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"mode\":1}", "key");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":2}", "mode");
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\"}", "mode");
    assert(wl_held() == 0);
    // and the reply to a refusal repeats none of what was sent
    cJSON *r = ask("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"x\"}");
    assert(!strstr(wire, SSID) && !strstr(wire, "\"x\""));
    assert(!ok_of(r) && !saved_of(r));
}

static void test_the_other_messages(void)
{
    fresh();
    // before a set, key / host / mode have nothing to change
    refused("{\"t\":\"net.mode\",\"mode\":1}", "not_provisioned");
    refused("{\"t\":\"net.key\",\"key\":\"" KEY_B64 "\"}", "not_provisioned");
    refused("{\"t\":\"net.host\",\"host\":\"10.0.0.2\",\"port\":18480}", "not_provisioned");
    assert(ok_of(ask(SET)));
    // key
    refused("{\"t\":\"net.key\",\"key\":\"???\"}", "key");
    refused("{\"t\":\"net.key\"}", "key");
    cJSON *r = ask("{\"t\":\"net.key\",\"id\":3,\"key\":\"" KEY2_B64 "\"}");
    assert(ok_of(r) && saved_of(r));
    config_net_t n;
    assert(config_net_load(&n) && n.key[0] == 9 && n.key[31] == 9 && !strcmp(n.ssid, SSID));
    // host
    refused("{\"t\":\"net.host\",\"host\":\"nope\",\"port\":18480}", "host");
    refused("{\"t\":\"net.host\",\"host\":\"10.0.0.2\",\"port\":7}", "port");
    r = ask("{\"t\":\"net.host\",\"host\":\"10.0.0.2\",\"port\":19000}");
    assert(ok_of(r) && !strcmp(cJSON_GetObjectItemCaseSensitive(r, "host")->valuestring, "10.0.0.2") && cJSON_GetObjectItemCaseSensitive(r, "port")->valueint == 19000);
    // mode
    refused("{\"t\":\"net.mode\",\"mode\":2}", "feature");   // B6: 2 is the phone; without "ble" in the welcome the dial refuses it before any write
    refused("{\"t\":\"net.mode\",\"mode\":3}", "mode");
    refused("{\"t\":\"net.mode\"}", "mode");
    r = ask("{\"t\":\"net.mode\",\"mode\":0}");
    assert(ok_of(r) && cJSON_GetObjectItemCaseSensitive(r, "mode")->valueint == 0);
    // clear
    r = ask("{\"t\":\"net.clear\"}");
    assert(ok_of(r) && !saved_of(r) && wl_held() == 0);
    refused("{\"t\":\"net.mode\",\"mode\":1}", "not_provisioned");
    // an op this firmware does not have
    r = ask("{\"t\":\"net.reboot\"}");
    assert(!ok_of(r) && !strcmp(err_of(r), "op"));
    r = ask("{\"t\":\"net.\"}");
    assert(!ok_of(r) && !strcmp(err_of(r), "op"));
    r = ask("{\"t\":\"net.state\",\"ok\":true}");
    assert(!ok_of(r) && !strcmp(err_of(r), "op"));
    assert(s_unknown == 3);
}

// W16c (bench 2026-11-01): net.set stored wl_mode = 1 and the running mode switch was never told, so the dial stayed on USB when unplugged. Every write
// (set, key, host, mode, clear) now tells it, after the write and before the reply; a failed write too (it may have left the marker off); a request
// that was refused before any write does not.
static void test_the_mode_switch_is_told_after_every_write(void)
{
    fresh();
    glue_told = 0;
    sends = 0;
    assert(ok_of(ask(SET)) && glue_told == 1 && glue_sends_at_told == 0);
    assert(ok_of(ask("{\"t\":\"net.key\",\"key\":\"" KEY2_B64 "\"}")) && glue_told == 2 && glue_sends_at_told == 0);
    assert(ok_of(ask("{\"t\":\"net.host\",\"host\":\"10.0.0.2\",\"port\":19000}")) && glue_told == 3 && glue_sends_at_told == 0);
    assert(ok_of(ask("{\"t\":\"net.mode\",\"mode\":0}")) && glue_told == 4 && glue_sends_at_told == 0);
    assert(ok_of(ask("{\"t\":\"net.clear\"}")) && glue_told == 5 && glue_sends_at_told == 0);
    // reads and refusals write nothing: no call
    refused("{\"t\":\"net.mode\",\"mode\":2}", "feature");
    refused("{\"t\":\"net.key\",\"key\":\"???\"}", "key");
    assert(ok_of(ask("{\"t\":\"net.get\"}")) && glue_told == 5);
    // a store that fails: the write may have gone half way, so the glue is told to read what NVS holds
    host_nvs_fail_after(0);
    refused(SET, "storage");
    host_nvs_fail_after(-1);
    assert(glue_told == 6);
}

static void test_off_the_cable_nothing_is_touched(void)
{
    const char *messages[] = {
        SET, "{\"t\":\"net.get\"}", "{\"t\":\"net.clear\"}", "{\"t\":\"net.mode\",\"mode\":1}",
        "{\"t\":\"net.key\",\"key\":\"" KEY_B64 "\"}", "{\"t\":\"net.host\",\"host\":\"10.0.0.2\",\"port\":18480}",
    };
    for (cable_xport_kind_t kind = CABLE_XPORT_WIFI; kind <= CABLE_XPORT_BLE; kind++) {
        for (unsigned i = 0; i < sizeof messages / sizeof messages[0]; i++) {
            fresh();
            s_transport = kind;
            refused(messages[i], "transport");
            assert(host_nvs_count() == 0);
            assert(!cJSON_GetObjectItemCaseSensitive(reply, "saved"));   // a refusal off the cable says nothing of what is held
        }
    }
    // even a dial that holds a configuration, asked over Wi-Fi to clear or change it, keeps it
    fresh();
    assert(ok_of(ask(SET)));
    s_transport = CABLE_XPORT_WIFI;
    for (unsigned i = 0; i < sizeof messages / sizeof messages[0]; i++) {
        refused(messages[i], "transport");
        assert(!cJSON_GetObjectItemCaseSensitive(reply, "ssid"));   // not even the SSID is told over another link
    }
    config_net_t n;
    assert(config_net_load(&n) && !strcmp(n.ssid, SSID) && n.key[1] == 1);
}

static void test_without_the_feature_nothing_is_touched(void)
{
    for (unsigned features = 0; features < 1024; features++) {
        if (features & CABLE_FEATURE_NET) continue;
        fresh();
        atomic_store(&s_features, features);
        refused(SET, "feature");
        assert(host_nvs_count() == 0);
    }
    fresh();
    assert(ok_of(ask(SET)));
    atomic_store(&s_features, 0);
    refused("{\"t\":\"net.clear\"}", "feature");
    config_net_t n;
    assert(config_net_load(&n));
}

static void test_a_failing_store(void)
{
    fresh();
    host_nvs_fail_after(0);
    refused(SET, "storage");
    host_nvs_fail_after(-1);
    assert(ok_of(ask(SET)));
    host_nvs_fail_after(0);
    refused("{\"t\":\"net.mode\",\"mode\":0}", "storage");
    refused("{\"t\":\"net.clear\"}", "storage");
}

static bool slot_is_clear(void)
{
    static const net_request_t zero;
    return !memcmp(&s_net_req, &zero, sizeof zero);
}
/** Hand a message to the handler without expecting a reply yet (the write is queued as a task). */
static void fire(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root);
    handle_net(cJSON_GetObjectItemCaseSensitive(root, "t")->valuestring, root);
    cJSON_Delete(root);
}

static void test_writes_run_in_the_store_task(void)
{
    fresh();
    defer_tasks = true;
    sends = 0;
    fire(SET);
    // The reader did the checking and the copying, and no more: nothing is written, nothing is answered, one task is waiting.
    assert(tasks_created == 1 && pending_fn && sends == 0);
    assert(!strcmp(task_name, "net_store") && task_stack == 4096);
    assert(host_nvs_mutations() == 0 && host_nvs_count() == 0);
    assert(!slot_is_clear() && !strcmp(s_net_req.pass, PASS) && s_net_req.op == 's' && s_net_req.id == 7);
    // A second write is turned away while the first waits: busy, and changes nothing. Its reply has no state.
    sends = 0;
    fire("{\"t\":\"net.clear\",\"id\":8}");
    assert(sends == 1 && tasks_created == 1);
    cJSON *r = cJSON_Parse(wire);
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok")) && !strcmp(err_of(r), "busy") && cJSON_GetObjectItemCaseSensitive(r, "id")->valueint == 8);
    assert(!cJSON_GetObjectItemCaseSensitive(r, "saved") && !cJSON_GetObjectItemCaseSensitive(r, "ssid"));
    cJSON_Delete(r);
    const char *more[] = {SET, "{\"t\":\"net.mode\",\"mode\":0}", "{\"t\":\"net.key\",\"key\":\"" KEY2_B64 "\"}", "{\"t\":\"net.host\",\"host\":\"10.0.0.2\",\"port\":19000}"};
    for (unsigned i = 0; i < sizeof more / sizeof more[0]; i++) {
        sends = 0;
        fire(more[i]);
        assert(sends == 1 && tasks_created == 1);
        r = cJSON_Parse(wire);
        assert(!strcmp(err_of(r), "busy"));
        cJSON_Delete(r);
    }
    // the first request's slot is untouched by all that, and so is NVS
    assert(!strcmp(s_net_req.pass, PASS) && s_net_req.op == 's' && host_nvs_count() == 0);
    // net.get while a write is waiting is answered busy: the state it would report is about to change
    sends = 0;
    fire("{\"t\":\"net.get\",\"id\":9}");
    assert(sends == 1 && tasks_created == 1);
    r = cJSON_Parse(wire);
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok")) && !strcmp(err_of(r), "busy") && cJSON_GetObjectItemCaseSensitive(r, "id")->valueint == 9);
    assert(!cJSON_GetObjectItemCaseSensitive(r, "saved") && !cJSON_GetObjectItemCaseSensitive(r, "ssid"));
    cJSON_Delete(r);
    // the task runs: it writes, answers, clears the slot, ends, and frees the way for the next
    sends = 0;
    run_pending_task();
    assert(sends == 1 && task_ended);
    r = cJSON_Parse(wire);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok")) && saved_of(r) && cJSON_GetObjectItemCaseSensitive(r, "id")->valueint == 7);
    cJSON_Delete(r);
    assert(slot_is_clear() && wl_held() == 7);
    assert(strstr(s_log, "net_store: stack high-water 1234 B free of 4096"));
    config_net_t n;
    assert(config_net_load(&n) && !strcmp(n.pass, PASS));
    // and once the line is free again, net.get is a read as before
    r = ask("{\"t\":\"net.get\"}");
    assert(ok_of(r) && saved_of(r) && !err_of(r));
    sends = 0;
    fire("{\"t\":\"net.mode\",\"mode\":0}");
    assert(tasks_created == 2 && sends == 0);
    run_pending_task();
    assert(sends == 1 && config_net_load(&n) && n.mode == 0 && slot_is_clear());
}

static void test_every_write_is_a_task_and_nothing_else_is(void)
{
    fresh();
    assert(ok_of(ask(SET)) && tasks_created == 1);
    unsigned before = tasks_created;
    const char *writes[] = {
        "{\"t\":\"net.mode\",\"mode\":1}", "{\"t\":\"net.key\",\"key\":\"" KEY2_B64 "\"}",
        "{\"t\":\"net.host\",\"host\":\"10.0.0.2\",\"port\":19000}", SET, "{\"t\":\"net.clear\"}",
    };
    for (unsigned i = 0; i < sizeof writes / sizeof writes[0]; i++) {
        assert(ok_of(ask(writes[i])));
        assert(tasks_created == before + i + 1);
        assert(slot_is_clear() && !atomic_load(&s_net_busy));
    }
    // reads, refusals and unknown ops start no task
    before = tasks_created;
    ask("{\"t\":\"net.get\"}");
    refused("{\"t\":\"net.mode\",\"mode\":5}", "mode");
    refused("{\"t\":\"net.key\",\"key\":\"nope\"}", "key");
    refused("{\"t\":\"net.host\",\"host\":\"x\",\"port\":19000}", "host");
    refused("{\"t\":\"net.set\",\"ssid\":\"\"}", "ssid");
    refused("{\"t\":\"net.zzz\"}", "op");
    s_transport = CABLE_XPORT_WIFI;
    refused(SET, "transport");
    s_transport = CABLE_XPORT_USB;
    atomic_store(&s_features, 0);
    refused(SET, "feature");
    atomic_store(&s_features, CABLE_FEATURE_NET);
    assert(tasks_created == before && slot_is_clear() && !atomic_load(&s_net_busy));
    // a refusal leaves the line free: the next good request goes through
    assert(ok_of(ask(SET)) && tasks_created == before + 1);
    // the not-provisioned refusals come from the task (they need NVS), and release the line too
    fresh();
    refused("{\"t\":\"net.mode\",\"mode\":1}", "not_provisioned");
    assert(slot_is_clear() && !atomic_load(&s_net_busy));
    assert(ok_of(ask(SET)));
}

#ifdef DEVICE_BLE_LINK
// ── B6: the phone-mode forms (an image with the BLE link, and a daemon that listed "ble") ─────────────────────────────────────────────────
#define KEY_ONLY "{\"t\":\"net.set\",\"id\":11,\"key\":\"" KEY_B64 "\"}"
static bool ble_of(const cJSON *r) { const cJSON *b = cJSON_GetObjectItemCaseSensitive(r, "ble"); assert(cJSON_IsBool(b)); return cJSON_IsTrue(b); }
static int mode_of(const cJSON *r) { const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "mode"); assert(cJSON_IsNumber(m)); return m->valueint; }
static void with_ble(void) { fresh(); atomic_store(&s_features, CABLE_FEATURE_NET | CABLE_FEATURE_BLE); }

static void test_ble_key_only_form_and_mode_2(void)
{
    with_ble();
    // net.get on a dial with nothing: ble false, still no mode
    cJSON *r = ask("{\"t\":\"net.get\"}");
    assert(ok_of(r) && !saved_of(r) && !ble_of(r) && !cJSON_GetObjectItemCaseSensitive(r, "mode"));
    // mode 2 with no phone settings: the task refuses it (it needs NVS)
    refused("{\"t\":\"net.mode\",\"mode\":2}", "not_provisioned");
    // the key-only form
    cJSON *root = NULL;
    r = ask_keep(KEY_ONLY, &root);
    assert(ok_of(r) && !err_of(r) && !saved_of(r) && ble_of(r) && mode_of(r) == 0);
    assert(cJSON_GetObjectItemCaseSensitive(r, "id")->valueint == 11);
    assert(!cJSON_GetObjectItemCaseSensitive(r, "key") && !strstr(wire, KEY_B64) && !cJSON_GetObjectItemCaseSensitive(r, "ssid"));
    assert(cJSON_GetObjectItemCaseSensitive(root, "key")->valuestring[0] == 0);          // scrubbed like any key
    cJSON_Delete(root);
    config_ble_t b;
    assert(config_ble_load(&b) && b.key[1] == 1 && b.key[31] == 31);
    config_net_t n;
    assert(!config_net_load(&n) && !host_nvs_has("wl_ssid") && host_nvs_has("bl_ver"));
    assert(slot_is_clear() && !atomic_load(&s_net_busy));
    // mode 2, then net.get tells it
    r = ask("{\"t\":\"net.mode\",\"mode\":2}");
    assert(ok_of(r) && ble_of(r) && mode_of(r) == 2 && !saved_of(r));
    r = ask("{\"t\":\"net.get\"}");
    assert(ok_of(r) && ble_of(r) && mode_of(r) == 2);
    assert(config_load_wl_effective() == 2);
    // Wi-Fi settings alongside: both held
    assert(ok_of(ask(SET)));
    r = ask("{\"t\":\"net.get\"}");
    assert(saved_of(r) && ble_of(r));
    // net.clear forgets both
    r = ask("{\"t\":\"net.clear\"}");
    assert(ok_of(r) && !saved_of(r) && !ble_of(r) && !host_nvs_has("bl_ver") && wl_held() == 0 && config_load_wl_effective() == 0);
    // a mode 2 over Wi-Fi settings alone is refused by the task
    assert(ok_of(ask(SET)));
    refused("{\"t\":\"net.mode\",\"mode\":2}", "not_provisioned");
}

static void test_ble_forms_refusals(void)
{
    // bad keys
    with_ble();
    refused("{\"t\":\"net.set\",\"key\":\"not base64\"}", "key");
    refused("{\"t\":\"net.set\"}", "key");
    refused("{\"t\":\"net.set\",\"key\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}", "key");   // 32 zero bytes
    assert(host_nvs_count() == 0);
    // a Wi-Fi form with a field missing stays a Wi-Fi form and is named by its first bad field, key-only or not
    refused("{\"t\":\"net.set\",\"key\":\"" KEY_B64 "\",\"mode\":1}", "ssid");
    refused("{\"t\":\"net.set\",\"key\":\"" KEY_B64 "\",\"port\":18480}", "ssid");
    refused("{\"t\":\"net.set\",\"key\":\"" KEY_B64 "\",\"ssid\":\"" SSID "\"}", "pass");
    assert(host_nvs_count() == 0);
    // without the daemon's "ble", nothing is touched, whatever else the welcome said
    for (unsigned features = 0; features < 2048; features++) {
        if (!(features & CABLE_FEATURE_NET) || (features & CABLE_FEATURE_BLE)) continue;
        fresh();
        atomic_store(&s_features, features);
        refused(KEY_ONLY, "feature");
        refused("{\"t\":\"net.mode\",\"mode\":2}", "feature");
        assert(host_nvs_count() == 0 && tasks_created == 0);
    }
    // "ble" without "net" is not enough: net.* is gated by net first
    fresh();
    atomic_store(&s_features, CABLE_FEATURE_BLE);
    refused(KEY_ONLY, "feature");
    assert(host_nvs_count() == 0);
    // off the cable, nothing is touched: the transport check comes first
    for (cable_xport_kind_t kind = CABLE_XPORT_WIFI; kind <= CABLE_XPORT_BLE; kind++) {
        with_ble();
        s_transport = kind;
        refused(KEY_ONLY, "transport");
        refused("{\"t\":\"net.mode\",\"mode\":2}", "transport");
        assert(host_nvs_count() == 0 && tasks_created == 0);
    }
    // the other Wi-Fi-only modes are unaffected by the BLE feature
    with_ble();
    refused("{\"t\":\"net.mode\",\"mode\":1}", "not_provisioned");
    // the form's task: it runs in the store task like every write, and tells the mode switch
    with_ble();
    defer_tasks = true;
    sends = 0;
    glue_told = 0;
    fire(KEY_ONLY);
    assert(tasks_created == 1 && sends == 0 && s_net_req.op == 'b' && host_nvs_count() == 0);
    run_pending_task();
    assert(sends == 1 && glue_told == 1 && slot_is_clear() && host_nvs_has("bl_ver"));
    // and nothing of the key was logged
    assert(!strstr(s_log, KEY_B64) && !strstr(s_log, "AAECAwQF"));
}
#else
// An image without the BLE link: even a daemon that lists "ble" gets nothing from the phone-mode forms, and nothing is written.
static void test_an_image_without_ble_refuses_the_phone_forms(void)
{
    fresh();
    atomic_store(&s_features, CABLE_FEATURE_NET | CABLE_FEATURE_BLE);
    refused("{\"t\":\"net.set\",\"key\":\"" KEY_B64 "\"}", "feature");
    refused("{\"t\":\"net.mode\",\"mode\":2}", "feature");
    assert(host_nvs_count() == 0 && tasks_created == 0 && !host_nvs_has("bl_ver"));
    // Wi-Fi is untouched
    assert(ok_of(ask(SET)) && wl_held() == 7);
}
#endif

static bool is_zeroes(const char *s) { return !s || !s[0]; }

static void test_secrets_are_scrubbed_on_every_exit(void)
{
    // each of these leaves handle_net by a different way; the password and the key of the parsed request are zero after every one
    struct { const char *json; const char *what; cable_xport_kind_t kind; unsigned features; bool busy; } t[] = {
        {SET, "served", CABLE_XPORT_USB, CABLE_FEATURE_NET, false},
        {SET, "off the cable", CABLE_XPORT_WIFI, CABLE_FEATURE_NET, false},
        {SET, "no feature", CABLE_XPORT_USB, 0, false},
        {SET, "busy", CABLE_XPORT_USB, CABLE_FEATURE_NET, true},
        {"{\"t\":\"net.set\",\"ssid\":\"\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "bad ssid", CABLE_XPORT_USB, CABLE_FEATURE_NET, false},
        {"{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"" PASS "\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":9}", "bad mode", CABLE_XPORT_USB, CABLE_FEATURE_NET, false},
        {"{\"t\":\"net.key\",\"key\":\"" KEY2_B64 "\",\"pass\":\"" PASS "\"}", "key, with a stray password", CABLE_XPORT_USB, CABLE_FEATURE_NET, false},
        {"{\"t\":\"net.get\",\"key\":\"" KEY_B64 "\",\"pass\":\"" PASS "\"}", "get, with stray secrets", CABLE_XPORT_USB, CABLE_FEATURE_NET, false},
        {"{\"t\":\"net.zzz\",\"key\":\"" KEY_B64 "\",\"pass\":\"" PASS "\"}", "unknown op", CABLE_XPORT_USB, CABLE_FEATURE_NET, false},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        fresh();
        s_transport = t[i].kind;
        atomic_store(&s_features, t[i].features);
        if (t[i].busy) atomic_store(&s_net_busy, true);
        cJSON *root = NULL;
        ask_keep(t[i].json, &root);
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass"), *key = cJSON_GetObjectItemCaseSensitive(root, "key");
        if (!is_zeroes(pass ? pass->valuestring : NULL) || !is_zeroes(key ? key->valuestring : NULL)) { fprintf(stderr, "not scrubbed: %s\n", t[i].what); assert(0); }
        cJSON_Delete(root);
    }
}

static void test_the_decoders_copy_of_a_frame_is_overwritten(void)
{
    memset(s_dec_buf, 0xAA, sizeof s_dec_buf);
    // inside the buffer: exactly the named bytes
    cable_link_scrub_payload(s_dec_buf + 10, 100);
    for (size_t i = 0; i < sizeof s_dec_buf; i++) assert(s_dec_buf[i] == ((i >= 10 && i < 110) ? 0 : 0xAA));
    // the whole of what is left of the buffer
    cable_link_scrub_payload(s_dec_buf + 200, sizeof s_dec_buf - 200);
    assert(s_dec_buf[sizeof s_dec_buf - 1] == 0 && s_dec_buf[199] == 0xAA);
    // a pointer outside the buffer, or a length that runs past it, or nothing, is ignored
    memset(s_dec_buf, 0xAA, sizeof s_dec_buf);
    uint8_t other[16] = {1};
    cable_link_scrub_payload(other, sizeof other);
    assert(other[0] == 1);
    cable_link_scrub_payload(s_dec_buf + 5, sizeof s_dec_buf);
    cable_link_scrub_payload(s_dec_buf + sizeof s_dec_buf + 1, 1);
    cable_link_scrub_payload(NULL, 4);
    for (size_t i = 0; i < sizeof s_dec_buf; i++) assert(s_dec_buf[i] == 0xAA);
    cable_link_scrub_payload(s_dec_buf + sizeof s_dec_buf, 0);   // the end of the buffer, nothing to do
}

static void test_a_task_that_cannot_start(void)
{
    fresh();
    fail_create = true;
    refused(SET, "storage");
    assert(slot_is_clear() && !atomic_load(&s_net_busy) && host_nvs_count() == 0);
    fail_create = false;
    assert(ok_of(ask(SET)));
}

static void test_json_guard(void)
{
    // the guard stands in front of the parser (cable_client.c checks this order in the source); a net.set that is nested too
    // deep, or too long, never reaches handle_net
    char deep[512] = "{\"t\":\"net.set\",\"ssid\":\"x\",\"z\":";
    for (int i = 0; i < CABLE_JSON_DEPTH + 1; i++) strcat(deep, "[");
    strcat(deep, "1");
    for (int i = 0; i < CABLE_JSON_DEPTH + 1; i++) strcat(deep, "]");
    strcat(deep, "}");
    assert(!cable_json_guard((const uint8_t *)deep, strlen(deep)));
    assert(cable_json_guard((const uint8_t *)SET, strlen(SET)));
    static char big[CABLE_JSON_MAX + 64];
    memset(big, 'a', sizeof big);
    memcpy(big, "{\"t\":\"net.set\",\"pass\":\"", 23);
    memcpy(big + sizeof big - 3, "\"}", 3);
    assert(!cable_json_guard((const uint8_t *)big, sizeof big - 1));
    // a flat object with more items than the guard's budget
    static char many[8192];
    strcpy(many, "{\"t\":\"net.set\"");
    for (int i = 0; i < CABLE_JSON_TOKENS; i++) strcat(many, ",\"k\":1");
    strcat(many, "}");
    assert(!cable_json_guard((const uint8_t *)many, strlen(many)));
}

static void test_nothing_is_logged(void)
{
    fresh();
    assert(ok_of(ask(SET)));
    refused("{\"t\":\"net.set\",\"ssid\":\"" SSID "\",\"pass\":\"short\",\"host\":\"" HOST "\",\"port\":18480,\"key\":\"" KEY_B64 "\",\"mode\":1}", "pass");
    assert(ok_of(ask("{\"t\":\"net.key\",\"key\":\"" KEY2_B64 "\"}")));
    assert(ok_of(ask("{\"t\":\"net.host\",\"host\":\"10.9.8.7\",\"port\":19000}")));
    assert(ok_of(ask("{\"t\":\"net.mode\",\"mode\":0}")));
    assert(ok_of(ask("{\"t\":\"net.clear\"}")));
    const char *secrets[] = {SSID, "correct horse", "horse", "short", HOST, "10.9.8.7", KEY_B64, KEY2_B64, "AAECAwQF", "CQkJCQkJ"};
    for (unsigned i = 0; i < sizeof secrets / sizeof secrets[0]; i++)
        if (strstr(s_log, secrets[i])) { fprintf(stderr, "logged %s\n%s", secrets[i], s_log); assert(0); }
    assert(strstr(s_log, "ssid_len=8 pass_len=21 host_len=12"));
}

int main(void)
{
    test_set_and_state();
    test_get_when_nothing_is_set();
    test_a_refusal_names_the_field();
    test_the_other_messages();
    test_the_mode_switch_is_told_after_every_write();
    test_off_the_cable_nothing_is_touched();
    test_without_the_feature_nothing_is_touched();
    test_a_failing_store();
    test_writes_run_in_the_store_task();
    test_every_write_is_a_task_and_nothing_else_is();
    test_a_task_that_cannot_start();
    test_secrets_are_scrubbed_on_every_exit();
    test_the_decoders_copy_of_a_frame_is_overwritten();
    test_json_guard();
    test_nothing_is_logged();
#ifdef DEVICE_BLE_LINK
    test_ble_key_only_form_and_mode_2();
    test_ble_forms_refusals();
#else
    test_an_image_without_ble_refuses_the_phone_forms();
#endif
    cJSON_Delete(reply);
    puts("net.*: set, get, clear, mode, key, host; USB only; feature gated; field names, no values; guard in front PASS (offline)");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='harness-cable-net-') as folder:
    out = Path(folder)
    (out / 'net.c').write_text(code)
    flags = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds')]
    subprocess.run(flags + ['-Wno-deprecated-declarations', '-c', str(JSON_DIR / 'cJSON.c'), '-o', str(out / 'cJSON.o')], check=True)
    # Two images: the default one (no BLE link) and one built with DEVICE_BLE_LINK, which is where the phone-mode forms exist.
    for name, extra in (('net', []), ('net_ble', ['-DDEVICE_BLE_LINK=1'])):
        subprocess.run(flags + extra + ['-I', str(JSON_DIR), '-I', str(MAIN), '-I', str(HERE / 'host_stubs'), '-I', str(HERE / 'host_nvs'),
                                        str(out / 'net.c'), str(MAIN / 'config_store.c'), str(MAIN / 'cable_json_guard.c'), str(HERE / 'host_nvs/host_nvs.c'),
                                        str(out / 'cJSON.o'), '-lm', '-o', str(out / name)], check=True)
        subprocess.run([str(out / name)], check=True)
