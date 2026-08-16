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

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

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
#define MAX_LISTENERS      2            /* 明文 + TLS 两个监听 */

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
static int s_listen_fds[MAX_LISTENERS] = {-1, -1};

/* ------------------------------------------------------------------ */
/* TLS：固件内置服务器证书（单向 TLS，验证设备身份）                     */
/* ------------------------------------------------------------------ */

extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_cert_pem_end");
extern const uint8_t server_key_pem_start[]  asm("_binary_server_key_pem_start");
extern const uint8_t server_key_pem_end[]    asm("_binary_server_key_pem_end");

static mbedtls_x509_crt s_srv_cert;
static mbedtls_pk_context s_srv_key;
static bool s_tls_ready = false;

static void tls_init(void)
{
    mbedtls_x509_crt_init(&s_srv_cert);
    mbedtls_pk_init(&s_srv_key);

    /* EMBED_FILES 字节数组无 null 结尾，而 mbedTLS 的 PEM 解析要求
     * 缓冲区以 '\0' 结尾，故先拷贝到带结尾符的缓冲区再解析。 */
    size_t cert_len = server_cert_pem_end - server_cert_pem_start;
    size_t key_len = server_key_pem_end - server_key_pem_start;
    uint8_t *buf = malloc((cert_len > key_len ? cert_len : key_len) + 1);
    if (!buf) {
        ESP_LOGE(TAG, "tls buf malloc failed");
        return;
    }

    int ret = 0;
    memcpy(buf, server_cert_pem_start, cert_len);
    buf[cert_len] = '\0';
    ret = mbedtls_x509_crt_parse(&s_srv_cert, buf, cert_len + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "cert parse failed: -0x%04X", -ret);
        free(buf);
        return;
    }
    memcpy(buf, server_key_pem_start, key_len);
    buf[key_len] = '\0';
    ret = mbedtls_pk_parse_key(&s_srv_key, buf, key_len + 1, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "key parse failed: -0x%04X", -ret);
        free(buf);
        return;
    }
    free(buf);
    s_tls_ready = true;
    ESP_LOGI(TAG, "TLS server cert loaded");
}

/* TLS BIO 回调：包装 socket fd（阻塞式） */
static int tls_send_cb(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    return send(fd, buf, len, 0);
}

static int tls_recv_cb(void *ctx, unsigned char *buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    return recv(fd, buf, len, 0);
}

/* 统一收发：plain 用 recv/send，TLS 用 mbedtls_ssl_read/write */
static int mb_net_recv(int fd, mbedtls_ssl_context *ssl, uint8_t *buf, size_t len)
{
    if (ssl) {
        return mbedtls_ssl_read(ssl, buf, len);
    }
    return recv(fd, buf, len, 0);
}

static int mb_net_send(int fd, mbedtls_ssl_context *ssl, const uint8_t *buf, size_t len)
{
    if (ssl) {
        return mbedtls_ssl_write(ssl, buf, len);
    }
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
                    xQueueSend(s_resp_queue, &resp, 0);
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

    /* 丢弃上一次残留帧，避免错配 */
    xQueueReset(s_resp_queue);

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
    bool tls;
} client_arg_t;

