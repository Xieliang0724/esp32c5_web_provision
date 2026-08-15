/*
 * rgb_led.c - RGB 状态灯实现（espressif/led_strip 托管组件, RMT 驱动）
 */
#include "esp_log.h"
#include "led_strip.h"

#include "rgb_led.h"

static const char *TAG = "rgb_led";

static led_strip_handle_t s_strip = NULL;

void rgb_led_init(void)
{
    /* 灯珠配置 */
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_PROV_LED_GPIO,
        .max_leds = 1,                              /* 单颗灯珠 */
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    /* RMT 驱动配置 */
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,          /* 10 MHz, 1 tick = 0.1us */
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "RGB LED init on GPIO%d", CONFIG_PROV_LED_GPIO);
}

void rgb_led_set_state(rgb_state_t st)
{
    if (!s_strip) {
        return;
    }
    uint8_t r = 0, g = 0, b = 0;
    switch (st) {
    case RGB_STATE_DEFAULT:      /* 橙色 */
        r = 255; g = 100; b = 0;
        break;
    case RGB_STATE_AP_CLIENT:    /* 蓝色 */
        r = 0;   g = 0;   b = 255;
        break;
    case RGB_STATE_CONNECTED:    /* 绿色 */
        r = 0;   g = 255; b = 0;
        break;
    default:
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
    ESP_LOGI(TAG, "LED state -> %s (%u,%u,%u)",
             st == RGB_STATE_DEFAULT ? "ORANGE" :
             st == RGB_STATE_AP_CLIENT ? "BLUE" : "GREEN",
             r, g, b);
}
