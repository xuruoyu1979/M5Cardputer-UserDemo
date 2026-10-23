/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * ESP-IDF platform implementation for ST25R3916 RFAL
 */
#include "rfal_platform.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"

static const char* TAG = "rfal-platform";

/* SPI device handle, attached to the bus already initialized by hal spi_init */
static spi_device_handle_t s_spi_dev = NULL;
static volatile bool s_cs_active = false;

/* Critical section spinlock shared with the platform macros */
portMUX_TYPE g_rfal_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* SPI                                                                 */
/* ------------------------------------------------------------------ */

void rfal_platform_spi_init(void)
{
    if (s_spi_dev != NULL) {
        return;
    }

    spi_host_device_t host = SDSPI_DEFAULT_HOST;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000,  /* start at 2 MHz */
        .mode           = 1,                /* SPI mode 1: CPOL=0 CPHA=1 */
        .spics_io_num   = -1,               /* CS driven manually by GPIO */
        .queue_size     = 1,
        .flags          = 0,
    };

    esp_err_t ret = spi_bus_add_device(host, &devcfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        s_spi_dev = NULL;
        return;
    }

    /* CS pin as GPIO output, idle high */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ST25R_SS_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)ST25R_SS_PIN, 1);

    ESP_LOGI(TAG, "ST25R3916 SPI device ready (mode 1, 2 MHz, CS=G%d)", ST25R_SS_PIN);
}

void rfal_platform_spi_select(void)
{
    s_cs_active = true;
    gpio_set_level((gpio_num_t)ST25R_SS_PIN, 0);
}

void rfal_platform_spi_deselect(void)
{
    gpio_set_level((gpio_num_t)ST25R_SS_PIN, 1);
    s_cs_active = false;
}

void rfal_platform_spi_transceive(const uint8_t* txBuf, uint8_t* rxBuf, size_t len)
{
    if (s_spi_dev == NULL || len == 0) {
        return;
    }

    static uint8_t s_dma_buf[600]; /* DMA-safe bounce buffer, max ST25R3916_BUF_LEN + margin */

    if (len > sizeof(s_dma_buf)) {
        len = sizeof(s_dma_buf);
    }

    if (txBuf != NULL) {
        memcpy(s_dma_buf, txBuf, len);
    } else {
        memset(s_dma_buf, 0x00, len); /* MOSI idle state */
    }

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = len * 8;
    t.tx_buffer = s_dma_buf;
    t.rx_buffer = s_dma_buf;

    esp_err_t ret = spi_device_polling_transmit(s_spi_dev, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi transmit failed: %s", esp_err_to_name(ret));
        return;
    }

    if (rxBuf != NULL) {
        memcpy(rxBuf, s_dma_buf, len);
    }
}

/* ------------------------------------------------------------------ */
/* Timer (ms, absolute deadlines based on esp_timer)                   */
/* ------------------------------------------------------------------ */

uint32_t rfal_platform_get_tick(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint32_t rfal_platform_timer_create(uint32_t t)
{
    return rfal_platform_get_tick() + t;
}

uint32_t rfal_platform_timer_remaining(uint32_t timer)
{
    uint32_t now = rfal_platform_get_tick();
    if (platformTimerIsExpired(timer)) {
        return 0;
    }
    return timer - now;
}

/* ------------------------------------------------------------------ */
/* IRQ pin (polling mode, no ISR)                                      */
/* ------------------------------------------------------------------ */

void rfal_platform_irq_pin_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ST25R_INT_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}
