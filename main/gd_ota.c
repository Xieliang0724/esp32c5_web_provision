/*
 * gd_ota.c - GD32 固件 OTA 推送实现
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "gd_ota.h"
#include "modbus_gw.h"

static const char *TAG = "gd_ota";

#define GDOTA_NS       "gd_ota"
#define KEY_ENABLED    "enabled"
#define KEY_UART       "uart"
#define KEY_TX         "tx"
#define KEY_RX         "rx"
#define KEY_BAUD       "baud"
#define KEY_SLAVE      "slave"
#define KEY_REG        "reg"
#define KEY_VALUE      "value"

#define GDOTA_RX_BUF   1024
#define GDOTA_TX_BUF   1024
#define GDOTA_UART_Q   8

#define TRIGGER_TIMEOUT_MS  2000   /* 等待 Modbus 触发应答 */
#define READY_TIMEOUT_MS    10000  /* 等待 bootloader 启动并完成握手 */
#define SYNC_RETRY_INTERVAL_MS 250
#define RESP_TIMEOUT_MS     1500   /* 等待每帧应答 */
#define OTA_COMMIT_TIMEOUT_MS 30000 /* 擦除/写入 App 后等待最终确认 */

static gdota_phase_t s_phase = GDOTA_IDLE;
static int s_percent = 0;
static char s_error[128] = "";
static int  s_last_resp_bytes = 0;

/* ------------------------------------------------------------------ */
/* 配置                                                                 */
/* ------------------------------------------------------------------ */

void gd_ota_load_config(gdota_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /* OTA 固定复用 modbus 那一路 UART：UART1 / GPIO8 / GPIO9 / 115200 */
    cfg->enabled = true;
    cfg->uart_num = 1;      /* UART_NUM_1，与 modbus 网关同路 */
    cfg->tx_gpio = 8;
    cfg->rx_gpio = 9;
    cfg->baud = 115200;
    cfg->slave = 1;
    cfg->trigger_reg = 49;
    cfg->trigger_value = 0x4F54;

    nvs_handle_t h;
    if (nvs_open(GDOTA_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, KEY_ENABLED, &u8) == ESP_OK) {
        cfg->enabled = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_SLAVE, &u8) == ESP_OK) {
        cfg->slave = u8;
    }
    if (nvs_get_u16(h, KEY_REG, &u16) == ESP_OK) {
        cfg->trigger_reg = u16;
    }
    if (nvs_get_u16(h, KEY_VALUE, &u16) == ESP_OK) {
        cfg->trigger_value = u16;
    }
    nvs_close(h);
}

