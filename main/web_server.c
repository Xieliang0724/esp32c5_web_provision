/*
 * web_server.c - Web 配网服务器实现
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "cJSON.h"

#include "config_store.h"
#include "modbus_gw.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "web_srv";

/* 内嵌网页 (main/www/index.html) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* 内嵌 TLS 服务器证书（用于 /api/cert 下载） */
extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_cert_pem_end");

#define MAX_POST_BODY 1024
#define SCAN_LIST_MAX 30

static httpd_handle_t s_server = NULL;
static bool s_scan_done = false;

static const char *AUTH_NAMES[] = {
    [WIFI_AUTH_OPEN]         = "OPEN",
    [WIFI_AUTH_WEP]          = "WEP",
    [WIFI_AUTH_WPA_PSK]      = "WPA_PSK",
    [WIFI_AUTH_WPA2_PSK]     = "WPA2_PSK",
    [WIFI_AUTH_WPA_WPA2_PSK] = "WPA_WPA2_PSK",
    [WIFI_AUTH_WPA3_PSK]     = "WPA3_PSK",
    [WIFI_AUTH_WPA2_WPA3_PSK]= "WPA2_WPA3_PSK",
    [WIFI_AUTH_OWE]          = "OWE",
};

static void scan_done_cb(void)
{
    s_scan_done = true;
}

static const char *auth_name(wifi_auth_mode_t m)
{
    if (m < sizeof(AUTH_NAMES) / sizeof(AUTH_NAMES[0]) && AUTH_NAMES[m]) {
        return AUTH_NAMES[m];
    }
    return "UNKNOWN";
}

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_json_obj(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = send_json(req, s);
    free(s);
    return ret;
}

/* 读取 POST body（JSON）到 buf */
static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        total += r;
    }
    buf[total] = '\0';
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 路由                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t handle_root(httpd_req_t *req)
{
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

/* 下载设备 TLS 证书（仅公钥证书，供客户端信任设备） */
static esp_err_t handle_cert_get(httpd_req_t *req)
{
    size_t len = server_cert_pem_end - server_cert_pem_start;
    httpd_resp_set_type(req, "application/x-pem-file");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=esp32c5_cert.pem");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)server_cert_pem_start, len);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    wifi_mgr_state_t st = wifi_mgr_get_state();
    const char *state_str = "unknown";
    switch (st) {
    case WIFI_MGR_STATE_CONFIG:     state_str = "config"; break;
    case WIFI_MGR_STATE_CONNECTING: state_str = "connecting"; break;
    case WIFI_MGR_STATE_CONNECTED:  state_str = "connected"; break;
    default: break;
    }

    /* 当前 STA 配置的 SSID */
    char ssid[33] = {0};
    wifi_config_t wcfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wcfg) == ESP_OK) {
        strlcpy(ssid, (char *)wcfg.sta.ssid, sizeof(ssid));
    }
    char ip[32] = {0};
    wifi_mgr_get_sta_ip(ip, sizeof(ip));

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "state", state_str);
    cJSON_AddStringToObject(obj, "ssid", ssid);
    cJSON_AddStringToObject(obj, "ip", ip);
    cJSON_AddBoolToObject(obj, "ap_on", wifi_mgr_is_ap_on());

    /* STA 网络信息：网关/掩码/DNS */
    char gw[32] = {0}, mask[32] = {0}, dns[32] = {0};
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    if (sta) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
            esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
        }
        esp_netif_dns_info_t dns_info = {0};
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns, sizeof(dns));
        }
    }
    cJSON_AddStringToObject(obj, "gateway", gw);
    cJSON_AddStringToObject(obj, "netmask", mask);
    cJSON_AddStringToObject(obj, "dns", dns);

    /* 信号强度（未连接时 rssi=0） */
    int8_t rssi = 0;
    wifi_ap_record_t ap_info = {0};
    if (wifi_mgr_get_sta_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    cJSON_AddNumberToObject(obj, "rssi", rssi);

    /* AP 信息 */
    char ap_ssid[33] = {0};
    wifi_mgr_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
    cJSON_AddStringToObject(obj, "ap_ssid", ap_ssid);
    cJSON_AddNumberToObject(obj, "ap_clients", wifi_mgr_get_ap_clients());

    /* 当前 Wi-Fi 模式 */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    cJSON_AddStringToObject(obj, "wifi_mode",
                            mode == WIFI_MODE_STA ? "STA" :
                            mode == WIFI_MODE_AP ? "AP" :
                            mode == WIFI_MODE_APSTA ? "APSTA" : "NULL");

    /* 固件版本（来自 git tag / PROJECT_VER） */
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(obj, "version", app ? app->version : "unknown");

    esp_err_t ret = send_json_obj(req, obj);
    cJSON_Delete(obj);
    return ret;
}

