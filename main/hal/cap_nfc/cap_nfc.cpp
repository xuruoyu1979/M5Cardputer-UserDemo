/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * CapNFC v2: ST25R3916 NFC-A poller, chip sequences ported from
 * M5UnitUnifiedNFC (MIT, https://github.com/M5Stack/M5Unit-NFC) which is
 * verified to work on the same Cap CC1101 hardware. Low level register
 * access goes through the rfal002 st25r3916_com layer on our ESP-IDF
 * SPI platform (rfal_platform.c, mode 1, 2 MHz, software CS, polled IRQ).
 */
#include "cap_nfc.h"
#include "mifare_crypto1.h"
#include "rfal_platform.h"
#include <mooncake_log.h>
#include <fmt/format.h>

extern "C" {
#include "st25r3916.h"
#include "st25r3916_com.h"
#include "st25r3916_irq.h"
#include <esp_random.h>
#include <esp_rom_sys.h>
}

static const std::string _tag = "Cap-NFC";

/* ---------------------------------------------------------------- */
/* Register name shorthands (rfal002 st25r3916_com.h names)          */
/* ---------------------------------------------------------------- */
// All ST25R3916_REG_* macros are available from the headers above.

/* M5 timeouts (src/nfc/a/nfca.hpp) */
static constexpr uint32_t TIMEOUT_REQ_WUP  = 4;   // ms
static constexpr uint32_t TIMEOUT_ANTICOLL = 8;
static constexpr uint32_t TIMEOUT_SELECT   = 8;
static constexpr uint32_t TIMEOUT_AUTH1    = 4;
static constexpr uint32_t TIMEOUT_AUTH2    = 16;
static constexpr uint32_t TIMEOUT_READ     = 12;

/* IRQ bit positions in the M5 flag layout: value = ((main_nfc << 16) | (error << 8) | passive)
 * main_nfc byte: bit7 OSC, bit6 WL, bit5 RXS, bit4 RXE, bit3 TXE, bit2 COL, bit1 RX_REST */
static constexpr uint32_t I_osc32     = ((uint32_t)0x80 << 24);
static constexpr uint32_t I_wl32      = ((uint32_t)0x40 << 24);
static constexpr uint32_t I_rxs32     = ((uint32_t)0x20 << 24);
static constexpr uint32_t I_rxe32     = ((uint32_t)0x10 << 24);
static constexpr uint32_t I_txe32     = ((uint32_t)0x08 << 24);
static constexpr uint32_t I_col32     = ((uint32_t)0x04 << 24);
static constexpr uint32_t I_rx_rest32 = ((uint32_t)0x02 << 24);
static constexpr uint32_t I_nre32     = ((uint32_t)0x40 << 16);
static constexpr uint32_t I_dct32     = ((uint32_t)0x80 << 16);
static constexpr uint32_t I_gpe32     = ((uint32_t)0x20 << 16);
static constexpr uint32_t I_apon32    = ((uint32_t)0x01 << 16);
static constexpr uint32_t I_cac32     = ((uint32_t)0x04 << 16);
static constexpr uint32_t I_cat32     = ((uint32_t)0x02 << 16);
/* error byte (byte1): CRC bit0? -> per M5 mapping error<<8; bits per datasheet:
 * ERR1 = 0x02, ERR2 = 0x04, CRC = 0x08, PAR = 0x10, ... (byte1 positions) */
static constexpr uint32_t I_crc32e    = ((uint32_t)0x08 << 8);
static constexpr uint32_t I_par32e    = ((uint32_t)0x10 << 8);

static inline bool irqIs(uint32_t flags, uint32_t bit)
{
    return (flags & bit) != 0;
}

/* ---------------------------------------------------------------- */
/* Misc helpers                                                      */
/* ---------------------------------------------------------------- */

std::string CapNFC::uidToHex(const uint8_t* uid, uint8_t len)
{
    std::string out;
    out.reserve(len * 2);
    for (uint8_t i = 0; i < len; i++) {
        out += fmt::format("{:02X}", uid[i]);
    }
    return out;
}

static uint8_t calcBcc(const uint8_t* d, int n)
{
    uint8_t b = 0;
    for (int i = 0; i < n; i++) b ^= d[i];
    return b;
}

/* Correct reflected CRC-16/A (MIFARE): init 0xC6C6, poly 0x1021 reflected */
static uint16_t crc16A_mifare(const uint8_t* p, uint16_t len)
{
    uint16_t crc = 0xC6C6;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t d = p[i] ^ (uint8_t)(crc & 0xFF);
        d = (uint8_t)(d ^ (d << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)d << 8) ^ ((uint16_t)d << 3) ^ ((uint16_t)d >> 4));
    }
    return crc;
}

/* ---------------------------------------------------------------- */
/* Low level chip I/O on st25r3916_com (SPI verified layer)          */
/* ---------------------------------------------------------------- */

