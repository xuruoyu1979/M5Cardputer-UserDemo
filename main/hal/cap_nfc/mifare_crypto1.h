/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * Crypto1 cipher for MIFARE Classic.
 * Standalone port of M5UnitUnifiedNFC's mifare_classic_crypto1.hpp
 * (MIT licensed); the M5Utility LFSR template is inlined so this header
 * has no external dependencies.
 */
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace mifare_classic {

inline bool oddParityBit(const uint8_t v)
{
    uint8_t p = v;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    return (~p) & 1;
}

/*! 48-bit Fibonacci LFSR (left shift), tap set per Crypto1 */
struct MLFSR48 {
    using State = std::array<bool, 48>;
    State _state{};
    uint32_t _count{0};

    bool step()
    {
        const State& s = _state;
        const bool fb = s[48 - 5] ^ s[48 - 6] ^ s[48 - 7] ^ s[48 - 9] ^ s[48 - 13] ^ s[48 - 19] ^ s[48 - 21] ^
                        s[48 - 23] ^ s[48 - 24] ^ s[48 - 29] ^ s[48 - 31] ^ s[48 - 33] ^ s[48 - 34] ^ s[48 - 36] ^
                        s[48 - 38] ^ s[48 - 39] ^ s[48 - 43] ^ s[48 - 48];
        // shift left by one
        for (size_t i = 0; i + 1 < 48; ++i) {
            _state[i] = _state[i + 1];
        }
        _state[47] = fb;
        return fb;
    }
};

class Crypto1 {
public:
    Crypto1() noexcept = default;
    explicit Crypto1(const uint64_t key48) noexcept
    {
        init(key48);
    }

    void init(const uint64_t key48) noexcept
    {
        _lfsr._state = MLFSR48::State{};
        // Bit order within byte reversed to LSB first (as M5 implementation)
        for (int i = 0; i < 48; ++i) {
            int byte_index = i >> 3;
            int bit_index  = i & 0x07;
            int reversed   = (byte_index << 3) + (bit_index ^ 7);
            bool bit       = (key48 >> reversed) & 1ULL;
            _lfsr._state[i] = bit;
        }
        _lfsr._count = 0;
    }

    inline uint32_t inject(uint32_t uid, uint32_t Nt, const bool encrypted = false) noexcept
    {
        return step32(uid ^ Nt, encrypted);
    }

    bool step_with(const bool in, const bool enc = false) noexcept
    {
        ++_lfsr._count;
        bool z = filter();
        (void)_lfsr.step();
        const bool ext = in ^ (enc ? z : 0);
        _lfsr._state[0] = _lfsr._state[0] ^ ext;
        return z;
    }

    uint8_t step8(const uint8_t in, const bool enc = false) noexcept
    {
        uint8_t v{};
        for (uint_fast8_t i = 0; i < 8; ++i) {
            v |= step_with((in >> i) & 1, enc) << i;
        }
        return v;
    }

    uint32_t step32(const uint32_t in, const bool enc = false) noexcept
    {
        uint32_t v{};
        for (uint32_t i = 0; i < 32; ++i) {
            bool t = step_with((in >> (i ^ 24)) & 1u, enc);
            v |= (uint32_t)t << (24 ^ i);
        }
        return v;
    }

    uint8_t encrypt(uint8_t buf[8], const uint32_t Nr, const uint32_t Ar) noexcept
    {
        uint8_t parity{};
        for (uint_fast8_t i = 0; i < 4; ++i) {
            const uint8_t v = ((Nr >> ((i ^ 0x03) << 3)) & 0xFF);
            buf[i]          = step8(v) ^ v;
            const bool z    = filter();
            parity |= (uint8_t)((z ^ oddParityBit(v)) & 0x01) << i;
        }
        for (uint_fast8_t pos = 4; pos < 8; ++pos) {
            const uint8_t i = pos - 4;
            const uint8_t v = (Ar >> (i << 3)) & 0xFF;
            const uint8_t ks = step8(0x00);
            buf[pos]         = ks ^ v;
            const bool z     = filter();
            parity |= (uint8_t)((z ^ oddParityBit(v)) & 0x01) << pos;
        }
        return parity;
    }

    uint32_t encrypt(uint8_t* out, const uint8_t* in, const uint8_t in_len) noexcept
    {
        uint32_t parity{};
        for (uint_fast8_t i = 0; i < in_len; ++i) {
            uint8_t ks = step8(0);
            out[i]     = in[i] ^ ks;
            parity |= ((uint32_t)((filter() ^ oddParityBit(in[i])) & 1)) << i;
        }
        return parity;
    }

    inline bool filter() const noexcept
    {
        const MLFSR48::State& s = _lfsr._state;
        const bool b5 = fb(s[6], s[4], s[2], s[0]);
        const bool a4 = fa(s[14], s[12], s[10], s[8]);
        const bool b3 = fb(s[22], s[20], s[18], s[16]);
        const bool b2 = fb(s[30], s[28], s[26], s[24]);
        const bool a1 = fa(s[38], s[36], s[34], s[32]);
        return fc(a1, b2, b3, a4, b5);
    }

private:
    inline static bool fa(bool a, bool b, bool c, bool d) noexcept
    {
        return ((a || b) ^ (a && d)) ^ (c && ((a ^ b) || d));
    }
    inline static bool fb(bool a, bool b, bool c, bool d) noexcept
    {
        return ((a && b) || c) ^ ((a ^ b) && (c || d));
    }
    inline static bool fc(bool a, bool b, bool c, bool d, bool e) noexcept
    {
        return (a || ((b || e) && (d ^ e))) ^ ((a ^ (b && d)) && ((c ^ d) || (b && e)));
    }

    MLFSR48 _lfsr;
};

}  // namespace mifare_classic