/* 运行时开关 SoftAP */
static esp_err_t handle_ap_post(httpd_req_t *req)
{
    char buf[MAX_POST_BODY];
    esp_err_t ret = recv_body(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ret;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_GetObjectItem(root, "ap_on");
    bool want_on = cJSON_IsBool(j) && cJSON_IsTrue(j);
    cJSON_Delete(root);

    esp_err_t r = want_on ? wifi_mgr_ap_enable() : wifi_mgr_ap_disable();
    cJSON *obj = cJSON_CreateObject();
    if (r == ESP_OK) {
        cJSON_AddStringToObject(obj, "status", "ok");
        cJSON_AddBoolToObject(obj, "ap_on", wifi_mgr_is_ap_on());
    } else {
        cJSON_AddStringToObject(obj, "status", "error");
        cJSON_AddStringToObject(obj, "msg",
            want_on ? esp_err_to_name(r) : "STA 未连接，关闭 AP 将导致设备失联");
    }
    ret = send_json_obj(req, obj);
    cJSON_Delete(obj);
    return ret;
}

/* 断开 STA 并进入配网模式（不删除已保存配置，重启后仍按原配置联网） */
static esp_err_t handle_disconnect_post(httpd_req_t *req)
{
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    esp_err_t ret = send_json_obj(req, ok);   /* 必须先响应，STA 断开后网页将失联 */
    cJSON_Delete(ok);

    /* 延迟执行：等响应发出后再切换模式 */
    esp_timer_create_args_t targs = {
        .callback = (void (*)(void *))wifi_mgr_enter_config_mode,
        .name = "delayed_config",
    };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        esp_timer_start_once(t, 300 * 1000);
    }
    return ret;
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();

    /* 方案B：仅配网模式允许扫描，避免抢占射频影响 STA 业务/连接 */
    if (wifi_mgr_get_state() != WIFI_MGR_STATE_CONFIG) {
        cJSON_AddStringToObject(obj, "status", "error");
        cJSON_AddStringToObject(obj, "msg", "STA 已联网/连接中，扫描已禁用以保障业务");
        esp_err_t ret = send_json_obj(req, obj);
        cJSON_Delete(obj);
        return ret;
    }

    if (s_scan_done) {
        s_scan_done = false;

        wifi_ap_record_t *records = NULL;
        uint16_t count = 0;
        esp_err_t ret = wifi_mgr_scan_get_results(&records, &count);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(obj, "status", "error");
            cJSON_AddStringToObject(obj, "msg", esp_err_to_name(ret));
        } else {
            cJSON *nets = cJSON_AddArrayToObject(obj, "networks");
            uint16_t shown = 0;
            for (uint16_t i = 0; i < count && shown < SCAN_LIST_MAX; i++) {
                if (records[i].ssid[0] == '\0') {
                    continue;   /* 隐藏 SSID */
                }
                cJSON *n = cJSON_CreateObject();
                cJSON_AddStringToObject(n, "ssid", (char *)records[i].ssid);
                cJSON_AddNumberToObject(n, "rssi", records[i].rssi);
                cJSON_AddStringToObject(n, "auth", auth_name(records[i].authmode));
                cJSON_AddNumberToObject(n, "channel", records[i].primary);
                cJSON_AddStringToObject(n, "band", wifi_mgr_band_of_channel(records[i].primary));
                cJSON_AddItemToArray(nets, n);
                shown++;
            }
            cJSON_AddStringToObject(obj, "status", "ok");
            if (records) {
                free(records);
            }
        }
    } else if (!wifi_mgr_is_scanning()) {
        /* 启动一次新扫描 */
        esp_err_t ret = wifi_mgr_scan_async(SCAN_LIST_MAX, scan_done_cb);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            cJSON_AddStringToObject(obj, "status", "error");
            cJSON_AddStringToObject(obj, "msg", esp_err_to_name(ret));
        } else {
            cJSON_AddStringToObject(obj, "status", "scanning");
        }
    } else {
        cJSON_AddStringToObject(obj, "status", "scanning");
    }

    esp_err_t ret = send_json_obj(req, obj);
    cJSON_Delete(obj);
    return ret;
}

