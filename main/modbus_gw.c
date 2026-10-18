/*
 * modbus_gw.c - Modbus RTU <-> TCP 网关实现
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"


#include "modbus_gw.h"

static const char *TAG = "mb_gw";

#define GW_UART_NUM        UART_NUM_1
#define UART_RX_BUF_SIZE   1024
#define UART_TX_BUF_SIZE   256
#define UART_EVT_QUEUE_LEN 20

#define MAX_FRAME_SIZE     260          /* RTU 最大帧：1 addr + 253 pdu + 2 crc */
#define MAX_TCP_CLIENTS    4
#define RTU_RESP_TIMEOUT_MS 1000        /* 等待从站响应超时 */
#define RX_IDLE_SYMBOLS    40           /* 帧间隙超时（符号数，约 4 字符） */
#define MAX_LISTENERS      1            /* 仅明文 Modbus TCP 监听 */

static gw_config_t s_cfg;
static bool s_running = false;

static QueueHandle_t s_uart_evt_queue = NULL;
static QueueHandle_t s_resp_queue = NULL;      /* 完整 RTU 帧队列（malloc 指针） */
static SemaphoreHandle_t s_uart_mutex = NULL;  /* 串行化 UART 事务（半双工） */
static SemaphoreHandle_t s_slot_mutex = NULL;  /* 客户端槽位分配互斥 */

static TaskHandle_t s_uart_rx_task = NULL;
static TaskHandle_t s_listener_tasks[MAX_LISTENERS] = {0};
static TaskHandle_t s_client_tasks[MAX_TCP_CLIENTS] = {0};
static int s_client_fds[MAX_TCP_CLIENTS] = {-1, -1, -1, -1};
static char s_client_ips[MAX_TCP_CLIENTS][16] = {{0}};  /* 与 s_client_fds 同下标，供 [NET] 日志展示 */
static int s_listen_fds[MAX_LISTENERS] = {-1};

/* 统一收发 */
static int mb_net_recv(int fd, uint8_t *buf, size_t len)
{
    return recv(fd, buf, len, 0);
}

static int mb_net_send(int fd, const uint8_t *buf, size_t len)
{
    return send(fd, buf, len, 0);
}

/* ------------------------------------------------------------------ */
/* Modbus CRC16                                                        */
/* ------------------------------------------------------------------ */

static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* 校验 RTU 帧 CRC（帧尾低字节在前） */
static bool rtu_crc_ok(const uint8_t *frame, size_t len)
{
    if (len < 4) {
        return false;
    }
    uint16_t crc = crc16_modbus(frame, len - 2);
    uint16_t rx = frame[len - 2] | (frame[len - 1] << 8);
    return crc == rx;
}

/* ------------------------------------------------------------------ */
/* UART 接收任务：按帧间隙切分 RTU 帧，完整帧送入响应队列               */
/* ------------------------------------------------------------------ */

