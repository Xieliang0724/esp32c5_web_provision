/*
 * modbus_gw.h - Modbus RTU <-> Modbus TCP 网关
 *
 * 功能（协议转换，非透传）：
 *   - ESP32 作为 Modbus TCP 服务端，监听本地端口（默认 502）
 *   - 通过 UART1 与 GD32（Modbus RTU 从站）通信
 *   - TCP 请求(MBAP) -> 去掉 MBAP 头 -> 组 RTU 帧(补 CRC16) -> 发 UART1
 *   - UART1 响应 -> 校验 CRC -> 组 MBAP 帧(回填事务ID) -> 发回 TCP 客户端
 *   - 支持客户端 IP 白名单（空 = 无限制）
 *   - UART1 TX/RX GPIO、波特率、端口均可配置
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"   /* gw_config_t */

/* 初始化：加载已保存配置，若启用则启动网关 */
void modbus_gw_init(void);

/* 应用新配置（停止旧网关并重启） */
esp_err_t modbus_gw_reconfigure(const gw_config_t *cfg);

/* 获取当前运行配置 */
void modbus_gw_get_config(gw_config_t *cfg);

bool modbus_gw_is_running(void);
