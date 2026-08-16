/*
 * wifi_mgr.c - Wi-Fi 状态机实现
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "config_store.h"
#include "rgb_led.h"
#include "wifi_mgr.h"

static const char *TAG = "wifi_mgr";

#define AP_IP_ADDR     "192.168.4.1"
#define AP_NETMASK     "255.255.255.0"
#define AP_GATEWAY     "192.168.4.1"
#define STA_IP_STR_LEN 32

#define CONN_RETRY_DELAY_MS   1000
#define CONN_TIMEOUT_MS       CONFIG_PROV_STA_TIMEOUT_MS
#define CONN_RETRY_MAX        CONFIG_PROV_CONNECT_RETRY_MAX
#define SCAN_MAX_APS          30

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static wifi_mgr_state_t s_state = WIFI_MGR_STATE_UNINIT;
static wifi_config_data_t s_cfg;             /* 当前生效配置 */
static uint8_t s_retry_count = 0;
static bool s_ap_on = false;
static bool s_ap_off_after_connect = false;  /* 本次连接成功后是否关闭 AP */

static esp_timer_handle_t s_conn_timeout_timer = NULL;
static esp_timer_handle_t s_conn_retry_timer = NULL;
static esp_timer_handle_t s_ap_fallback_timer = NULL;   /* STA 断开延迟后开启 AP 兜底 */

static wifi_mgr_ap_cb_t s_ap_cb = NULL;
static bool s_scanning = false;
static uint16_t s_scan_max_aps = SCAN_MAX_APS;
static wifi_mgr_scan_done_cb_t s_scan_done_cb = NULL;

static char s_sta_ip[STA_IP_STR_LEN] = {0};
static uint8_t s_ap_clients = 0;   /* 当前连上 SoftAP 的客户端数量 */
static char s_ap_ssid[33] = {0};   /* 当前 SoftAP SSID */

