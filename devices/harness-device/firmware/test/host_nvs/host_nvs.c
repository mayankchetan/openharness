// An in-memory stand-in for the NVS calls config_store.c makes (nvs.h in this folder). Test code only.
#include "nvs.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 64
#define MAX_VALUE 128

typedef struct { char key[16]; uint8_t data[MAX_VALUE]; size_t len; int used; } entry_t;
static entry_t s_entries[MAX_KEYS];
static int s_fail_after = -1;
static int s_mutations;
static const char *s_forbidden[4];

static void check_forbidden(const char *key, const void *out)
{
    for (int i = 0; i < 4; i++) if (out && s_forbidden[i] && !strcmp(s_forbidden[i], key)) __builtin_trap();
}
void host_nvs_forbid_read(const char *key)
{
    for (int i = 0; i < 4; i++) if (!s_forbidden[i]) { s_forbidden[i] = key; return; }
}

static entry_t *find(const char *key)
{
    for (int i = 0; i < MAX_KEYS; i++)
        if (s_entries[i].used && !strcmp(s_entries[i].key, key)) return &s_entries[i];
    return NULL;
}

// The power cut: once armed and spent, every mutating call fails and changes nothing.
static int mutating_allowed(void)
{
    if (s_fail_after >= 0 && s_mutations >= s_fail_after) return 0;
    s_mutations++;
    return 1;
}

void host_nvs_reset(void) { memset(s_entries, 0, sizeof s_entries); memset(s_forbidden, 0, sizeof s_forbidden); s_fail_after = -1; s_mutations = 0; }
void host_nvs_fail_after(int mutations) { s_fail_after = mutations; s_mutations = 0; }
int host_nvs_mutations(void) { return s_mutations; }
int host_nvs_count(void) { int n = 0; for (int i = 0; i < MAX_KEYS; i++) n += s_entries[i].used; return n; }
int host_nvs_has(const char *key) { return find(key) != NULL; }
size_t host_nvs_size(const char *key) { entry_t *e = find(key); return e ? e->len : 0; }
const void *host_nvs_raw(const char *key) { entry_t *e = find(key); return e ? e->data : NULL; }

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { host_nvs_reset(); return ESP_OK; }

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name; (void)mode;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return mutating_allowed() ? ESP_OK : ESP_FAIL; }

static esp_err_t put(const char *key, const void *value, size_t len)
{
    if (len > MAX_VALUE || strlen(key) >= sizeof s_entries[0].key) return ESP_FAIL;
    if (!mutating_allowed()) return ESP_FAIL;
    entry_t *e = find(key);
    if (!e) {
        for (int i = 0; i < MAX_KEYS && !e; i++) if (!s_entries[i].used) e = &s_entries[i];
        if (!e) return ESP_FAIL;
        memset(e, 0, sizeof *e);
        strcpy(e->key, key);
        e->used = 1;
    }
    memcpy(e->data, value, len);
    e->len = len;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    if (!mutating_allowed()) return ESP_FAIL;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    memset(e, 0, sizeof *e);
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h)
{
    (void)h;
    if (!mutating_allowed()) return ESP_FAIL;
    memset(s_entries, 0, sizeof s_entries);
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    check_forbidden(key, out);
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_FAIL;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) { (void)h; return put(key, value, strlen(value) + 1); }

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    (void)h;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len != 1) return ESP_FAIL;
    *out = e->data[0];
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value) { (void)h; return put(key, &value, 1); }

esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    (void)h;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len != 2) return ESP_FAIL;
    memcpy(out, e->data, 2);
    return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value) { (void)h; return put(key, &value, 2); }

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    check_forbidden(key, out);
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_FAIL;
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len) { (void)h; return put(key, value, len); }
