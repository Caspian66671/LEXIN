#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#ifndef CONFIG_SLAVE_IDF_TARGET_ESP32C6
#define CONFIG_SLAVE_IDF_TARGET_ESP32C6 1
#endif
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lexin_actions.h"
#include "lexin_display_test.h"
#include "lexin_edge_advisor.h"
#include "lexin_interaction.h"
#include "lexin_face_auth.h"
#include "lexin_launcher.h"
#include "lexin_triggers.h"
#include "lexin_vision.h"
#include "lexin_voice.h"
#include "lexin_wifi.h"

static const char *TAG = "lexin";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define PROXY_DISCOVERY_TIMEOUT_MS 60
#define PROXY_FAST_HTTP_TIMEOUT_MS 6000
#define PROXY_AI_HTTP_TIMEOUT_MS 8000
#define ACTION_WIFI_WAIT_MS 3000
#define PROXY_DISCOVERY_ATTEMPTS 2
#define PROXY_DISCOVERY_RETRY_DELAY_MS 250
#define PROXY_BASE_URL_MAX_LEN 128
#define PROXY_ACTION_URL_MAX_LEN 160
#define PROXY_RESPONSE_MAX_LEN 1024
#define PROXY_DISCOVERY_MAX_HOSTS 254
#define PROXY_DISCOVERY_UDP_PORT (CONFIG_LEXIN_PROXY_PORT + 1)
#define TRIGGER_WORKER_COUNT 4
#define WIFI_NVS_NAMESPACE "wifi_mem"
#define CAPTURE_UPLOAD_TIMEOUT_MS 10000
#define EMOTION_PERIOD_MS (30 * 60 * 1000)
#define EMOTION_WINDOW_MS (30 * 1000)
#define EMOTION_WINDOW_SAMPLE_MS 2000
#define EMOTION_MANUAL_SAMPLE_MS 5000
#define EMOTION_UPLOAD_TIMEOUT_MS 7000
#define EMOTION_REPORT_TIMEOUT_MS 20000
#define EMOTION_PENDING_MAX 32
#define EMOTION_JSON_MAX_LEN 640
#define EMOTION_REPORT_BUSY_TEXT "REPORT BUSY"
#define CAPTURE_PAYLOAD_BYTES \
    (LEXIN_VISION_PREVIEW_WIDTH * LEXIN_VISION_PREVIEW_HEIGHT * sizeof(uint16_t))

static QueueHandle_t btn_queue;
static EventGroupHandle_t wifi_event_group;
static SemaphoreHandle_t context_mutex;
static SemaphoreHandle_t proxy_mutex;
static int wifi_retry_count;
static bool wifi_enabled;
static bool wifi_driver_ready;
static bool wifi_ignore_next_disconnect;
static char wifi_status_text[96] = "WiFi offline";
static int wifi_last_disconnect_reason;
static char proxy_base_url[PROXY_BASE_URL_MAX_LEN];
static char cached_weather_context[256];
static char cached_time_context[256];
static bool capture_upload_busy;
static SemaphoreHandle_t emotion_mutex;
static QueueHandle_t emotion_upload_queue;
static bool emotion_report_busy;
static bool emotion_periodic_started;
static int64_t emotion_last_manual_ms;
static lexin_vision_mood_t emotion_last_manual_mood = LEXIN_VISION_MOOD_AWAY;

typedef struct {
    const char *source;
    uint32_t source_id;
    lexin_action_id_t action_id;
} trigger_event_t;

typedef struct {
    char text[PROXY_RESPONSE_MAX_LEN];
    size_t len;
} proxy_response_t;

typedef struct {
    char user_id[32];
    char user_name[64];
    char source[16];
    char mood[16];
    uint8_t mood_confidence;
    bool face_detected;
    uint32_t frame_id;
    uint32_t inference_ms;
    uint16_t camera_fps_x10;
    uint32_t device_ms;
} emotion_sample_t;

static emotion_sample_t emotion_pending[EMOTION_PENDING_MAX];
static int emotion_pending_count;

static void emotion_handle_snapshot(const lexin_vision_snapshot_t *snapshot);
static void emotion_periodic_task(void *arg);

static void vision_snapshot_callback(const lexin_vision_snapshot_t *snapshot, void *user_data)
{
    (void)user_data;
    lexin_screen_update_vision_context(snapshot);
    emotion_handle_snapshot(snapshot);
}

static void voice_snapshot_callback(const lexin_voice_snapshot_t *snapshot, void *user_data)
{
    (void)user_data;
    lexin_screen_update_voice_context(snapshot);
}

static void face_auth_callback(const lexin_face_auth_snapshot_t *snapshot, void *user_data)
{
    (void)user_data;
    lexin_screen_update_face_auth(snapshot);
}

static void wifi_set_status(const char *status)
{
    snprintf(wifi_status_text, sizeof(wifi_status_text), "%s", status ? status : "WiFi idle");
}

static uint32_t wifi_ssid_hash(const char *ssid)
{
    uint32_t hash = 2166136261u;
    while (ssid != NULL && *ssid != '\0') {
        hash ^= (uint8_t)*ssid++;
        hash *= 16777619u;
    }
    return hash;
}

static void wifi_password_nvs_key(const char *ssid, char *key, size_t key_size)
{
    snprintf(key, key_size, "p%08lx", (unsigned long)wifi_ssid_hash(ssid));
}

