#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// Encoding names accepted on the command line, so an encodings list can be
// written as "h264,zrle,raw,quality9" instead of "50,16,0,-23".
inline std::optional<int32_t> encoding_from_name(std::string_view name) {
    std::string key{};
    key.reserve(name.size());
    for (char c : name) {
        // Spelling is forgiving: separators inside a name are ignored, so
        // "copy-rect", "copy_rect" and "copyrect" are all the same encoding.
        if (c == '-' || c == '_' || c == ' ') {
            continue;
        }
        key.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    if (key.empty()) {
        return std::nullopt;
    }
    // The levelled pseudo-encodings are ranges rather than single names, so
    // they are matched by prefix and the trailing digit becomes the level.
    auto levelled = [&key](std::string_view prefix,
                           int32_t base) -> std::optional<int32_t> {
        if (!key.starts_with(prefix) || key.size() != prefix.size() + 1) {
            return std::nullopt;
        }
        char digit = key[prefix.size()];
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        return base + (digit - '0');
    };
    // Quality 0 is -32 and quality 9 is -23; compression is the same shape at
    // -256..-247.
    for (std::string_view prefix : {"quality", "q"}) {
        if (auto value = levelled(prefix, -32)) {
            return value;
        }
    }
    for (std::string_view prefix : {"compression", "compress", "c"}) {
        if (auto value = levelled(prefix, -256)) {
            return value;
        }
    }
    if (key == "raw") return 0;
    if (key == "copyrect") return 1;
    if (key == "rre") return 2;
    if (key == "corre") return 4;
    if (key == "hextile") return 5;
    if (key == "zlib") return 6;
    if (key == "tight") return 7;
    if (key == "zlibhex") return 8;
    if (key == "trle") return 15;
    if (key == "zrle") return 16;
    if (key == "h264" || key == "h.264") return 50;
    if (key == "cursor") return -239;
    if (key == "desktopsize") return -223;
    if (key == "lastrect") return -224;
    if (key == "continuousupdates") return -313;
    return std::nullopt;
}

// Parses a whole encodings list, e.g. "h264,zrle,0,quality9". Entries are
// separated by any of ",; " and may be either a name or a signed number.
// Returns nothing if any entry is unrecognised, so a typo is reported rather
// than silently dropping an encoding.
inline std::optional<std::vector<int32_t>> parse_encodings(
        std::string_view text) {
    std::vector<int32_t> result{};
    size_t position = 0;
    while (position < text.size()) {
        auto end = text.find_first_of(",; ", position);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        auto token = text.substr(position, end - position);
        position = end + 1;
        if (token.empty()) {
            continue;
        }
        bool numeric = !token.empty() &&
            (std::isdigit(static_cast<unsigned char>(token.front())) ||
             ((token.front() == '-' || token.front() == '+') &&
              token.size() > 1));
        if (numeric) {
            try {
                result.push_back(
                    static_cast<int32_t>(std::stol(std::string{token})));
            }
            catch (...) {
                return std::nullopt;
            }
            continue;
        }
        auto value = encoding_from_name(token);
        if (!value) {
            return std::nullopt;
        }
        result.push_back(*value);
    }
    return result;
}
}
