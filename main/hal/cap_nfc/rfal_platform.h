/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * ESP-IDF platform layer for ST25R3916 RFAL (rfal002, adapted from
 * Trezor firmware's rfal_platform.h)
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"

/* XTensa specreg.h defines BR/PS as register-number macros which collide with
 * RFAL struct member names (e.g. rfalNfcDepSlpAttReq.BR) - drop them */
#ifdef BR
#undef BR
#endif
#ifdef PS
#undef PS
#endif

/* Pin configuration (from hal_config.h, values inlined to keep this C header standalone) */
#define ST25R_SS_PIN 6    /* NFC SPI CS  -> G6 */
#define ST25R_INT_PIN 4   /* NFC IRQ     -> G4 */

/* Dummy port: this platform drives GPIOs directly by number */
#define ST25R_SS_PORT 0
#define ST25R_INT_PORT 0

/* Platform init provided by rfal_platform.c */
void rfal_platform_spi_init(void);
void rfal_platform_spi_transceive(const uint8_t* txBuf, uint8_t* rxBuf, size_t len);

/*
 * Critical sections: RFAL runs on a single task here, SPI transactions and the
 * IRQ status variable are only touched from the polling worker context.
 */
#include "freertos/task.h"
extern portMUX_TYPE g_rfal_spinlock;
#define platformProtectST25RComm()      taskENTER_CRITICAL(&g_rfal_spinlock)
#define platformUnprotectST25RComm()    taskEXIT_CRITICAL(&g_rfal_spinlock)

#define platformProtectST25RIrqStatus()      platformProtectST25RComm()
#define platformUnprotectST25RIrqStatus()    platformUnprotectST25RComm()

#define platformProtectWorker()              platformProtectST25RComm()
#define platformUnprotectWorker()            platformUnprotectST25RComm()

/* LEDs not present on this platform */
#define platformLedsInitialize()         do { } while(0)
#define platformLedOn(port, pin)         do { } while(0)
#define platformLedOff(port, pin)        do { } while(0)
#define platformLedToggle(port, pin)     do { } while(0)

/* GPIO helpers (direct GPIO number usage) */
#define platformGpioSet(port, pin)       gpio_set_level((gpio_num_t)(pin), 1)
#define platformGpioClear(port, pin)     gpio_set_level((gpio_num_t)(pin), 0)
#define platformGpioToggle(port, pin)    gpio_set_level((gpio_num_t)(pin), !gpio_get_level((gpio_num_t)(pin)))
#define platformGpioIsHigh(port, pin)    (gpio_get_level((gpio_num_t)(pin)) == 1)
#define platformGpioIsLow(port, pin)     (gpio_get_level((gpio_num_t)(pin)) == 0)

/*
 * Timers: uint32_t milliseconds deadline based on esp_timer_get_time().
 * Timer values are absolute deadlines so create() = now + t.
 */
uint32_t rfal_platform_timer_create(uint32_t t);
#define platformTimerCreate(t)           rfal_platform_timer_create(t)
#define platformTimerIsExpired(timer)    ((int32_t)((timer) - rfal_platform_get_tick()) <= 0)
uint32_t rfal_platform_get_tick(void);
#define platformGetSysTick()             rfal_platform_get_tick()
#define platformTimerDestroy(timer)      ((void)(timer))
uint32_t rfal_platform_timer_remaining(uint32_t timer);
#define platformTimerGetRemaining(timer) rfal_platform_timer_remaining(timer)

#define platformDelay(t)                 vTaskDelay((t) / portTICK_PERIOD_MS)

#define platformAssert(exp)              do { if(!(exp)) { abort(); } } while(0)
#define platformErrorHandle()            do { } while(0)  /* no trap */

/* IRQ: polling mode, no ISR. Workers call st25r3916CheckForReceivedInterrupts
 * via st25r3916ProcessInterrupts / rfalWorker paths through these no-ops. */
#define platformIrqST25RPinInitialize()  rfal_platform_irq_pin_init()
#define platformIrqST25RSetCallback(cb)  ((void)(cb))

void rfal_platform_irq_pin_init(void);

/* SPI CS is handled by the ESP-IDF SPI driver's hardware CS */
#define platformSpiSelect()              rfal_platform_spi_select()
#define platformSpiDeselect()            rfal_platform_spi_deselect()

void rfal_platform_spi_select(void);
void rfal_platform_spi_deselect(void);

#define platformSpiTxRx(txBuf, rxBuf, len) rfal_platform_spi_transceive((txBuf), (rxBuf), (len))

/* Log: route to ESP_LOG, silence by default to avoid flooding */
#define RFAL_PLATFORM_LOG_ENABLE 1
#if RFAL_PLATFORM_LOG_ENABLE
#include "esp_log.h"
#define platformLog(...) ESP_LOGI("RFAL", __VA_ARGS__)
#else
#define platformLog(...) do { } while(0)
#endif

/*
 ******************************************************************************
 * RFAL feature configuration: NFC-A only (ISO14443A / MIFARE / NTAG T2T)
 ******************************************************************************
 */
#define RFAL_FEATURE_LISTEN_MODE             false
#define RFAL_FEATURE_WAKEUP_MODE             false
#define RFAL_FEATURE_LOWPOWER_MODE           false
#define RFAL_FEATURE_NFCA                    true
#define RFAL_FEATURE_NFCB                    false
#define RFAL_FEATURE_NFCF                    false
#define RFAL_FEATURE_NFCV                    false
#define RFAL_FEATURE_T1T                     false
#define RFAL_FEATURE_T2T                     true
#define RFAL_FEATURE_T4T                     true
#define RFAL_FEATURE_ST25TB                  false
#define RFAL_FEATURE_ST25xV                  false
#define RFAL_FEATURE_DYNAMIC_ANALOG_CONFIG   false
#define RFAL_FEATURE_DPO                     false
#define RFAL_FEATURE_ISO_DEP                 true
#define RFAL_FEATURE_ISO_DEP_POLL            true
#define RFAL_FEATURE_ISO_DEP_LISTEN          false
#define RFAL_FEATURE_NFC_DEP                 false

#define RFAL_FEATURE_ISO_DEP_IBLOCK_MAX_LEN  256U
#define RFAL_FEATURE_NFC_DEP_BLOCK_MAX_LEN   254U
#define RFAL_FEATURE_NFC_RF_BUF_LEN          258U
#define RFAL_FEATURE_ISO_DEP_APDU_MAX_LEN    512U
#define RFAL_FEATURE_NFC_DEP_PDU_MAX_LEN     512U

/* Use software CS control: ST25R SPI transactions require CS low across
 * multi-step register transactions (read cmd byte then data), the ESP-IDF
 * per-transaction auto CS would release CS in between. */
#define ST25R_COM_SINGLETXRX                1  /* combine each transaction into one SPI call */

/* ST25R3916 device selection (chip variant, used by analog config / com tables) */
#define ST25R3916                           1

#ifdef __cplusplus
}
#endif
