/*
 * lexin_voice.c
 *
 * Lightweight voice input for the LeXin pet. Captures mono PCM
 * from the on-board ES8311 microphone (via the BSP codec handle),
 * segments the audio by a simple energy gate, and streams each
 * utterance to the local proxy through HTTP chunked upload. The proxy
 * runs FunASR for wake-word + speech recognition, then routes the
 * question to either a hand-written rule table or the DeepSeek API.
 *
 * This component intentionally avoids espressif/esp-sr to keep the
 * build small, the boot path stable, and to leave room for the camera
 * and edge advisor already on the device. It is structured so an
 * ESP-SR WakeNet path can be slotted in later without changing the
 * snapshot or callback contract.
 *
 * Design constraints from the rest of the project:
 *   - The 16 kHz mono PCM input is read from the I2S channel owned by
 *     the BSP; we do not own the I2S driver. The BSP exposes
 *     bsp_audio_codec_microphone_init() which internally configures
 *     the I2S RX channel for duplex mono at the requested rate.
 *   - HTTP uploads must reuse the same proxy discovery path as
 *     lexin_main.c (UDP discovery + gateway + scanning) so the user
 *     only has to start tools/lexin_proxy.js.
 *   - The voice task must not block the LVGL UI. All snapshot updates
 *     are posted through a FreeRTOS task that wakes the UI callback.
 */

#include "lexin_voice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "lexin_voice";

/* Tunable constants come from CMakeLists.txt but we re-state the
 * minimum set here as documentation for future readers. */
#ifndef LEXIN_VOICE_SAMPLE_RATE
#define LEXIN_VOICE_SAMPLE_RATE 22050
#endif
#ifndef LEXIN_VOICE_FRAME_MS
#define LEXIN_VOICE_FRAME_MS 30
#endif
#ifndef LEXIN_VOICE_RMS_THRESHOLD
#define LEXIN_VOICE_RMS_THRESHOLD 80
#endif
#ifndef LEXIN_VOICE_SILENCE_MS
#define LEXIN_VOICE_SILENCE_MS 900
#endif
#ifndef LEXIN_VOICE_MAX_UTTER_MS
#define LEXIN_VOICE_MAX_UTTER_MS 6000
#endif
#ifndef LEXIN_VOICE_COOLDOWN_MS
#define LEXIN_VOICE_COOLDOWN_MS 1500
#endif

#define LEXIN_VOICE_PROXY_URL_MAX 128
#define LEXIN_VOICE_PROXY_HOST_MAX 32
#define LEXIN_VOICE_MAX_UTTER_FRAMES                                        \
    ((LEXIN_VOICE_SAMPLE_RATE * LEXIN_VOICE_MAX_UTTER_MS) / 1000)
#define LEXIN_VOICE_FRAME_SAMPLES                                           \
    ((LEXIN_VOICE_SAMPLE_RATE * LEXIN_VOICE_FRAME_MS) / 1000)
#define LEXIN_VOICE_DISCOVERY_TIMEOUT_MS 60
#define LEXIN_VOICE_UDP_DISCOVERY_PORT (CONFIG_LEXIN_PROXY_PORT + 1)
#define LEXIN_VOICE_DISCOVERY_MAX_HOSTS 254
#define LEXIN_VOICE_HTTP_TIMEOUT_MS 6000

/* Snapshot of the world for the voice service. Mirrors
 * lexin_vision_snapshot_t in spirit: small, copy-by-value, refreshed
 * from one task, read by many. */
typedef struct {
    lexin_voice_callback_t callback;
    void *user_data;
    SemaphoreHandle_t mutex;
    lexin_voice_snapshot_t snapshot;

    esp_codec_dev_handle_t mic;
    bool mic_ready;
    bool running;

    /* Discovery state copied from lexin_main. The voice task does its
     * own discovery so the two paths do not stomp on each other. */
    char proxy_url[LEXIN_VOICE_PROXY_URL_MAX];

    /* Sent as request headers so the proxy can key the conversation per
     * user and switch between chat and daily-plan capture. */
    char user_id[48];
    char mode[16];
} voice_service_t;

