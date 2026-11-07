/*
 * Wi-Fi, SNTP and the setup hotspot. See wifi_link.h.
 */

#include "wifi_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi";

#define NVS_NS        "wifi"
#define NTP_SERVER    "pool.ntp.org"
#define MAX_RETRIES   3
#define SCAN_MAX      20
#define RETRY_MS      (5 * 60 * 1000)    /* try again this often while the time is unset */
#define STOP_DELAY_MS 800                /* let the last packets out before the radio stops */
#define TIME_VALID_AFTER 1700000000      /* 2023-11: anything earlier is "not set" */

static volatile wifi_link_state_t s_state = WIFI_LINK_IDLE;
static bool s_wifi_up;                   /* esp_wifi_init() done */
static esp_netif_t *s_sta, *s_ap;
static httpd_handle_t s_httpd;
static esp_timer_handle_t s_stop_timer, s_retry_timer, s_prov_timer;
static int s_retries;
static volatile bool s_prov_done;
static bool s_sntp_started;

static char s_ssid[33], s_pass[65];      /* the saved network */
static int s_tz_min;
static char s_ap_ssid[33], s_ap_pass[12];
static wifi_ap_record_t s_scan[SCAN_MAX];
static uint16_t s_scan_n;

/* ---- Saved network ------------------------------------------------------- */

static void load_saved(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) {
        s_ssid[0] = 0;
    }
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) {
        s_pass[0] = 0;
    }
    int32_t tz = 0;
    nvs_get_i32(h, "tz", &tz);
    s_tz_min = (int)tz;
    nvs_close(h);
}

static void save_network(const char *ssid, const char *pass, int tz_min)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "network not saved");
        return;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_set_i32(h, "tz", tz_min);
    nvs_commit(h);
    nvs_close(h);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass);
    s_tz_min = tz_min;
}

/* ---- The radio ------------------------------------------------------------ */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void connect_saved(void);

/* The driver's fields are fixed arrays (the SSID is not NUL-terminated at 32). */
static void put(uint8_t *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    memcpy(dst, src, n < cap ? n : cap);
}

static bool ensure_wifi(void)
{
    if (s_wifi_up) {
        return true;
    }
    if (esp_netif_init() != ESP_OK) {
        return false;
    }
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return false;                    /* INVALID_STATE: already created */
    }
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* our own NVS keys hold the network */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
    s_wifi_up = true;
    return true;
}

static void radio_off(void)
{
    if (s_wifi_up) {
        esp_wifi_stop();
    }
}

static void stop_timer_cb(void *arg)
{
    if (s_state != WIFI_LINK_PROVISIONING) {
        radio_off();
    }
}

static void prov_timer_cb(void *arg)
{
    wifi_link_provision_stop();            /* the phone has chosen: leave the hotspot, join */
}

static void retry_timer_cb(void *arg)
{
    if (s_state != WIFI_LINK_PROVISIONING && s_state != WIFI_LINK_CONNECTING &&
        !wifi_link_time_valid() && wifi_link_has_network()) {
        wifi_link_retry();
    }
}

static void time_synced(struct timeval *tv)
{
    ESP_LOGI(TAG, "time set from the network");
    s_state = WIFI_LINK_IDLE;
    esp_timer_start_once(s_stop_timer, STOP_DELAY_MS * 1000);
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    s_state = WIFI_LINK_FAILED;
    esp_timer_start_once(s_stop_timer, STOP_DELAY_MS * 1000);
    if (!wifi_link_time_valid()) {
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_MS * 1000);
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_state == WIFI_LINK_CONNECTING) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == WIFI_LINK_CONNECTING) {
            if (++s_retries <= MAX_RETRIES) {
                esp_wifi_connect();
            } else {
                fail("could not join the network");
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "joined; asking %s for the time", NTP_SERVER);
        if (!s_sntp_started) {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
            sntp.sync_cb = time_synced;
            s_sntp_started = esp_netif_sntp_init(&sntp) == ESP_OK;
        } else {
            esp_netif_sntp_start();
        }
    }
}

