/*
 * lexin_face_auth.c
 *
 * Face-based login / unlock screen component. Uses the existing
 * lexin_vision (ESP-WHO face detector + preview buffer) to locate a
 * face, crops the face region, and posts it to the local proxy's
 * /face-recognize endpoint. The proxy computes a 64-bit average hash
 * and compares it against the registration database.
 *
 * When a face is recognised the component caches the user identity
 * in NVS so the login survives resets. Unknown faces trigger the
 * registration workflow (name typed on screen → posted to
 * /face-register).
 */

#include "lexin_face_auth.h"
#include "lexin_vision.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "face_auth";

#define FACE_AUTH_NVS_NS     "face_auth"
#define FACE_AUTH_NVS_KEY_ID "user_id"
#define FACE_AUTH_NVS_KEY_NM "user_name"

#define FACE_AUTH_STABLE_FRAMES 4     /* consecutive frames needed */
#define FACE_AUTH_AWAY_FRAMES   5     /* sustained no-face frames (~1s at 200ms)
                                       * required before a manual lock releases
                                       * its "wait for face to leave" guard.
                                       * A single dropped detection frame must
                                       * not re-arm auto-unlock while the user
                                       * is still sitting in front of camera. */
#define FACE_AUTH_UPLOAD_MS     200   /* min interval between uploads */
#define FACE_AUTH_FACE_MIN_W     48   /* ignore tiny faces */
#define FACE_AUTH_FACE_MIN_H     48
#define FACE_AUTH_PROXY_URL_MAX  128
#define FACE_AUTH_RESPONSE_MAX   1024
#define FACE_AUTH_DISCOVERY_TIMEOUT_MS 60
#define FACE_AUTH_UDP_DISCOVERY_PORT (CONFIG_LEXIN_PROXY_PORT + 1)
#define FACE_AUTH_DISCOVERY_MAX_HOSTS 254

typedef struct {
    lexin_face_auth_callback_t cb;
    void *user_data;
    SemaphoreHandle_t mutex;
    lexin_face_auth_snapshot_t snapshot;

    bool running;
    char proxy_url[FACE_AUTH_PROXY_URL_MAX];
    uint16_t *crop_buf;
    size_t crop_buf_pixels;

    /* Active user from NVS */
    char cached_user_id[32];
    char cached_user_name[64];

    /* Stable face tracking */
    uint16_t stable_face_x, stable_face_y, stable_face_w, stable_face_h;
    int stable_count;
    int64_t last_upload_ms;
    int64_t last_box_publish_ms;
    bool registration_pending;
    bool manual_lock_wait_away;
    int no_face_count;              /* consecutive frames with no valid face */
    char registration_name[64];
} face_auth_t;

static face_auth_t s_fa;

/* ------------------------------------------------------------------ */
/*  Snapshot helpers                                                   */
/* ------------------------------------------------------------------ */

static void fa_lock(void)
{
    if (s_fa.mutex) xSemaphoreTakeRecursive(s_fa.mutex, portMAX_DELAY);
}

static void fa_unlock(void)
{
    if (s_fa.mutex) xSemaphoreGiveRecursive(s_fa.mutex);
}

static void fa_publish(void)
{
    if (!s_fa.cb) return;
    lexin_face_auth_snapshot_t copy;
    fa_lock();
    copy = s_fa.snapshot;
    fa_unlock();
    s_fa.cb(&copy, s_fa.user_data);
}

static void fa_set_state(lexin_face_auth_state_t s, const char *status)
{
    fa_lock();
    s_fa.snapshot.state = s;
    s_fa.snapshot.updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (status) snprintf(s_fa.snapshot.status_text, sizeof(s_fa.snapshot.status_text),
                         "%s", status);
    fa_unlock();
    fa_publish();
}

/* ------------------------------------------------------------------ */
/*  Proxy discovery (lightweight copy)                                 */
/* ------------------------------------------------------------------ */