static inline bool wrReg(uint8_t reg, uint8_t val)
{
    return st25r3916WriteRegister(reg, val) == RFAL_ERR_NONE;
}
static inline bool rdReg(uint8_t reg, uint8_t& val)
{
    return st25r3916ReadRegister(reg, &val) == RFAL_ERR_NONE;
}
static inline bool setRegBits(uint8_t reg, uint8_t bits)
{
    return st25r3916SetRegisterBits(reg, bits) == RFAL_ERR_NONE;
}
static inline bool clrRegBits(uint8_t reg, uint8_t bits)
{
    return st25r3916ClrRegisterBits(reg, bits) == RFAL_ERR_NONE;
}
static inline bool cmd(uint8_t c)
{
    return st25r3916ExecuteCommand(c) == RFAL_ERR_NONE;
}
static inline bool wrFifo(const uint8_t* d, uint16_t n)
{
    return st25r3916WriteFifo(d, n) == RFAL_ERR_NONE;
}
static inline bool rdFifoRaw(uint8_t* d, uint16_t n)
{
    return st25r3916ReadFifo(d, n) == RFAL_ERR_NONE;
}

/* M5 readInterrupts(): value = (main_nfc << 16) | (error << 8) | passive.
 * Reading the main interrupt register clears it on chip. */
static bool readIrqRegs(uint32_t& value)
{
    uint8_t mainNfc[2] = {0}, err = 0, passive = 0;
    if (st25r3916ReadMultipleRegisters(ST25R3916_REG_IRQ_MAIN, mainNfc, 1) != RFAL_ERR_NONE) return false;
    if (st25r3916ReadMultipleRegisters(ST25R3916_REG_IRQ_MAIN + 1, mainNfc + 1, 1) != RFAL_ERR_NONE) return false;
    if (st25r3916ReadMultipleRegisters(ST25R3916_REG_IRQ_MAIN + 2, &err, 1) != RFAL_ERR_NONE) return false;
    if (st25r3916ReadMultipleRegisters(ST25R3916_REG_IRQ_MAIN + 3, &passive, 1) != RFAL_ERR_NONE) return false;
    value = ((uint32_t)mainNfc[0] << 24) | ((uint32_t)mainNfc[1] << 16) | ((uint32_t)err << 8) | passive;
    return true;
}

uint32_t CapNFC::read_irq_cached()
{
    uint32_t v = 0;
    readIrqRegs(v);
    return v;
}

/* M5 wait_for_interrupt (polled version): poll the IRQ regs until any of
 * `bits` seen or timeout; returns flags | I_nre32 on timeout. */
