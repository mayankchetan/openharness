#pragma once
void host_log_warn(const char *tag, const char *fmt, ...);
#define ESP_LOGW(tag, ...) host_log_warn(tag, __VA_ARGS__)
void host_log_info(const char *tag, const char *fmt, ...);
#define ESP_LOGI(tag, ...) host_log_info(tag, __VA_ARGS__)