/* 依据当前状态刷新指示灯颜色 */
static void update_led(void)
{
    if (s_state == WIFI_MGR_STATE_CONNECTED) {
        rgb_led_set_state(RGB_STATE_CONNECTED);      /* 绿色：联网成功 */
    } else if (s_ap_clients > 0) {
        rgb_led_set_state(RGB_STATE_AP_CLIENT);      /* 蓝色：有设备连上 AP */
    } else {
        rgb_led_set_state(RGB_STATE_DEFAULT);        /* 橙色：默认 */
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

/* ------------------------------------------------------------------ */
/* 内部工具                                                             */
/* ------------------------------------------------------------------ */

static void set_state(wifi_mgr_state_t st)
{
    s_state = st;
    ESP_LOGI(TAG, "state -> %s",
             st == WIFI_MGR_STATE_CONFIG ? "CONFIG" :
             st == WIFI_MGR_STATE_CONNECTING ? "CONNECTING" :
             st == WIFI_MGR_STATE_CONNECTED ? "CONNECTED" : "UNINIT");
}

/* 停止->设置模式/配置->启动，保证模式切换干净 */
static esp_err_t apply_wifi_mode(wifi_mode_t mode,
                                 const wifi_ap_config_t *ap_cfg,
                                 const wifi_sta_config_t *sta_cfg)
{
    esp_err_t ret = esp_wifi_stop();   /* 未启动时返回 NOT_STARTED，忽略 */
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "wifi stop: %s", esp_err_to_name(ret));
    }
    ret = esp_wifi_set_mode(mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    wifi_config_t cfg = {0};
    if (ap_cfg) {
        memcpy(&cfg.ap, ap_cfg, sizeof(cfg.ap));
        ret = esp_wifi_set_config(WIFI_IF_AP, &cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set ap config failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    if (sta_cfg) {
        memcpy(&cfg.sta, sta_cfg, sizeof(cfg.sta));
        ret = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "set sta config failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

/* 按配置设置 STA 静态 IP 或 DHCP */
static esp_err_t apply_sta_ip(const wifi_config_data_t *cfg)
{
    if (cfg->ip_mode == IP_MODE_STATIC) {
        esp_netif_ip_info_t ip_info = {0};
        bool dns_ok = false;

        if (esp_netif_str_to_ip4(cfg->ip, &ip_info.ip) != ESP_OK ||
            esp_netif_str_to_ip4(cfg->gateway, &ip_info.gw) != ESP_OK) {
            ESP_LOGE(TAG, "invalid static ip/gateway");
            return ESP_ERR_INVALID_ARG;
        }
        /* 掩码缺省 255.255.255.0 */
        if (cfg->netmask[0] == '\0' ||
            esp_netif_str_to_ip4(cfg->netmask, &ip_info.netmask) != ESP_OK) {
            esp_netif_str_to_ip4(AP_NETMASK, &ip_info.netmask);
        }

        esp_netif_dhcpc_stop(s_sta_netif);   /* 先停 DHCP */
        esp_netif_set_ip_info(s_sta_netif, &ip_info);

        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (cfg->dns[0] != '\0' && esp_netif_str_to_ip4(cfg->dns, &dns.ip.u_addr.ip4) == ESP_OK) {
            dns_ok = true;
        } else {
            dns.ip.u_addr.ip4 = ip_info.gw;      /* 缺省用网关做 DNS */
            dns_ok = true;
        }
        if (dns_ok) {
            esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "static ip %s mask %s gw %s",
                 cfg->ip, cfg->netmask[0] ? cfg->netmask : "255.255.255.0", cfg->gateway);
    } else {
        esp_netif_dhcpc_start(s_sta_netif);  /* 已启动则返回 ALREADY_STARTED，忽略 */
        ESP_LOGI(TAG, "use DHCP");
    }
    return ESP_OK;
}

/* 构造 SoftAP 配置：SSID = <前缀>-<MAC后4位> */
static void build_ap_config(wifi_ap_config_t *ap_cfg)
{
    memset(ap_cfg, 0, sizeof(*ap_cfg));

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X",
             CONFIG_PROV_AP_SSID_PREFIX, mac[4], mac[5]);

    strlcpy((char *)ap_cfg->ssid, ssid, sizeof(ap_cfg->ssid));
    strlcpy(s_ap_ssid, ssid, sizeof(s_ap_ssid));
    ap_cfg->ssid_len = strlen(ssid);
    ap_cfg->channel = CONFIG_PROV_AP_CHANNEL;
    ap_cfg->max_connection = 4;
#ifdef CONFIG_PROV_AP_PASSWORD
    if (strlen(CONFIG_PROV_AP_PASSWORD) > 0) {
        ap_cfg->authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)ap_cfg->password, CONFIG_PROV_AP_PASSWORD,
                sizeof(ap_cfg->password));
    } else
#endif
    {
        ap_cfg->authmode = WIFI_AUTH_OPEN;
    }
    ESP_LOGI(TAG, "SoftAP ssid=%s auth=%d", ssid, ap_cfg->authmode);
}

static void build_sta_config(const wifi_config_data_t *cfg, wifi_sta_config_t *sta_cfg)
{
    memset(sta_cfg, 0, sizeof(*sta_cfg));
    strlcpy((char *)sta_cfg->ssid, cfg->ssid, sizeof(sta_cfg->ssid));
    if (cfg->password[0]) {
        strlcpy((char *)sta_cfg->password, cfg->password, sizeof(sta_cfg->password));
    }
    sta_cfg->scan_method = WIFI_FAST_SCAN;        /* 已指定 SSID，快速扫描即可 */
    sta_cfg->sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta_cfg->threshold.authmode = WIFI_AUTH_OPEN;  /* 接受开放网络及以下 */
}

/* ------------------------------------------------------------------ */
/* 连接超时 / 重试定时器                                                 */
/* ------------------------------------------------------------------ */

static void on_conn_timeout(void *arg)
{
    if (s_state != WIFI_MGR_STATE_CONNECTING) {
        return;
    }
    ESP_LOGW(TAG, "connect timeout (%d/%d)", s_retry_count + 1, CONN_RETRY_MAX);
    s_retry_count++;
    if (s_retry_count >= CONN_RETRY_MAX) {
        wifi_mgr_enter_config_mode();
        return;
    }
    esp_wifi_disconnect();    /* 清理挂起的连接 */
    esp_wifi_connect();
    esp_timer_start_once(s_conn_timeout_timer, CONN_TIMEOUT_MS * 1000);
}

static void on_conn_retry(void *arg)
{
    if (s_state != WIFI_MGR_STATE_CONNECTING) {
        return;
    }
    ESP_LOGI(TAG, "retry connect (%d/%d)", s_retry_count, CONN_RETRY_MAX);
    esp_err_t ret = esp_wifi_connect();
    if (ret == ESP_OK) {
        esp_timer_start_once(s_conn_timeout_timer, CONN_TIMEOUT_MS * 1000);
    }
}

/* STA 断开超过阈值仍没连上 -> 开启 AP 兜底，防止 AP/STA 同时失联 */
static void on_ap_fallback_timeout(void *arg)
{
    if (s_state == WIFI_MGR_STATE_CONNECTED) {
        return;   /* 已重连，无需兜底 */
    }
    if (s_ap_on) {
        return;   /* AP 已开 */
    }
    ESP_LOGW(TAG, "STA down too long, enabling fallback AP");
    wifi_mgr_ap_enable();
}

/* 在 STA 断开且 AP 关闭时，延迟开启 AP 兜底（短暂抖动不触发） */
static void arm_ap_fallback(void)
{
    uint32_t delay_s = s_cfg.ap_fallback_delay;
    if (delay_s == 0) {
        delay_s = 15;   /* 缺省 15 秒 */
    }
    esp_timer_stop(s_ap_fallback_timer);
    esp_timer_start_once(s_ap_fallback_timer, delay_s * 1000000ULL);
}

static void arm_retry_timer(void)
{
    esp_timer_stop(s_conn_retry_timer);
    esp_timer_start_once(s_conn_retry_timer, CONN_RETRY_DELAY_MS * 1000);
}

static void disarm_conn_timers(void)
{
    esp_timer_stop(s_conn_timeout_timer);
    esp_timer_stop(s_conn_retry_timer);
    esp_timer_stop(s_ap_fallback_timer);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t wifi_mgr_init(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) return ret;

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) {
        return ESP_FAIL;
    }

    /* SoftAP 固定 192.168.4.1/24 */
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_str_to_ip4(AP_IP_ADDR, &ip_info.ip);
    esp_netif_str_to_ip4(AP_NETMASK, &ip_info.netmask);
    esp_netif_str_to_ip4(AP_GATEWAY, &ip_info.gw);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) return ret;

    /* 不把 Wi-Fi 自身配置写入 NVS，避免与我们自己的配置冲突 */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    /* C5 双频：2.4G + 5G */
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    /* 配网/扫描期间关闭省电，保证响应速度 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL);

    esp_timer_create_args_t targs = {
        .callback = on_conn_timeout,
        .name = "conn_timeout",
    };
    esp_timer_create(&targs, &s_conn_timeout_timer);
    targs.callback = on_conn_retry;
    targs.name = "conn_retry";
    esp_timer_create(&targs, &s_conn_retry_timer);
    targs.callback = on_ap_fallback_timeout;
    targs.name = "ap_fallback";
    esp_timer_create(&targs, &s_ap_fallback_timer);

    return ESP_OK;
}

void wifi_mgr_set_ap_cb(wifi_mgr_ap_cb_t cb)
{
    s_ap_cb = cb;
}

void wifi_mgr_start(void)
{
    bool configured = config_store_load(&s_cfg);
    if (configured) {
        wifi_mgr_connect(&s_cfg);
    } else {
        wifi_mgr_enter_config_mode();
    }
}

void wifi_mgr_enter_config_mode(void)
{
    set_state(WIFI_MGR_STATE_CONFIG);
    s_retry_count = 0;
    s_ap_clients = 0;
    disarm_conn_timers();
    s_scanning = false;
    update_led();   /* 橙色 */

    wifi_ap_config_t ap_cfg;
    build_ap_config(&ap_cfg);
    apply_wifi_mode(WIFI_MODE_APSTA, &ap_cfg, NULL);
    /* AP_START 事件里会回调 s_ap_cb(true) 启动 Web 服务器 */
}