static const char *wifi_reason_name(int reason)
{
    switch (reason) {
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Beacon timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "No AP found";
    case WIFI_REASON_AUTH_FAIL:
        return "Auth failed";
    case WIFI_REASON_ASSOC_FAIL:
        return "Assoc failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "Handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL:
        return "Connection failed";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "Security not compatible";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "Auth mode mismatch";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Signal too weak";
    default:
        return "Connect failed";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (wifi_enabled) {
            wifi_set_status("Connecting...");
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_set_status("Waiting IP...");
        ESP_LOGI(TAG, "WiFi associated, waiting for DHCP");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        wifi_last_disconnect_reason = disc ? disc->reason : 0;
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (wifi_ignore_next_disconnect) {
            wifi_ignore_next_disconnect = false;
            ESP_LOGI(TAG, "Ignore expected WiFi disconnect before reconnect, reason=%d",
                     wifi_last_disconnect_reason);
            return;
        }
        if (wifi_enabled && wifi_retry_count < CONFIG_LEXIN_WIFI_MAXIMUM_RETRY) {
            wifi_retry_count++;
            wifi_set_status("Reconnecting...");
            ESP_LOGW(TAG, "WiFi disconnected, reason=%d, retry %d",
                     wifi_last_disconnect_reason, wifi_retry_count);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            if (wifi_enabled) {
                snprintf(wifi_status_text, sizeof(wifi_status_text),
                         "%s (%d)", wifi_reason_name(wifi_last_disconnect_reason),
                         wifi_last_disconnect_reason);
            } else {
                wifi_set_status("WiFi offline");
            }
            ESP_LOGW(TAG, "WiFi still disconnected, reason=%d, stop reconnecting until restart",
                     wifi_last_disconnect_reason);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_retry_count = 0;
        xEventGroupClearBits(wifi_event_group, WIFI_FAIL_BIT);
        snprintf(wifi_status_text, sizeof(wifi_status_text), "Connected " IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        wifi_set_status("Lost IP");
        ESP_LOGW(TAG, "WiFi lost IP");
    }
}

static bool wifi_credentials_configured(void)
{
    const char *ssid = CONFIG_LEXIN_WIFI_SSID;
    const char *password = CONFIG_LEXIN_WIFI_PASSWORD;

    if (ssid[0] == '\0' || password[0] == '\0') {
        return false;
    }
    if (strcmp(ssid, "abc") == 0 && strcmp(password, "abc123456") == 0) {
        return false;
    }
    return true;
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();
    wifi_enabled = wifi_credentials_configured();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_country(&country));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_LEXIN_WIFI_SSID,
            .password = CONFIG_LEXIN_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (wifi_enabled) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_driver_ready = true;

    if (wifi_enabled) {
        wifi_set_status("Connecting...");
        ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", CONFIG_LEXIN_WIFI_SSID);
    } else {
        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        wifi_set_status("Tap WiFi to connect");
        ESP_LOGI(TAG, "WiFi is not configured; driver is ready for scan/connect");
    }
}

bool lexin_wifi_is_connected(void)
{
    if (!wifi_event_group) {
        return false;
    }
    return (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

const char *lexin_wifi_status_text(void)
{
    return wifi_status_text;
}

bool lexin_wifi_get_saved_password(const char *ssid, char *password, size_t password_size)
{
    if (!ssid || ssid[0] == '\0' || !password || password_size == 0) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    char key[16];
    wifi_password_nvs_key(ssid, key, sizeof(key));
    size_t required = password_size;
    err = nvs_get_str(handle, key, password, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        password[0] = '\0';
        return false;
    }

    password[password_size - 1] = '\0';
    return true;
}

esp_err_t lexin_wifi_save_password(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || !password || password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char key[16];
    wifi_password_nvs_key(ssid, key, sizeof(key));
    err = nvs_set_str(handle, key, password);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved WiFi password for SSID: %s", ssid);
    } else {
        ESP_LOGW(TAG, "Failed to save WiFi password for SSID %s: %s",
                 ssid, esp_err_to_name(err));
    }
    return err;
}

esp_err_t lexin_wifi_scan(lexin_wifi_ap_t *aps, uint16_t *count)
{
    if (!aps || !count || *count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_driver_ready) {
        wifi_set_status("WiFi driver not ready");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_set_status("Scanning...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        wifi_set_status("Scan failed");
        return err;
    }

    uint16_t found = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&found));
    uint16_t requested = *count;
    if (found < requested) {
        requested = found;
    }
    wifi_ap_record_t records[LEXIN_WIFI_MAX_APS] = {0};
    if (requested > LEXIN_WIFI_MAX_APS) {
        requested = LEXIN_WIFI_MAX_APS;
    }
    err = esp_wifi_scan_get_ap_records(&requested, records);
    if (err != ESP_OK) {
        wifi_set_status("Scan read failed");
        return err;
    }

    for (uint16_t i = 0; i < requested; i++) {
        snprintf(aps[i].ssid, sizeof(aps[i].ssid), "%s", (const char *)records[i].ssid);
        memcpy(aps[i].bssid, records[i].bssid, sizeof(aps[i].bssid));
        aps[i].rssi = records[i].rssi;
        aps[i].authmode = (uint8_t)records[i].authmode;
        aps[i].channel = records[i].primary;
    }
    *count = requested;
    wifi_set_status(requested > 0 ? "Select network" : "No networks found");
    return ESP_OK;
}

static esp_err_t wifi_connect_with_optional_ap(const char *ssid, const char *password,
                                               const lexin_wifi_ap_t *ap)
{
    if (!ssid || ssid[0] == '\0') {
        wifi_set_status("Select a network");
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_driver_ready) {
        wifi_set_status("WiFi driver not ready");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             password ? password : "");
    wifi_config.sta.threshold.authmode = (password && password[0] != '\0')
        ? WIFI_AUTH_WPA_PSK
        : WIFI_AUTH_OPEN;
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (ap != NULL) {
        memcpy(wifi_config.sta.bssid, ap->bssid, sizeof(wifi_config.sta.bssid));
        wifi_config.sta.channel = ap->channel;
        wifi_config.sta.bssid_set = true;
    }

    wifi_enabled = true;
    wifi_retry_count = 0;
    wifi_last_disconnect_reason = 0;
    proxy_base_url[0] = '\0';
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    wifi_set_status("Connecting...");

    wifi_ignore_next_disconnect = false;
    if (lexin_wifi_is_connected()) {
        wifi_ignore_next_disconnect = true;
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK) {
            wifi_ignore_next_disconnect = false;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_ignore_next_disconnect = false;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        wifi_set_status("Connect start failed");
    }
    ESP_LOGI(TAG, "Connecting to WiFi SSID from screen: %s, password_len=%u",
             ssid, (unsigned)strlen(password ? password : ""));
    return err == ESP_ERR_WIFI_CONN ? ESP_OK : err;
}

esp_err_t lexin_wifi_connect(const char *ssid, const char *password)
{
    return wifi_connect_with_optional_ap(ssid, password, NULL);
}

esp_err_t lexin_wifi_connect_ap(const lexin_wifi_ap_t *ap, const char *password)
{
    if (!ap) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Do not pin the connection to the scanned BSSID/channel. Some routers use
     * band steering, mesh roaming, or multiple APs under the same SSID, and the
     * scanned record can become stale before the connection attempt completes.
     */
    return wifi_connect_with_optional_ap(ap->ssid, password, NULL);
}

static bool tcp_port_open(uint32_t host_addr)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = PROXY_DISCOVERY_TIMEOUT_MS * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_LEXIN_PROXY_PORT),
        .sin_addr.s_addr = host_addr,
    };

    bool ok = connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0;
    close(sock);
    return ok;
}

static const char *set_proxy_base_url(uint32_t addr)
{
    snprintf(proxy_base_url, sizeof(proxy_base_url),
             "http://" IPSTR ":%d",
             IP2STR((esp_ip4_addr_t *)&addr), CONFIG_LEXIN_PROXY_PORT);
    ESP_LOGI(TAG, "Found fast proxy: %s", proxy_base_url);
    return proxy_base_url;
}

