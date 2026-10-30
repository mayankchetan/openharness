// Host stand-in for esp_log.h in the TLS wrapper test: the line goes to a hook the test can read back.
#pragma once
void host_tls_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) host_tls_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) host_tls_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) host_tls_log('I', tag, __VA_ARGS__)