void wifi_mgr_connect(const wifi_config_data_t *cfg)
{
    if (cfg->ssid[0] == '\0') {
        ESP_LOGW(TAG, "empty ssid, stay in config mode");
        wifi_mgr_enter_config_mode();
        return;
    }

    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    s_ap_off_after_connect = cfg->ap_off;
    s_retry_count = 0;
    s_scanning = false;
    disarm_conn_timers();

    set_state(WIFI_MGR_STATE_CONNECTING);

    apply_sta_ip(cfg);

    wifi_sta_config_t sta_cfg;
    build_sta_config(cfg, &sta_cfg);

    /* 连接期间 SoftAP 始终开启：即使 ap_off=true，也等“联网成功”后才关 AP。
     * 这样 SSID/密码填错时，手机仍可连热点随时修正，不会失联。
     * （GOT_IP 事件里按 s_ap_off_after_connect 决定是否关闭 AP） */
    wifi_ap_config_t ap_cfg;
    build_ap_config(&ap_cfg);
    apply_wifi_mode(WIFI_MODE_APSTA, &ap_cfg, &sta_cfg);
    ESP_LOGI(TAG, "connecting to %s (AP %s after connect)",
             cfg->ssid, cfg->ap_off ? "off" : "on");

    /* STA_START 事件后开始连接；这里先启动超时定时器作为兜底 */
    esp_timer_start_once(s_conn_timeout_timer, CONN_TIMEOUT_MS * 1000);
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    return s_state;
}

