#pragma once
// The part of the IDF's NVS API that config_store.c uses, over memory, for the host tests (test_net_config.c).
//
// One namespace is enough (config_store.c opens only "pair"). The store can be told to start failing after N more mutating
// calls (a set, an erase or a commit): that is a power cut at that point, and the test looks at what the store holds then.
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) { __builtin_trap(); } } while (0)

typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len);
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);

// ── test control ──
void host_nvs_reset(void);                    // empty store, no failure armed
void host_nvs_fail_after(int mutations);      // the next `mutations` mutating calls work, every one after fails (-1: never fail)
int  host_nvs_mutations(void);                // mutating calls made since the last reset or fail_after
int  host_nvs_count(void);                    // keys held
int  host_nvs_has(const char *key);           // 1 when the key is held
size_t host_nvs_size(const char *key);        // bytes held under the key, 0 when absent
const void *host_nvs_raw(const char *key);    // the stored bytes (strings include their nul), NULL when absent