static bool fa_tcp_open(uint32_t addr)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) return false;
    struct timeval tv = { .tv_sec = 0, .tv_usec = FACE_AUTH_DISCOVERY_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(CONFIG_LEXIN_PROXY_PORT),
                             .sin_addr.s_addr = addr };
    bool ok = connect(sock, (struct sockaddr *)&d, sizeof(d)) == 0;
    close(sock);
    return ok;
}

static uint32_t fa_udp_discover(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) return 0;
    int en = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &en, sizeof(en));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 450 * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const char req[] = "LEXIN_DISCOVER_V1";
    struct sockaddr_in bc = { .sin_family = AF_INET,
                              .sin_port = htons(FACE_AUTH_UDP_DISCOVERY_PORT),
                              .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    sendto(sock, req, sizeof(req) - 1, 0, (struct sockaddr *)&bc, sizeof(bc));
    char reply[48] = {0}; struct sockaddr_in src = {0}; socklen_t sl = sizeof(src);
    int n = recvfrom(sock, reply, sizeof(reply) - 1, 0, (struct sockaddr *)&src, &sl);
    close(sock);
    return (n > 0 && strncmp(reply, "LEXIN_PROXY_V1", 14) == 0) ? src.sin_addr.s_addr : 0;
}

static void fa_set_proxy_url(uint32_t addr)
{
    snprintf(s_fa.proxy_url, sizeof(s_fa.proxy_url), "http://" IPSTR ":%d",
             IP2STR((esp_ip4_addr_t *)&addr), CONFIG_LEXIN_PROXY_PORT);
    ESP_LOGI(TAG, "Face auth proxy: %s", s_fa.proxy_url);
}

static const char *fa_ensure_proxy(void)
{
    if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") != 0 &&
        strlen(CONFIG_LEXIN_PROXY_BASE_URL) > 0)
        return CONFIG_LEXIN_PROXY_BASE_URL;
    if (s_fa.proxy_url[0]) {
        const char *host = strstr(s_fa.proxy_url, "://");
        host = host ? host + 3 : s_fa.proxy_url;
        char host_only[32] = {0};
        size_t host_len = 0;
        while (host[host_len] != '\0' && host[host_len] != ':' &&
               host_len + 1 < sizeof(host_only)) {
            host_only[host_len] = host[host_len];
            host_len++;
        }
        struct in_addr cached;
        if (inet_aton(host_only, &cached) && fa_tcp_open(cached.s_addr)) {
            return s_fa.proxy_url;
        }
        ESP_LOGW(TAG, "Cached face auth proxy is stale: %s", s_fa.proxy_url);
        s_fa.proxy_url[0] = '\0';
    }

    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK ||
        ip.ip.addr == 0) {
        ESP_LOGW(TAG, "Face auth waits for WiFi IP before proxy discovery");
        return NULL;
    }

    if (strlen(CONFIG_LEXIN_PROXY_PREFERRED_HOST) > 0) {
        struct in_addr a;
        if (inet_aton(CONFIG_LEXIN_PROXY_PREFERRED_HOST, &a) && fa_tcp_open(a.s_addr)) {
            fa_set_proxy_url(a.s_addr);
            return s_fa.proxy_url;
        }
    }
    uint32_t ann = fa_udp_discover();
    if (ann && fa_tcp_open(ann)) {
        fa_set_proxy_url(ann);
        return s_fa.proxy_url;
    }
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.gw.addr &&
        ip.gw.addr != ip.ip.addr && fa_tcp_open(ip.gw.addr)) {
        fa_set_proxy_url(ip.gw.addr);
        return s_fa.proxy_url;
    }

    uint32_t host_ip = ntohl(ip.ip.addr);
    uint32_t network = host_ip & 0xffffff00UL;
    uint32_t self_host = host_ip & 0xffUL;
    ESP_LOGI(TAG, "Scanning subnet for face auth proxy near .%lu",
             (unsigned long)self_host);
    for (uint32_t distance = 1; distance <= FACE_AUTH_DISCOVERY_MAX_HOSTS; distance++) {
        int32_t candidates[2] = {
            (int32_t)self_host - (int32_t)distance,
            (int32_t)self_host + (int32_t)distance,
        };
        for (size_t i = 0; i < 2; i++) {
            int32_t h = candidates[i];
            if (h <= 0 || h >= 255) {
                continue;
            }
            uint32_t addr = htonl(network | (uint32_t)h);
            if (fa_tcp_open(addr)) {
                fa_set_proxy_url(addr);
                return s_fa.proxy_url;
            }
        }
    }

    ESP_LOGW(TAG, "Face auth proxy not found on this WiFi");
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  JSON helpers (minimal, copied from lexin_voice)                    */
/* ------------------------------------------------------------------ */