static void connect_saved(void)
{
    if (!s_ssid[0] || !ensure_wifi()) {
        s_state = WIFI_LINK_IDLE;
        return;
    }
    wifi_config_t cfg = { 0 };
    put(cfg.sta.ssid, sizeof(cfg.sta.ssid), s_ssid);
    put(cfg.sta.password, sizeof(cfg.sta.password), s_pass);
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    s_retries = 0;
    s_state = WIFI_LINK_CONNECTING;
    esp_wifi_stop();                       /* from APSTA, or already stopped */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (esp_wifi_start() != ESP_OK) {
        fail("radio would not start");
    }
}

/* ---- The setup page --------------------------------------------------------- */

static void scan_now(void)
{
    s_scan_n = 0;
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return;
    }
    uint16_t n = SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&n, s_scan) == ESP_OK) {
        s_scan_n = n;                     /* sorted strongest first by the driver */
    }
}

static void html_escape(httpd_req_t *r, const char *s)
{
    for (; *s; s++) {
        const char *rep = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" :
                          *s == '"' ? "&quot;" : NULL;
        if (rep) {
            httpd_resp_sendstr_chunk(r, rep);
        } else {
            httpd_resp_send_chunk(r, s, 1);
        }
    }
}

static const char PAGE_HEAD[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Crack the Safe</title><style>body{font:18px sans-serif;background:#111;color:#eee;margin:0;padding:20px}"
    "h1{font-size:22px}select,input,button{width:100%;font-size:18px;padding:12px;margin:8px 0;box-sizing:border-box}"
    "button{background:#c9a227;border:0;border-radius:8px;font-weight:bold}a{color:#c9a227}</style>"
    "<h1>Crack the Safe: Wi-Fi</h1>";

static esp_err_t page_get(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_send_chunk(r, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_sendstr_chunk(r, "<p>The safe uses this only to set its clock.</p>"
                                "<form method=post action=/save><label>Network</label><select name=ssid>");
    char last[33] = "";
    for (int i = 0; i < s_scan_n; i++) {
        const char *name = (const char *)s_scan[i].ssid;
        if (!name[0] || !strcmp(name, last)) {
            continue;
        }
        snprintf(last, sizeof(last), "%s", name);
        httpd_resp_sendstr_chunk(r, "<option>");
        html_escape(r, name);
        httpd_resp_sendstr_chunk(r, "</option>");
    }
    httpd_resp_sendstr_chunk(r,
        "</select><a href=/rescan>Search again</a><label><br>Password</label>"
        "<input name=pass type=password autocomplete=off>"
        "<input type=hidden name=tz id=tz><button>Join</button></form>"
        "<script>document.getElementById('tz').value=-new Date().getTimezoneOffset()</script>");
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t rescan_get(httpd_req_t *r)
{
    scan_now();
    httpd_resp_set_status(r, "303 See Other");
    httpd_resp_set_hdr(r, "Location", "/");
    return httpd_resp_send(r, NULL, 0);
}

/* application/x-www-form-urlencoded value of `key`, decoded, into out. */
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    const size_t klen = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        const size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            size_t o = 0;
            const char *end = p + seg;
            for (const char *s = p + klen + 1; s < end && o + 1 < out_len; s++) {
                if (*s == '+') {
                    out[o++] = ' ';
                } else if (*s == '%' && s + 2 < end) {
                    unsigned v;
                    char hex[3] = { s[1], s[2], 0 };
                    if (sscanf(hex, "%x", &v) == 1) {
                        out[o++] = (char)v;
                        s += 2;
                    }
                } else {
                    out[o++] = *s;
                }
            }
            out[o] = 0;
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return false;
}

static esp_err_t save_post(httpd_req_t *r)
{
    char body[256];
    if (r->content_len >= sizeof(body)) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too long");
    }
    int got = httpd_req_recv(r, body, r->content_len);
    if (got <= 0) {
        return ESP_FAIL;
    }
    body[got] = 0;
    char ssid[33], pass[65], tz[12];
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no network chosen");
    }
    form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "tz", tz, sizeof(tz));
    save_network(ssid, pass, atoi(tz));
    s_prov_done = true;
    ESP_LOGI(TAG, "network \"%s\" chosen on the phone (utc%+d min): joining shortly", ssid, atoi(tz));
    esp_timer_start_once(s_prov_timer, 2500 * 1000);   /* after this reply has gone out */
    httpd_resp_set_type(r, "text/html");
    httpd_resp_send_chunk(r, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_sendstr_chunk(r, "<p>Saved. The safe is joining the network. "
                                "You can close this page and rejoin your usual Wi-Fi.</p>");
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t any_get(httpd_req_t *r, httpd_err_code_t err)
{
    httpd_resp_set_status(r, "303 See Other");     /* phones' probe URLs land on the page */
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    return httpd_resp_send(r, NULL, 0);
}

static bool http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 4;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        return false;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = page_get },
        { .uri = "/rescan", .method = HTTP_GET,  .handler = rescan_get },
        { .uri = "/save",   .method = HTTP_POST, .handler = save_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, any_get);
    return true;
}