/* 延迟执行连接/复位，避免在 HTTP handler 内部重启 Wi-Fi/停服务器 */
static void delayed_connect(void *arg)
{
    wifi_mgr_connect((const wifi_config_data_t *)arg);
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    char buf[MAX_POST_BODY];
    esp_err_t ret = recv_body(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ret;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    if (!cJSON_IsString(j_ssid) || strlen(j_ssid->valuestring) == 0 ||
        strlen(j_ssid->valuestring) >= CFG_SSID_MAX_LEN) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    wifi_config_data_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.ssid, j_ssid->valuestring, sizeof(cfg.ssid));

    cJSON *j_pass = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(j_pass) && strlen(j_pass->valuestring) < CFG_PASS_MAX_LEN) {
        strlcpy(cfg.password, j_pass->valuestring, sizeof(cfg.password));
    }

    cJSON *j_mode = cJSON_GetObjectItem(root, "ip_mode");
    if (cJSON_IsString(j_mode) && strcmp(j_mode->valuestring, "static") == 0) {
        cfg.ip_mode = IP_MODE_STATIC;
    }

    if (cfg.ip_mode == IP_MODE_STATIC) {
        cJSON *j_ip = cJSON_GetObjectItem(root, "ip");
        cJSON *j_gw = cJSON_GetObjectItem(root, "gateway");
        if (!cJSON_IsString(j_ip) || strlen(j_ip->valuestring) == 0 ||
            !cJSON_IsString(j_gw) || strlen(j_gw->valuestring) == 0) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "static mode needs ip & gateway");
            return ESP_FAIL;
        }
        strlcpy(cfg.ip, j_ip->valuestring, sizeof(cfg.ip));
        strlcpy(cfg.gateway, j_gw->valuestring, sizeof(cfg.gateway));
        cJSON *j_mask = cJSON_GetObjectItem(root, "netmask");
        if (cJSON_IsString(j_mask)) {
            strlcpy(cfg.netmask, j_mask->valuestring, sizeof(cfg.netmask));
        }
        cJSON *j_dns = cJSON_GetObjectItem(root, "dns");
        if (cJSON_IsString(j_dns)) {
            strlcpy(cfg.dns, j_dns->valuestring, sizeof(cfg.dns));
        }
    }

    cJSON *j_ap_off = cJSON_GetObjectItem(root, "ap_off");
    cfg.ap_off = (cJSON_IsBool(j_ap_off) && cJSON_IsTrue(j_ap_off)) ? true : false;

    cJSON *j_fb = cJSON_GetObjectItem(root, "ap_fallback_delay");
    if (cJSON_IsNumber(j_fb) && j_fb->valueint >= 0 && j_fb->valueint <= 3600) {
        cfg.ap_fallback_delay = j_fb->valueint;
    }

    cJSON_Delete(root);

    ret = config_store_save(&cfg);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ret;
    }

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    ret = send_json_obj(req, ok);
    cJSON_Delete(ok);

    /* 响应已发出，延迟 300ms 再连接（避免从 handler 内部停掉服务器） */
    wifi_config_data_t *copy = malloc(sizeof(wifi_config_data_t));
    if (copy) {
        memcpy(copy, &cfg, sizeof(cfg));
        esp_timer_create_args_t targs = {
            .callback = delayed_connect,
            .arg = copy,
            .name = "delayed_connect",
        };
        esp_timer_handle_t t = NULL;
        if (esp_timer_create(&targs, &t) == ESP_OK) {
            esp_timer_start_once(t, 300 * 1000);
        }
    }
    return ret;
}

