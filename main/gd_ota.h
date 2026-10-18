/*
 * gd_ota.h - 通过 ESP32 向 GD32 推送固件（OTA）
 *
 * 物理：ESP32 UART1 (GPIO8/9) 接到 GD32 USART2 (PA2/PA3)，与 Modbus 共用。
 * 流程：
 *   1. 以 Modbus RTU 单寄存器写触发 GD32 进入 OTA（写从站寄存器 trigger_reg）。
 *   2. 等待 GD32 bootloader 在 OTA UART 上回 READY。
 *   3. 按 GD32 ota_proto.h 协议发送 SYNC/START/DATA/END 帧。
 *   4. 等待最终确认，更新进度。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* --- 与 GD32 ota_proto.h 保持一致 --- */
#define GDOTA_MAGIC       0xA5u
#define GDOTA_FRAME_SYNC  0x00u
#define GDOTA_FRAME_START 0x01u
#define GDOTA_FRAME_DATA  0x02u
#define GDOTA_FRAME_END   0x03u
#define GDOTA_FRAME_ABORT 0x04u
#define GDOTA_RESP_TYPE   0x05u
#define GDOTA_ST_OK       0x00u
#define GDOTA_ST_CRC      0x01u
#define GDOTA_ST_SEQ      0x02u
#define GDOTA_ST_BUSY     0x03u
#define GDOTA_ST_ABORT    0x04u
#define GDOTA_ST_READY    0x05u

/* 每次 DATA 帧最大载荷（与 GD32 OTA_BUF_SIZE 一致） */
#define GDOTA_DATA_CHUNK  512u
/* 固件最大字节数：受 GD32 bootloader RAM 缓冲限制 */
#define GDOTA_MAX_SIZE    (64u * 1024u)

typedef struct {
    bool     enabled;        /* 是否启用 GD32 OTA 推送 */
    uint8_t  uart_num;       /* 推送固件用的 UART（UART_NUM_2 等） */
    int8_t   tx_gpio;        /* UART TX GPIO */
    int8_t   rx_gpio;        /* UART RX GPIO */
    uint32_t baud;           /* 波特率，默认 115200 */
    uint8_t  slave;          /* 触发用的 Modbus 从站地址（默认 1） */
    uint16_t trigger_reg;    /* 触发寄存器（默认 49） */
    uint16_t trigger_value;  /* 触发值（默认 0x4F54） */
} gdota_config_t;

typedef enum {
    GDOTA_IDLE = 0,   /* 空闲 */
    GDOTA_BUSY,       /* 上传/推送中 */
    GDOTA_DONE,       /* 成功 */
    GDOTA_ERROR,      /* 失败 */
} gdota_phase_t;

void gd_ota_init(void);
void gd_ota_load_config(gdota_config_t *cfg);
esp_err_t gd_ota_save_config(const gdota_config_t *cfg);

/* 受理一次升级：拷贝固件并在后台任务中推送。返回 ESP_OK 表示已开始。 */
esp_err_t gd_ota_begin(const uint8_t *fw, size_t len);

/* 查询当前状态；percent 为 0-100，仅 BUSY/DONE/ERROR 时有效。 */
bool gd_ota_get_state(gdota_phase_t *phase, int *percent);

/* 最近一次失败的简短原因（仅 ERROR 时有意义），用于网页显示 */
const char *gd_ota_get_error(void);
