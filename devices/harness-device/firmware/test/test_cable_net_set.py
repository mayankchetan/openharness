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
JSON_DIR = Path(os.environ['IDF_PATH']) / 'components/json/cJSON'


def function(name, source=CLIENT):
    match = re.search(r'^[^\n]*\b' + name + r'\([^;]*?\)\r?\n\{.*?^\}', source, re.M | re.S)
    assert match, name
    return match.group(0) + '\n'


# ── source checks ────────────────────────────────────────────────────────────────────────────────────

handle_net = function('handle_net')
net_reply = function('net_reply')
net_block = CLIENT[CLIENT.index("THE DIAL'S WI-FI SETTINGS"):CLIENT.index('static void handle_message(')]

# 1. Off the cable, nothing is touched: the transport check is the first thing handle_net does, before any config_net_ call.
assert 'cable_link_transport() != CABLE_XPORT_USB' in handle_net
assert handle_net.index('cable_link_transport() != CABLE_XPORT_USB') < handle_net.index('config_net_')
assert handle_net.index('cable_client_supports(CABLE_FEATURE_NET)') < handle_net.index('config_net_')

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
assert not log_calls(net_block), 'handle_net logs nothing itself; config_store.c logs lengths'
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
assert 'net_scrub((char *)net_text(p, "pass"))' in handle_net
assert 'net_scrub((char *)net_text(p, "key"))' in handle_net

# 6. The hello offers net on the cable only.
assert re.search(r'if \(cable_link_transport\(\) == CABLE_XPORT_USB\) msg_string\(&root, "net", "v1"\);', CLIENT)

# ── the real handler ─────────────────────────────────────────────────────────────────────────────────

code = r'''
#include "cJSON.h"
#include "cable_features.h"
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
'''
for name in ['cable_client_supports', 'send_json', 'msg', 'msg_check', 'msg_string', 'msg_number', 'msg_bool',
             'net_scrub', 'net_text', 'net_whole', 'net_reply', 'handle_net']:
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
    refused("{\"t\":\"net.mode\",\"mode\":2}", "mode");
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
            assert(!saved_of(reply));
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
    test_off_the_cable_nothing_is_touched();
    test_without_the_feature_nothing_is_touched();
    test_a_failing_store();
    test_json_guard();
    test_nothing_is_logged();
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
    subprocess.run(flags + ['-I', str(JSON_DIR), '-I', str(MAIN), '-I', str(HERE / 'host_stubs'), '-I', str(HERE / 'host_nvs'),
                            str(out / 'net.c'), str(MAIN / 'config_store.c'), str(MAIN / 'cable_json_guard.c'), str(HERE / 'host_nvs/host_nvs.c'),
                            str(out / 'cJSON.o'), '-lm', '-o', str(out / 'net')], check=True)
    subprocess.run([str(out / 'net')], check=True)