static void uart_rx_task(void *arg)
{
    uint8_t *frame = malloc(MAX_FRAME_SIZE);
    if (!frame) {
        ESP_LOGE(TAG, "rx frame malloc failed");
        vTaskDelete(NULL);
        return;
    }
    size_t frame_len = 0;

    uart_event_t evt;
    while (s_running) {
        if (xQueueReceive(s_uart_evt_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case UART_DATA: {
            size_t avail = 0;
            uart_get_buffered_data_len(GW_UART_NUM, &avail);
            while (avail > 0 && frame_len < MAX_FRAME_SIZE) {
                size_t to_read = (avail < MAX_FRAME_SIZE - frame_len) ? avail : (MAX_FRAME_SIZE - frame_len);
                int n = uart_read_bytes(GW_UART_NUM, frame + frame_len, to_read, 0);
                if (n < 0) {
                    break;
                }
                frame_len += n;
                avail -= n;
            }
            /* 线路空闲超过阈值 -> RTU 帧完整 */
            if (evt.timeout_flag && frame_len > 0) {
                /* 队列条目 = [2字节帧长][帧数据]，便于事务端解析 */
                uint8_t *resp = malloc(frame_len + 2);
                if (resp) {
                    resp[0] = frame_len & 0xFF;
                    resp[1] = (frame_len >> 8) & 0xFF;
                    memcpy(resp + 2, frame, frame_len);
                    if (xQueueSend(s_resp_queue, &resp, 0) != pdTRUE) {
                        free(resp);   /* 队列满，丢弃并释放，防泄漏 */
                    }
                }
                frame_len = 0;
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            uart_flush_input(GW_UART_NUM);
            frame_len = 0;
            break;
        default:
            break;
        }
    }
    free(frame);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* RTU 事务：组帧 -> 发 UART -> 等响应（带校验）                        */
/* ------------------------------------------------------------------ */

static void log_frame_hex(const char *dir, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    /* 最多打印 64 字节，避免长帧刷屏 */
    size_t show = len < 64 ? len : 64;
    char hex[3 * 64 + 1] = {0};
    for (size_t i = 0; i < show; i++) {
        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "%s %uB: %s%s", dir, len, hex, len > show ? "..." : "");
}

static bool rtu_transaction(uint8_t uid, const uint8_t *pdu, size_t pdu_len,
                            uint8_t *resp_pdu, size_t *resp_pdu_len)
{
    if (pdu_len > MAX_FRAME_SIZE - 4) {
        return false;
    }
    uint8_t req[MAX_FRAME_SIZE];
    size_t req_len = 0;
    req[req_len++] = uid;
    memcpy(req + req_len, pdu, pdu_len);
    req_len += pdu_len;
    uint16_t crc = crc16_modbus(req, req_len);
    req[req_len++] = crc & 0xFF;        /* CRC 低字节在前 */
    req[req_len++] = (crc >> 8) & 0xFF;

    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "uart busy");
        return false;
    }

    /* 丢弃上一次残留帧（逐个取出释放，直接 Reset 会丢指针泄漏） */
    uint8_t *stale = NULL;
    while (xQueueReceive(s_resp_queue, &stale, 0) == pdTRUE) {
        free(stale);
    }

    int written = uart_write_bytes(GW_UART_NUM, req, req_len);
    if (written != req_len) {
        ESP_LOGW(TAG, "uart write only %d/%d", written, req_len);
        xSemaphoreGive(s_uart_mutex);
        return false;
    }
    uart_wait_tx_done(GW_UART_NUM, pdMS_TO_TICKS(200));
    log_frame_hex("TX RTU ->", req, req_len);

    /* 等待响应帧（校验地址 + CRC） */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RTU_RESP_TIMEOUT_MS);
    bool got = false;
    while (s_running && xTaskGetTickCount() < deadline) {
        uint8_t *resp = NULL;
        if (xQueueReceive(s_resp_queue, &resp, pdMS_TO_TICKS(50)) == pdTRUE && resp) {
            size_t rlen = resp[0] | (resp[1] << 8);   /* 帧长前缀 */
            uint8_t *rf = resp + 2;
            if (rlen >= 4 && rf[0] == uid && rtu_crc_ok(rf, rlen)) {
                *resp_pdu_len = rlen - 3;   /* 去掉 addr 和 2 字节 CRC */
                memcpy(resp_pdu, rf + 1, *resp_pdu_len);
                got = true;
                log_frame_hex("RX RTU <-", rf, rlen);
            } else {
                ESP_LOGW(TAG, "RX invalid frame (addr=%02X want=%02X crc=%s)",
                         rf[0], uid, rtu_crc_ok(rf, rlen) ? "ok" : "bad");
            }
            free(resp);
            if (got) {
                break;
            }
        }
    }
    xSemaphoreGive(s_uart_mutex);
    return got;
}

/* ------------------------------------------------------------------ */
/* TCP 客户端任务：读 MBAP 请求 -> RTU 事务 -> 回 MBAP 响应             */
/* ------------------------------------------------------------------ */

static void close_client(int idx)
{
    if (idx >= 0 && idx < MAX_TCP_CLIENTS && s_client_fds[idx] >= 0) {
        close(s_client_fds[idx]);
        s_client_fds[idx] = -1;
        s_client_tasks[idx] = NULL;
        s_client_ips[idx][0] = '\0';
    }
}

static int find_free_client_slot(void)
{
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] < 0) {
            return i;
        }
    }
    return -1;
}

/* 客户端任务参数 */
typedef struct {
    int  fd;
    int  slot;   /* 槽位下标，退出时按它清理（fd 号会被复用，不能作标识） */
} client_arg_t;