static voice_service_t s_voice;
static TaskHandle_t s_voice_task;
static uint32_t s_last_rms_publish_ms;
static uint32_t s_last_rms_log_ms;

/* Forward declarations for proxy HTTP reply handling. */
static void proxy_reply_dispatch(const char *json, size_t len);
static void set_state(lexin_voice_state_t state, const char *status);

/* ------------------------------------------------------------------ */
/*  Tiny JSON helpers                                                  */
/* ------------------------------------------------------------------ */

/* Copy a JSON string field bounded by "key":"..." into out. Only used
 * for the short, single-line replies we generate on the device, so we
 * keep it deliberately strict and skip embedded escapes. */
static bool copy_json_string(const char *json, const char *key, char *out,
                                size_t out_size)
{
    if (out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (json == NULL || key == NULL) {
        return false;
    }
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) {
        return false;
    }
    const char *p = strstr(json, needle);
    if (p == NULL) {
        return false;
    }
    p += n;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
        }
        if (i + 1 < out_size) {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return true;
}

static bool copy_json_int(const char *json, const char *key, int *out)
{
    if (json == NULL || key == NULL) {
        return false;
    }
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) {
        return false;
    }
    const char *p = strstr(json, needle);
    if (p == NULL) {
        return false;
    }
    p += n;
    while (*p == ' ' || *p == '\t' || *p == ':') {
        p++;
    }
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    }
    int value = 0;
    bool seen = false;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
        seen = true;
    }
    if (!seen) {
        return false;
    }
    *out = sign * value;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Snapshot helpers                                                   */
/* ------------------------------------------------------------------ */

static void snapshot_lock(voice_service_t *svc)
{
    if (svc->mutex) {
        xSemaphoreTakeRecursive(svc->mutex, portMAX_DELAY);
    }
}

static void snapshot_unlock(voice_service_t *svc)
{
    if (svc->mutex) {
        xSemaphoreGiveRecursive(svc->mutex);
    }
}

static void publish_snapshot(voice_service_t *svc)
{
    if (svc->callback == NULL) {
        return;
    }
    lexin_voice_snapshot_t copy;
    snapshot_lock(svc);
    copy = svc->snapshot;
    snapshot_unlock(svc);
    svc->callback(&copy, svc->user_data);
}

static void set_state(lexin_voice_state_t state, const char *status)
{
    if (!s_voice.mutex) {
        return;
    }
    snapshot_lock(&s_voice);
    s_voice.snapshot.state = state;
    s_voice.snapshot.updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (status != NULL) {
        snprintf(s_voice.snapshot.status, sizeof(s_voice.snapshot.status),
                 "%s", status);
    }
    snapshot_unlock(&s_voice);
    publish_snapshot(&s_voice);
}

static void set_rms(uint16_t rms)
{
    if (!s_voice.mutex) {
        return;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    snapshot_lock(&s_voice);
    s_voice.snapshot.rms = rms;
    s_voice.snapshot.updated_at_ms = now_ms;
    snapshot_unlock(&s_voice);
    if ((now_ms - s_last_rms_publish_ms) >= 250) {
        s_last_rms_publish_ms = now_ms;
        publish_snapshot(&s_voice);
    }
}

/* ------------------------------------------------------------------ */
/*  Proxy discovery (mirrors lexin_main.c but kept independent)        */
/* ------------------------------------------------------------------ */

static bool tcp_port_open(uint32_t host_addr, uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        return false;
    }
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = LEXIN_VOICE_DISCOVERY_TIMEOUT_MS * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = host_addr,
    };
    bool ok = connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0;
    close(sock);
    return ok;
}

