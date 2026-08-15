/*
 * wifi_mgr.h - Wi-Fi 状态机：SoftAP 配网模式 / STA 连接模式
 *
 * 职责：
 *   - 初始化 Wi-Fi（双频 2.4G+5G）
 *   - 进入 SoftAP 配网模式（Web 配网页面）
 *   - 使用保存的配置连接 STA（支持 DHCP / 静态 IP）
 *   - 连接失败自动回退到配网模式
 *   - 双频 AP 扫描
 *   - 可选：连接成功后关闭 SoftAP
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_wifi.h"

#include "config_store.h"

typedef enum {
    WIFI_MGR_STATE_UNINIT = 0, /* 未初始化 */
    WIFI_MGR_STATE_CONFIG,     /* SoftAP 配网模式 */
    WIFI_MGR_STATE_CONNECTING, /* STA 连接中 */
    WIFI_MGR_STATE_CONNECTED,  /* STA 已连接（获得 IP） */
} wifi_mgr_state_t;

/* SoftAP 开关回调：ap_on=true 表示 AP 已启动，false 表示已停止。
 * 用于外部模块（如 Web 服务器）跟随 AP 生命周期启停。 */
typedef void (*wifi_mgr_ap_cb_t)(bool ap_on);

/* 扫描完成回调 */
typedef void (*wifi_mgr_scan_done_cb_t)(void);

esp_err_t wifi_mgr_init(void);
void wifi_mgr_start(void);

/* 使用给定配置连接 STA（保存配置并应用） */
void wifi_mgr_connect(const wifi_config_data_t *cfg);

/* 进入 SoftAP 配网模式 */
void wifi_mgr_enter_config_mode(void);

wifi_mgr_state_t wifi_mgr_get_state(void);
bool wifi_mgr_is_ap_on(void);
bool wifi_mgr_is_scanning(void);

void wifi_mgr_get_sta_ip(char *buf, size_t len);

/* 运行时开关 SoftAP（不写入 NVS，重启后按保存的配置恢复） */
esp_err_t wifi_mgr_ap_enable(void);
esp_err_t wifi_mgr_ap_disable(void);

/* 网络信息查询（供 Web 状态页使用） */
uint8_t wifi_mgr_get_ap_clients(void);
void wifi_mgr_get_ap_ssid(char *buf, size_t len);
esp_netif_t *wifi_mgr_get_sta_netif(void);
esp_err_t wifi_mgr_get_sta_ap_info(wifi_ap_record_t *ap_info);   /* rssi/ssid 等，未连接返回错误 */

/* 异步双频扫描，完成后回调 cb（可为 NULL） */
esp_err_t wifi_mgr_scan_async(uint16_t max_aps, wifi_mgr_scan_done_cb_t cb);

/* 获取扫描结果；调用者负责 free(records) */
esp_err_t wifi_mgr_scan_get_results(wifi_ap_record_t **records, uint16_t *count);

void wifi_mgr_set_ap_cb(wifi_mgr_ap_cb_t cb);

/* 将信道号映射为频段字符串（"2.4G"/"5G"） */
const char *wifi_mgr_band_of_channel(uint8_t channel);