static uint32_t discover_proxy_udp(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return 0;
    }

    int enabled = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 450 * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    const char request[] = "LEXIN_DISCOVER_V1";
    struct sockaddr_in broadcast = {
        .sin_family = AF_INET,
        .sin_port = htons(PROXY_DISCOVERY_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(sock, request, sizeof(request) - 1, 0,
           (struct sockaddr *)&broadcast, sizeof(broadcast));

    char reply[48] = {0};
    struct sockaddr_in source = {0};
    socklen_t source_len = sizeof(source);
    int received = recvfrom(sock, reply, sizeof(reply) - 1, 0,
                            (struct sockaddr *)&source, &source_len);
    close(sock);

    if (received <= 0 || strncmp(reply, "LEXIN_PROXY_V1", 14) != 0) {
        return 0;
    }
    return source.sin_addr.s_addr;
}

static const char *discover_proxy_base_url(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read STA IP info for proxy discovery");
        return NULL;
    }

    if (strlen(CONFIG_LEXIN_PROXY_PREFERRED_HOST) > 0) {
        struct in_addr preferred_addr;
        if (inet_aton(CONFIG_LEXIN_PROXY_PREFERRED_HOST, &preferred_addr) != 0) {
            ESP_LOGI(TAG, "Checking preferred proxy host: %s",
                     CONFIG_LEXIN_PROXY_PREFERRED_HOST);
            if (tcp_port_open(preferred_addr.s_addr)) {
                return set_proxy_base_url(preferred_addr.s_addr);
            }
            ESP_LOGW(TAG, "Preferred proxy is unavailable; discovering this PC automatically");
        } else {
            ESP_LOGW(TAG, "Configured proxy host is invalid: %s",
                     CONFIG_LEXIN_PROXY_PREFERRED_HOST);
        }
    }

    uint32_t announced_addr = discover_proxy_udp();
    if (announced_addr != 0 && tcp_port_open(announced_addr)) {
        ESP_LOGI(TAG, "Proxy discovered by UDP announcement");
        return set_proxy_base_url(announced_addr);
    }

    if (ip_info.gw.addr != 0 && ip_info.gw.addr != ip_info.ip.addr &&
        tcp_port_open(ip_info.gw.addr)) {
        ESP_LOGI(TAG, "Gateway proxy host is reachable");
        return set_proxy_base_url(ip_info.gw.addr);
    }

    uint32_t ip = ntohl(ip_info.ip.addr);
    uint32_t network = ip & 0xffffff00UL;

    uint32_t self_host = ip & 0xffUL;
    ESP_LOGI(TAG, "Discovering proxy near device host .%lu on port %d",
             (unsigned long)self_host, CONFIG_LEXIN_PROXY_PORT);
    for (uint32_t distance = 1; distance <= PROXY_DISCOVERY_MAX_HOSTS; distance++) {
        int32_t candidates[2] = {
            (int32_t)self_host - (int32_t)distance,
            (int32_t)self_host + (int32_t)distance,
        };
        for (size_t index = 0; index < 2; index++) {
            int32_t offset = candidates[index];
            if (offset <= 0 || offset >= 255) {
                continue;
            }
            uint32_t addr = htonl(network | (uint32_t)offset);
            if (tcp_port_open(addr)) {
                return set_proxy_base_url(addr);
            }
        }
    }

    ESP_LOGE(TAG, "Fast proxy not found. Start tools/lexin_proxy.js on this WiFi.");
    return NULL;
}

static const char *get_proxy_base_url(void)
{
    if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") != 0 &&
        strlen(CONFIG_LEXIN_PROXY_BASE_URL) > 0) {
        return CONFIG_LEXIN_PROXY_BASE_URL;
    }
    if (proxy_base_url[0] != '\0') {
        return proxy_base_url;
    }
    if (!proxy_mutex || xSemaphoreTake(proxy_mutex, pdMS_TO_TICKS(18000)) != pdTRUE) {
        return NULL;
    }
    const char *result = proxy_base_url[0] != '\0' ?
        proxy_base_url : discover_proxy_base_url();
    xSemaphoreGive(proxy_mutex);
    return result;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        proxy_response_t *response = (proxy_response_t *)evt->user_data;
        if (response != NULL && response->len < sizeof(response->text) - 1) {
            size_t space = sizeof(response->text) - 1 - response->len;
            size_t copy_len = evt->data_len < space ? evt->data_len : space;
            memcpy(response->text + response->len, evt->data, copy_len);
            response->len += copy_len;
            response->text[response->len] = '\0';
        }
    }
    return ESP_OK;
}

static void proxy_warmup_task(void *arg)
{
    if (!wifi_enabled) {
        ESP_LOGI(TAG, "Skip proxy warmup in offline mode");
        vTaskDelete(NULL);
    }
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    if (get_proxy_base_url() != NULL) {
        ESP_LOGI(TAG, "Fast proxy endpoint is ready");
        lexin_enqueue_trigger("warmup", 0, LEXIN_ACTION_WEATHER);
        lexin_enqueue_trigger("warmup", 1, LEXIN_ACTION_TIME);
    }
    vTaskDelete(NULL);
}