static bool fa_json_str(const char *json, const char *key, char *out, size_t out_sz)
{
    if (!out_sz) return false;
    out[0] = 0;
    if (!json || !key) return false;
    char n[64]; int nn = snprintf(n, sizeof(n), "\"%s\"", key);
    if (nn <= 0 || (size_t)nn >= sizeof(n)) return false;
    const char *p = strstr(json, n);
    if (!p) return false;
    p += nn;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) p++;
        if (i+1 < out_sz) out[i++] = *p;
        p++;
    }
    out[i] = 0;
    return true;
}

static bool fa_json_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key) return false;
    char n[64]; int nn = snprintf(n, sizeof(n), "\"%s\"", key);
    if (nn <= 0 || (size_t)nn >= sizeof(n)) return false;
    const char *p = strstr(json, n);
    if (!p) return false;
    p += nn; while (*p == ' ' || *p == '\t' || *p == ':') p++;
    *out = (strncmp(p, "true", 4) == 0);
    return true;
}

/* ------------------------------------------------------------------ */
/*  HTTP upload                                                        */
/* ------------------------------------------------------------------ */

typedef struct { char buf[FACE_AUTH_RESPONSE_MAX]; size_t len; } fa_http_buf_t;

static esp_err_t fa_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        fa_http_buf_t *b = (fa_http_buf_t *)evt->user_data;
        if (b && b->len < sizeof(b->buf) - 1) {
            size_t take = evt->data_len;
            if (b->len + take >= sizeof(b->buf)) take = sizeof(b->buf) - 1 - b->len;
            memcpy(b->buf + b->len, evt->data, take);
            b->len += take;
            b->buf[b->len] = 0;
        }
    }
    return ESP_OK;
}

