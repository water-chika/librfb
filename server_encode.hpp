#pragma once

// Server side encoders. The framebuffer is kept in the capture's native
// BGRX layout and converted to whatever the client asked for at send time,
// which is what SetPixelFormat is allowed to change at any point.

#include <zlib.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "server_platform.hpp"

namespace rfb::server {

struct pixel_format {
    uint8_t bits_per_pixel{32};
    uint8_t depth{24};
    uint8_t big_endian_flag{0};
    uint8_t true_colour_flag{1};
    uint16_t red_max{255};
    uint16_t green_max{255};
    uint16_t blue_max{255};
    uint8_t red_shift{16};
    uint8_t green_shift{8};
    uint8_t blue_shift{0};
};

// The format the capture layer natively produces, offered in ServerInit.
inline pixel_format native_pixel_format() {
    return pixel_format{};
}

// Converts internal BGRX pixels to the client's format and serialises them.
class pixel_writer {
public:
    pixel_writer() { set_format(native_pixel_format()); }

    void set_format(const pixel_format& format) {
        if (format.true_colour_flag == 0) {
            throw std::runtime_error{
                "colour map pixel formats are not supported; ask the viewer "
                "for true colour"};
        }
        if (format.bits_per_pixel != 8 && format.bits_per_pixel != 16 &&
            format.bits_per_pixel != 32) {
            throw std::runtime_error{"unsupported bits-per-pixel"};
        }
        m_format = format;
        m_bytes_per_pixel = format.bits_per_pixel / 8;

        std::array<uint8_t, 4> shifts{};
        for (int i = 0; i < m_bytes_per_pixel; i++) {
            shifts[i] = static_cast<uint8_t>(
                format.big_endian_flag
                    ? (m_bytes_per_pixel - 1 - i) * 8
                    : i * 8);
        }
        m_pixel_shifts = shifts;

        // ZRLE's CPIXEL drops the one byte that carries no colour bits, but
        // only for the 32bpp/depth<=24 case, and the shorter form is
        // mandatory there rather than optional.
        uint32_t mask = (static_cast<uint32_t>(format.red_max) << format.red_shift) |
                        (static_cast<uint32_t>(format.green_max) << format.green_shift) |
                        (static_cast<uint32_t>(format.blue_max) << format.blue_shift);
        m_cpixel_bytes = m_bytes_per_pixel;
        m_cpixel_shifts = m_pixel_shifts;
        if (format.bits_per_pixel == 32 && format.depth <= 24) {
            uint8_t dropped_shift = 0xff;
            if ((mask & 0xff000000u) == 0) {
                dropped_shift = 24;
            }
            else if ((mask & 0x000000ffu) == 0) {
                dropped_shift = 0;
            }
            if (dropped_shift != 0xff) {
                m_cpixel_bytes = 3;
                int out = 0;
                for (int i = 0; i < 4; i++) {
                    if (m_pixel_shifts[i] != dropped_shift) {
                        m_cpixel_shifts[out++] = m_pixel_shifts[i];
                    }
                }
            }
        }

        // The overwhelmingly common request is exactly what the capture
        // already holds, so recognise it and skip per pixel arithmetic.
        m_direct_copy = format.bits_per_pixel == 32 && format.big_endian_flag == 0 &&
                        format.red_max == 255 && format.green_max == 255 &&
                        format.blue_max == 255 && format.red_shift == 16 &&
                        format.green_shift == 8 && format.blue_shift == 0;
    }

    const pixel_format& format() const { return m_format; }
    int bytes_per_pixel() const { return m_bytes_per_pixel; }
    int cpixel_bytes() const { return m_cpixel_bytes; }

    // src points at one BGRX pixel.
    uint32_t to_pixel_value(const uint8_t* src) const {
        uint32_t blue = src[0];
        uint32_t green = src[1];
        uint32_t red = src[2];
        return ((red * m_format.red_max / 255) << m_format.red_shift) |
               ((green * m_format.green_max / 255) << m_format.green_shift) |
               ((blue * m_format.blue_max / 255) << m_format.blue_shift);
    }

    void append_pixel(std::vector<uint8_t>& out, uint32_t value) const {
        for (int i = 0; i < m_bytes_per_pixel; i++) {
            out.push_back(static_cast<uint8_t>(value >> m_pixel_shifts[i]));
        }
    }
    void append_cpixel(std::vector<uint8_t>& out, uint32_t value) const {
        for (int i = 0; i < m_cpixel_bytes; i++) {
            out.push_back(static_cast<uint8_t>(value >> m_cpixel_shifts[i]));
        }
    }

    // Raw encoding of a rectangle, appended to out.
    void append_raw(std::vector<uint8_t>& out, const frame_buffer& frame,
                    int x, int y, int width, int height) const {
        auto row_bytes = static_cast<size_t>(width) * m_bytes_per_pixel;
        auto offset = out.size();
        out.resize(offset + row_bytes * height);
        for (int row = 0; row < height; row++) {
            const uint8_t* src = frame.pixels.data() +
                (static_cast<size_t>(y + row) * frame.stride()) +
                static_cast<size_t>(x) * 4;
            uint8_t* dst = out.data() + offset + row_bytes * row;
            if (m_direct_copy) {
                std::memcpy(dst, src, row_bytes);
                continue;
            }
            for (int column = 0; column < width; column++) {
                auto value = to_pixel_value(src + static_cast<size_t>(column) * 4);
                for (int i = 0; i < m_bytes_per_pixel; i++) {
                    dst[static_cast<size_t>(column) * m_bytes_per_pixel + i] =
                        static_cast<uint8_t>(value >> m_pixel_shifts[i]);
                }
            }
        }
    }

private:
    pixel_format m_format{};
    int m_bytes_per_pixel{4};
    int m_cpixel_bytes{3};
    std::array<uint8_t, 4> m_pixel_shifts{{0, 8, 16, 24}};
    std::array<uint8_t, 4> m_cpixel_shifts{{0, 8, 16, 24}};
    bool m_direct_copy{true};
};

// ZRLE: 64x64 tiles, each either solid, palette indexed or raw, all pushed
// through one zlib stream that lives as long as the connection.
class zrle_encoder {
public:
    zrle_encoder() {
        // Level 6 is zlib's default and a good trade for screen content;
        // higher levels cost far more CPU than they save bandwidth.
        if (deflateInit(&m_zlib, 6) != Z_OK) {
            throw std::runtime_error{"deflateInit fail"};
        }
    }
    zrle_encoder(const zrle_encoder&) = delete;
    zrle_encoder& operator=(const zrle_encoder&) = delete;
    ~zrle_encoder() { deflateEnd(&m_zlib); }