static void tcp_client_task(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;
    int slot = ca->slot;
    free(ca);

    uint8_t mbap[7];
    uint8_t buf[MAX_FRAME_SIZE];

    while (s_running) {
        /* 读 MBAP 头（7 字节） */
        size_t got = 0;
        while (got < sizeof(mbap) && s_running) {
            int n = mb_net_recv(fd, mbap + got, sizeof(mbap) - got);
            if (n <= 0) {
                goto client_done;   /* EOF / 连接关闭 / stop */
            }
            got += n;
        }
        uint16_t tid = (mbap[0] << 8) | mbap[1];
        uint16_t len = (mbap[4] << 8) | mbap[5];
        if (len < 1 || len > MAX_FRAME_SIZE - 7) {
            ESP_LOGW(TAG, "bad MBAP len=%u", len);
            goto client_done;
        }
        /* MBAP: uid 已在 7 字节头中 (mbap[6])，len = uid(1) + PDU，
         * 因此这里只需再读 len-1 字节的 PDU */
        uint8_t uid = mbap[6];
        size_t pdu_len = len - 1;
        got = 0;
        while (got < pdu_len && s_running) {
            int n = mb_net_recv(fd, buf + got, pdu_len - got);
            if (n <= 0) {
                goto client_done;
            }
            got += n;
        }
        uint8_t *pdu = buf;

        /* RTU 事务 */
        uint8_t resp_pdu[MAX_FRAME_SIZE];
        size_t resp_pdu_len = 0;
        if (rtu_transaction(uid, pdu, pdu_len, resp_pdu, &resp_pdu_len) && s_running) {
            /* 组 MBAP 响应：[tid][0x0000][len'=1+pdu_len][uid][pdu] */
            uint8_t rsp[7 + MAX_FRAME_SIZE];
            rsp[0] = (tid >> 8) & 0xFF;
            rsp[1] = tid & 0xFF;
            rsp[2] = 0;
            rsp[3] = 0;
            uint16_t rlen = 1 + resp_pdu_len;
            rsp[4] = (rlen >> 8) & 0xFF;
            rsp[5] = rlen & 0xFF;
            rsp[6] = uid;
            memcpy(rsp + 7, resp_pdu, resp_pdu_len);
            size_t total = 7 + resp_pdu_len;
            size_t sent = 0;
            while (sent < total && s_running) {
                int n = mb_net_send(fd, rsp + sent, total - sent);
                if (n <= 0) {
                    goto client_done;
                }
                sent += n;
            }
        }
        /* 无响应：继续等待下一个请求（客户端自行超时） */
    }

client_done:
    ESP_LOGI(TAG, "client disconnected");
    /* 按 slot 清理；fd 可能已被 gw_stop 关闭并复用给新连接，
     * 仅当槽位仍归属本任务的 fd 时才关闭 */
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    if (slot >= 0 && slot < MAX_TCP_CLIENTS && s_client_fds[slot] == fd) {
        close_client(slot);
    }
    xSemaphoreGive(s_slot_mutex);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* TCP 监听任务                                                        */
/* ------------------------------------------------------------------ */

/* 监听任务参数 */
typedef struct {
    uint16_t port;
    int      idx;   /* 对应 s_listen_fds 槽位 */
} listener_arg_t;

static void tcp_listener_task(void *arg)
{
    listener_arg_t *la = (listener_arg_t *)arg;
    uint16_t port = la->port;
    int idx = la->idx;
    free(la);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        s_listen_fds[idx] = -1;
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind port %u failed", port);
        close(fd);
        s_listen_fds[idx] = -1;
        vTaskDelete(NULL);
        return;
    }
    listen(fd, 4);
    s_listen_fds[idx] = fd;
    ESP_LOGI(TAG, "Modbus TCP listening on port %u", port);

    while (s_running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(fd, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            if (!s_running) {
                break;
            }
            continue;
        }

        /* 客户端 IP 白名单 */
        char peer_ip[16];
        strlcpy(peer_ip, inet_ntoa(peer.sin_addr), sizeof(peer_ip));
        if (s_cfg.client_ip[0] != '\0') {
            if (strcmp(peer_ip, s_cfg.client_ip) != 0) {
                ESP_LOGW(TAG, "reject client %s (allowlist %s)", peer_ip, s_cfg.client_ip);
                close(client);
                continue;
            }
            ESP_LOGI(TAG, "client %s allowed", peer_ip);
        } else {
            ESP_LOGI(TAG, "client connected (no allowlist)");
        }

        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        if (!s_running) {
            /* gw_stop 已在持锁清理槽位：此连接作废，避免停止后
             * 反把新 fd 写入数组、占用新实例的槽位 */
            xSemaphoreGive(s_slot_mutex);
            close(client);
            break;
        }
        int slot = find_free_client_slot();
        if (slot >= 0) {
            s_client_fds[slot] = client;
            strlcpy(s_client_ips[slot], peer_ip, sizeof(s_client_ips[slot]));
        }
        xSemaphoreGive(s_slot_mutex);
        if (slot < 0) {
            ESP_LOGW(TAG, "too many clients, reject");
            close(client);
            continue;
        }
        client_arg_t *ca = malloc(sizeof(client_arg_t));
        if (!ca) {
            /* 归还槽位，避免泄漏 */
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_client_fds[slot] = -1;
            s_client_tasks[slot] = NULL;
            s_client_ips[slot][0] = '\0';
            xSemaphoreGive(s_slot_mutex);
            ESP_LOGE(TAG, "client arg malloc failed");
            close(client);
            continue;
        }
        ca->fd = client;
        ca->slot = slot;
        if (xTaskCreate(tcp_client_task, "mb_tcp_client", 8192, ca, 6,
                        &s_client_tasks[slot]) != pdPASS) {
            /* 任务创建失败，归还槽位 */
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_client_fds[slot] = -1;
            s_client_tasks[slot] = NULL;
            s_client_ips[slot][0] = '\0';
            xSemaphoreGive(s_slot_mutex);
            ESP_LOGE(TAG, "client task create failed");
            free(ca);
            close(client);
            continue;
        }
    }

    s_listen_fds[idx] = -1;
    close(fd);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

static void gw_stop_ex(bool keep_uart)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* shutdown 先于 close：lwIP 下 close 无法唤醒阻塞在 accept()/recv()
     * 的任务，只 close 会泄漏监听/客户任务（每次 reconfigure 累积） */
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listen_fds[i] >= 0) {
            shutdown(s_listen_fds[i], SHUT_RDWR);
            close(s_listen_fds[i]);
            s_listen_fds[i] = -1;
        }
    }
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] >= 0) {
            shutdown(s_client_fds[i], SHUT_RDWR);
            close(s_client_fds[i]);
            s_client_fds[i] = -1;
            s_client_tasks[i] = NULL;
            s_client_ips[i][0] = '\0';
        }
    }
    xSemaphoreGive(s_slot_mutex);
    /* 唤醒 UART RX 任务 */
    if (s_uart_evt_queue) {
        uart_event_t evt = { .type = UART_FIFO_OVF };
        xQueueSend(s_uart_evt_queue, &evt, 0);
    }
    /* 任务退出后会自己 vTaskDelete(NULL)，这里只需等待其自然退出，
     * 绝不能再用句柄 vTaskDelete —— 双重删除会破坏 FreeRTOS 任务链表。
     * 等待时间覆盖最长的进行中 RTU 事务（RTU_RESP_TIMEOUT_MS）。 */
    vTaskDelay(pdMS_TO_TICKS(RTU_RESP_TIMEOUT_MS + 500));

    s_uart_rx_task = NULL;
    for (int i = 0; i < MAX_LISTENERS; i++) {
        s_listener_tasks[i] = NULL;
    }
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        s_client_tasks[i] = NULL;
    }
    /* 响应队列与互斥锁不随 stop 删除（首次创建后跨重启复用），
     * 避免仍在运行的事务访问已释放对象 */
    if (!keep_uart) {
        uart_driver_delete(GW_UART_NUM);
    }
    ESP_LOGI(TAG, "gateway stopped%s", keep_uart ? " (uart kept)" : "");
}

