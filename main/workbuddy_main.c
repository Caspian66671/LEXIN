#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#ifndef CONFIG_SLAVE_IDF_TARGET_ESP32C6
#define CONFIG_SLAVE_IDF_TARGET_ESP32C6 1
#endif
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "workbuddy_actions.h"
#include "workbuddy_display_test.h"
#include "workbuddy_triggers.h"

static const char *TAG = "workbuddy";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define PROXY_DISCOVERY_TIMEOUT_MS 120
#define PROXY_HTTP_TIMEOUT_MS 6000
#define ACTION_WIFI_WAIT_MS 60000
#define PROXY_DISCOVERY_ATTEMPTS 5
#define PROXY_DISCOVERY_RETRY_DELAY_MS 800
#define PROXY_BASE_URL_MAX_LEN 128
#define PROXY_ACTION_URL_MAX_LEN 160
#define PROXY_RESPONSE_MAX_LEN 512
#define PROXY_DISCOVERY_MAX_HOSTS 254

static QueueHandle_t btn_queue;
static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count;
static char proxy_base_url[PROXY_BASE_URL_MAX_LEN];

typedef struct {
    const char *source;
    uint32_t source_id;
    workbuddy_action_id_t action_id;
} trigger_event_t;

typedef struct {
    char text[PROXY_RESPONSE_MAX_LEN];
    size_t len;
} proxy_response_t;

static proxy_response_t proxy_response;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (wifi_retry_count < CONFIG_WORKBUDDY_WIFI_MAXIMUM_RETRY) {
            wifi_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d", wifi_retry_count);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "WiFi still disconnected, keep reconnecting");
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_retry_count = 0;
        xEventGroupClearBits(wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

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
            .ssid = CONFIG_WORKBUDDY_WIFI_SSID,
            .password = CONFIG_WORKBUDDY_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", CONFIG_WORKBUDDY_WIFI_SSID);
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
        .sin_port = htons(CONFIG_WORKBUDDY_PROXY_PORT),
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
             IP2STR((esp_ip4_addr_t *)&addr), CONFIG_WORKBUDDY_PROXY_PORT);
    ESP_LOGI(TAG, "Found fast proxy: %s", proxy_base_url);
    return proxy_base_url;
}

static const char *get_proxy_base_url(void)
{
    if (strcmp(CONFIG_WORKBUDDY_PROXY_BASE_URL, "auto") != 0 &&
        strlen(CONFIG_WORKBUDDY_PROXY_BASE_URL) > 0) {
        return CONFIG_WORKBUDDY_PROXY_BASE_URL;
    }

    if (proxy_base_url[0] != '\0') {
        return proxy_base_url;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read STA IP info for proxy discovery");
        return NULL;
    }

    if (strlen(CONFIG_WORKBUDDY_PROXY_PREFERRED_HOST) > 0) {
        struct in_addr preferred_addr;
        if (inet_aton(CONFIG_WORKBUDDY_PROXY_PREFERRED_HOST, &preferred_addr) != 0 &&
            tcp_port_open(preferred_addr.s_addr)) {
            ESP_LOGI(TAG, "Preferred proxy host is reachable: %s",
                     CONFIG_WORKBUDDY_PROXY_PREFERRED_HOST);
            return set_proxy_base_url(preferred_addr.s_addr);
        }
        ESP_LOGW(TAG, "Preferred proxy host not reachable: %s",
                 CONFIG_WORKBUDDY_PROXY_PREFERRED_HOST);
    }

    if (ip_info.gw.addr != 0 && ip_info.gw.addr != ip_info.ip.addr &&
        tcp_port_open(ip_info.gw.addr)) {
        ESP_LOGI(TAG, "Gateway proxy host is reachable");
        return set_proxy_base_url(ip_info.gw.addr);
    }

    uint32_t ip = ntohl(ip_info.ip.addr);
    uint32_t network = ip & 0xffffff00UL;

    ESP_LOGI(TAG, "Discovering fast proxy in local /24 on port %d", CONFIG_WORKBUDDY_PROXY_PORT);
    for (uint32_t offset = 1; offset <= PROXY_DISCOVERY_MAX_HOSTS; offset++) {
        uint32_t host = network | offset;
        if (host == ip || offset == 255) {
            continue;
        }

        uint32_t addr = htonl(host);
        if (tcp_port_open(addr)) {
            return set_proxy_base_url(addr);
        }
    }

    ESP_LOGE(TAG, "Fast proxy not found. Start tools/workbuddy_proxy.js on this WiFi.");
    return NULL;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        ESP_LOGI(TAG, "Proxy response chunk: %.*s", evt->data_len, (char *)evt->data);

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
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    if (get_proxy_base_url() != NULL) {
        ESP_LOGI(TAG, "Fast proxy endpoint is ready");
    }
    vTaskDelete(NULL);
}

static void request_proxy_action(const workbuddy_action_t *action)
{
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ACTION_WIFI_WAIT_MS));
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "%s requested, but WiFi did not connect in time", action->name);
        workbuddy_screen_show_error(action->id);
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
        workbuddy_screen_show_error(action->id);
        return;
    }

    char action_url[PROXY_ACTION_URL_MAX_LEN];
    snprintf(action_url, sizeof(action_url), "%s%s", base_url, action->path);

    memset(&proxy_response, 0, sizeof(proxy_response));
    esp_http_client_config_t config = {
        .url = action_url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .timeout_ms = PROXY_HTTP_TIMEOUT_MS,
        .user_data = &proxy_response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        workbuddy_screen_show_error(action->id);
        return;
    }

    ESP_LOGI(TAG, "GET fast proxy %s: %s", action->name, action_url);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Proxy HTTP status=%d, content_length=%lld",
                 status_code, esp_http_client_get_content_length(client));
        if (proxy_response.len > 0) {
            ESP_LOGI(TAG, "Proxy response collected: %s", proxy_response.text);
        }
        if (status_code >= 200 && status_code < 300 && proxy_response.len > 0) {
            workbuddy_screen_show_result_text(action->id, proxy_response.text);
        } else {
            workbuddy_screen_show_error(action->id);
        }
    } else {
        ESP_LOGE(TAG, "Proxy request failed: %s", esp_err_to_name(err));
        workbuddy_screen_show_error(action->id);
    }

    esp_http_client_cleanup(client);
}

static void trigger_task(void *arg)
{
    trigger_event_t event;
    while (1) {
        if (xQueueReceive(btn_queue, &event, portMAX_DELAY)) {
            const workbuddy_action_t *action = workbuddy_get_action(event.action_id);
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
            workbuddy_screen_show_querying(action->id);
            request_proxy_action(action);
        }
    }
}

void workbuddy_enqueue_trigger(const char *source, uint32_t source_id,
                               workbuddy_action_id_t action_id)
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
    btn_queue = xQueueCreate(1, sizeof(trigger_event_t));
    xTaskCreate(trigger_task, "trigger_task", 12288, NULL, 10, NULL);
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
    workbuddy_start_screen_ui();
    wifi_init_sta();
    xTaskCreate(proxy_warmup_task, "proxy_warmup", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Ready. Touch launcher apps for weather, schedule, and AI insight via fast proxy.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
