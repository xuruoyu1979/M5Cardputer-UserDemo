/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake_log_signal.h>
#include <M5Unified.h>
#include <cstdint>
#include <string>
#include "mifare_crypto1.h"

/*
 * CapNFC v2: standalone ST25R3916 NFC-A driver ported from M5UnitUnifiedNFC
 * (unit_ST25R3916 / configure_nfc_a / nfca* sequences, MIT) on top of the
 * verified SPI platform layer. The RFAL state machine is no longer used.
 */
class CapNFC {
public:
    bool init();
    void deinit();

    bool startPolling();
    void worker();  // call from app loop; runs one poll attempt (blocking, few ms)
    void deactivate();

    // Signal: uid hex string, type string
    mclog::Signal<const std::string&, const std::string&> onTagDetected;

    inline bool isInited() const
    {
        return _is_inited;
    }
    inline bool isPolling() const
    {
        return _is_polling;
    }
    inline const std::string& getLastUid() const
    {
        return _last_uid;
    }
    inline const std::string& getLastType() const
    {
        return _last_type;
    }

    // Dump helpers, use while a tag is active
    bool dumpTag(std::string& output);

    static std::string uidToHex(const uint8_t* uid, uint8_t len);

private:
    bool _is_inited    = false;
    bool _is_polling   = false;
    bool _tag_active   = false;
    bool _encrypted    = false;
    std::string _last_uid;
    std::string _last_type;
    mifare_classic::Crypto1 _crypto1;

    /* chip-level helpers (M5 sequence equivalents, st25r3916_com based) */
    bool m5_begin_sequence();
    bool configure_nfc_a();
    bool nfca_poll_once(/* out */ uint8_t uid[10], uint8_t& uidLen, uint8_t& sak, uint16_t& atqa);

    /* low-level transceive primitives */
    bool nfca_request(uint16_t& atqa, bool wakeup, uint32_t* dbgFlags = nullptr, uint16_t* dbgFifo = nullptr);
    bool nfca_anticoll(uint8_t rbuf[5], uint8_t lv);
    bool nfca_transceive(uint8_t* rx, uint16_t& rx_len, const uint8_t* tx, uint16_t tx_len, uint32_t timeout_ms,
                         uint16_t min_rx_len);
    bool wait_for_irq(uint32_t waitBits32, uint32_t timeout_ms, uint32_t& outFlags);
    bool wait_for_fifo(uint32_t timeout_ms, uint16_t required_size);
    bool read_fifo(uint16_t& actual, uint8_t* buf, uint16_t buf_size);
    bool write_fwt_timer(uint32_t ms);
    uint32_t read_irq_cached();

    /* MIFARE */
    bool mifare_auth(const uint8_t uid[4], uint8_t block, bool keyB, const uint8_t key[6]);
    bool mifare_read_block(uint8_t out[16], uint8_t addr);
};