static void tcp_client_task(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;
    bool tls = ca->tls;
    free(ca);

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_conf;
    mbedtls_ssl_context *ssl_p = NULL;

    if (tls) {
        if (!s_tls_ready) {
            ESP_LOGE(TAG, "TLS not ready, close client");
            close(fd);
            vTaskDelete(NULL);
            return;
        }
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&ssl_conf);
        int ret = mbedtls_ssl_config_defaults(&ssl_conf,
                                              MBEDTLS_SSL_IS_SERVER,
                                              MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0) {
            mbedtls_ssl_conf_authmode(&ssl_conf, MBEDTLS_SSL_VERIFY_NONE);  /* 单向：不验证客户端 */
            mbedtls_ssl_conf_own_cert(&ssl_conf, &s_srv_cert, &s_srv_key);
            ret = mbedtls_ssl_setup(&ssl, &ssl_conf);
        }
        if (ret == 0) {
            mbedtls_ssl_set_bio(&ssl, (void *)(intptr_t)fd, tls_send_cb, tls_recv_cb, NULL);
            ret = mbedtls_ssl_handshake(&ssl);
        }
        if (ret != 0) {
            ESP_LOGW(TAG, "TLS handshake failed: -0x%04X", -ret);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&ssl_conf);
            close(fd);
            vTaskDelete(NULL);
            return;
        }
        ssl_p = &ssl;
        ESP_LOGI(TAG, "TLS client handshake OK");
    }

    uint8_t mbap[7];
    uint8_t buf[MAX_FRAME_SIZE];

    while (s_running) {
        /* 读 MBAP 头（7 字节） */
        size_t got = 0;
        while (got < sizeof(mbap) && s_running) {
            int n = mb_net_recv(fd, ssl_p, mbap + got, sizeof(mbap) - got);
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
            int n = mb_net_recv(fd, ssl_p, buf + got, pdu_len - got);
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
                int n = mb_net_send(fd, ssl_p, rsp + sent, total - sent);
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
    if (ssl_p) {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&ssl_conf);
    }
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] == fd) {
            close_client(i);
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* TCP 监听任务                                                        */
/* ------------------------------------------------------------------ */

/* 监听任务参数 */
typedef struct {
    uint16_t port;
    bool     tls;
    int      idx;   /* 对应 s_listen_fds 槽位 */
} listener_arg_t;

static void tcp_listener_task(void *arg)
{
    listener_arg_t *la = (listener_arg_t *)arg;
    uint16_t port = la->port;
    bool tls = la->tls;
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
    ESP_LOGI(TAG, "Modbus TCP%s listening on port %u", tls ? " (TLS)" : "", port);

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
        if (s_cfg.client_ip[0] != '\0') {
            char peer_ip[16];
            strlcpy(peer_ip, inet_ntoa(peer.sin_addr), sizeof(peer_ip));
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
        int slot = find_free_client_slot();
        if (slot >= 0) {
            s_client_fds[slot] = client;
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
            xSemaphoreGive(s_slot_mutex);
            ESP_LOGE(TAG, "client arg malloc failed");
            close(client);
            continue;
        }
        ca->fd = client;
        ca->tls = tls;
        if (xTaskCreate(tcp_client_task, "mb_tcp_client", 8192, ca, 6,
                        &s_client_tasks[slot]) != pdPASS) {
            /* 任务创建失败，归还槽位 */
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_client_fds[slot] = -1;
            s_client_tasks[slot] = NULL;
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

static void gw_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listen_fds[i] >= 0) {
            close(s_listen_fds[i]);
            s_listen_fds[i] = -1;
        }
    }
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] >= 0) {
            close(s_client_fds[i]);
            s_client_fds[i] = -1;
        }
    }
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
    uart_driver_delete(GW_UART_NUM);
    ESP_LOGI(TAG, "gateway stopped");
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
    if (s_cfg.tls_enabled && s_cfg.tls_port == 0) {
        s_cfg.tls_port = 802;
    }
    if (s_cfg.tls_enabled && s_cfg.tls_port == s_cfg.port) {
        ESP_LOGE(TAG, "tls_port must differ from port");
        return ESP_ERR_INVALID_ARG;
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
    if (ret != ESP_OK) {
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

    /* TLS 初始化（解析内置证书/私钥，仅一次） */
    if (s_cfg.tls_enabled && !s_tls_ready) {
        tls_init();
    }

    s_running = true;

    xTaskCreate(uart_rx_task, "mb_uart_rx", 4096, NULL, 7, &s_uart_rx_task);

    listener_arg_t *la = malloc(sizeof(listener_arg_t));
    if (la) {
        la->port = s_cfg.port;
        la->tls = false;
        la->idx = 0;
        xTaskCreate(tcp_listener_task, "mb_tcp_listen", 4096, la, 5, &s_listener_tasks[0]);
    }
    if (s_cfg.tls_enabled && s_tls_ready) {
        listener_arg_t *la2 = malloc(sizeof(listener_arg_t));
        if (la2) {
            la2->port = s_cfg.tls_port;
            la2->tls = true;
            la2->idx = 1;
            xTaskCreate(tcp_listener_task, "mb_tls_listen", 4096, la2, 5, &s_listener_tasks[1]);
        }
    }
    ESP_LOGI(TAG, "gateway started (port %u%s, allowlist=%s)",
             s_cfg.port, s_cfg.tls_enabled ? " + TLS port " : "",
             s_cfg.client_ip[0] ? s_cfg.client_ip : "none");
    if (s_cfg.tls_enabled) {
        ESP_LOGI(TAG, "TLS port %u", s_cfg.tls_port);
    }
    return ESP_OK;
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
