/*
 * rgb_led.h - 板载 RGB 状态灯（WS2812 可寻址灯珠, GPIO27）
 *
 * 状态颜色：
 *   RGB_STATE_DEFAULT   橙色 - AP/STA 均未连接（默认/配网待连接）
 *   RGB_STATE_AP_CLIENT 蓝色 - 有设备连上了 SoftAP（正在配网）
 *   RGB_STATE_CONNECTED 绿色 - STA 联网成功（已连上路由器）
 */
#pragma once

typedef enum {
    RGB_STATE_DEFAULT = 0,   /* 橙色 */
    RGB_STATE_AP_CLIENT,     /* 蓝色 */
    RGB_STATE_CONNECTED,     /* 绿色 */
} rgb_state_t;

/* 初始化 LED 驱动（WS2812 @ CONFIG_PROV_LED_GPIO） */
void rgb_led_init(void);

/* 设置指示灯状态（颜色） */
void rgb_led_set_state(rgb_state_t st);
