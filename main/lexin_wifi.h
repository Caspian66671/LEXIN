#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define LEXIN_WIFI_MAX_APS 8
#define LEXIN_WIFI_SSID_MAX_LEN 32
#define LEXIN_WIFI_PASSWORD_MAX_LEN 64

typedef struct {
    char ssid[LEXIN_WIFI_SSID_MAX_LEN + 1];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
} lexin_wifi_ap_t;

esp_err_t lexin_wifi_scan(lexin_wifi_ap_t *aps, uint16_t *count);
esp_err_t lexin_wifi_connect(const char *ssid, const char *password);
esp_err_t lexin_wifi_connect_ap(const lexin_wifi_ap_t *ap, const char *password);
bool lexin_wifi_get_saved_password(const char *ssid, char *password, size_t password_size);
esp_err_t lexin_wifi_save_password(const char *ssid, const char *password);
bool lexin_wifi_is_connected(void);
const char *lexin_wifi_status_text(void);
