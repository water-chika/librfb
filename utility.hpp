#pragma once

namespace rfb {
uint16_t from_big_endian_bytes(uint8_t b0, uint8_t b1) {
    return
        (static_cast<uint16_t>(b0) << (1*8)) |
        (static_cast<uint16_t>(b1) << (0*8)) |
        0;
}
uint8_t to_big_endian_byte(uint16_t n, uint8_t i) {
    assert(i < sizeof(n));
    return static_cast<uint8_t>(n >> (((sizeof(n)-1)-i)*8));
}
uint32_t from_big_endian_bytes(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    return
        (static_cast<uint16_t>(b0) << (3*8)) |
        (static_cast<uint16_t>(b1) << (2*8)) |
        (static_cast<uint16_t>(b2) << (1*8)) |
        (static_cast<uint16_t>(b3) << (0*8)) |
        0;
}
uint8_t to_big_endian_byte(uint32_t n, uint8_t i) {
    assert(i < sizeof(n));
    return static_cast<uint8_t>(n >> (((sizeof(n)-1)-i)*8));
}
uint32_t to_big_endian(uint32_t n) {
    return
        ((n&(0xff<<(3*8))) >> (3*8)) |
        ((n&(0xff<<(2*8))) >> (1*8)) |
        ((n&(0xff<<(1*8))) << (1*8)) |
        ((n&(0xff<<(0*8))) << (3*8)) |
        0;
}
}
