/*
 * app_main.c - ESP32-C5 Web 配网固件入口
 *
 * 流程：
 *   1. 初始化 NVS / Wi-Fi / Web 服务器
 *   2. 若有已保存配置 -> 直接连接 STA（可选关闭 SoftAP）
 *   3. 若无配置或连接失败 -> 进入 SoftAP 配网模式（192.168.4.1）
 *
 * 复位按键（可选）：长按 CONFIG_PROV_RESET_GPIO 3 秒
 *   清除已保存配置并重启进入配网模式。
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"   /* esp_restart() (v6.0: 由 esp_restart.h 迁移至此) */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include "config_store.h"
#include "modbus_gw.h"
#include "rgb_led.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "app_main";

#ifndef CONFIG_PROV_RESET_GPIO
#define CONFIG_PROV_RESET_GPIO (-1)
#endif

#if CONFIG_PROV_RESET_GPIO >= 0

#define RESET_BTN_PRESS_MS 3000
#define RESET_BTN_POLL_MS  50

static void reset_btn_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_PROV_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    uint32_t pressed_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RESET_BTN_POLL_MS));
        /* BOOT 按键按下为低电平 */
        if (gpio_get_level(CONFIG_PROV_RESET_GPIO) == 0) {
            pressed_ms += RESET_BTN_POLL_MS;
            if (pressed_ms >= RESET_BTN_PRESS_MS) {
                ESP_LOGW(TAG, "reset button long-pressed, clearing config...");
                config_store_clear();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            pressed_ms = 0;
        }
    }
}

static void start_reset_btn_task(void)
{
    xTaskCreate(reset_btn_task, "reset_btn", 3072, NULL, 5, NULL);
}

#endif /* CONFIG_PROV_RESET_GPIO >= 0 */

/* Web 服务器常驻运行：SoftAP 开启时可经 192.168.4.1 访问；
 * 热点关闭（ap_off）后仍可经路由器分配的 IP 访问，方便再次配网。
 * 服务器在 app_main 中启动一次，不随 AP 开关启停。 */
static void ensure_web_server(void)
{
    esp_err_t ret = web_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed: %s", esp_err_to_name(ret));
    }
}

/* mDNS：局域网内可通过 http://esp32c5.local 访问配网页面 */
static void init_mdns(void)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mdns init failed: %s", esp_err_to_name(ret));
        return;
    }
    mdns_hostname_set("esp32c5");
    mdns_instance_name_set("ESP32-C5 Web Provision");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS ready: http://esp32c5.local");
}

/* OTA 回滚保护：确认当前固件可用，取消 bootloader 的自动回退倒计时。
 * 必须在核心服务起来后调用，否则崩溃循环时会一直卡在"待确认"状态。 */
static void confirm_ota_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image marked valid, rollback cancelled");
    }
}

/* 定时向 UART0（控制台口）打印网络状态，供 GD32 等下游设备解析，
 * 格式固定：[NET] sta=.. ssid=.. ip=.. gw=.. rssi=.. ap=.. port=.. client=..
 * 走 UART0 而非 UART1，避免与 modbus_gw 的 RTU 二进制帧混在一起。 */
static void net_status_log_task(void *arg)
{
    const char *STA_NAMES[] = {
        [WIFI_MGR_STATE_UNINIT]     = "UNINIT",
        [WIFI_MGR_STATE_CONFIG]     = "CONFIG",
        [WIFI_MGR_STATE_CONNECTING] = "CONNECTING",
        [WIFI_MGR_STATE_CONNECTED]  = "CONNECTED",
    };
    while (1) {
        wifi_mgr_state_t st = wifi_mgr_get_state();
        const char *sta_str = (st <= WIFI_MGR_STATE_CONNECTED) ? STA_NAMES[st] : "UNKNOWN";

        char ssid[33] = "-";
        char ip[32] = "0.0.0.0";
        char gw[32] = "0.0.0.0";
        int8_t rssi = 0;

        if (st == WIFI_MGR_STATE_CONNECTED) {
            wifi_mgr_get_sta_ip(ip, sizeof(ip));
            esp_netif_t *sta = wifi_mgr_get_sta_netif();
            if (sta) {
                esp_netif_ip_info_t ip_info = {0};
                if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
                    esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
                }
            }
            wifi_config_t wcfg = {0};
            if (esp_wifi_get_config(WIFI_IF_STA, &wcfg) == ESP_OK) {
                strlcpy(ssid, (char *)wcfg.sta.ssid, sizeof(ssid));
            }
            wifi_ap_record_t ap_info = {0};
            if (wifi_mgr_get_sta_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi;
            }
        }

        gw_config_t gwcfg;
        gw_config_load(&gwcfg);
        uint16_t port = (gwcfg.enabled && modbus_gw_is_running()) ? gwcfg.port : 0;

        char client_ip[16] = "0.0.0.0";
        modbus_gw_get_client_ip(client_ip, sizeof(client_ip));

        printf("[NET] sta=%s ssid=%s ip=%s gw=%s rssi=%d ap=%s port=%u client=%s\r\n",
               sta_str, ssid, ip, gw, rssi, wifi_mgr_is_ap_on() ? "ON" : "OFF", port, client_ip);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void start_net_status_log_task(void)
{
    xTaskCreate(net_status_log_task, "net_status_log", 3072, NULL, 3, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C5 web provisioning firmware starting");

    esp_err_t ret = config_store_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = wifi_mgr_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 网关必须在 wifi_mgr_init（esp_netif_init/lwIP）之后启动，
     * 否则 socket() 会因 lwIP 未初始化而断言崩溃 */
    modbus_gw_init();

#if CONFIG_PROV_LED_GPIO >= 0
    rgb_led_init();
    rgb_led_set_state(RGB_STATE_DEFAULT);   /* 初始橙色 */
#endif

    /* Web 服务器常驻：SoftAP 开启时可经 192.168.4.1 访问，
     * 热点关闭（ap_off）后仍可经路由器分配的 IP 访问，方便再次配网。 */
    ensure_web_server();
    init_mdns();

#if CONFIG_PROV_RESET_GPIO >= 0
    start_reset_btn_task();
#endif

    wifi_mgr_start();   /* 有配置 -> 连接；无配置 -> SoftAP 配网 */

    confirm_ota_valid();
    start_net_status_log_task();
}