static void cache_fast_context(lexin_action_id_t action_id, const char *text)
{
    if (!text || !context_mutex ||
        (action_id != LEXIN_ACTION_WEATHER && action_id != LEXIN_ACTION_TIME)) {
        return;
    }
    if (xSemaphoreTake(context_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    char *target = action_id == LEXIN_ACTION_WEATHER ?
        cached_weather_context : cached_time_context;
    snprintf(target, 256, "%s", text);
    xSemaphoreGive(context_mutex);
}

static void show_fast_local_advisor(void)
{
    char edge_context[PROXY_RESPONSE_MAX_LEN] = {0};
    bool has_context = false;
    if (context_mutex && xSemaphoreTake(context_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        has_context = cached_weather_context[0] != '\0' || cached_time_context[0] != '\0';
        snprintf(edge_context, sizeof(edge_context), "%s\n%s",
                 cached_weather_context, cached_time_context);
        xSemaphoreGive(context_mutex);
    }
    if (!has_context) {
        return;
    }

    char interaction_context[320];
    char advisor_text[PROXY_RESPONSE_MAX_LEN];
    lexin_interaction_build_context(interaction_context, sizeof(interaction_context));
    size_t used = strlen(edge_context);
    if (used + 1 < sizeof(edge_context)) {
        edge_context[used++] = '\n';
        edge_context[used] = '\0';
    }
    if (used < sizeof(edge_context) - 1) {
        strncat(edge_context, interaction_context,
                sizeof(edge_context) - strlen(edge_context) - 1);
    }
    if (lexin_edge_advisor_infer_text(edge_context,
                                           advisor_text,
                                           sizeof(advisor_text))) {
        ESP_LOGI(TAG, "Fast local advisor result: %s", advisor_text);
        lexin_screen_show_result_text(LEXIN_ACTION_AI_INSIGHT, advisor_text);
    }
}

static void show_cached_action(lexin_action_id_t action_id)
{
    if (!context_mutex ||
        (action_id != LEXIN_ACTION_WEATHER && action_id != LEXIN_ACTION_TIME)) {
        return;
    }
    char cached[256] = {0};
    if (xSemaphoreTake(context_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        const char *source = action_id == LEXIN_ACTION_WEATHER ?
            cached_weather_context : cached_time_context;
        snprintf(cached, sizeof(cached), "%s", source);
        xSemaphoreGive(context_mutex);
    }
    if (cached[0] != '\0') {
        lexin_screen_show_result_text(action_id, cached);
    }
}

static void request_proxy_action(const lexin_action_t *action)
{
    if (!wifi_enabled) {
        ESP_LOGW(TAG, "%s requested, but WiFi is not configured", action->name);
        lexin_screen_show_error(action->id);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ACTION_WIFI_WAIT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "%s requested, but WiFi did not connect in time", action->name);
        lexin_screen_show_error(action->id);
        return;
    }

    const char *base_url = NULL;
    for (int attempt = 1; attempt <= PROXY_DISCOVERY_ATTEMPTS; attempt++) {
        base_url = get_proxy_base_url();
        if (base_url != NULL) {
            break;
        }
        ESP_LOGW(TAG, "Fast proxy not ready for %s, retry %d/%d",
                 action->name, attempt, PROXY_DISCOVERY_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(PROXY_DISCOVERY_RETRY_DELAY_MS));
    }
    if (base_url == NULL) {
        ESP_LOGE(TAG, "%s requested, but fast proxy endpoint is not ready", action->name);
        lexin_screen_show_error(action->id);
        return;
    }

    char action_url[PROXY_ACTION_URL_MAX_LEN];
    snprintf(action_url, sizeof(action_url), "%s%s", base_url, action->path);

    proxy_response_t response = {0};
    esp_http_client_config_t config = {
        .url = action_url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .timeout_ms = action->id == LEXIN_ACTION_AI_INSIGHT ?
            PROXY_AI_HTTP_TIMEOUT_MS : PROXY_FAST_HTTP_TIMEOUT_MS,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        lexin_screen_show_error(action->id);
        return;
    }

    ESP_LOGI(TAG, "GET fast proxy %s: %s", action->name, action_url);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Proxy HTTP status=%d, content_length=%lld",
                 status_code, esp_http_client_get_content_length(client));
        if (response.len > 0) {
            ESP_LOGI(TAG, "Proxy response collected: %s", response.text);
        }
        if (status_code >= 200 && status_code < 300 && response.len > 0) {
            cache_fast_context(action->id, response.text);
            if (action->id == LEXIN_ACTION_AI_INSIGHT) {
                char interaction_context[320];
                char edge_context[PROXY_RESPONSE_MAX_LEN];
                char advisor_text[PROXY_RESPONSE_MAX_LEN];
                lexin_interaction_build_context(interaction_context, sizeof(interaction_context));
                edge_context[0] = '\0';
                strncpy(edge_context, response.text, sizeof(edge_context) - 1);
                edge_context[sizeof(edge_context) - 1] = '\0';
                size_t used = strlen(edge_context);
                if (used + 1 < sizeof(edge_context)) {
                    edge_context[used++] = '\n';
                    edge_context[used] = '\0';
                }
                if (used < sizeof(edge_context) - 1) {
                    strncpy(edge_context + used, interaction_context,
                            sizeof(edge_context) - used - 1);
                    edge_context[sizeof(edge_context) - 1] = '\0';
                }
                ESP_LOGI(TAG, "Edge advisor enriched context: %s", edge_context);
                if (lexin_edge_advisor_infer_text(edge_context,
                                                       advisor_text,
                                                       sizeof(advisor_text))) {
                    ESP_LOGI(TAG, "Edge advisor result: %s", advisor_text);
                    lexin_screen_show_result_text(action->id, advisor_text);
                } else {
                    lexin_screen_show_error(action->id);
                }
            } else {
                lexin_screen_show_result_text(action->id, response.text);
            }
        } else {
            lexin_screen_show_error(action->id);
        }
    } else {
        ESP_LOGE(TAG, "Proxy request failed: %s", esp_err_to_name(err));
        if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") == 0) {
            proxy_base_url[0] = '\0';
            ESP_LOGW(TAG, "Cleared stale proxy address; the next request will rediscover it");
        }
        lexin_screen_show_error(action->id);
    }

    esp_http_client_cleanup(client);
}