    // Appends a full ZRLE rectangle body (u32 length + deflated tiles).
    void append_rect(std::vector<uint8_t>& out, const pixel_writer& writer,
                     const frame_buffer& frame, int x, int y, int width, int height) {
        m_tiles.clear();
        for (int tile_y = 0; tile_y < height; tile_y += 64) {
            int tile_height = std::min(64, height - tile_y);
            for (int tile_x = 0; tile_x < width; tile_x += 64) {
                int tile_width = std::min(64, width - tile_x);
                encode_tile(writer, frame, x + tile_x, y + tile_y,
                            tile_width, tile_height);
            }
        }

        m_compressed.clear();
        deflate_tiles();

        auto length = static_cast<uint32_t>(m_compressed.size());
        out.push_back(static_cast<uint8_t>(length >> 24));
        out.push_back(static_cast<uint8_t>(length >> 16));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.push_back(static_cast<uint8_t>(length));
        out.insert(out.end(), m_compressed.begin(), m_compressed.end());
    }

private:
    void encode_tile(const pixel_writer& writer, const frame_buffer& frame,
                     int x, int y, int width, int height) {
        int count = width * height;
        m_pixels.clear();
        m_pixels.reserve(static_cast<size_t>(count));
        for (int row = 0; row < height; row++) {
            const uint8_t* src = frame.pixels.data() +
                (static_cast<size_t>(y + row) * frame.stride()) +
                static_cast<size_t>(x) * 4;
            for (int column = 0; column < width; column++) {
                m_pixels.push_back(writer.to_pixel_value(src + static_cast<size_t>(column) * 4));
            }
        }

        // A palette of more than 16 entries cannot be encoded, so give up on
        // the scan as soon as it grows past that.
        m_palette.clear();
        bool palette_usable = true;
        for (auto pixel : m_pixels) {
            bool found = false;
            for (auto entry : m_palette) {
                if (entry == pixel) { found = true; break; }
            }
            if (found) continue;
            if (m_palette.size() == 16) { palette_usable = false; break; }
            m_palette.push_back(pixel);
        }

        if (palette_usable && m_palette.size() == 1) {
            m_tiles.push_back(1); // solid
            writer.append_cpixel(m_tiles, m_palette[0]);
            return;
        }
        if (palette_usable && m_palette.size() >= 2) {
            append_palette_tile(writer, width, height);
            return;
        }

        m_tiles.push_back(0); // raw
        for (auto pixel : m_pixels) {
            writer.append_cpixel(m_tiles, pixel);
        }
    }

    void append_palette_tile(const pixel_writer& writer, int width, int height) {
        auto palette_size = static_cast<uint8_t>(m_palette.size());
        m_tiles.push_back(palette_size);
        for (auto entry : m_palette) {
            writer.append_cpixel(m_tiles, entry);
        }

        int bits = 1;
        if (palette_size > 2) bits = 2;
        if (palette_size > 4) bits = 4;

        // Indices are packed most significant bit first and every row starts
        // on a byte boundary.
        for (int row = 0; row < height; row++) {
            uint8_t byte = 0;
            int used = 0;
            for (int column = 0; column < width; column++) {
                auto pixel = m_pixels[static_cast<size_t>(row) * width + column];
                uint8_t index = 0;
                for (uint8_t i = 0; i < palette_size; i++) {
                    if (m_palette[i] == pixel) { index = i; break; }
                }
                byte = static_cast<uint8_t>((byte << bits) | index);
                used += bits;
                if (used == 8) {
                    m_tiles.push_back(byte);
                    byte = 0;
                    used = 0;
                }
            }
            if (used != 0) {
                m_tiles.push_back(static_cast<uint8_t>(byte << (8 - used)));
            }
        }
    }

    void deflate_tiles() {
        constexpr size_t chunk = 64 * 1024;
        m_zlib.next_in = m_tiles.data();
        m_zlib.avail_in = static_cast<uInt>(m_tiles.size());
        do {
            auto offset = m_compressed.size();
            m_compressed.resize(offset + chunk);
            m_zlib.next_out = m_compressed.data() + offset;
            m_zlib.avail_out = static_cast<uInt>(chunk);
            // Z_SYNC_FLUSH ends every rectangle at a byte boundary while
            // keeping the dictionary, which is what ZRLE requires.
            int ret = deflate(&m_zlib, Z_SYNC_FLUSH);
            if (ret != Z_OK) {
                throw std::runtime_error{"zlib deflate fail"};
            }
            m_compressed.resize(offset + chunk - m_zlib.avail_out);
        } while (m_zlib.avail_out == 0);
    }

    z_stream m_zlib{};
    std::vector<uint8_t> m_tiles{};
    std::vector<uint8_t> m_compressed{};
    std::vector<uint32_t> m_pixels{};
    std::vector<uint32_t> m_palette{};
};

} // namespace rfb::server