static esp_err_t handle_reset_post(httpd_req_t *req)
{
    config_store_clear();
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    esp_err_t ret = send_json_obj(req, ok);
    cJSON_Delete(ok);

    /* 延迟进入配网模式 */
    esp_timer_create_args_t targs = {
        .callback = (void (*)(void *))wifi_mgr_enter_config_mode,
        .name = "delayed_reset",
    };
    esp_timer_handle_t t = NULL;
    if (esp_timer_create(&targs, &t) == ESP_OK) {
        esp_timer_start_once(t, 300 * 1000);
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Modbus 网关配置接口                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handle_gw_get(httpd_req_t *req)
{
    gw_config_t cfg;
    gw_config_load(&cfg);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "enabled", cfg.enabled);
    cJSON_AddNumberToObject(obj, "port", cfg.port);
    cJSON_AddNumberToObject(obj, "baud", cfg.baud);
    cJSON_AddNumberToObject(obj, "tx", cfg.tx_gpio);
    cJSON_AddNumberToObject(obj, "rx", cfg.rx_gpio);
    cJSON_AddStringToObject(obj, "client_ip", cfg.client_ip);
    cJSON_AddBoolToObject(obj, "tls_enabled", cfg.tls_enabled);
    cJSON_AddNumberToObject(obj, "tls_port", cfg.tls_port);
    esp_err_t ret = send_json_obj(req, obj);
    cJSON_Delete(obj);
    return ret;
}

static bool valid_gpio(int g)
{
    return g >= 0 && g <= 48;
}

static esp_err_t handle_gw_post(httpd_req_t *req)
{
    char buf[MAX_POST_BODY];
    esp_err_t ret = recv_body(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ret;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    gw_config_t cfg;
    gw_config_load(&cfg);

    cJSON *j = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(j)) {
        cfg.enabled = cJSON_IsTrue(j);
    }
    j = cJSON_GetObjectItem(root, "port");
    if (cJSON_IsNumber(j) && j->valueint >= 1 && j->valueint <= 65535) {
        cfg.port = j->valueint;
    }
    j = cJSON_GetObjectItem(root, "baud");
    if (cJSON_IsNumber(j) && j->valueint >= 300 && j->valueint <= 2000000) {
        cfg.baud = j->valueint;
    }
    j = cJSON_GetObjectItem(root, "tx");
    if (cJSON_IsNumber(j) && valid_gpio(j->valueint)) {
        cfg.tx_gpio = j->valueint;
    }
    j = cJSON_GetObjectItem(root, "rx");
    if (cJSON_IsNumber(j) && valid_gpio(j->valueint)) {
        cfg.rx_gpio = j->valueint;
    }
    j = cJSON_GetObjectItem(root, "client_ip");
    if (cJSON_IsString(j) && strlen(j->valuestring) < sizeof(cfg.client_ip)) {
        strlcpy(cfg.client_ip, j->valuestring, sizeof(cfg.client_ip));
    }
    j = cJSON_GetObjectItem(root, "tls_enabled");
    if (cJSON_IsBool(j)) {
        cfg.tls_enabled = cJSON_IsTrue(j);
    }
    j = cJSON_GetObjectItem(root, "tls_port");
    if (cJSON_IsNumber(j) && j->valueint >= 1 && j->valueint <= 65535) {
        cfg.tls_port = j->valueint;
    }

    if (cfg.tx_gpio == cfg.rx_gpio) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tx and rx must differ");
        return ESP_FAIL;
    }
    /* 避免与串口控制台冲突 */
    if (cfg.tx_gpio == 11 || cfg.rx_gpio == 11 || cfg.tx_gpio == 12 || cfg.rx_gpio == 12) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "GPIO11/12 used by console UART");
        return ESP_FAIL;
    }
    /* TLS 端口不能与明文端口相同 */
    if (cfg.tls_enabled && cfg.tls_port == cfg.port) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "TLS port must differ from plain port");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    ret = gw_config_save(&cfg);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ret;
    }

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    ret = send_json_obj(req, ok);
    cJSON_Delete(ok);

    /* 应用新配置（任务创建/删除在 HTTP handler 内安全） */
    esp_err_t gret = modbus_gw_reconfigure(&cfg);
    ESP_LOGI(TAG, "gateway reconfigure: %s", esp_err_to_name(gret));
    return ret;
}

/* ------------------------------------------------------------------ */
/* 服务器生命周期                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t start_httpd(void)
{
    if (s_server) {
        return ESP_OK;   /* 幂等 */
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 10;
    return httpd_start(&s_server, &cfg);
}

static esp_err_t register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/api/cert",     .method = HTTP_GET,  .handler = handle_cert_get },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = handle_status },
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = handle_scan },
        { .uri = "/api/config",   .method = HTTP_POST, .handler = handle_config_post },
        { .uri = "/api/reset",    .method = HTTP_POST, .handler = handle_reset_post },
        { .uri = "/api/gw",       .method = HTTP_GET,  .handler = handle_gw_get },
        { .uri = "/api/gw",       .method = HTTP_POST, .handler = handle_gw_post },
        { .uri = "/api/ap",       .method = HTTP_POST, .handler = handle_ap_post },
        { .uri = "/api/disconnect", .method = HTTP_POST, .handler = handle_disconnect_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t ret = httpd_register_uri_handler(server, &uris[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    esp_err_t ret = start_httpd();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = register_handlers(s_server);
    if (ret != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "web server stopped");
    httpd_stop(s_server);
    s_server = NULL;
    return ESP_OK;
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}