static void capture_upload_task(void *arg)
{
    (void)arg;
    uint16_t *pixels = (uint16_t *)heap_caps_malloc(CAPTURE_PAYLOAD_BYTES,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = (uint16_t *)malloc(CAPTURE_PAYLOAD_BYTES);
    }
    if (!pixels) {
        ESP_LOGE(TAG, "No memory for board capture upload");
        lexin_screen_set_capture_status("CAPTURE NO MEM");
        capture_upload_busy = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t frame_id = 0;
    esp_err_t err = lexin_vision_copy_preview(pixels,
                                               LEXIN_VISION_PREVIEW_WIDTH * LEXIN_VISION_PREVIEW_HEIGHT,
                                               &frame_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Capture preview not ready: %s", esp_err_to_name(err));
        lexin_screen_set_capture_status("CAPTURE NO FRAME");
        free(pixels);
        capture_upload_busy = false;
        vTaskDelete(NULL);
        return;
    }

    if (!wifi_enabled) {
        ESP_LOGW(TAG, "Capture requested, but WiFi is not configured");
        lexin_screen_set_capture_status("CAPTURE NO WIFI");
        free(pixels);
        capture_upload_busy = false;
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ACTION_WIFI_WAIT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Capture requested, but WiFi is not connected");
        lexin_screen_set_capture_status("CAPTURE WIFI WAIT");
        free(pixels);
        capture_upload_busy = false;
        vTaskDelete(NULL);
        return;
    }

    const char *base_url = NULL;
    for (int attempt = 1; attempt <= PROXY_DISCOVERY_ATTEMPTS; attempt++) {
        base_url = get_proxy_base_url();
        if (base_url != NULL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PROXY_DISCOVERY_RETRY_DELAY_MS));
    }
    if (base_url == NULL) {
        ESP_LOGE(TAG, "Capture requested, but proxy endpoint is not ready");
        lexin_screen_set_capture_status("CAPTURE NO PROXY");
        free(pixels);
        capture_upload_busy = false;
        vTaskDelete(NULL);
        return;
    }

    char action_url[PROXY_ACTION_URL_MAX_LEN];
    snprintf(action_url, sizeof(action_url), "%s/capture", base_url);
    char width_header[8];
    char height_header[8];
    char frame_header[16];
    snprintf(width_header, sizeof(width_header), "%u", LEXIN_VISION_PREVIEW_WIDTH);
    snprintf(height_header, sizeof(height_header), "%u", LEXIN_VISION_PREVIEW_HEIGHT);
    snprintf(frame_header, sizeof(frame_header), "%lu", (unsigned long)frame_id);

    proxy_response_t response = {0};
    esp_http_client_config_t config = {
        .url = action_url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = CAPTURE_UPLOAD_TIMEOUT_MS,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init capture HTTP client");
        lexin_screen_set_capture_status("CAPTURE HTTP INIT");
        free(pixels);
        capture_upload_busy = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "content-type", "application/octet-stream");
    esp_http_client_set_header(client, "x-lexin-width", width_header);
    esp_http_client_set_header(client, "x-lexin-height", height_header);
    esp_http_client_set_header(client, "x-lexin-format", "rgb565le");
    esp_http_client_set_header(client, "x-lexin-frame-id", frame_header);
    esp_http_client_set_post_field(client, (const char *)pixels, CAPTURE_PAYLOAD_BYTES);

    lexin_screen_set_capture_status("CAPTURE SENDING");
    ESP_LOGI(TAG, "POST board capture frame=%lu bytes=%u to %s",
             (unsigned long)frame_id, (unsigned)CAPTURE_PAYLOAD_BYTES, action_url);
    err = esp_http_client_perform(client);
    int status_code = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        ESP_LOGI(TAG, "Capture saved by proxy: %s", response.text);
        lexin_screen_set_capture_status("CAPTURE SAVED");
    } else {
        ESP_LOGW(TAG, "Capture upload failed err=%s status=%d",
                 esp_err_to_name(err), status_code);
        if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") == 0) {
            proxy_base_url[0] = '\0';
        }
        lexin_screen_set_capture_status("CAPTURE FAILED");
    }

    esp_http_client_cleanup(client);
    free(pixels);
    capture_upload_busy = false;
    vTaskDelete(NULL);
}

void lexin_request_board_capture(void)
{
    if (capture_upload_busy) {
        lexin_screen_set_capture_status("CAPTURE BUSY");
        return;
    }
    capture_upload_busy = true;
    lexin_screen_set_capture_status("CAPTURE QUEUED");
    if (xTaskCreate(capture_upload_task, "capture_upload", 8192, NULL, 6, NULL) != pdPASS) {
        capture_upload_busy = false;
        lexin_screen_set_capture_status("CAPTURE TASK FAIL");
    }
}

static const char *emotion_mood_name(lexin_vision_mood_t mood)
{
    switch (mood) {
    case LEXIN_VISION_MOOD_HAPPY:
        return "happy";
    case LEXIN_VISION_MOOD_TIRED:
        return "tired";
    case LEXIN_VISION_MOOD_STRESSED:
        return "stressed";
    case LEXIN_VISION_MOOD_SURPRISED:
        return "surprised";
    case LEXIN_VISION_MOOD_FOCUSED:
        return "focused";
    case LEXIN_VISION_MOOD_AWAY:
    default:
        return "away";
    }
}

static bool emotion_sample_from_snapshot(const lexin_vision_snapshot_t *snapshot,
                                         const char *source,
                                         emotion_sample_t *out)
{
    if (!snapshot || !out || !lexin_face_auth_is_logged_in() ||
        !snapshot->face_detected || snapshot->mood == LEXIN_VISION_MOOD_AWAY) {
        return false;
    }

    const char *uid = lexin_face_auth_current_user_id();
    const char *uname = lexin_face_auth_current_user_name();
    if (!uid || uid[0] == '\0') {
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->user_id, sizeof(out->user_id), "%s", uid);
    snprintf(out->user_name, sizeof(out->user_name), "%s", uname && uname[0] ? uname : uid);
    snprintf(out->source, sizeof(out->source), "%s", source ? source : "manual");
    snprintf(out->mood, sizeof(out->mood), "%s", emotion_mood_name(snapshot->mood));
    out->mood_confidence = snapshot->mood_confidence;
    out->face_detected = snapshot->face_detected;
    out->frame_id = snapshot->frame_id;
    out->inference_ms = snapshot->inference_ms;
    out->camera_fps_x10 = snapshot->camera_fps_x10;
    out->device_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return true;
}

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t used = 0;
    while (*src != '\0' && used + 1 < dst_size) {
        unsigned char c = (unsigned char)*src++;
        if ((c == '"' || c == '\\') && used + 2 < dst_size) {
            dst[used++] = '\\';
            dst[used++] = (char)c;
        } else if (c >= 0x20) {
            dst[used++] = (char)c;
        }
    }
    dst[used] = '\0';
}

static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    if (dst_size == 0) {
        return;
    }
    size_t used = 0;
    while (src && *src != '\0' && used + 1 < dst_size) {
        unsigned char c = (unsigned char)*src++;
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~';
        if (safe) {
            dst[used++] = (char)c;
        } else if (used + 3 < dst_size) {
            dst[used++] = '%';
            dst[used++] = hex[c >> 4];
            dst[used++] = hex[c & 0x0f];
        } else {
            break;
        }
    }
    dst[used] = '\0';
}

