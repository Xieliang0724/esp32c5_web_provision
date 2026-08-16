/*
 * config_store.h - 配网配置的 NVS 持久化
 *
 * 保存字段：
 *   - ssid / password      : 目标路由器凭据
 *   - ip_mode              : 0 = DHCP 自动获取, 1 = 静态 IP
 *   - ip / netmask / gw / dns : 静态 IP 参数（字符串形式）
 *   - ap_off               : 1 = 连接成功后关闭 SoftAP, 0 = 保持 SoftAP
 *   - configured           : 是否已配置过（1/0）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CFG_SSID_MAX_LEN   33
#define CFG_PASS_MAX_LEN   65
#define CFG_IP_STR_MAX_LEN 16

typedef enum {
    IP_MODE_DHCP = 0,   /* 自动获取 */
    IP_MODE_STATIC = 1, /* 静态 IP */
} ip_mode_t;

typedef struct {
    bool    configured;                  /* 是否已有有效配置 */
    char    ssid[CFG_SSID_MAX_LEN];
    char    password[CFG_PASS_MAX_LEN];
    uint8_t ip_mode;                     /* ip_mode_t */
    char    ip[CFG_IP_STR_MAX_LEN];
    char    netmask[CFG_IP_STR_MAX_LEN];
    char    gateway[CFG_IP_STR_MAX_LEN];
    char    dns[CFG_IP_STR_MAX_LEN];
    bool    ap_off;                      /* 连接成功后是否关闭 SoftAP */
    uint16_t ap_fallback_delay;          /* STA 断开多少秒后开 AP 兜底，默认 15 */
} wifi_config_data_t;

/* 初始化 NVS 分区（幂等） */
esp_err_t config_store_init(void);

/* 从 NVS 加载配置，未配置时返回 false 且 cfg 清零 */
bool config_store_load(wifi_config_data_t *cfg);

/* 保存配置到 NVS */
esp_err_t config_store_save(const wifi_config_data_t *cfg);

/* 清除已保存的配置 */
esp_err_t config_store_clear(void);

/* ------------------------------------------------------------------ */
/* Modbus 网关配置（独立 NVS 命名空间 mb_gw）                           */
/* ------------------------------------------------------------------ */

typedef struct {
    bool     enabled;              /* 是否启用网关 */
    uint16_t port;                 /* Modbus TCP 端口，默认 502 */
    uint32_t baud;                 /* UART1 波特率 */
    int8_t   tx_gpio;              /* UART1 TX GPIO */
    int8_t   rx_gpio;              /* UART1 RX GPIO */
    char     client_ip[16];        /* 允许客户端 IP，空 = 无限制 */
    bool     tls_enabled;          /* 是否启用 TLS 监听（单向，固件内置证书） */
    uint16_t tls_port;             /* TLS 端口，默认 802 */
} gw_config_t;

/* 加载网关配置；无保存记录时返回默认值（未启用） */
void gw_config_load(gw_config_t *cfg);

/* 保存网关配置 */
esp_err_t gw_config_save(const gw_config_t *cfg);
