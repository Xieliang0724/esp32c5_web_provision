/*
 * web_server.h - Web 配网服务器
 *
 * 路由：
 *   GET  /              配网页面（内嵌 HTML）
 *   GET  /api/status    设备状态
 *   GET  /api/scan      扫描 Wi-Fi（双频），返回 SSID 列表
 *   POST /api/config    提交配网配置
 *   POST /api/reset     清除配置并回到配网模式
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);
bool web_server_is_running(void);