static void http_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

/* ---- Public ------------------------------------------------------------------ */

bool wifi_link_provision_start(void)
{
    if (s_state == WIFI_LINK_PROVISIONING) {
        return true;
    }
    if (!ensure_wifi()) {
        return false;
    }
    esp_wifi_stop();
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";   /* no look-alikes */
    for (int i = 0; i < 8; i++) {
        s_ap_pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    s_ap_pass[8] = 0;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "CrackTheSafe-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = { 0 };
    put(ap.ap.ssid, sizeof(ap.ap.ssid), s_ap_ssid);
    put(ap.ap.password, sizeof(ap.ap.password), s_ap_pass);
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_prov_done = false;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) {
        return false;
    }
    scan_now();
    if (!http_start()) {
        esp_wifi_stop();
        return false;
    }
    s_state = WIFI_LINK_PROVISIONING;
    ESP_LOGI(TAG, "setup hotspot %s up, %u networks seen", s_ap_ssid, (unsigned)s_scan_n);
    return true;
}

void wifi_link_provision_stop(void)
{
    if (s_state != WIFI_LINK_PROVISIONING) {
        return;
    }
    http_stop();
    s_state = WIFI_LINK_IDLE;
    if (s_prov_done) {
        connect_saved();                   /* the phone chose a network: join it now */
    } else {
        radio_off();
    }
}

void wifi_link_provision_info(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    snprintf(ssid, ssid_len, "%s", s_ap_ssid);
    snprintf(pass, pass_len, "%s", s_ap_pass);
}

bool wifi_link_provision_done(void)
{
    return s_prov_done;
}

void wifi_link_retry(void)
{
    if (s_state != WIFI_LINK_PROVISIONING && s_state != WIFI_LINK_CONNECTING) {
        connect_saved();
    }
}

void wifi_link_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_ssid[0] = s_pass[0] = 0;
    if (s_state != WIFI_LINK_PROVISIONING) {
        s_state = WIFI_LINK_IDLE;
        radio_off();
    }
}

void wifi_link_init(void)
{
    const esp_timer_create_args_t stop = { .callback = stop_timer_cb, .name = "wifi_stop" };
    const esp_timer_create_args_t retry = { .callback = retry_timer_cb, .name = "wifi_retry" };
    esp_timer_create(&stop, &s_stop_timer);
    esp_timer_create(&retry, &s_retry_timer);
    const esp_timer_create_args_t prov = { .callback = prov_timer_cb, .name = "wifi_prov" };
    esp_timer_create(&prov, &s_prov_timer);
    load_saved();
    if (s_ssid[0]) {
        ESP_LOGI(TAG, "saved network \"%s\": joining", s_ssid);
        connect_saved();
    }
}

wifi_link_state_t wifi_link_state(void)
{
    return s_state;
}

bool wifi_link_has_network(void)
{
    return s_ssid[0] != 0;
}

const char *wifi_link_saved_ssid(void)
{
    return s_ssid;
}

bool wifi_link_time_valid(void)
{
    struct timeval tv;
    return gettimeofday(&tv, NULL) == 0 && tv.tv_sec > TIME_VALID_AFTER;
}

uint32_t wifi_link_unix_time(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec <= TIME_VALID_AFTER) {
        return 0;
    }
    return (uint32_t)tv.tv_sec;
}

int wifi_link_tz_minutes(void)
{
    return s_tz_min;
}