static void gw_stop(void)
{
    gw_stop_ex(false);
}

static esp_err_t gw_start(const gw_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    if (s_cfg.tx_gpio < 0 || s_cfg.rx_gpio < 0 || s_cfg.tx_gpio == s_cfg.rx_gpio) {
        ESP_LOGE(TAG, "invalid gpio tx=%d rx=%d", s_cfg.tx_gpio, s_cfg.rx_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cfg.port == 0) {
        s_cfg.port = 502;
    }
    if (s_cfg.baud == 0) {
        s_cfg.baud = 9600;
    }
    uart_config_t uart_cfg = {
        .baud_rate = s_cfg.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(GW_UART_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE,
                                        UART_EVT_QUEUE_LEN, &s_uart_evt_queue, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* 从 pause 恢复：UART1 驱动仍在，直接复用 */
        ESP_LOGI(TAG, "UART1 driver already installed, resuming");
        ret = ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uart_param_config(GW_UART_NUM, &uart_cfg);
    uart_set_pin(GW_UART_NUM, s_cfg.tx_gpio, s_cfg.rx_gpio, -1, -1);
    uart_set_rx_timeout(GW_UART_NUM, RX_IDLE_SYMBOLS);
    uart_flush_input(GW_UART_NUM);
    ESP_LOGI(TAG, "UART1 %d baud, TX=GPIO%d RX=GPIO%d", s_cfg.baud, s_cfg.tx_gpio, s_cfg.rx_gpio);

    /* 队列/互斥锁首次创建后复用，避免重复创建泄漏 */
    if (!s_resp_queue) {
        s_resp_queue = xQueueCreate(4, sizeof(uint8_t *));
    }
    if (!s_uart_mutex) {
        s_uart_mutex = xSemaphoreCreateMutex();
    }
    if (!s_slot_mutex) {
        s_slot_mutex = xSemaphoreCreateMutex();
    }
    if (!s_resp_queue || !s_uart_mutex || !s_slot_mutex) {
        ESP_LOGE(TAG, "queue/mutex create failed");
        uart_driver_delete(GW_UART_NUM);
        return ESP_ERR_NO_MEM;
    }

    s_running = true;

    if (xTaskCreate(uart_rx_task, "mb_uart_rx", 4096, NULL, 7, &s_uart_rx_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create UART RX task");
        goto start_fail;
    }

    listener_arg_t *la = malloc(sizeof(listener_arg_t));
    if (la) {
        la->port = s_cfg.port;
        la->idx = 0;
        if (xTaskCreate(tcp_listener_task, "mb_tcp_listen", 4096, la, 5,
                        &s_listener_tasks[0]) != pdPASS) {
            free(la);
            ESP_LOGE(TAG, "failed to create TCP listener task");
            goto start_fail;
        }
    }
    ESP_LOGI(TAG, "gateway started (port %u, allowlist=%s)",
             s_cfg.port, s_cfg.client_ip[0] ? s_cfg.client_ip : "none");
    return ESP_OK;

start_fail:
    gw_stop();
    return ESP_ERR_NO_MEM;
}

void modbus_gw_init(void)
{
    gw_config_t cfg;
    gw_config_load(&cfg);
    if (cfg.enabled) {
        if (gw_start(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "gateway start failed, disabled");
            gw_stop();
        }
    } else {
        ESP_LOGI(TAG, "gateway disabled");
    }
}

void modbus_gw_stop(void)
{
    gw_stop();
}

void modbus_gw_pause(void)
{
    gw_stop_ex(true);
}

esp_err_t modbus_gw_start(void)
{
    gw_config_t cfg;
    gw_config_load(&cfg);
    if (!cfg.enabled) {
        ESP_LOGI(TAG, "gateway disabled by config");
        return ESP_OK;
    }
    return gw_start(&cfg);
}

esp_err_t modbus_gw_reconfigure(const gw_config_t *cfg)
{
    gw_stop();
    if (!cfg->enabled) {
        ESP_LOGI(TAG, "gateway disabled by config");
        return ESP_OK;
    }
    return gw_start(cfg);
}

void modbus_gw_get_config(gw_config_t *cfg)
{
    memcpy(cfg, &s_cfg, sizeof(s_cfg));
}

bool modbus_gw_is_running(void)
{
    return s_running;
}

bool modbus_gw_write_holding(uint8_t uid, uint16_t reg, uint16_t value)
{
    if (!s_running) {
        ESP_LOGW(TAG, "gw not running, cannot trigger");
        return false;
    }
    uint8_t pdu[5];
    pdu[0] = 0x06;                   /* 写单个寄存器 */
    pdu[1] = (uint8_t)(reg >> 8);
    pdu[2] = (uint8_t)(reg & 0xFF);
    pdu[3] = (uint8_t)(value >> 8);
    pdu[4] = (uint8_t)(value & 0xFF);
    uint8_t resp_pdu[8];
    size_t resp_len = 0;
    bool ok = rtu_transaction(uid, pdu, sizeof(pdu), resp_pdu, &resp_len);
    if (ok && (resp_len != sizeof(pdu) || memcmp(resp_pdu, pdu, sizeof(pdu)) != 0)) {
        ESP_LOGW(TAG, "trigger write got non-echo response");
        ok = false;
    }
    ESP_LOGI(TAG, "trigger write reg=%u val=0x%04X -> %s", reg, value, ok ? "ok" : "noresp");
    return ok;
}

bool modbus_gw_get_client_ip(char *buf, size_t len)
{
    if (!s_slot_mutex) {
        return false;
    }
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] >= 0) {
            strlcpy(buf, s_client_ips[i], len);
            xSemaphoreGive(s_slot_mutex);
            return true;
        }
    }
    xSemaphoreGive(s_slot_mutex);
    return false;
}