esp_err_t gd_ota_save_config(const gdota_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(GDOTA_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(h, KEY_ENABLED, cfg->enabled ? 1 : 0);
    if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_UART, cfg->uart_num);
    if (ret == ESP_OK) ret = nvs_set_i8(h, KEY_TX, cfg->tx_gpio);
    if (ret == ESP_OK) ret = nvs_set_i8(h, KEY_RX, cfg->rx_gpio);
    if (ret == ESP_OK) ret = nvs_set_u32(h, KEY_BAUD, cfg->baud);
    if (ret == ESP_OK) ret = nvs_set_u8(h, KEY_SLAVE, cfg->slave);
    if (ret == ESP_OK) ret = nvs_set_u16(h, KEY_REG, cfg->trigger_reg);
    if (ret == ESP_OK) ret = nvs_set_u16(h, KEY_VALUE, cfg->trigger_value);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

void gd_ota_init(void)
{
    gdota_config_t cfg;
    gd_ota_load_config(&cfg);
    ESP_LOGI(TAG, "GD OTA config: uart=%u tx=%d rx=%d baud=%lu slave=%u reg=%u val=0x%04X",
             cfg.uart_num, cfg.tx_gpio, cfg.rx_gpio, (unsigned long)cfg.baud,
             cfg.slave, cfg.trigger_reg, cfg.trigger_value);
}

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

/* 组一帧：frame[0]=magic, ty, seq, lenLo, lenHi, payload, crcLo, crcHi */
static size_t build_frame(uint8_t *out, uint8_t type, uint8_t seq,
                          const uint8_t *payload, size_t len)
{
    out[0] = GDOTA_MAGIC;
    out[1] = type;
    out[2] = seq;
    out[3] = (uint8_t)(len & 0xFF);
    out[4] = (uint8_t)((len >> 8) & 0xFF);
    if (len) memcpy(out + 5, payload, len);
    uint16_t c = crc16_modbus(out, 5 + len);
    out[5 + len] = (uint8_t)(c & 0xFF);
    out[6 + len] = (uint8_t)(c >> 8);
    return 7 + len;
}

/* 解析应答帧：A5 05 status seq crcLo crcHi（6 字节），返回 status 或 -1 */
static int parse_resp(const uint8_t *buf, size_t len)
{
    if (len < 6 || buf[0] != GDOTA_MAGIC || buf[1] != GDOTA_RESP_TYPE) return -1;
    uint16_t c = crc16_modbus(buf, 4);
    uint16_t rx = buf[4] | (buf[5] << 8);
    if (c != rx) return -2;
    return buf[2];
}

/* 从 UART 读一帧应答，返回 status（0xFF 表示成功读到），-1 超时/错 */
static int wait_resp(uart_port_t uart, uint8_t *resp, size_t maxlen, uint32_t timeout_ms)
{
    size_t got = 0;
    s_last_resp_bytes = 0;
    uint32_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    while (esp_timer_get_time() / 1000 < deadline) {
        int n = uart_read_bytes(uart, resp + got, maxlen - got,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            got += n;
            if (got >= 6) {
                int status = parse_resp(resp, got);
                if (status != -1) return status;
                if (got > 6) return -3;   /* 丢弃多余帧 */
            }
        } else if (got >= 6) {
            return parse_resp(resp, got);
        }
    }
    s_last_resp_bytes = (int)got;
    return -1;
}

/* ------------------------------------------------------------------ */
/* OTA 任务                                                            */
/* ------------------------------------------------------------------ */

static gdota_config_t s_cfg;
static uint8_t *s_fw = NULL;
static size_t s_fw_len = 0;

static void ota_task(void *arg)
{
    s_phase = GDOTA_BUSY;
    s_percent = 0;
    uart_port_t uart = (uart_port_t)s_cfg.uart_num;
    QueueHandle_t evt;
    bool uart_installed = false;
    bool gateway_stopped = false;

    /* 1. 触发：借 modbus 网关正在用的 UART1 写触发寄存器，确认 GD32 收到 */
    int trig_ok = modbus_gw_write_holding(s_cfg.slave, s_cfg.trigger_reg, s_cfg.trigger_value);
    ESP_LOGI(TAG, "trigger ok=%d", trig_ok);
    if (!trig_ok) {
        snprintf(s_error, sizeof(s_error), "触发未生效：modbus 网关未运行或从站无响应");
        goto fail;
    }

    /* 给 GD32 时间复位进入 bootloader */
    vTaskDelay(pdMS_TO_TICKS(100));
    /* 停掉网关，彻底释放 UART1，由 gd_ota 全新重装，确保 RX 激活 */
    modbus_gw_stop();
    gateway_stopped = true;

    uart_config_t uc = {
        .baud_rate = s_cfg.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(uart, GDOTA_RX_BUF, GDOTA_TX_BUF, GDOTA_UART_Q, &evt, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uart install failed");
        snprintf(s_error, sizeof(s_error), "UART 初始化失败");
        goto fail;
    }
    uart_installed = true;
    uart_param_config(uart, &uc);
    uart_set_pin(uart, s_cfg.tx_gpio, s_cfg.rx_gpio, -1, -1);
    uart_flush_input(uart);

    /* 2. SYNC 握手：bootloader 收 SYNC 后回 READY，再开始传输 */
    s_percent = 5;
    uint8_t resp[64];
    uint8_t frame[GDOTA_DATA_CHUNK + 8];
    uint8_t start_payload[6];
    start_payload[0] = (uint8_t)(s_fw_len & 0xFF);
    start_payload[1] = (uint8_t)((s_fw_len >> 8) & 0xFF);
    start_payload[2] = (uint8_t)((s_fw_len >> 16) & 0xFF);
    start_payload[3] = (uint8_t)((s_fw_len >> 24) & 0xFF);
    uint16_t total_crc = crc16_modbus(s_fw, s_fw_len);
    start_payload[4] = (uint8_t)(total_crc & 0xFF);
    start_payload[5] = (uint8_t)((total_crc >> 8) & 0xFF);

    size_t n;
    int w = 0;
    int st = -1;
    uint32_t sync_attempts = 0;
    uint64_t ready_deadline_us =
        esp_timer_get_time() + (uint64_t)READY_TIMEOUT_MS * 1000ULL;

    while (esp_timer_get_time() < ready_deadline_us) {
        /* The app may still be erasing its update-status sector after the
         * Modbus trigger. Retry until the bootloader has actually started. */
        uart_flush_input(uart);
        n = build_frame(frame, GDOTA_FRAME_SYNC, 0, NULL, 0);
        w = uart_write_bytes(uart, frame, n);
        sync_attempts++;
        esp_err_t tx_ret = uart_wait_tx_done(uart, pdMS_TO_TICKS(200));
        if (w == (int)n && tx_ret == ESP_OK) {
            st = wait_resp(uart, resp, sizeof(resp), SYNC_RETRY_INTERVAL_MS);
            if (st == GDOTA_ST_READY || st >= 0) {
                break;
            }
        } else {
            ESP_LOGW(TAG, "SYNC send failed: write=%d/%u tx=%s",
                     w, (unsigned)n, esp_err_to_name(tx_ret));
            st = -4;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (st != GDOTA_ST_READY) {
        ESP_LOGE(TAG, "no bootloader READY after %u SYNC attempts (status=%d, rxbytes=%d)",
                 (unsigned)sync_attempts, st, s_last_resp_bytes);
        snprintf(s_error, sizeof(s_error),
                 "触发已确认，但 GD32 bootloader 未回 READY (status=%d, rx=%d字节, SYNC=%u)",
                 st, s_last_resp_bytes, (unsigned)sync_attempts);
        goto fail;
    }
    ESP_LOGI(TAG, "bootloader ready");
    uart_flush_input(uart);  /* discard any duplicate READY replies from retries */

    n = build_frame(frame, GDOTA_FRAME_START, 0, start_payload, sizeof(start_payload));
    uart_write_bytes(uart, frame, n); uart_wait_tx_done(uart, pdMS_TO_TICKS(200));
    st = wait_resp(uart, resp, sizeof(resp), RESP_TIMEOUT_MS);
    if (st != GDOTA_ST_OK) { ESP_LOGE(TAG, "start resp=%d", st); goto fail; }

    /* 3. DATA */
    size_t off = 0;
    uint8_t seq = 0;
    while (off < s_fw_len) {
        size_t chunk = s_fw_len - off;
        if (chunk > GDOTA_DATA_CHUNK) chunk = GDOTA_DATA_CHUNK;
        n = build_frame(frame, GDOTA_FRAME_DATA, seq, s_fw + off, chunk);
        uart_write_bytes(uart, frame, n); uart_wait_tx_done(uart, pdMS_TO_TICKS(500));
        st = wait_resp(uart, resp, sizeof(resp), RESP_TIMEOUT_MS);
        if (st != GDOTA_ST_OK) {
            ESP_LOGE(TAG, "data seq=%u resp=%d", seq, st);
            goto fail;
        }
        off += chunk;
        seq++;
        s_percent = 10 + (int)(((uint64_t)off * 90) / s_fw_len);
    }

    /* 4. END */
    n = build_frame(frame, GDOTA_FRAME_END, seq, NULL, 0);
    uart_write_bytes(uart, frame, n); uart_wait_tx_done(uart, pdMS_TO_TICKS(200));
    st = wait_resp(uart, resp, sizeof(resp), OTA_COMMIT_TIMEOUT_MS);
    if (st != GDOTA_ST_READY) { ESP_LOGE(TAG, "end resp=%d", st); goto fail; }

    s_percent = 100;
    s_phase = GDOTA_DONE;
    ESP_LOGW(TAG, "GD32 OTA complete");
    if (uart_installed) uart_driver_delete(uart);
    free(s_fw); s_fw = NULL;
    if (gateway_stopped) {
        modbus_gw_start();
    }
    vTaskDelete(NULL);

fail:
    s_phase = GDOTA_ERROR;
    ESP_LOGE(TAG, "GD32 OTA failed: %s", s_error[0] ? s_error : "(unknown)");
    if (s_fw) { free(s_fw); s_fw = NULL; }
    if (uart_installed) uart_driver_delete(uart);
    if (gateway_stopped) {
        modbus_gw_start();
    }
    vTaskDelete(NULL);
}

esp_err_t gd_ota_begin(const uint8_t *fw, size_t len)
{
    if (s_phase == GDOTA_BUSY) return ESP_ERR_INVALID_STATE;
    if (len == 0 || len > GDOTA_MAX_SIZE) return ESP_ERR_INVALID_SIZE;
    if (fw == NULL) return ESP_ERR_INVALID_ARG;
    s_error[0] = '\0';
    s_last_resp_bytes = 0;

    uint8_t *buf = malloc(len);
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, fw, len);

    gd_ota_load_config(&s_cfg);
    if (!s_cfg.enabled || s_cfg.tx_gpio < 0 || s_cfg.rx_gpio < 0) {
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    s_fw = buf;
    s_fw_len = len;
    s_phase = GDOTA_BUSY;
    s_percent = 0;
    if (xTaskCreate(ota_task, "gd_ota", 6144, NULL, 5, NULL) != pdPASS) {
        free(buf); s_fw = NULL; s_phase = GDOTA_ERROR;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool gd_ota_get_state(gdota_phase_t *phase, int *percent)
{
    if (phase) *phase = s_phase;
    if (percent) *percent = s_percent;
    return true;
}

const char *gd_ota_get_error(void)
{
    return s_error;
}