bool wifi_mgr_is_ap_on(void)
{
    return s_ap_on;
}

bool wifi_mgr_is_scanning(void)
{
    return s_scanning;
}

void wifi_mgr_get_sta_ip(char *buf, size_t len)
{
    strlcpy(buf, s_sta_ip, len);
}

uint8_t wifi_mgr_get_ap_clients(void)
{
    return s_ap_clients;
}

void wifi_mgr_get_ap_ssid(char *buf, size_t len)
{
    strlcpy(buf, s_ap_ssid, len);
}

esp_netif_t *wifi_mgr_get_sta_netif(void)
{
    return s_sta_netif;
}

esp_err_t wifi_mgr_get_sta_ap_info(wifi_ap_record_t *ap_info)
{
    memset(ap_info, 0, sizeof(*ap_info));
    return esp_wifi_sta_get_ap_info(ap_info);
}

/* 运行时开启 SoftAP：保持 STA 连接不变，仅把模式切为 APSTA 并启动 AP */
esp_err_t wifi_mgr_ap_enable(void)
{
    if (s_ap_on) {
        return ESP_OK;
    }
    wifi_ap_config_t ap_cfg;
    build_ap_config(&ap_cfg);

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ap enable set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    wifi_config_t cfg = {0};
    memcpy(&cfg.ap, &ap_cfg, sizeof(ap_cfg));
    ret = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ap enable set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SoftAP enabled by user");
    return ESP_OK;
}

/* 运行时关闭 SoftAP：仅 STA 已联网时允许。
 * 连接中禁止关闭 —— 否则 STA 重试期间 AP 也关，设备将完全失联。 */