static bool fa_post_face(const char *url, const char *path,
                          const uint16_t *pixels, int w, int h,
                          const char *name, char *resp, size_t resp_sz)
{
    if (!url || !pixels || w <= 0 || h <= 0) return false;
    char full[192];
    snprintf(full, sizeof(full), "%s%s", url, path);

    fa_http_buf_t rb = {0};
    esp_http_client_config_t cfg = {
        .url = full, .method = HTTP_METHOD_POST,
        .event_handler = fa_http_event, .timeout_ms = 8000, .user_data = &rb,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    char wh[12]; snprintf(wh, sizeof(wh), "%d", w);
    char hh[12]; snprintf(hh, sizeof(hh), "%d", h);
    esp_http_client_set_header(cli, "content-type", "application/octet-stream");
    esp_http_client_set_header(cli, "x-face-width", wh);
    esp_http_client_set_header(cli, "x-face-height", hh);
    if (name && name[0]) esp_http_client_set_header(cli, "x-face-name", name);
    esp_http_client_set_post_field(cli, (const char *)pixels, w * h * 2);

    esp_err_t err = esp_http_client_perform(cli);
    int st = (err == ESP_OK) ? esp_http_client_get_status_code(cli) : 0;
    bool ok = (err == ESP_OK && st >= 200 && st < 300);
    if (!ok) {
        ESP_LOGW(TAG, "Face POST %s failed: err=%s status=%d", path, esp_err_to_name(err), st);
    }
    if (ok && rb.len > 0 && resp && resp_sz > 0) {
        size_t cp = rb.len < resp_sz - 1 ? rb.len : resp_sz - 1;
        memcpy(resp, rb.buf, cp); resp[cp] = 0;
    }
    esp_http_client_cleanup(cli);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Face cropping                                                      */
/* ------------------------------------------------------------------ */

static bool fa_crop_face(const lexin_vision_snapshot_t *vs)
{
    if (!vs->face_detected || vs->face_width < FACE_AUTH_FACE_MIN_W ||
        vs->face_height < FACE_AUTH_FACE_MIN_H)
        return false;

    /* Map face coordinates from input resolution to preview resolution */
    int fx = (vs->face_x * LEXIN_VISION_PREVIEW_WIDTH) / vs->input_width;
    int fy = (vs->face_y * LEXIN_VISION_PREVIEW_HEIGHT) / vs->input_height;
    int fw = (vs->face_width * LEXIN_VISION_PREVIEW_WIDTH) / vs->input_width;
    int fh = (vs->face_height * LEXIN_VISION_PREVIEW_HEIGHT) / vs->input_height;

    /* Clamp */
    if (fx < 0) { fw += fx; fx = 0; }
    if (fy < 0) { fh += fy; fy = 0; }
    if (fx + fw > LEXIN_VISION_PREVIEW_WIDTH)  fw = LEXIN_VISION_PREVIEW_WIDTH  - fx;
    if (fy + fh > LEXIN_VISION_PREVIEW_HEIGHT) fh = LEXIN_VISION_PREVIEW_HEIGHT - fy;
    if (fw < FACE_AUTH_FACE_MIN_W || fh < FACE_AUTH_FACE_MIN_H) return false;

    /* Read preview pixels */
    uint16_t *prev = heap_caps_malloc(LEXIN_VISION_PREVIEW_WIDTH *
                                       LEXIN_VISION_PREVIEW_HEIGHT * 2,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!prev) prev = malloc(LEXIN_VISION_PREVIEW_WIDTH *
                              LEXIN_VISION_PREVIEW_HEIGHT * 2);
    if (!prev) return false;

    uint32_t fid;
    esp_err_t r = lexin_vision_copy_preview(prev, LEXIN_VISION_PREVIEW_WIDTH *
                                                   LEXIN_VISION_PREVIEW_HEIGHT, &fid);
    if (r != ESP_OK) { free(prev); return false; }

    /* Allocate crop buffer if needed */
    size_t np = (size_t)fw * fh;
    if (!s_fa.crop_buf || s_fa.crop_buf_pixels < np) {
        if (s_fa.crop_buf) free(s_fa.crop_buf);
        s_fa.crop_buf = heap_caps_malloc(np * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_fa.crop_buf) s_fa.crop_buf = malloc(np * 2);
        if (!s_fa.crop_buf) { free(prev); return false; }
        s_fa.crop_buf_pixels = np;
    }

    /* Copy row by row */
    uint16_t *dst = s_fa.crop_buf;
    for (int row = 0; row < fh; row++) {
        memcpy(dst + row * fw, prev + (fy + row) * LEXIN_VISION_PREVIEW_WIDTH + fx,
               fw * sizeof(uint16_t));
    }
    free(prev);

    /* Update snapshot with face coords (in preview space) */
    fa_lock();
    s_fa.snapshot.face_x = (uint16_t)fx; s_fa.snapshot.face_y = (uint16_t)fy;
    s_fa.snapshot.face_w = (uint16_t)fw; s_fa.snapshot.face_h = (uint16_t)fh;
    s_fa.snapshot.confidence = vs->confidence;
    fa_unlock();
    return true;
}

/* ------------------------------------------------------------------ */
/*  NVS helpers                                                        */
/* ------------------------------------------------------------------ */

static void fa_nvs_save_user(const char *uid, const char *uname)
{
    nvs_handle_t h;
    if (nvs_open(FACE_AUTH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, FACE_AUTH_NVS_KEY_ID, uid);
    nvs_set_str(h, FACE_AUTH_NVS_KEY_NM, uname);
    nvs_commit(h);
    nvs_close(h);
    snprintf(s_fa.cached_user_id, sizeof(s_fa.cached_user_id), "%s", uid ? uid : "");
    snprintf(s_fa.cached_user_name, sizeof(s_fa.cached_user_name), "%s", uname ? uname : "");
}

static void fa_nvs_load_user(void)
{
    nvs_handle_t h;
    if (nvs_open(FACE_AUTH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_fa.cached_user_id);
    nvs_get_str(h, FACE_AUTH_NVS_KEY_ID, s_fa.cached_user_id, &sz);
    sz = sizeof(s_fa.cached_user_name);
    nvs_get_str(h, FACE_AUTH_NVS_KEY_NM, s_fa.cached_user_name, &sz);
    nvs_close(h);
}

static void fa_clear_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(FACE_AUTH_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, FACE_AUTH_NVS_KEY_ID);
        nvs_erase_key(h, FACE_AUTH_NVS_KEY_NM);
        nvs_commit(h); nvs_close(h);
    }
    s_fa.cached_user_id[0] = 0;
    s_fa.cached_user_name[0] = 0;
}

/* ------------------------------------------------------------------ */
/*  Task                                                               */
/* ------------------------------------------------------------------ */

static void fa_task(void *arg)
{
    (void)arg;
    /* Load cached user. Even if cached, we still scan on start so the
     * face is re-verified. If no face, we fall back to the cached id
     * after a timeout. */
    fa_nvs_load_user();

    fa_set_state(LEXIN_FACE_AUTH_IDLE, "CAMERA STARTING");

    while (s_fa.running) {
        lexin_vision_snapshot_t vs;
        lexin_vision_get_snapshot(&vs);

        if (!vs.service_ready || !vs.camera_ready) {
            fa_set_state(LEXIN_FACE_AUTH_IDLE, "CAMERA WAIT");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool has_face = vs.face_detected &&
            vs.face_width >= FACE_AUTH_FACE_MIN_W &&
            vs.face_height >= FACE_AUTH_FACE_MIN_H;

        fa_lock();
        bool face_changed = (s_fa.stable_face_w != vs.face_width ||
                             s_fa.stable_face_h != vs.face_height);
        fa_unlock();

        if (!has_face) {
            /* Reset stability counter */
            fa_lock();
            s_fa.stable_count = 0;
            s_fa.stable_face_w = 0;
            s_fa.snapshot.face_detected = false;
            s_fa.snapshot.face_x = 0;
            s_fa.snapshot.face_y = 0;
            s_fa.snapshot.face_w = 0;
            s_fa.snapshot.face_h = 0;
            s_fa.snapshot.confidence = 0;
            /* Only release the manual-lock guard after the face has been
             * genuinely absent for several consecutive frames. A single
             * dropped detection frame (the detector flickers constantly)
             * must not re-arm auto-unlock while the user is still present,
             * otherwise the lock screen bounces back to the launcher within
             * a second or two without a deliberate re-scan. */
            if (s_fa.manual_lock_wait_away) {
                if (++s_fa.no_face_count >= FACE_AUTH_AWAY_FRAMES) {
                    s_fa.manual_lock_wait_away = false;
                    s_fa.no_face_count = 0;
                }
            }
            fa_unlock();
            fa_publish();
            if (s_fa.snapshot.state != LEXIN_FACE_AUTH_RECOGNIZED &&
                s_fa.snapshot.state != LEXIN_FACE_AUTH_REGISTERED &&
                s_fa.snapshot.state != LEXIN_FACE_AUTH_UNKNOWN) {
                fa_set_state(LEXIN_FACE_AUTH_SCANNING, "SEARCH_FACE");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* Face detected — track stability */
        fa_lock();
        s_fa.no_face_count = 0;   /* AWAY guard counts consecutive no-face frames */
        s_fa.snapshot.face_detected = true;
        s_fa.snapshot.confidence = vs.confidence;
        if (vs.input_width > 0 && vs.input_height > 0) {
            s_fa.snapshot.face_x = (uint16_t)((vs.face_x * LEXIN_VISION_PREVIEW_WIDTH) / vs.input_width);
            s_fa.snapshot.face_y = (uint16_t)((vs.face_y * LEXIN_VISION_PREVIEW_HEIGHT) / vs.input_height);
            s_fa.snapshot.face_w = (uint16_t)((vs.face_width * LEXIN_VISION_PREVIEW_WIDTH) / vs.input_width);
            s_fa.snapshot.face_h = (uint16_t)((vs.face_height * LEXIN_VISION_PREVIEW_HEIGHT) / vs.input_height);
        } else {
            s_fa.snapshot.face_x = vs.face_x;
            s_fa.snapshot.face_y = vs.face_y;
            s_fa.snapshot.face_w = vs.face_width;
            s_fa.snapshot.face_h = vs.face_height;
        }
        if (face_changed) {
            s_fa.stable_count = 1;
            s_fa.stable_face_x = vs.face_x; s_fa.stable_face_y = vs.face_y;
            s_fa.stable_face_w = vs.face_width; s_fa.stable_face_h = vs.face_height;
        } else {
            s_fa.stable_count++;
        }
        int stable = s_fa.stable_count;
        bool is_reg = s_fa.registration_pending;
        bool wait_away = s_fa.manual_lock_wait_away;
        int64_t ui_now = esp_timer_get_time() / 1000;
        bool publish_box = ui_now - s_fa.last_box_publish_ms >= 250;
        if (publish_box) {
            s_fa.last_box_publish_ms = ui_now;
        }
        fa_unlock();
        if (publish_box) {
            fa_publish();
        }

        if (wait_away) {
            fa_set_state(LEXIN_FACE_AUTH_SCANNING, "STEP_AWAY");
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (is_reg || stable >= FACE_AUTH_STABLE_FRAMES) {
            int64_t now = esp_timer_get_time() / 1000;
            fa_lock();
            int64_t last = s_fa.last_upload_ms;
            fa_unlock();
            if (!is_reg && now - last < FACE_AUTH_UPLOAD_MS) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            fa_set_state(LEXIN_FACE_AUTH_DETECTED, "FACE_DETECTED");

            /* Crop face */
            if (!fa_crop_face(&vs)) {
                fa_set_state(LEXIN_FACE_AUTH_SCANNING, "CROP_FAILED");
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }

            const char *proxy = fa_ensure_proxy();
            if (!proxy) {
                fa_set_state(LEXIN_FACE_AUTH_SCANNING, "WAIT_WIFI_PROXY");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            char resp[FACE_AUTH_RESPONSE_MAX] = {0};
            bool ok = false;

            if (is_reg) {
                /* Registration path */
                fa_set_state(LEXIN_FACE_AUTH_REGISTERING, "REGISTERING");
                char name_buf[64];
                fa_lock();
                snprintf(name_buf, sizeof(name_buf), "%s",
                         s_fa.registration_name[0] ?
                         s_fa.registration_name : s_fa.snapshot.user_name);
                fa_unlock();
                if (!name_buf[0]) {
                    ESP_LOGW(TAG, "Face registration skipped: missing pending user name");
                    fa_set_state(LEXIN_FACE_AUTH_ERROR, "REGISTER_NAME_EMPTY");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    continue;
                }
                ESP_LOGI(TAG, "Uploading face registration for user: %s", name_buf);
                ok = fa_post_face(proxy, "/face-register", s_fa.crop_buf,
                                  (int)s_fa.snapshot.face_w, (int)s_fa.snapshot.face_h,
                                  name_buf, resp, sizeof(resp));
                if (ok) {
                    char uid[32] = {0}, un[64] = {0};
                    fa_json_str(resp, "user_id", uid, sizeof(uid));
                    fa_json_str(resp, "user_name", un, sizeof(un));
                    if (!uid[0]) snprintf(uid, sizeof(uid), "%s", "local");
                    if (!un[0]) snprintf(un, sizeof(un), "%s", name_buf);
                    fa_nvs_save_user(uid, un);
                    fa_lock();
                    snprintf(s_fa.snapshot.user_id, sizeof(s_fa.snapshot.user_id), "%s", uid);
                    snprintf(s_fa.snapshot.user_name, sizeof(s_fa.snapshot.user_name), "%s", un);
                    s_fa.snapshot.recognized = true;
                    s_fa.registration_pending = false;
                    s_fa.registration_name[0] = 0;
                    fa_unlock();
                    fa_set_state(LEXIN_FACE_AUTH_REGISTERED, "REGISTERED");
                    ESP_LOGI(TAG, "User registered: %s (%s)", un, uid);
                } else {
                    ESP_LOGW(TAG, "Face registration upload failed; will retry");
                    fa_set_state(LEXIN_FACE_AUTH_ERROR, "REGISTER_FAILED");
                }
            } else {
                /* Recognition path */
                ok = fa_post_face(proxy, "/face-recognize", s_fa.crop_buf,
                                  (int)s_fa.snapshot.face_w, (int)s_fa.snapshot.face_h,
                                  NULL, resp, sizeof(resp));
                if (ok) {
                    bool rec = false;
                    fa_json_bool(resp, "recognized", &rec);
                    if (rec) {
                        /* A manual lock may have happened while this HTTP
                         * round-trip was in flight (fa_post_face blocks this
                         * task). Applying a stale result would instantly
                         * re-unlock the device right after the user pressed
                         * Lock, so discard it and keep waiting for the face
                         * to leave. */
                        fa_lock();
                        bool stale = s_fa.manual_lock_wait_away;
                        fa_unlock();
                        if (stale) {
                            ESP_LOGI(TAG, "Recognition result discarded: manual lock during upload");
                            fa_set_state(LEXIN_FACE_AUTH_SCANNING, "STEP_AWAY");
                            vTaskDelay(pdMS_TO_TICKS(250));
                            continue;
                        }
                        char uid[32] = {0}, un[64] = {0};
                        fa_json_str(resp, "user_id", uid, sizeof(uid));
                        fa_json_str(resp, "user_name", un, sizeof(un));
                        fa_nvs_save_user(uid, un);
                        fa_lock();
                        snprintf(s_fa.snapshot.user_id, sizeof(s_fa.snapshot.user_id), "%s", uid);
                        snprintf(s_fa.snapshot.user_name, sizeof(s_fa.snapshot.user_name), "%s", un);
                        s_fa.snapshot.recognized = true;
                        fa_unlock();
                        char st[96];
                        snprintf(st, sizeof(st), "欢迎回来 %s", un[0] ? un : "用户");
                        fa_set_state(LEXIN_FACE_AUTH_RECOGNIZED, st);
                        ESP_LOGI(TAG, "Recognized: %s (%s)", un, uid);
                    } else {
                        bool was_logged_in = false;
                        fa_lock();
                        was_logged_in = s_fa.snapshot.recognized;
                        if (!was_logged_in) {
                            s_fa.snapshot.recognized = false;
                            s_fa.snapshot.user_id[0] = 0;
                            if (!s_fa.registration_pending) {
                                s_fa.snapshot.user_name[0] = 0;
                            }
                        }
                        fa_unlock();
                        if (!was_logged_in) {
                        fa_set_state(LEXIN_FACE_AUTH_UNKNOWN, "未识别到用户");
                        }
                        ESP_LOGI(TAG, "Unknown face%s",
                                 was_logged_in ? " ignored while logged in" : "");
                    }
                } else {
                    fa_set_state(LEXIN_FACE_AUTH_ERROR, "识别请求失败");
                }
            }

            fa_lock();
            s_fa.last_upload_ms = now;
            fa_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (s_fa.crop_buf) { free(s_fa.crop_buf); s_fa.crop_buf = NULL; }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t lexin_face_auth_start(lexin_face_auth_callback_t cb, void *user_data)
{
    if (s_fa.running) return ESP_OK;
    memset(&s_fa, 0, sizeof(s_fa));
    s_fa.cb = cb;
    s_fa.user_data = user_data;
    s_fa.mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_fa.mutex) return ESP_ERR_NO_MEM;
    fa_nvs_load_user();
    s_fa.snapshot.state = LEXIN_FACE_AUTH_IDLE;
    s_fa.running = true;
    if (xTaskCreatePinnedToCore(fa_task, "face_auth", 8192, NULL, 5, NULL, 1) != pdPASS) {
        s_fa.running = false;
        vSemaphoreDelete(s_fa.mutex);
        s_fa.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void lexin_face_auth_get_snapshot(lexin_face_auth_snapshot_t *s)
{
    if (!s) return;
    if (!s_fa.mutex) { memset(s, 0, sizeof(*s)); return; }
    fa_lock();
    *s = s_fa.snapshot;
    fa_unlock();
}

esp_err_t lexin_face_auth_register(const char *user_name)
{
    if (!user_name || !user_name[0]) return ESP_ERR_INVALID_ARG;
    fa_lock();
    s_fa.snapshot.user_id[0] = 0;
    snprintf(s_fa.snapshot.user_name, sizeof(s_fa.snapshot.user_name), "%s", user_name);
    s_fa.snapshot.recognized = false;
    s_fa.snapshot.state = LEXIN_FACE_AUTH_REGISTERING;
    snprintf(s_fa.snapshot.status_text, sizeof(s_fa.snapshot.status_text), "%s", "REGISTERING");
    s_fa.snapshot.updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_fa.registration_pending = true;
    snprintf(s_fa.registration_name, sizeof(s_fa.registration_name), "%s", user_name);
    s_fa.stable_count = FACE_AUTH_STABLE_FRAMES; /* trigger upload immediately */
    fa_unlock();
    fa_publish();
    ESP_LOGI(TAG, "Face registration queued for upload: %s", user_name);
    return ESP_OK;
}

esp_err_t lexin_face_auth_login_by_id(const char *user_id, const char *user_name)
{
    if (!user_id || !user_id[0]) return ESP_ERR_INVALID_ARG;
    fa_nvs_save_user(user_id, user_name ? user_name : "");
    fa_lock();
    snprintf(s_fa.snapshot.user_id, sizeof(s_fa.snapshot.user_id), "%s", user_id);
    snprintf(s_fa.snapshot.user_name, sizeof(s_fa.snapshot.user_name), "%s",
             user_name ? user_name : "");
    s_fa.snapshot.recognized = true;
    s_fa.snapshot.state = LEXIN_FACE_AUTH_RECOGNIZED;
    fa_unlock();
    fa_publish();
    return ESP_OK;
}

bool lexin_face_auth_is_logged_in(void)
{
    if (!s_fa.mutex) return false;
    fa_lock();
    bool ok = s_fa.snapshot.recognized;
    fa_unlock();
    return ok;
}

void lexin_face_auth_lock(void)
{
    if (!s_fa.mutex) return;
    fa_lock();
    s_fa.snapshot.recognized = false;
    s_fa.snapshot.user_id[0] = 0;
    s_fa.snapshot.user_name[0] = 0;
    s_fa.snapshot.state = LEXIN_FACE_AUTH_SCANNING;
    snprintf(s_fa.snapshot.status_text, sizeof(s_fa.snapshot.status_text),
             "%s", "LOCKED");
    s_fa.snapshot.updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_fa.registration_pending = false;
    s_fa.registration_name[0] = 0;
    s_fa.manual_lock_wait_away = true;
    s_fa.no_face_count = 0;
    fa_unlock();
    fa_publish();
    ESP_LOGI(TAG, "Manual lock: waiting for face to leave before unlock resumes");
}

const char *lexin_face_auth_current_user_id(void)
{
    return s_fa.cached_user_id[0] ? s_fa.cached_user_id : NULL;
}

const char *lexin_face_auth_current_user_name(void)
{
    return s_fa.cached_user_name[0] ? s_fa.cached_user_name : NULL;
}