static uint32_t udp_discover(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        return 0;
    }
    int enabled = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
    struct timeval tv = {
        .tv_sec = 0,
        .tv_usec = 450 * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const char req[] = "LEXIN_DISCOVER_V1";
    struct sockaddr_in bc = {
        .sin_family = AF_INET,
        .sin_port = htons(LEXIN_VOICE_UDP_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(sock, req, sizeof(req) - 1, 0, (struct sockaddr *)&bc, sizeof(bc));
    char reply[48] = {0};
    struct sockaddr_in source = {0};
    socklen_t slen = sizeof(source);
    int n = recvfrom(sock, reply, sizeof(reply) - 1, 0,
                        (struct sockaddr *)&source, &slen);
    close(sock);
    if (n <= 0 || strncmp(reply, "LEXIN_PROXY_V1", 14) != 0) {
        return 0;
    }
    return source.sin_addr.s_addr;
}

static const char *ensure_proxy_url(void)
{
    if (strcmp(CONFIG_LEXIN_PROXY_BASE_URL, "auto") != 0 &&
        strlen(CONFIG_LEXIN_PROXY_BASE_URL) > 0) {
        return CONFIG_LEXIN_PROXY_BASE_URL;
    }
    if (s_voice.proxy_url[0] != '\0') {
        return s_voice.proxy_url;
    }

    /* Preferred host (static IP) takes priority. */
    if (strlen(CONFIG_LEXIN_PROXY_PREFERRED_HOST) > 0) {
        struct in_addr addr;
        if (inet_aton(CONFIG_LEXIN_PROXY_PREFERRED_HOST, &addr) != 0 &&
            tcp_port_open(addr.s_addr, CONFIG_LEXIN_PROXY_PORT)) {
            snprintf(s_voice.proxy_url, sizeof(s_voice.proxy_url),
                     "http://" IPSTR ":%d", IP2STR((esp_ip4_addr_t *)&addr.s_addr),
                     CONFIG_LEXIN_PROXY_PORT);
            return s_voice.proxy_url;
        }
    }

    uint32_t announced = udp_discover();
    if (announced != 0 && tcp_port_open(announced, CONFIG_LEXIN_PROXY_PORT)) {
        snprintf(s_voice.proxy_url, sizeof(s_voice.proxy_url),
                 "http://" IPSTR ":%d", IP2STR((esp_ip4_addr_t *)&announced),
                 CONFIG_LEXIN_PROXY_PORT);
        return s_voice.proxy_url;
    }

    /* Fall back to gateway. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    bool have_ip = netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
                   ip.ip.addr != 0;
    if (have_ip &&
        ip.gw.addr != 0 && ip.gw.addr != ip.ip.addr &&
        tcp_port_open(ip.gw.addr, CONFIG_LEXIN_PROXY_PORT)) {
        snprintf(s_voice.proxy_url, sizeof(s_voice.proxy_url),
                 "http://" IPSTR ":%d", IP2STR((esp_ip4_addr_t *)&ip.gw.addr),
                 CONFIG_LEXIN_PROXY_PORT);
        return s_voice.proxy_url;
    }

    /* Last resort: walk the local /24. Voice uses its own lightweight
     * discovery path, so keep the scan range as wide as the main proxy
     * discovery. A narrow scan can miss the PC and leave the UI stuck in
     * UPLOAD even though weather/calendar already work. */
    if (!have_ip) {
        return NULL;
    }
    uint32_t ip_addr = ntohl(ip.ip.addr);
    uint32_t network = ip_addr & 0xffffff00UL;
    uint8_t self = (uint8_t)(ip_addr & 0xffUL);
    for (int d = 1; d <= LEXIN_VOICE_DISCOVERY_MAX_HOSTS; d++) {
        int cands[2] = {self - d, self + d};
        for (int i = 0; i < 2; i++) {
            int off = cands[i];
            if (off <= 0 || off >= 255) {
                continue;
            }
            uint32_t addr = htonl(network | (uint32_t)off);
            if (tcp_port_open(addr, CONFIG_LEXIN_PROXY_PORT)) {
                snprintf(s_voice.proxy_url, sizeof(s_voice.proxy_url),
                         "http://" IPSTR ":%d", IP2STR((esp_ip4_addr_t *)&addr),
                         CONFIG_LEXIN_PROXY_PORT);
                return s_voice.proxy_url;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  WAV upload (single HTTP request)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} http_buf_t;

static esp_err_t upload_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        http_buf_t *b = (http_buf_t *)evt->user_data;
        if (b && b->len < b->cap) {
            size_t take = evt->data_len;
            if (b->len + take > b->cap) {
                take = b->cap - b->len;
            }
            memcpy(b->buf + b->len, evt->data, take);
            b->len += take;
        }
    }
    return ESP_OK;
}

/* Construct a minimal RIFF/WAVE header for 16 kHz / 16-bit / mono
 * PCM. The body is the raw int16 samples written little-endian. */
static size_t build_wav_header(uint8_t *hdr, uint32_t pcm_bytes)
{
    uint32_t byte_rate = LEXIN_VOICE_SAMPLE_RATE * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;
    uint16_t channels = 1;
    uint16_t format = 1; /* PCM */
    uint32_t chunk_size = 36 + pcm_bytes;
    uint8_t *p = hdr;
    memcpy(p, "RIFF", 4);
    p += 4;
    uint32_t le = chunk_size;
    memcpy(p, &le, 4);
    p += 4;
    memcpy(p, "WAVE", 4);
    p += 4;
    memcpy(p, "fmt ", 4);
    p += 4;
    le = 16;
    memcpy(p, &le, 4);
    p += 4;
    memcpy(p, &format, 2);
    p += 2;
    memcpy(p, &channels, 2);
    p += 2;
    le = LEXIN_VOICE_SAMPLE_RATE;
    memcpy(p, &le, 4);
    p += 4;
    le = byte_rate;
    memcpy(p, &le, 4);
    p += 4;
    memcpy(p, &block_align, 2);
    p += 2;
    memcpy(p, &bits_per_sample, 2);
    p += 2;
    memcpy(p, "data", 4);
    p += 4;
    le = pcm_bytes;
    memcpy(p, &le, 4);
    p += 4;
    return (size_t)(p - hdr);
}

typedef struct {
    int16_t *samples;
    size_t count;
} utterance_t;

static bool upload_utterance(const char *proxy_url, const utterance_t *utt)
{
    if (proxy_url == NULL || utt == NULL || utt->count == 0) {
        return false;
    }
    uint32_t pcm_bytes = (uint32_t)(utt->count * sizeof(int16_t));
    uint8_t header[44];
    size_t header_len = build_wav_header(header, pcm_bytes);

    char url[192];
    snprintf(url, sizeof(url), "%s/voice-stream", proxy_url);

    char *body = heap_caps_malloc(header_len + pcm_bytes, MALLOC_CAP_SPIRAM);
    if (!body) {
        body = malloc(header_len + pcm_bytes);
    }
    if (!body) {
        return false;
    }
    memcpy(body, header, header_len);
    memcpy(body + header_len, utt->samples, pcm_bytes);

    http_buf_t resp = {
        .buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM),
        .cap = 2048,
        .len = 0,
    };
    if (!resp.buf) {
        resp.buf = malloc(2048);
    }
    if (!resp.buf) {
        free(body);
        return false;
    }
    resp.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = upload_http_event,
        .timeout_ms = LEXIN_VOICE_HTTP_TIMEOUT_MS,
        .user_data = &resp,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        free(resp.buf);
        return false;
    }
    char sample_rate_header[12];
    snprintf(sample_rate_header, sizeof(sample_rate_header), "%d",
             LEXIN_VOICE_SAMPLE_RATE);
    esp_http_client_set_header(client, "content-type", "audio/wav");
    esp_http_client_set_header(client, "x-lexin-sample-rate", sample_rate_header);
    esp_http_client_set_header(client, "x-lexin-channels", "1");
    char user_id_header[48];
    char mode_header[16];
    snapshot_lock(&s_voice);
    snprintf(user_id_header, sizeof(user_id_header), "%s",
             s_voice.user_id[0] ? s_voice.user_id : "device");
    snprintf(mode_header, sizeof(mode_header), "%s",
             s_voice.mode[0] ? s_voice.mode : "chat");
    snapshot_unlock(&s_voice);
    esp_http_client_set_header(client, "x-lexin-user-id", user_id_header);
    esp_http_client_set_header(client, "x-lexin-voice-mode", mode_header);
    esp_http_client_set_post_field(client, body, header_len + pcm_bytes);

    ESP_LOGI(TAG, "upload voice samples=%u pcm=%u url=%s user=%s mode=%s",
             (unsigned)utt->count, (unsigned)pcm_bytes, url, user_id_header,
             mode_header);
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    if (err != ESP_OK || status < 200 || status >= 300 || resp.len == 0) {
        ESP_LOGW(TAG, "voice upload failed err=%s status=%d len=%u",
                    esp_err_to_name(err), status, (unsigned)resp.len);
        s_voice.proxy_url[0] = '\0';
        esp_http_client_cleanup(client);
        free(body);
        free(resp.buf);
        return false;
    }
    resp.buf[resp.len < resp.cap ? resp.len : resp.cap - 1] = '\0';
    ESP_LOGI(TAG, "voice reply (%d): %s", status, resp.buf);
    proxy_reply_dispatch(resp.buf, resp.len);

    esp_http_client_cleanup(client);
    free(body);
    free(resp.buf);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Reply dispatch (UI update)                                         */
/* ------------------------------------------------------------------ */

/* Decoded reply from the proxy. */
typedef struct {
    char transcript[256];
    char reply[512];
    char status[64];
    int backend;
    bool is_error;
    bool has_text;
} reply_t;

static void dispatch_to_main(const reply_t *r)
{
    snapshot_lock(&s_voice);
    if (r->transcript[0] != '\0') {
        snprintf(s_voice.snapshot.transcript, sizeof(s_voice.snapshot.transcript),
                 "%s", r->transcript);
    }
    if (r->reply[0] != '\0') {
        snprintf(s_voice.snapshot.reply, sizeof(s_voice.snapshot.reply), "%s",
                 r->reply);
    }
    s_voice.snapshot.backend = (uint8_t)(r->backend > 0 ? r->backend : 0);
    s_voice.snapshot.state = r->is_error ? LEXIN_VOICE_STATE_ERROR
                                            : LEXIN_VOICE_STATE_REPLY;
    s_voice.snapshot.updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (r->status[0] != '\0') {
        snprintf(s_voice.snapshot.status, sizeof(s_voice.snapshot.status),
                 "%s", r->status);
    }
    snapshot_unlock(&s_voice);
    publish_snapshot(&s_voice);
}

static void voice_make_display_safe(reply_t *r)
{
    (void)r;
}

static void proxy_reply_dispatch(const char *json, size_t len)
{
    (void)len;
    if (json == NULL) {
        return;
    }
    reply_t r = {0};
    copy_json_string(json, "transcript", r.transcript, sizeof(r.transcript));
    copy_json_string(json, "reply", r.reply, sizeof(r.reply));
    copy_json_string(json, "status", r.status, sizeof(r.status));
    int backend = 0;
    if (copy_json_int(json, "backend", &backend)) {
        r.backend = backend;
    }
    int error_int = 0;
    if (copy_json_int(json, "error", &error_int) && error_int != 0) {
        r.is_error = true;
    }
    if (r.reply[0] == '\0' && !r.is_error) {
        ESP_LOGW(TAG, "voice reply missing 'reply' field: %s", json);
        snprintf(r.reply, sizeof(r.reply), "代理返回格式错误");
        r.is_error = true;
    }
    voice_make_display_safe(&r);
    dispatch_to_main(&r);
}

/* ------------------------------------------------------------------ */
/*  Audio capture pipeline                                             */
/* ------------------------------------------------------------------ */

static bool open_microphone(void)
{
    if (s_voice.mic != NULL) {
        return true;
    }
    s_voice.mic = bsp_audio_codec_microphone_init();
    if (s_voice.mic == NULL) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        return false;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = LEXIN_VOICE_SAMPLE_RATE,
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
    };
    esp_err_t err = esp_codec_dev_open(s_voice.mic, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %s", esp_err_to_name(err));
        s_voice.mic = NULL;
        return false;
    }
    /* ES8311 microphone gain is a percentage in [0, 100]. Use a louder
     * bring-up value so the RMS readout reacts clearly during testing. */
    esp_codec_dev_set_in_gain(s_voice.mic, 80.0);
    s_voice.mic_ready = true;
    ESP_LOGI(TAG, "microphone ready (%d Hz mono 16-bit)",
             LEXIN_VOICE_SAMPLE_RATE);
    return true;
}

/* Compute int16 RMS of a 30 ms frame. The threshold is calibrated for
 * the on-board mic + ES8311 in a quiet room. */
static uint16_t compute_rms(const int16_t *samples, size_t n)
{
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = samples[i];
        acc += (uint64_t)(v * v);
    }
    if (n == 0) {
        return 0;
    }
    /* sqrt(acc / n); we use a fast sqrtf for cheap UI feedback. */
    double mean = (double)acc / (double)n;
    double rms = sqrt(mean);
    if (rms > 65535.0) {
        return 65535;
    }
    return (uint16_t)rms;
}