bool CapNFC::wait_for_irq(uint32_t waitBits32, uint32_t timeout_ms, uint32_t& outFlags)
{
    uint32_t flags = 0;
    uint32_t t0    = rfal_platform_get_tick();
    do {
        uint32_t v = 0;
        if (readIrqRegs(v)) {
            flags |= v;
        }
        if (flags & waitBits32) {
            outFlags = flags;
            return true;
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    } while (rfal_platform_get_tick() - t0 < timeout_ms);
    outFlags = flags | I_nre32;
    return false;
}

bool CapNFC::read_fifo(uint16_t& actual, uint8_t* buf, uint16_t buf_size)
{
    actual = 0;
    uint8_t st1 = 0, st2 = 0;
    rdReg(ST25R3916_REG_FIFO_STATUS1, st1);
    rdReg(ST25R3916_REG_FIFO_STATUS2, st2);
    uint16_t bytes = (uint16_t)((st2 & 0xC0) << 2) | st1;  /* fifo_b9:8 = bits7:6 */
    if (bytes == 0) return false;
    uint16_t n = (bytes < buf_size) ? bytes : buf_size;
    if (!rdFifoRaw(buf, n)) return false;
    actual = n;
    return true;
}

bool CapNFC::wait_for_fifo(uint32_t timeout_ms, uint16_t required_size)
{
    uint32_t flags = 0;
    wait_for_irq(I_rxe32, timeout_ms, flags);

    if (flags & I_rxe32) {
        return true;
    }
    /* RXS but no RXE yet: poll FIFO fill level */
    if (flags & I_rxs32) {
        uint32_t t0 = rfal_platform_get_tick();
        do {
            uint8_t st1 = 0, st2 = 0;
            rdReg(ST25R3916_REG_FIFO_STATUS1, st1);
            rdReg(ST25R3916_REG_FIFO_STATUS2, st2);
            uint16_t bytes = (uint16_t)((st2 & 0xC0) << 2) | st1;  /* fifo_b9:8 = bits7:6 */
            if (bytes >= (required_size ? required_size : 1)) {
                return true;
            }
        } while (rfal_platform_get_tick() - t0 < timeout_ms);
    }
    return false;
}

bool CapNFC::write_fwt_timer(uint32_t ms)
{
    uint8_t temv = 0;
    rdReg(ST25R3916_REG_TIMER_EMV_CONTROL, temv);
    bool nstep = (temv & 0x01) != 0;  // nrt_step
    /* integer math per M5 calculate_nrt */
    constexpr uint32_t FC = 13560000;
    uint64_t step_num = nstep ? (4096ULL * 1000000ULL) : (64ULL * 1000000ULL);
    uint64_t us       = (uint64_t)ms * 1000ULL;
    uint64_t nrt      = (us * FC + step_num - 1) / step_num;
    if (nrt > 0xFFFFULL) nrt = 0xFFFFULL;
    if (nrt < 1ULL) nrt = 1ULL;
    wrReg(0x10, (uint8_t)(nrt & 0xFF));         // NRT LSB
    wrReg(0x11, (uint8_t)((nrt >> 8) & 0xFF));  // NRT MSB
    return true;
}

/* ---------------------------------------------------------------- */
/* Init sequence (M5 UnitST25R3916::begin + configure_nfc_a)         */
/* ---------------------------------------------------------------- */

bool CapNFC::m5_begin_sequence()
{
    /* Defensive reset */
    cmd(ST25R3916_CMD_STOP);
    vTaskDelay(2 / portTICK_PERIOD_MS);

    /* 1) set default */
    if (!cmd(ST25R3916_CMD_SET_DEFAULT)) return false;

    /* 2) overheat protection patch: test reg 0x04 = 0x10 */
    st25r3916WriteTestRegister(0x04, 0x10);

    /* 3) IO config: SPI pull downs + io_drv_lvl, sup3v unset (5V rail) */
    wrReg(ST25R3916_REG_IO_CONF2, 0x18 | ST25R3916_REG_IO_CONF2_io_drv_lvl);

    /* Antenna / TX driver: M5 writes TX_DRIVER = (13 << 4) => am mod 22% */
    wrReg(ST25R3916_REG_TX_DRIVER, 0xD0);

    /* MCU_CLK disabled */
    setRegBits(ST25R3916_REG_IO_CONF1, 0x07);

    /* RES_AM_MOD: minimum non-overlap then restore (M5 toggles around AAT enable) */
    setRegBits(ST25R3916_REG_IO_CONF2, ST25R3916_REG_IO_CONF2_aat_en);

    /* Field detector thresholds */
    wrReg(ST25R3916_REG_FIELD_THRESHOLD_ACTV, 0x13);   // trg 105mV, rfe 205mV
    wrReg(ST25R3916_REG_FIELD_THRESHOLD_DEACTV, 0x02); // trg 75mV, rfe 150mV

    /* Passive target / EMD */
    wrReg(ST25R3916_REG_PASSIVE_TARGET, 0x5F);        // fdel 5, ptm
    wrReg(ST25R3916_REG_EMD_SUP_CONF, 0x40);          // start on first 4 bits

    /* Antenna tuning (AAT) both to 0x82 */
    wrReg(ST25R3916_REG_ANT_TUNE_A, 0x82);
    wrReg(ST25R3916_REG_ANT_TUNE_B, 0x82);

    /* External field detector automatic */
    setRegBits(ST25R3916_REG_OP_CONTROL, 0x03);
    cmd(ST25R3916_CMD_CLEAR_FIFO);

    /* 4) oscillator on */
    st25r3916ClearAndEnableInterrupts(ST25R3916_IRQ_MASK_OSC);
    setRegBits(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    {
        uint32_t f = 0;
        wait_for_irq(I_osc32, 10, f);
    }
    st25r3916DisableInterrupts(ST25R3916_IRQ_MASK_OSC);

    /* Adjust regulators */
    if (!cmd(ST25R3916_CMD_ADJUST_REGULATORS)) return false;
    vTaskDelay(5 / portTICK_PERIOD_MS);

    return true;
}

bool CapNFC::configure_nfc_a()
{
    /* M5 configure_nfc_a() */
    cmd(ST25R3916_CMD_STOP);
    clrRegBits(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_wu);

    /* Initiator ISO14443A, nfc_ar8_auto */
    wrReg(ST25R3916_REG_MODE, 0x09);  // ISO14443A | bit0
    wrReg(ST25R3916_REG_BIT_RATE, 0x00);  // 106/106

    /* ISO14443A settings: standard */
    wrReg(ST25R3916_REG_ISO14443A_NFC, 0x00);
    clrRegBits(ST25R3916_REG_AUX, ST25R3916_REG_AUX_dis_corr);

    /* Overshoot/undershoot protection */
    wrReg(ST25R3916_REG_OVERSHOOT_CONF1, 0x40);
    wrReg(ST25R3916_REG_OVERSHOOT_CONF2, 0x03);
    wrReg(ST25R3916_REG_UNDERSHOOT_CONF1, 0x40);
    wrReg(ST25R3916_REG_UNDERSHOOT_CONF2, 0x03);

    /* Correlator config (space B) */
    wrReg((uint8_t)(ST25R3916_SPACE_B | 0x0C), 0x47);
    wrReg((uint8_t)(ST25R3916_SPACE_B | 0x0D), 0x00);

    /* Receiver configuration ("Stability-focused") */
    wrReg(ST25R3916_REG_RX_CONF1, 0x08);  // z_600k
    wrReg(ST25R3916_REG_RX_CONF2, 0x2D);  // sqm_dyn | agc_en | agc_m | agc6_3
    wrReg(ST25R3916_REG_RX_CONF3, 0xD8);
    wrReg(ST25R3916_REG_RX_CONF4, 0x22);

    cmd(ST25R3916_CMD_RESET_RXGAIN);

    /* Initial field on (collision avoidance) then enable tx+rx */
    if (!cmd(ST25R3916_CMD_INITIAL_RF_COLLISION)) return false;
    vTaskDelay(5 / portTICK_PERIOD_MS);
    setRegBits(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_tx_en | ST25R3916_REG_OP_CONTROL_rx_en);

    return true;
}

/* ---------------------------------------------------------------- */
/* NFC-A poll primitives (M5 sequences)                              */
/* ---------------------------------------------------------------- */

bool CapNFC::nfca_request(uint16_t& atqa, bool wakeup, uint32_t* dbgFlags, uint16_t* dbgFifo)
{
    atqa = 0;
    if (dbgFlags) *dbgFlags = 0;
    if (dbgFifo) *dbgFifo = 0;

    /* v10: mirror the verified manual success path (reqaWindowSample)
     * exactly. No NRT/fwt timer, no antcl settings, no aux bits - just:
     * clear FIFO -> NUM_TX_BYTES2 = 0 -> direct command -> sample. */
    cmd(ST25R3916_CMD_CLEAR_FIFO);
    wrReg(ST25R3916_REG_NUM_TX_BYTES2, 0);
    {
        uint32_t dummy = 0;
        readIrqRegs(dummy);  // clear pending IRQ (read-clear)
    }
    cmd(wakeup ? ST25R3916_CMD_TRANSMIT_WUPA : ST25R3916_CMD_TRANSMIT_REQA);

    /* Sample IRQ + FIFO for up to 20 ms. ATQA arrives within ~5 ms; the
     * direct command handles all rx timing internally. */
    uint32_t t0 = rfal_platform_get_tick();
    bool gotRx  = false;
    uint32_t lastIrq = 0;
    while (rfal_platform_get_tick() - t0 < 20) {
        uint32_t irq = read_irq_cached();
        lastIrq |= irq;
        if (dbgFlags) *dbgFlags = lastIrq;
        if (irq & (I_rxe32 | I_rxs32 | I_col32)) {
            if (irq & I_rxe32) {
                gotRx = true;
                break;
            }
            /* RXS seen: wait for FIFO to fill (2 bytes) */
            uint8_t st1 = 0, st2 = 0;
            rdReg(ST25R3916_REG_FIFO_STATUS1, st1);
            rdReg(ST25R3916_REG_FIFO_STATUS2, st2);
            if (((uint16_t)((st2 & 0xC0) << 2) | st1) >= 2) {
                gotRx = true;
                break;
            }
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }

    if (!gotRx) {
        /* collect diagnostics for the log */
        uint8_t st1 = 0, st2 = 0;
        rdReg(ST25R3916_REG_FIFO_STATUS1, st1);
        rdReg(ST25R3916_REG_FIFO_STATUS2, st2);
        if (dbgFifo) *dbgFifo = (uint16_t)((st2 & 0xC0) << 2) | st1;
        return false;
    }

    uint16_t actual = 0;
    uint8_t buf[2]  = {0};
    if (!read_fifo(actual, buf, 2) || actual < 2) {
        if (dbgFifo) *dbgFifo = actual;
        return false;
    }
    atqa = (uint16_t)((uint16_t)buf[0] << 8) | buf[1];
    return true;
}

bool CapNFC::nfca_anticoll(uint8_t rbuf[5], uint8_t lv)
{
    if (lv < 1 || lv > 3) return false;

    if (!write_fwt_timer(TIMEOUT_ANTICOLL)) return false;
    wrReg(ST25R3916_REG_ISO14443A_NFC, 0x01);  // antcl
    clrRegBits(ST25R3916_REG_AUX, ST25R3916_REG_AUX_no_crc_rx);

    uint8_t frame[7] = {(uint8_t)(0x91 + lv * 2), 0x20};
    uint32_t sbytes = 2, sbits = 0;
    uint8_t rbufOffset = 0;
    uint32_t count = 32;
    bool collision = false;
    uint16_t actual = 0;
    uint8_t collByte = 1;

    do {
        { uint32_t dummy = 0; readIrqRegs(dummy); }  // clear pending IRQ
        cmd(ST25R3916_CMD_CLEAR_FIFO);
        if (!wrFifo(frame, (uint16_t)(sbytes + (sbits != 0 ? 1 : 0)))) return false;
        /* M5 writeNumberOfTransmittedBytes(bytes, bits):
         * val16 = ((bytes & 0x1FF) << 3) | (bits & 0x07), written as reg16 at 0x22 */
        {
            /* write_register16BE at 0x22: high byte first.
             * Datasheet: reg pair forms BE16; 0x23 bits3-7 = ntx, bits0-2 = nbtx */
            uint16_t val = (uint16_t)(((sbytes & 0x01FF) << 3) | (sbits & 0x07));
            wrReg(0x22, (uint8_t)((val >> 8) & 0xFF)); /* NUM_TX_BYTES1 = high */
            wrReg(0x23, (uint8_t)(val & 0xFF));        /* NUM_TX_BYTES2 = low (ntx|nbtx) */
        }

        cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

        uint32_t flags = 0;
        if (!wait_for_irq(I_rxe32 | I_col32, TIMEOUT_ANTICOLL, flags)) {
            mclog::tagInfo(_tag, "poll: anticoll fail at lv={} irq_timeout flags=0x{:08X} (TXE={} RXS={} RXE={} COL={} NRE={} WL={})",
                           lv, flags, irqIs(flags, I_txe32), irqIs(flags, I_rxs32), irqIs(flags, I_rxe32),
                           irqIs(flags, I_col32), irqIs(flags, I_nre32), irqIs(flags, I_wl32));
            return false;
        }
        collision = irqIs(flags, I_col32);
        if (!collision && !irqIs(flags, I_rxe32)) {
            mclog::tagInfo(_tag, "poll: anticoll fail at lv={} flags=0x{:08X}", lv, flags);
            return false;
        }

        uint8_t cd = 0;
        rdReg(0x20, cd);  // collision display
        if (!read_fifo(actual, rbuf + rbufOffset, (uint16_t)(5 - rbufOffset)) || actual == 0) {
            return false;
        }

        if (collision) {
            uint8_t cbytes = (cd >> 4) & 0x0F;
            uint8_t cbits  = (cd >> 1) & 0x07;
            collByte = rbuf[rbufOffset + actual - 1];
            collByte |= (uint8_t)(1U << cbits);

            sbytes = cbytes + (cbits == 0x07);
            sbits  = (cbits + 1) & 0x07;
            if (sbytes < 2 || sbytes >= sizeof(frame)) {
                return false;
            }
            frame[1] = (uint8_t)((sbytes << 4) | sbits);
            memcpy(frame + 2 + rbufOffset, rbuf + rbufOffset, actual);
            frame[sbytes] = collByte;
            rbufOffset    = actual - 1;
        }

        if (sbits) {
            rbuf[rbufOffset] = (uint8_t)((rbuf[rbufOffset] >> sbits) << sbits);
            rbuf[rbufOffset] |= collByte;
        }
    } while (collision && count--);

    if (collision) return false;
    if (rbufOffset + actual != 5 || rbuf[4] != calcBcc(rbuf, 4)) {
        return false;
    }
    return true;
}

bool CapNFC::nfca_transceive(uint8_t* rx, uint16_t& rx_len, const uint8_t* tx, uint16_t tx_len,
                             uint32_t timeout_ms, uint16_t min_rx_len)
{
    if (!tx || !tx_len) return false;

    if (timeout_ms) {
        write_fwt_timer(timeout_ms);
    }
    wrReg(ST25R3916_REG_ISO14443A_NFC, 0x00);  // standard
    clrRegBits(ST25R3916_REG_AUX, ST25R3916_REG_AUX_no_crc_rx);

    { uint32_t dummy; readIrqRegs(dummy); }  // clearInterrupts
    cmd(ST25R3916_CMD_CLEAR_FIFO);
    if (!wrFifo(tx, tx_len)) return false;
    {
        uint16_t val = (uint16_t)(((tx_len & 0x01FF) << 3) | 0);
        wrReg(0x22, (uint8_t)((val >> 8) & 0xFF));
        wrReg(0x23, (uint8_t)(val & 0xFF));
    }
    cmd(ST25R3916_CMD_TRANSMIT_WITH_CRC);

    if (!wait_for_fifo(timeout_ms, min_rx_len)) {
        mclog::tagInfo(_tag, "poll: transceive fifo-timeout tmo={} min={}", timeout_ms, min_rx_len);
        return false;
    }

    uint16_t actual = 0;
    if (!read_fifo(actual, rx, rx_len) || actual == 0) {
        rx_len = 0;
        mclog::tagInfo(_tag, "poll: transceive empty fifo");
        return false;
    }
    rx_len = actual;
    /* NACK check for 4-bit ACK (value 0x0A) */
    if (actual == 1 && rx_len == 1) {
        /* bits info omitted; single byte may be 4-bit ACK */
    }
    return true;
}

/* Full single-tag poll: REQA -> (cascade anticoll+select) x N -> SAK */
bool CapNFC::nfca_poll_once(uint8_t uid[10], uint8_t& uidLen, uint8_t& sak, uint16_t& atqa)
{
    uidLen = 0;
    sak    = 0;
    atqa   = 0;

    uint32_t reqaFlags = 0;
    uint16_t reqaFifo  = 0;
    if (!nfca_request(atqa, false, &reqaFlags, &reqaFifo)) {
        return false;
    }

    uint8_t rbuf[3];
    uint8_t lv = 1;
    uint8_t offset = 0;
    bool completed = false;

    do {
        uint8_t cascade[5];
        if (!nfca_anticoll(cascade, lv)) {
            return false;
        }

        /* copy UID bytes (skip CT 0x88 if present) */
        uint8_t skip = (cascade[0] == 0x88) ? 1 : 0;
        if (skip) {
            memcpy(uid + offset, cascade + 1, 3);
            offset += 3;
        } else {
            memcpy(uid + offset, cascade, 4);
            offset += 4;
        }

        /* SELECT */
        uint8_t selFrame[7] = {(uint8_t)(0x91 + lv * 2), 0x70};
        memcpy(selFrame + 2, cascade, 5);
        uint16_t rxLen = 3;
        if (!nfca_transceive(rbuf, rxLen, selFrame, sizeof(selFrame), TIMEOUT_SELECT, 3) || rxLen != 3) {
            return false;
        }

        sak = rbuf[0];
        if (((sak & 0x24) == 0x20) || ((sak & 0x24) == 0x00)) {
            completed = true;
        }
        ++lv;
    } while (!completed && lv < 4);

    if (!completed) {
        return false;
    }
    uidLen = offset;
    return true;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

bool CapNFC::init()
{
    mclog::tagInfo(_tag, "init (M5 driver)");

    if (_is_inited) {
        return true;
    }

    rfal_platform_spi_init();

    /* Reset ST25R3916 IRQ bookkeeping used by st25r3916_com layer */
    st25r3916InitInterrupts();

    if (!m5_begin_sequence()) {
        mclog::tagError(_tag, "begin sequence failed");
        return false;
    }

    /* Verify chip identity */
    uint8_t id = 0;
    rdReg(ST25R3916_REG_IC_IDENTITY, id);
    if ((id & 0xF8) != (5U << 3)) {
        mclog::tagError(_tag, "unexpected IC_IDENTITY 0x{:02X}", id);
        return false;
    }

    if (!configure_nfc_a()) {
        mclog::tagError(_tag, "configure_nfc_a failed");
        return false;
    }

    /* diag log: key registers */
    {
        uint8_t mode = 0, op = 0, rx3 = 0;
        rdReg(ST25R3916_REG_MODE, mode);
        rdReg(ST25R3916_REG_OP_CONTROL, op);
        rdReg(ST25R3916_REG_RX_CONF3, rx3);
        mclog::tagInfo(_tag, "ready: IC_ID=0x{:02X} MODE=0x{:02X} OP=0x{:02X} RX_CONF3=0x{:02X}", id, mode, op, rx3);
    }

    _is_inited = true;
    return true;
}

void CapNFC::deinit()
{
    if (!_is_inited) {
        return;
    }
    cmd(ST25R3916_CMD_STOP);
    clrRegBits(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_tx_en | ST25R3916_REG_OP_CONTROL_rx_en);
    _is_polling = false;
    _tag_active = false;
    _is_inited  = false;
}

bool CapNFC::startPolling()
{
    if (!_is_inited) {
        return false;
    }

    /* Re-apply NFC-A config (fresh field state) */
    if (!configure_nfc_a()) {
        return false;
    }
    _is_polling = true;
    _tag_active = false;
    mclog::tagInfo(_tag, "polling started (NFC-A, M5 driver)");
    return true;
}

void CapNFC::deactivate()
{
    if (!_is_inited) {
        return;
    }
    /* HLTA + field cycle on next poll */
    uint8_t rbuf[2];
    uint16_t rlen = 0;
    uint8_t hlt[2] = {0x50, 0x00};
    nfca_transceive(rbuf, rlen, hlt, 2, TIMEOUT_REQ_WUP, 0);
    _tag_active = false;
}

void CapNFC::worker()
{
    if (!_is_polling || _tag_active) {
        return;
    }

    uint8_t uid[10];
    uint8_t uidLen = 0, sak = 0;
    uint16_t atqa = 0;

    /* REQA has sporadic single-shot misses (observed 2/3 success manually):
     * retry a couple of times per worker call before giving up this cycle.
     * Each attempt starts from a fresh field state (the manual diagnostic
     * path re-configured the field before every successful REQA; ordinary
     * transceives / NRT expiry may leave the receiver in a bad state). */
    bool found = false;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        if (attempt > 0) {
            configure_nfc_a();  /* fresh field for retries */
        }
        found = nfca_poll_once(uid, uidLen, sak, atqa);
    }

    if (!found) {
        return;
    }

    _tag_active = true;
    _last_uid   = uidToHex(uid, uidLen);

    /* type by SAK (M5 sak_to_type simplified) */
    if (sak & 0x08) {
        if (sak & 0x10) {
            _last_type = (sak & 0x20) ? "MIFARE Classic 4K" : "MIFARE Classic 4K";
        } else {
            _last_type = "MIFARE Classic 1K";
        }
    } else if (sak & 0x10) {
        _last_type = "MIFARE Plus";
    } else if (sak & 0x20) {
        _last_type = "ISO14443-4";
    } else {
        _last_type = "NTAG/MIFARE UL";
    }
    _last_type += fmt::format(" (SAK {:02X})", sak);

    mclog::tagInfo(_tag, "tag detected: {} [{}] ATQA={:04X}", _last_type, _last_uid, atqa);
    onTagDetected.emit(_last_uid, _last_type);
}

/* ---------------------------------------------------------------- */
/* Dump (NTAG pages / MIFARE classic blocks with default key)        */
/* ---------------------------------------------------------------- */

bool CapNFC::dumpTag(std::string& output)
{
    if (!_tag_active) {
        return false;
    }

    /* Pause polling during the dump so the worker never re-activates the
     * tag between READ commands (defensive: keyboard events currently run
     * on the same task as the worker, but keep the contract explicit). */
    bool pollingWasActive = _is_polling;
    _is_polling = false;

    output = fmt::format("UID: {}\nType: {}\n", _last_uid, _last_type);

    uint8_t uid[10];
    uint8_t uidLen = 0, sak = 0;
    uint16_t atqa = 0;

    /* ISO14443-3: a tag in ACTIVE state (just SELECTed by the worker) does
     * not answer REQA - only IDLE tags do. So first try the FAST PATH: a
     * single T2T READ (page 0) directly against the already-active tag.
     * If (and only if) that fails, power-cycle the tag back to IDLE by
     * dropping the field, then run a full REQA->anticoll->SELECT recovery. */
    auto fieldCycleAndReselect = [&]() -> bool {
        for (int attempt = 0; attempt < 2; attempt++) {
            if (attempt > 0 || true) {
                /* drop the field: unpowered tag resets to IDLE within ~10 ms */
                clrRegBits(ST25R3916_REG_OP_CONTROL,
                           ST25R3916_REG_OP_CONTROL_tx_en | ST25R3916_REG_OP_CONTROL_rx_en);
                vTaskDelay(10 / portTICK_PERIOD_MS);
                configure_nfc_a();  /* fresh field (collision avoidance + tx/rx on) */
            }
            if (nfca_poll_once(uid, uidLen, sak, atqa)) {
                return true;
            }
        }
        return false;
    };

    uint8_t probe[20];
    uint16_t probeLen = 18;
    uint8_t readCmd[2] = {0x30, 0x00};
    bool fastPathOk = nfca_transceive(probe, probeLen, readCmd, 2, TIMEOUT_READ, 16);

    static const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    if (!fastPathOk) {
        /* Tag not in usable ACTIVE state: recover via field cycle + re-select */
        if (!fieldCycleAndReselect()) {
            output += "tag re-select failed\n";
            _is_polling = pollingWasActive;
            return false;
        }
    }

    if (sak & 0x08) {
        /* MIFARE Classic: dump with default key A */
        uint8_t sectors = (sak & 0x10) ? 40 : 16;
        output += fmt::format("MIFARE Classic ({} sectors, key A FF..FF):\n", sectors);
        for (uint8_t s = 0; s < sectors; s++) {
            uint8_t blockIdx = s * 4;
            if (!mifare_auth(uid, blockIdx, false, defaultKey)) {
                output += fmt::format("S{:02d}: auth failed\n", s);
                continue;
            }
            for (uint8_t b = 0; b < 4; b++) {
                uint8_t data[16];
                if (!mifare_read_block(data, (uint8_t)(blockIdx + b))) {
                    output += fmt::format("  B{:3d}: read failed\n", blockIdx + b);
                    continue;
                }
                std::string line = fmt::format("  B{:3d}: ", blockIdx + b);
                for (int i = 0; i < 16; i++) line += fmt::format("{:02X} ", data[i]);
                output += line + "\n";
            }
        }
        _is_polling = pollingWasActive;
        return true;
    }

    /* NTAG / MIFARE UL (T2T): read pages until failure */
    output += "NTAG/MIFARE UL pages:\n";
    uint8_t page = 0;
    bool havePage0 = fastPathOk && probeLen >= 16;
    while (page < 256) {
        uint8_t rx[20];
        uint16_t rxLen = 18;  /* 16 data + 2 CRC (fifo may hold both) */
        if (havePage0) {
            memcpy(rx, probe, sizeof(probe));
            rxLen = probeLen;
            havePage0 = false;  /* only for the first iteration */
        } else {
            uint8_t cmdBuf[2] = {0x30, page};
            if (!nfca_transceive(rx, rxLen, cmdBuf, 2, TIMEOUT_READ, 16) || rxLen < 16) {
                mclog::tagInfo(_tag, "dump: T2T read fail page={} rxLen={}", page, rxLen);
                if (page == 0) {
                    output += "T2T read failed\n";
                    _is_polling = pollingWasActive;
                    return false;
                }
                break;
            }
        }
        std::string line = fmt::format("  P{:3d}: ", page);
        for (int i = 0; i < 16; i++) line += fmt::format("{:02X} ", rx[i]);
        output += line + "\n";
        page += 4;
    }

    _is_polling = pollingWasActive;
    return true;
}

/* ---------------------------------------------------------------- */
/* MIFARE Classic auth + read (Crypto1, from M5 sequences)           */
/* ---------------------------------------------------------------- */

bool CapNFC::mifare_auth(const uint8_t uid[4], uint8_t block, bool keyB, const uint8_t key[6])
{
    uint64_t key48 = 0;
    for (int i = 0; i < 6; i++) {
        key48 |= (uint64_t)key[i] << ((5 - i) * 8);
    }

    /* AUTH1 (plain) */
    uint8_t authFrame[2] = {(uint8_t)(keyB ? 0x61 : 0x60), block};
    uint8_t RB[4];
    uint16_t rlen = 4;
    if (!nfca_transceive(RB, rlen, authFrame, 2, TIMEOUT_AUTH1, 4)) {
        return false;
    }

    esp_rom_delay_us(87);

    uint32_t u32 = ((uint32_t)uid[0] << 24) | ((uint32_t)uid[1] << 16) | ((uint32_t)uid[2] << 8) | uid[3];
    uint32_t Nt  = ((uint32_t)RB[0] << 24) | ((uint32_t)RB[1] << 16) | ((uint32_t)RB[2] << 8) | RB[3];

    uint32_t Nr = esp_random();
    uint32_t Ar{}, suc3{};

    _crypto1.init(key48);
    (void)_crypto1.inject(u32, Nt, false);

    /* suc2/suc3: LFSR successors of the (byteswapped) card nonce, i.e. the
     * tag's PRNG output 2 and 3 rounds after Nt (M5 suc_23) */
    {
        mifare_classic::MLFSR48 lfsr;
        // seed LSB-first with byteswapped Nt
        uint32_t n = __builtin_bswap32(Nt);
        for (int i = 0; i < 32; i++) {
            lfsr._state[i] = (n >> i) & 1u;
        }
        (void)lfsr.step(); (void)lfsr.step();  // round 1..2
        // read 32 keystream bits as suc2
        for (int i = 0; i < 32; i++) {
            Ar |= (uint32_t)lfsr.step() << i;
        }
        for (int i = 0; i < 32; i++) {
            suc3 |= (uint32_t)lfsr.step() << i;
        }
    }

    uint8_t AB[8];
    uint8_t parity = _crypto1.encrypt(AB, Nr, Ar);

    /* append parity bits to bitstream (9 bits per byte) */
    uint8_t bitstream[9];
    {
        uint32_t bitpos = 0;
        for (uint32_t i = 0; i < 8; i++) {
            uint8_t v = AB[i];
            for (int k = 0; k < 8; k++) {
                uint8_t bit = (v >> k) & 1;
                if (bit) bitstream[bitpos >> 3] |= (uint8_t)(1 << (bitpos & 7));
                bitpos++;
            }
            uint8_t p = (parity >> i) & 1;
            if (p) bitstream[bitpos >> 3] |= (uint8_t)(1 << (bitpos & 7));
            bitpos++;
        }
    }

    cmd(ST25R3916_CMD_RESET_RXGAIN);

    write_fwt_timer(TIMEOUT_AUTH2);
    wrReg(ST25R3916_REG_ISO14443A_NFC, 0x80);  // no_tx_par
    setRegBits(ST25R3916_REG_AUX, ST25R3916_REG_AUX_no_crc_rx);
    { uint32_t dummy; readIrqRegs(dummy); }
    cmd(ST25R3916_CMD_CLEAR_FIFO);
    wrFifo(bitstream, sizeof(bitstream));
    {   /* 9 bytes, 0 bits */
        uint16_t val = (uint16_t)((9U << 3) | 0);
        wrReg(0x22, (uint8_t)((val >> 8) & 0xFF));
        wrReg(0x23, (uint8_t)(val & 0xFF));
    }
    cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

    if (!wait_for_fifo(TIMEOUT_AUTH2, 4)) {
        return false;
    }

    uint16_t actual = 0;
    uint8_t BA[4];
    if (!read_fifo(actual, BA, 4) || actual != 4) {
        return false;
    }

    uint8_t At2[4];
    for (int i = 0; i < 4; i++) {
        At2[i] = BA[i] ^ _crypto1.step8(0);
    }
    uint32_t At32 = (uint32_t)At2[0] | ((uint32_t)At2[1] << 8) | ((uint32_t)At2[2] << 16) | ((uint32_t)At2[3] << 24);
    _encrypted = (At32 == suc3);
    return _encrypted;
}

bool CapNFC::mifare_read_block(uint8_t out[16], uint8_t addr)
{
    if (!_encrypted) {
        /* plain read (should not happen after successful auth) */
        uint16_t rlen = 16;
        uint8_t cmdBuf[2] = {0x30, addr};
        return nfca_transceive(out, rlen, cmdBuf, 2, TIMEOUT_READ, 16) && rlen == 16;
    }

    /* encrypted read: send cmd encrypted, receive 16+2 crc encrypted */
    uint8_t cmdBuf[2] = {0x30, addr};
    uint8_t encTx[4];
    uint8_t plain[4];
    uint16_t crc = crc16A_mifare(cmdBuf, 2);
    plain[0] = cmdBuf[0];
    plain[1] = cmdBuf[1];
    plain[2] = (uint8_t)(crc & 0xFF);
    plain[3] = (uint8_t)(crc >> 8);

    uint32_t parity = _crypto1.encrypt(encTx, plain, 4);

    /* build bitstream with parity (9 bits/byte) */
    uint8_t bitstream[5];
    memset(bitstream, 0, sizeof(bitstream));
    {
        uint32_t bitpos = 0;
        for (uint32_t i = 0; i < 4; i++) {
            uint8_t v = encTx[i];
            for (int k = 0; k < 8; k++) {
                if ((v >> k) & 1) bitstream[bitpos >> 3] |= (uint8_t)(1 << (bitpos & 7));
                bitpos++;
            }
            if ((parity >> i) & 1) bitstream[bitpos >> 3] |= (uint8_t)(1 << (bitpos & 7));
            bitpos++;
        }
    }

    wrReg(ST25R3916_REG_ISO14443A_NFC, 0x80);  // no_tx_par
    { uint32_t dummy; readIrqRegs(dummy); }
    cmd(ST25R3916_CMD_CLEAR_FIFO);
    wrFifo(bitstream, 5);
    {   /* 4 bytes + 4 bits */
        uint16_t val = (uint16_t)((4U << 3) | 4U);
        wrReg(0x22, (uint8_t)((val >> 8) & 0xFF));
        wrReg(0x23, (uint8_t)(val & 0xFF));
    }
    cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

    if (!wait_for_fifo(TIMEOUT_READ, 18)) {
        return false;
    }

    uint16_t actual = 0;
    uint8_t rbuf[18];
    if (!read_fifo(actual, rbuf, 18) || actual != 18) {
        return false;
    }

    for (int i = 0; i < 18; i++) {
        rbuf[i] ^= _crypto1.step8(0);
    }

    /* verify CRC over decrypted 16 bytes */
    uint16_t calc = crc16A_mifare(rbuf, 16);
    uint16_t recv = (uint16_t)rbuf[16] | ((uint16_t)rbuf[17] << 8);
    if (calc != recv) {
        mclog::tagWarn(_tag, "mifare read crc mismatch");
        return false;
    }
    memcpy(out, rbuf, 16);
    return true;
}

/* ---------------------------------------------------------------- */
/* Diagnostics (kept from v6/v7)                                     */