esp_err_t wifi_mgr_ap_disable(void)
{
    if (!s_ap_on) {
        return ESP_OK;
    }
    if (s_state != WIFI_MGR_STATE_CONNECTED) {
        ESP_LOGW(TAG, "refuse AP off: STA not connected");
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t empty_cfg = {0};   /* AP ssid 为空 -> AP 不启动 */
    esp_wifi_set_config(WIFI_IF_AP, &empty_cfg);
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ap disable set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SoftAP disabled by user");
    return ESP_OK;
}

esp_err_t wifi_mgr_scan_async(uint16_t max_aps, wifi_mgr_scan_done_cb_t cb)
{
    if (s_scanning) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_max_aps = max_aps > 0 ? max_aps : SCAN_MAX_APS;
    s_scan_done_cb = cb;
    s_scanning = true;

    esp_err_t ret = esp_wifi_scan_start(NULL, false);   /* 默认扫描全部信道（双频） */
    if (ret != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_scan_get_results(wifi_ap_record_t **records, uint16_t *count)
{
    uint16_t ap_num = 0;
    esp_err_t ret = esp_wifi_scan_get_ap_num(&ap_num);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ap_num > s_scan_max_aps) {
        ap_num = s_scan_max_aps;
    }
    if (ap_num == 0) {
        *records = NULL;
        *count = 0;
        return ESP_OK;
    }
    wifi_ap_record_t *buf = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    uint16_t num = ap_num;
    ret = esp_wifi_scan_get_ap_records(&num, buf);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }
    *records = buf;
    *count = num;
    return ESP_OK;
}

const char *wifi_mgr_band_of_channel(uint8_t channel)
{
    if (channel >= 1 && channel <= 14) {
        return "2.4G";
    }
    return "5G";
}

/* ------------------------------------------------------------------ */
/* 事件处理                                                             */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        /* STA 接口已启动，发起连接 */
        if (s_state == WIFI_MGR_STATE_CONNECTING) {
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(ret));
            }
        }
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "STA connected to AP");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected, reason=%d", ev->reason);
        if (s_state != WIFI_MGR_STATE_CONNECTING && s_state != WIFI_MGR_STATE_CONNECTED) {
            break;
        }
        disarm_conn_timers();
        s_retry_count++;
        if (s_retry_count >= CONN_RETRY_MAX) {
            ESP_LOGW(TAG, "max retries reached, enter config mode");
            wifi_mgr_enter_config_mode();
        } else {
            /* 关键：切回 CONNECTING，否则 on_conn_retry 会因 state != CONNECTING
             * 提前返回，导致既不开 AP 也不重连，设备永久失联 */
            set_state(WIFI_MGR_STATE_CONNECTING);
            arm_retry_timer();   /* 稍后重连 */
            /* AP 兜底：连续断开超过阈值（默认15s）才开 AP，短暂抖动不触发 */
            if (!s_ap_on) {
                arm_ap_fallback();
            }
        }
        update_led();   /* 断网：回到橙色/蓝色 */
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
        s_ap_clients++;
        ESP_LOGI(TAG, "AP client connected (%d)", s_ap_clients);
        update_led();   /* 蓝色 */
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
        ESP_LOGI(TAG, "AP client disconnected (%d)", s_ap_clients);
        update_led();
        break;

    case WIFI_EVENT_AP_START:
        s_ap_on = true;
        ESP_LOGI(TAG, "SoftAP started");
        if (s_ap_cb) {
            s_ap_cb(true);
        }
        break;

    case WIFI_EVENT_AP_STOP:
        s_ap_on = false;
        ESP_LOGI(TAG, "SoftAP stopped");
        if (s_ap_cb) {
            s_ap_cb(false);
        }
        break;

    case WIFI_EVENT_SCAN_DONE:
        s_scanning = false;
        if (s_scan_done_cb) {
            wifi_mgr_scan_done_cb_t cb = s_scan_done_cb;
            s_scan_done_cb = NULL;
            cb();
        }
        break;

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&ev->ip_info.ip, s_sta_ip, sizeof(s_sta_ip));
        ESP_LOGI(TAG, "GOT IP: %s", s_sta_ip);
        disarm_conn_timers();
        esp_timer_stop(s_ap_fallback_timer);   /* 已重连，取消 AP 兜底 */
        set_state(WIFI_MGR_STATE_CONNECTED);
        s_retry_count = 0;   /* 联网成功，重置重试预算 */
        update_led();   /* 绿色：联网成功 */

        /* 按配置关闭 SoftAP */
        if (s_ap_off_after_connect && s_ap_on) {
            ESP_LOGI(TAG, "turning off SoftAP as configured");
            wifi_config_t empty_cfg = {0};   /* AP ssid 为空 -> AP 不启动 */
            esp_wifi_set_config(WIFI_IF_AP, &empty_cfg);
            esp_wifi_set_mode(WIFI_MODE_STA);
            /* AP_STOP 事件触发 s_ap_cb(false) */
        }
        break;
    }

    case IP_EVENT_STA_LOST_IP:
        ESP_LOGW(TAG, "STA lost IP");
        break;

    default:
        break;
    }
}