static void voice_task(void *arg)
{
    (void)arg;
    int16_t frame[LEXIN_VOICE_FRAME_SAMPLES];
    /* Allocate the utterance buffer on the heap: 8 seconds of 16 kHz
     * mono PCM is 256 KB, too large for the task stack. Prefer SPIRAM
     * (PSRAM) because the project has 16 MB of external memory, and
     * fall back to internal RAM if PSRAM is exhausted. */
    int16_t *utt_samples = heap_caps_malloc(
        LEXIN_VOICE_MAX_UTTER_FRAMES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!utt_samples) {
        utt_samples = malloc(LEXIN_VOICE_MAX_UTTER_FRAMES * sizeof(int16_t));
    }
    if (!utt_samples) {
        ESP_LOGE(TAG, "no memory for utterance buffer");
        set_state(LEXIN_VOICE_STATE_ERROR, "NO MEMORY");
        s_voice.running = false;
        vTaskDelete(NULL);
        return;
    }
    utterance_t utt = { .samples = utt_samples, .count = 0 };
    bool in_utt = false;
    int32_t silence_ms = 0;
    int32_t utt_ms = 0;
    int32_t last_upload_ms = 0;

    set_state(LEXIN_VOICE_STATE_IDLE, "BOOTING");

    while (s_voice.running) {
        if (!s_voice.mic_ready) {
            if (!open_microphone()) {
                set_state(LEXIN_VOICE_STATE_ERROR, "MIC INIT FAIL");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            set_state(LEXIN_VOICE_STATE_LISTENING, "READY");
        }

        int ret = esp_codec_dev_read(s_voice.mic, frame, sizeof(frame));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "microphone read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t samples = LEXIN_VOICE_FRAME_SAMPLES;
        if (samples == 0) {
            continue;
        }
        uint16_t rms = compute_rms(frame, samples);
        set_rms(rms);
        uint32_t now_log_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((now_log_ms - s_last_rms_log_ms) >= 2000) {
            s_last_rms_log_ms = now_log_ms;
            ESP_LOGI(TAG, "listening rms=%u threshold=%d in_utt=%d mic=%s",
                     rms, LEXIN_VOICE_RMS_THRESHOLD, in_utt ? 1 : 0,
                     s_voice.mic_ready ? "ready" : "wait");
        }

        if (rms >= LEXIN_VOICE_RMS_THRESHOLD) {
            if (!in_utt) {
                in_utt = true;
                utt.count = 0;
                silence_ms = 0;
                utt_ms = 0;
                ESP_LOGI(TAG, "voice activity rms=%u threshold=%d",
                         rms, LEXIN_VOICE_RMS_THRESHOLD);
                set_state(LEXIN_VOICE_STATE_HEARD, "VOICE");
            }
            silence_ms = 0;
            if (utt.count + samples <= LEXIN_VOICE_MAX_UTTER_FRAMES) {
                memcpy(utt.samples + utt.count, frame, samples * sizeof(int16_t));
                utt.count += samples;
            }
            utt_ms += LEXIN_VOICE_FRAME_MS;
        } else if (in_utt) {
            /* Continue recording a little silence so the ASR hears the
             * tail of the word. Stop after LEXIN_VOICE_SILENCE_MS. */
            if (utt.count + samples <= LEXIN_VOICE_MAX_UTTER_FRAMES) {
                memcpy(utt.samples + utt.count, frame, samples * sizeof(int16_t));
                utt.count += samples;
            }
            utt_ms += LEXIN_VOICE_FRAME_MS;
            silence_ms += LEXIN_VOICE_FRAME_MS;
        }

        if (in_utt) {
            int32_t now_ms = (int32_t)(esp_timer_get_time() / 1000);
            bool too_long = utt_ms >= LEXIN_VOICE_MAX_UTTER_MS;
            bool silent_enough = silence_ms >= LEXIN_VOICE_SILENCE_MS;
            bool just_uploaded = last_upload_ms != 0 &&
                (now_ms - last_upload_ms) < LEXIN_VOICE_COOLDOWN_MS;
            if (too_long || (silent_enough && !just_uploaded)) {
                in_utt = false;
                if (utt.count > 0) {
                    set_state(LEXIN_VOICE_STATE_UPLOADING, "UPLOAD");
                    const char *proxy = ensure_proxy_url();
                    bool ok = false;
                    if (proxy != NULL) {
                        ok = upload_utterance(proxy, &utt);
                    }
                    if (!ok) {
                        set_state(LEXIN_VOICE_STATE_ERROR, "PROXY OFFLINE");
                    }
                    last_upload_ms = now_ms;
                }
                utt.count = 0;
                utt_ms = 0;
                silence_ms = 0;
                /* Cool down so we don't immediately re-trigger. */
                int32_t wait_ms = LEXIN_VOICE_COOLDOWN_MS;
                while (wait_ms > 0 && s_voice.running) {
                    int chunk = wait_ms > 50 ? 50 : wait_ms;
                    vTaskDelay(pdMS_TO_TICKS(chunk));
                    wait_ms -= chunk;
                }
                if (s_voice.running) {
                    set_state(LEXIN_VOICE_STATE_LISTENING, "READY");
                }
            }
        }
    }

    if (s_voice.mic) {
        esp_codec_dev_close(s_voice.mic);
        s_voice.mic = NULL;
    }
    s_voice.mic_ready = false;
    if (utt_samples) {
        free(utt_samples);
    }
    s_voice_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t lexin_voice_start(lexin_voice_callback_t callback, void *user_data)
{
    if (s_voice.running) {
        return ESP_OK;
    }
    memset(&s_voice, 0, sizeof(s_voice));
    s_voice.callback = callback;
    s_voice.user_data = user_data;
    s_voice.mutex = xSemaphoreCreateRecursiveMutex();
    if (s_voice.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_voice.snapshot.state = LEXIN_VOICE_STATE_IDLE;
    s_voice.snapshot.updated_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_rms_publish_ms = 0;
    s_last_rms_log_ms = 0;
    s_voice.running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(voice_task, "lexin_voice",
                                                8192, NULL, 6, &s_voice_task, 1);
    if (ok != pdPASS) {
        s_voice.running = false;
        vSemaphoreDelete(s_voice.mutex);
        s_voice.mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voice task started");
    return ESP_OK;
}

void lexin_voice_get_snapshot(lexin_voice_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    if (s_voice.mutex == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }
    snapshot_lock(&s_voice);
    *snapshot = s_voice.snapshot;
    snapshot_unlock(&s_voice);
}

void lexin_voice_set_user_id(const char *user_id)
{
    if (s_voice.mutex == NULL) {
        return;
    }
    snapshot_lock(&s_voice);
    snprintf(s_voice.user_id, sizeof(s_voice.user_id), "%s",
             user_id ? user_id : "");
    snapshot_unlock(&s_voice);
}

void lexin_voice_set_mode(const char *mode)
{
    if (s_voice.mutex == NULL) {
        return;
    }
    snapshot_lock(&s_voice);
    snprintf(s_voice.mode, sizeof(s_voice.mode), "%s", mode ? mode : "chat");
    snapshot_unlock(&s_voice);
}

/* Provide a way for the main loop (or the test firmware) to push an
 * externally-received reply (e.g. a future TTS / WebSocket path) into
 * the same snapshot state. */
void lexin_voice_post_reply(const char *transcript, const char *reply,
                              uint8_t backend, bool is_error)
{
    reply_t r = {0};
    if (transcript) {
        snprintf(r.transcript, sizeof(r.transcript), "%s", transcript);
    }
    if (reply) {
        snprintf(r.reply, sizeof(r.reply), "%s", reply);
    }
    r.backend = backend;
    r.is_error = is_error;
    dispatch_to_main(&r);
}