static bool emotion_enqueue_pending(const emotion_sample_t *sample)
{
    if (!sample || !emotion_mutex ||
        xSemaphoreTake(emotion_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    if (emotion_pending_count >= EMOTION_PENDING_MAX) {
        memmove(&emotion_pending[0], &emotion_pending[1],
                sizeof(emotion_pending[0]) * (EMOTION_PENDING_MAX - 1));
        emotion_pending_count = EMOTION_PENDING_MAX - 1;
    }
    emotion_pending[emotion_pending_count++] = *sample;
    xSemaphoreGive(emotion_mutex);
    return true;
}

static bool emotion_upload_sample(const emotion_sample_t *sample)
{
    if (!sample || !wifi_enabled) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ACTION_WIFI_WAIT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        return false;
    }

    const char *base_url = NULL;
    for (int attempt = 1; attempt <= PROXY_DISCOVERY_ATTEMPTS; attempt++) {
        base_url = get_proxy_base_url();
        if (base_url != NULL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PROXY_DISCOVERY_RETRY_DELAY_MS));
    }
    if (base_url == NULL) {
        return false;
    }

    char user_id[80];
    char user_name[140];
    char source[40];
    char mood[40];
    json_escape(sample->user_id, user_id, sizeof(user_id));
    json_escape(sample->user_name, user_name, sizeof(user_name));
    json_escape(sample->source, source, sizeof(source));
    json_escape(sample->mood, mood, sizeof(mood));

    char json[EMOTION_JSON_MAX_LEN];
    int written = snprintf(json, sizeof(json),
                           "{\"user_id\":\"%s\",\"user_name\":\"%s\","
                           "\"source\":\"%s\",\"mood\":\"%s\","
                           "\"mood_confidence\":%u,\"face_detected\":%s,"
                           "\"frame_id\":%lu,\"inference_ms\":%lu,"
                           "\"camera_fps_x10\":%u,\"device_ms\":%lu}",
                           user_id, user_name, source, mood,
                           (unsigned)sample->mood_confidence,
                           sample->face_detected ? "true" : "false",
                           (unsigned long)sample->frame_id,
                           (unsigned long)sample->inference_ms,
                           (unsigned)sample->camera_fps_x10,
                           (unsigned long)sample->device_ms);
    if (written <= 0 || written >= (int)sizeof(json)) {
        ESP_LOGW(TAG, "Emotion sample JSON too large");
        return false;
    }

    char action_url[PROXY_ACTION_URL_MAX_LEN];
    snprintf(action_url, sizeof(action_url), "%s/emotion-log", base_url);
    proxy_response_t response = {0};
    esp_http_client_config_t config = {
        .url = action_url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = EMOTION_UPLOAD_TIMEOUT_MS,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return false;
    }

    esp_http_client_set_header(client, "content-type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));
    esp_err_t err = esp_http_client_perform(client);
    int status_code = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        ESP_LOGI(TAG, "Emotion logged: %s %s frame=%lu",
                 sample->source, sample->mood, (unsigned long)sample->frame_id);
        return true;
    }

    ESP_LOGW(TAG, "Emotion log failed err=%s status=%d",
             esp_err_to_name(err), status_code);
    if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") == 0) {
        proxy_base_url[0] = '\0';
    }
    return false;
}

static void emotion_flush_pending(void)
{
    while (emotion_mutex &&
           xSemaphoreTake(emotion_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (emotion_pending_count <= 0) {
            xSemaphoreGive(emotion_mutex);
            return;
        }
        emotion_sample_t sample = emotion_pending[0];
        xSemaphoreGive(emotion_mutex);

        if (!emotion_upload_sample(&sample)) {
            return;
        }

        if (xSemaphoreTake(emotion_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (emotion_pending_count > 0) {
                memmove(&emotion_pending[0], &emotion_pending[1],
                        sizeof(emotion_pending[0]) * (emotion_pending_count - 1));
                emotion_pending_count--;
            }
            xSemaphoreGive(emotion_mutex);
        }
    }
}

static void emotion_queue_sample(const emotion_sample_t *sample)
{
    if (!sample) {
        return;
    }
    if (emotion_upload_queue &&
        xQueueSend(emotion_upload_queue, sample, 0) == pdTRUE) {
        return;
    }
    emotion_enqueue_pending(sample);
}

static void emotion_upload_task(void *arg)
{
    (void)arg;
    emotion_sample_t sample;
    while (1) {
        if (xQueueReceive(emotion_upload_queue, &sample, portMAX_DELAY) == pdTRUE) {
            emotion_flush_pending();
            if (!emotion_upload_sample(&sample)) {
                emotion_enqueue_pending(&sample);
            }
        }
    }
}

static void emotion_record_snapshot(const lexin_vision_snapshot_t *snapshot,
                                    const char *source)
{
    emotion_sample_t sample;
    if (emotion_sample_from_snapshot(snapshot, source, &sample)) {
        emotion_queue_sample(&sample);
    }
}

static void emotion_handle_snapshot(const lexin_vision_snapshot_t *snapshot)
{
    if (!snapshot || !lexin_screen_is_emotion_live()) {
        return;
    }

    int64_t now_ms = snapshot->updated_at_ms > 0 ?
        snapshot->updated_at_ms : (esp_timer_get_time() / 1000);
    bool mood_changed = snapshot->mood != emotion_last_manual_mood;
    if (mood_changed || now_ms - emotion_last_manual_ms >= EMOTION_MANUAL_SAMPLE_MS) {
        emotion_last_manual_ms = now_ms;
        emotion_last_manual_mood = snapshot->mood;
        emotion_record_snapshot(snapshot, "manual");
    }
}

static void emotion_periodic_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EMOTION_PERIOD_MS));
        if (!lexin_face_auth_is_logged_in()) {
            continue;
        }

        int64_t start_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "Emotion periodic sample window opened");
        while ((esp_timer_get_time() / 1000) - start_ms < EMOTION_WINDOW_MS) {
            lexin_vision_snapshot_t snapshot;
            lexin_vision_get_snapshot(&snapshot);
            emotion_record_snapshot(&snapshot, "periodic");
            vTaskDelay(pdMS_TO_TICKS(EMOTION_WINDOW_SAMPLE_MS));
        }
        emotion_flush_pending();
        ESP_LOGI(TAG, "Emotion periodic sample window closed");
    }
}

static void emotion_report_task(void *arg)
{
    bool monthly = (bool)(uintptr_t)arg;
    if (!wifi_enabled) {
        lexin_screen_set_capture_status("REPORT NO WIFI");
        emotion_report_busy = false;
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ACTION_WIFI_WAIT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        lexin_screen_set_capture_status("REPORT WIFI WAIT");
        emotion_report_busy = false;
        vTaskDelete(NULL);
        return;
    }

    const char *base_url = NULL;
    for (int attempt = 1; attempt <= PROXY_DISCOVERY_ATTEMPTS; attempt++) {
        base_url = get_proxy_base_url();
        if (base_url != NULL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PROXY_DISCOVERY_RETRY_DELAY_MS));
    }
    if (base_url == NULL) {
        lexin_screen_set_capture_status("REPORT NO PROXY");
        emotion_report_busy = false;
        vTaskDelete(NULL);
        return;
    }

    const char *uid = lexin_face_auth_current_user_id();
    if (!uid || uid[0] == '\0') {
        lexin_screen_set_capture_status("REPORT NO USER");
        emotion_report_busy = false;
        vTaskDelete(NULL);
        return;
    }

    char encoded_uid[96];
    url_encode(uid, encoded_uid, sizeof(encoded_uid));

    char action_url[PROXY_ACTION_URL_MAX_LEN];
    snprintf(action_url, sizeof(action_url), "%s/%s?user_id=%s", base_url,
             monthly ? "emotion-month" : "emotion-report", encoded_uid);

    proxy_response_t response = {0};
    esp_http_client_config_t config = {
        .url = action_url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .timeout_ms = EMOTION_REPORT_TIMEOUT_MS,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        lexin_screen_set_capture_status("REPORT HTTP INIT");
        emotion_report_busy = false;
        vTaskDelete(NULL);
        return;
    }

    lexin_screen_set_capture_status("REPORT LOADING");
    ESP_LOGI(TAG, "GET emotion %s report: %s", monthly ? "month" : "daily", action_url);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status_code >= 200 && status_code < 300 && response.len > 0) {
        lexin_screen_show_emotion_report(response.text, monthly);
        lexin_screen_set_capture_status("REPORT READY");
    } else {
        ESP_LOGW(TAG, "Emotion report failed err=%s status=%d",
                 esp_err_to_name(err), status_code);
        if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") == 0) {
            proxy_base_url[0] = '\0';
        }
        lexin_screen_show_emotion_report(
            "MODEL: LOCAL\nSAMPLES: 0\nDOMINANT: UNKNOWN\nAVG_SCORE: 0.00\n"
            "STRESS_TIRED: 0%\nHAPPY_FOCUSED: 0%\nSCORES: 0\n"
            "ADVICE: Report request failed. Check WiFi and proxy.",
            monthly);
        lexin_screen_set_capture_status("REPORT FAILED");
    }

    emotion_report_busy = false;
    vTaskDelete(NULL);
}

void lexin_request_emotion_report(bool monthly)
{
    if (emotion_report_busy) {
        lexin_screen_set_capture_status(EMOTION_REPORT_BUSY_TEXT);
        return;
    }
    emotion_report_busy = true;
    lexin_screen_set_capture_status("REPORT QUEUED");
    if (xTaskCreate(emotion_report_task, "emotion_report", 8192,
                    (void *)(uintptr_t)monthly, 6, NULL) != pdPASS) {
        emotion_report_busy = false;
        lexin_screen_set_capture_status("REPORT TASK FAIL");
    }
}

/* ------------------------------------------------------------------ */
/* Daily plan networking                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    PLAN_OP_TODAY = 0,
    PLAN_OP_MONTH,
    PLAN_OP_TOGGLE,
    PLAN_OP_DAY,
    PLAN_OP_DELETE,
} plan_op_t;

typedef struct {
    plan_op_t op;
    int index;
    int year;
    int month;
    int day;
} plan_req_t;

static void plan_worker_task(void *arg)
{
    plan_req_t req = *(plan_req_t *)arg;
    free(arg);

    if (!wifi_enabled) {
        vTaskDelete(NULL);
        return;
    }
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ACTION_WIFI_WAIT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        vTaskDelete(NULL);
        return;
    }

    const char *base_url = NULL;
    for (int attempt = 1; attempt <= PROXY_DISCOVERY_ATTEMPTS; attempt++) {
        base_url = get_proxy_base_url();
        if (base_url != NULL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(PROXY_DISCOVERY_RETRY_DELAY_MS));
    }
    if (base_url == NULL) {
        vTaskDelete(NULL);
        return;
    }

    const char *uid = lexin_face_auth_current_user_id();
    if (!uid || uid[0] == '\0') {
        uid = "device";
    }
    char encoded_uid[96];
    url_encode(uid, encoded_uid, sizeof(encoded_uid));

    char action_url[PROXY_ACTION_URL_MAX_LEN];
    esp_http_client_method_t method = HTTP_METHOD_GET;
    switch (req.op) {
    case PLAN_OP_MONTH:
        snprintf(action_url, sizeof(action_url), "%s/plan-month?user_id=%s",
                 base_url, encoded_uid);
        break;
    case PLAN_OP_TOGGLE:
        method = HTTP_METHOD_POST;
        if (req.year > 0) {
            /* Editing a specific (past) day from the calendar's day view. */
            snprintf(action_url, sizeof(action_url),
                     "%s/plan-done?user_id=%s&index=%d&date=%04d-%02d-%02d",
                     base_url, encoded_uid, req.index, req.year, req.month, req.day);
        } else {
            snprintf(action_url, sizeof(action_url), "%s/plan-done?user_id=%s&index=%d",
                     base_url, encoded_uid, req.index);
        }
        break;
    case PLAN_OP_DELETE:
        method = HTTP_METHOD_POST;
        if (req.year > 0) {
            snprintf(action_url, sizeof(action_url),
                     "%s/plan-delete?user_id=%s&index=%d&date=%04d-%02d-%02d",
                     base_url, encoded_uid, req.index, req.year, req.month, req.day);
        } else {
            snprintf(action_url, sizeof(action_url), "%s/plan-delete?user_id=%s&index=%d",
                     base_url, encoded_uid, req.index);
        }
        break;
    case PLAN_OP_DAY:
        snprintf(action_url, sizeof(action_url), "%s/plan?user_id=%s&date=%04d-%02d-%02d",
                 base_url, encoded_uid, req.year, req.month, req.day);
        break;
    case PLAN_OP_TODAY:
    default:
        snprintf(action_url, sizeof(action_url), "%s/plan?user_id=%s",
                 base_url, encoded_uid);
        break;
    }

    proxy_response_t response = {0};
    esp_http_client_config_t config = {
        .url = action_url,
        .method = method,
        .event_handler = http_event_handler,
        .timeout_ms = PROXY_FAST_HTTP_TIMEOUT_MS,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        vTaskDelete(NULL);
        return;
    }
    if (method == HTTP_METHOD_POST) {
        esp_http_client_set_post_field(client, "", 0);
    }

    ESP_LOGI(TAG, "GET/POST plan: %s", action_url);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status_code >= 200 && status_code < 300 && response.len > 0) {
        switch (req.op) {
        case PLAN_OP_MONTH:
            lexin_screen_update_plan_month(response.text);
            break;
        case PLAN_OP_DAY:
            lexin_screen_update_plan_day(response.text);
            break;
        case PLAN_OP_TOGGLE:
        case PLAN_OP_DELETE:
            if (req.year > 0) {
                /* Dated edit from the day view: stay on that day's page. */
                lexin_screen_update_plan_day(response.text);
            } else {
                lexin_screen_update_plan(response.text);
            }
            break;
        case PLAN_OP_TODAY:
        default:
            lexin_screen_update_plan(response.text);
            break;
        }
    } else {
        ESP_LOGW(TAG, "Plan request failed err=%s status=%d",
                 esp_err_to_name(err), status_code);
        if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") == 0) {
            proxy_base_url[0] = '\0';
        }
    }
    vTaskDelete(NULL);
}

static void plan_enqueue(plan_op_t op, int index, int year, int month, int day)
{
    plan_req_t *req = malloc(sizeof(plan_req_t));
    if (req == NULL) {
        return;
    }
    req->op = op;
    req->index = index;
    req->year = year;
    req->month = month;
    req->day = day;
    if (xTaskCreate(plan_worker_task, "plan_req", 8192, req, 6, NULL) != pdPASS) {
        free(req);
        ESP_LOGW(TAG, "Failed to spawn plan worker");
    }
}

void lexin_plan_fetch_today(void)
{
    plan_enqueue(PLAN_OP_TODAY, 0, 0, 0, 0);
}

void lexin_plan_fetch_month(void)
{
    plan_enqueue(PLAN_OP_MONTH, 0, 0, 0, 0);
}

void lexin_plan_toggle(int index)
{
    plan_enqueue(PLAN_OP_TOGGLE, index, 0, 0, 0);
}

void lexin_plan_delete(int index)
{
    plan_enqueue(PLAN_OP_DELETE, index, 0, 0, 0);
}

void lexin_plan_toggle_day(int index, int year, int month, int day)
{
    plan_enqueue(PLAN_OP_TOGGLE, index, year, month, day);
}

void lexin_plan_delete_day(int index, int year, int month, int day)
{
    plan_enqueue(PLAN_OP_DELETE, index, year, month, day);
}

void lexin_plan_fetch_day(int year, int month, int day)
{
    plan_enqueue(PLAN_OP_DAY, 0, year, month, day);
}

static void trigger_task(void *arg)
{
    trigger_event_t event;
    while (1) {
        if (xQueueReceive(btn_queue, &event, portMAX_DELAY)) {
            const lexin_action_t *action = lexin_get_action(event.action_id);
            if (action == NULL) {
                ESP_LOGW(TAG, "Unknown action from %s%lu",
                         event.source, (unsigned long)event.source_id);
                continue;
            }

            printf("{\"event\":\"trigger\",\"source\":\"%s%lu\",\"action\":\"%s\",\"title\":\"%s\"}\n",
                   event.source, (unsigned long)event.source_id,
                   action->name, action->title_json);
            fflush(stdout);
            ESP_LOGI(TAG, "%s%lu triggered %s action",
                     event.source, (unsigned long)event.source_id, action->name);
            if (action->id == LEXIN_ACTION_AI_INSIGHT) {
                show_fast_local_advisor();
            } else {
                show_cached_action(action->id);
            }
            request_proxy_action(action);
        }
    }
}

void lexin_enqueue_trigger(const char *source, uint32_t source_id,
                               lexin_action_id_t action_id)
{
    trigger_event_t event = {
        .source = source,
        .source_id = source_id,
        .action_id = action_id,
    };
    if (xQueueSend(btn_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Drop trigger from %s%lu because another action is pending",
                 source, (unsigned long)source_id);
    }
}

static void init_trigger_queue(void)
{
    context_mutex = xSemaphoreCreateMutex();
    proxy_mutex = xSemaphoreCreateMutex();
    emotion_mutex = xSemaphoreCreateMutex();
    emotion_upload_queue = xQueueCreate(8, sizeof(emotion_sample_t));
    btn_queue = xQueueCreate(4, sizeof(trigger_event_t));
    if (emotion_upload_queue) {
        xTaskCreate(emotion_upload_task, "emotion_upload", 8192, NULL, 6, NULL);
    }
    for (int worker = 0; worker < TRIGGER_WORKER_COUNT; ++worker) {
        char name[16];
        snprintf(name, sizeof(name), "trigger_%d", worker);
        xTaskCreate(trigger_task, name, 12288, NULL, 10, NULL);
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_trigger_queue();
    lexin_start_screen_ui();
    // Keep model initialization from blocking the display's first frame.
    vTaskDelay(pdMS_TO_TICKS(800));
    lexin_edge_advisor_init();
    esp_err_t vision_ret = lexin_vision_start(vision_snapshot_callback, NULL);
    if (vision_ret != ESP_OK) {
        ESP_LOGW(TAG, "Vision service did not start: %s", esp_err_to_name(vision_ret));
    }
    wifi_init_sta();
    esp_err_t fa_ret = lexin_face_auth_start(face_auth_callback, NULL);
    if (fa_ret != ESP_OK) {
        ESP_LOGW(TAG, "Face auth did not start: %s", esp_err_to_name(fa_ret));
    }
    if (wifi_enabled) {
        xTaskCreate(proxy_warmup_task, "proxy_warmup", 4096, NULL, 5, NULL);
    }

    /* Wait for face login; lock screen is up from screen_ui_task. */
    ESP_LOGI(TAG, "Lock screen active. Waiting for face recognition...");
    int64_t last_lock_log_ms = esp_timer_get_time() / 1000;
    while (!lexin_face_auth_is_logged_in()) {
        vTaskDelay(pdMS_TO_TICKS(200));
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_lock_log_ms > 10000) {
            ESP_LOGI(TAG, "Still locked. Waiting for recognized or registered face...");
            last_lock_log_ms = now_ms;
        }
        if (false) {
            ESP_LOGW(TAG, "Face auth timeout, showing launcher");
            lexin_face_auth_login_by_id("cached", "乐鑫用户");
            break;
        }
    }
    const char *uid = lexin_face_auth_current_user_id();
    const char *uname = lexin_face_auth_current_user_name();
    ESP_LOGI(TAG, "Unlocked. User: %s (%s)", uname ? uname : "?", uid ? uid : "?");
    /* Load this user's interaction history from NVS. */
    lexin_interaction_set_user(uid ? uid : "cached");
    if (!emotion_periodic_started) {
        emotion_periodic_started = true;
        if (xTaskCreate(emotion_periodic_task, "emotion_periodic", 8192,
                        NULL, 5, NULL) != pdPASS) {
            emotion_periodic_started = false;
            ESP_LOGW(TAG, "Emotion periodic task did not start");
        }
    }

    /* Defer voice start until after unlock to avoid audio race. */
    esp_err_t voice_ret = lexin_voice_start(voice_snapshot_callback, NULL);
    if (voice_ret != ESP_OK) {
        ESP_LOGW(TAG, "Voice service did not start: %s", esp_err_to_name(voice_ret));
    }
    lexin_voice_set_user_id(uid ? uid : "device");
    if (lexin_launcher_current_screen() == LEXIN_SCREEN_LAUNCHER) {
        lexin_screen_show_idle();
    } else {
        ESP_LOGI(TAG, "Skip launcher redraw after unlock; active screen=%d",
                 (int)lexin_launcher_current_screen());
    }
    ESP_LOGI(TAG, "Ready. Touch launcher apps; net=%s.",
             wifi_enabled ? "on" : "offline");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
