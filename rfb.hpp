#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

// OpenSSL's low-level DES API is deprecated in 3.x but is the only practical way
// to get single DES, which the RFB "VNC Authentication" security type mandates
// (EVP moved single DES into the legacy provider).
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/des.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include <cpp_helper.hpp>
#include <socket_helper.hpp>

#define INPUT_SIZE 4096

namespace rfb {
using cpp_helper::configure;
using cpp_helper::empty_configurable_class;
}

#include "zrle.hpp"
#include "h264.hpp"

namespace rfb {

// Thrown when the peer closes the connection, as distinct from an I/O error.
// A clean shutdown is normal (the server exited, the tunnel dropped), so
// callers generally want to unwind and quit rather than report a failure.
struct connection_closed : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class encoding : uint32_t {
    raw = 0,
    zrle = 16,
    h264 = 50,
    // Private librfb pseudo-encoding: "server supports live rescaling, and
    // the rectangle header carries the size it is now serving". -1729 as
    // unsigned. Pseudo-encodings are ignorable by definition, so a standard
    // server simply never mentions it and a standard viewer never sees it.
    scale_ext = 0xFFFFF93Fu,
};
static auto encoding_str_map = std::map<rfb::encoding, std::string_view>{
    {encoding::raw, "raw"},
    {encoding::zrle, "ZRLE"},
    {encoding::h264, "H264"},
    {encoding::scale_ext, "SCALE_EXT"},
};

auto& operator<<(std::ostream& out, rfb::encoding encode) {
    return out << encoding_str_map[encode];
}

}

template<>
class std::formatter<rfb::encoding, char> {
public:
    template<typename ParseContext>
    constexpr ParseContext::iterator parse(ParseContext& ctx) {
        auto ite = ctx.begin();
        if (ite == ctx.end()) {
            return ite;
        }
        return ite;
    }
    template<typename FormatContext>
    FormatContext::iterator format(rfb::encoding encoding, FormatContext& ctx) const {
        auto& str = rfb::encoding_str_map[encoding];
        return std::copy(str.begin(), str.end(), ctx.out());
    }
};

namespace rfb {

uint16_t parse_3digits(char d100, char d10, char d) {
    return (d100-'0')*100 + (d10-'0')*10 + (d-'0');
}


template<typename T>
class add_process_framebuffer_update : public T{
public:
    using parent = T;
    add_process_framebuffer_update(const configure auto& conf) : parent{conf},
        internal_frame(parent::get_width()*parent::get_height()*4)
    {
    }
private:
    bool scale_ext_supported{false};
public:

    bool is_scale_ext_supported() const { return scale_ext_supported; }

    auto process_framebuffer_update(auto& framebuffer_update_head) {
        uint16_t rectangles_count = from_big_endian_bytes(framebuffer_update_head[2], framebuffer_update_head[3]);
        //std::cerr << "rectangles count: " << rectangles_count << std::endl;
        std::vector<uint8_t> last_update{};
        for (uint16_t i = 0; i < rectangles_count; i++) {
            std::array<uint8_t, 12> rectangle{};
            try{
            parent::rfb_read(rectangle);
            }
            catch (connection_closed&) {
                throw;
            }
            catch (std::exception& e) {
                throw std::runtime_error(std::format("framebuffer update rectangle read fail: {}", e.what()));
            }
            uint16_t fb_width = parent::get_width();
            uint16_t fb_height = parent::get_height();
            uint16_t x = from_big_endian_bytes(rectangle[0], rectangle[1]);
            uint16_t y = from_big_endian_bytes(rectangle[2], rectangle[3]);
            uint16_t width = from_big_endian_bytes(rectangle[4], rectangle[5]);
            uint16_t height = from_big_endian_bytes(rectangle[6], rectangle[7]);
            encoding_type = static_cast<encoding>(from_big_endian_bytes(rectangle[8], rectangle[9], rectangle[10], rectangle[11]));
            frame_network_byte_count = 0;
            if (false) {
                std::cerr << "x: " << x << std::endl;
                std::cerr << "y: " << y << std::endl;
                std::cerr << "width: " << width << std::endl;
                std::cerr << "height: " << height << std::endl;
                std::cerr << "encoding_type: " << encoding_type << std::endl;
            }
            if (encoding_type == encoding::scale_ext) {
                // Carries no pixel data: the header IS the message. Seeing it
                // at all is the server advertising support, so the client may
                // start asking for scale changes from here on.
                auto frame_bytes = static_cast<size_t>(width) * height * 4;
                if (!width || !height || frame_bytes > 256 * 1024 * 1024)
                    throw std::runtime_error{"invalid framebuffer dimensions"};
                scale_ext_supported = true;
                parent::set_framebuffer_size(width, height);
                internal_frame.assign(frame_bytes, 0);
                fb_width = width;
                fb_height = height;
                continue;
            }
            if (!width || !height || x > fb_width || y > fb_height ||
                width > fb_width - x || height > fb_height - y)
                throw std::runtime_error{"framebuffer rectangle is out of bounds"};
            if (encoding_type == encoding::raw) {
                auto pixel_bytes = static_cast<size_t>(width) * height * 4;
                auto pixels = std::vector<uint8_t>(pixel_bytes);
                frame_network_byte_count += pixel_bytes;
                try{
                parent::rfb_read(pixels);
                }
                catch (connection_closed&) {
                    throw;
                }
                catch (std::exception& e) {
                    throw std::runtime_error(std::format("framebuffer update rectangle pixels read fail: {}",
                                e.what()));
                }
                for (int  y_ = 0; y_ < height; y_++) {
                    for (int x_ = 0; x_ < width; x_++) {
                        internal_frame[((y+y_)*fb_width+(x+x_))*4+0] = pixels[(y_*width + x_)*4+0];
                        internal_frame[((y+y_)*fb_width+(x+x_))*4+1] = pixels[(y_*width + x_)*4+1];
                        internal_frame[((y+y_)*fb_width+(x+x_))*4+2] = pixels[(y_*width + x_)*4+2];
                        internal_frame[((y+y_)*fb_width+(x+x_))*4+3] = pixels[(y_*width + x_)*4+3];
                    }
                }
            }
            else if (encoding_type == encoding::zrle) {
                std::array<uint8_t, 4> length_buf;
                try{
                parent::rfb_read(length_buf);
                }
                catch (connection_closed&) {
                    throw;
                }
                catch (std::exception& e) {
                    throw std::runtime_error("framebuffer update rectangle zrle length read fail");
                }
                auto length = from_big_endian_bytes(length_buf[0], length_buf[1], length_buf[2], length_buf[3]);
                //std::cerr << "zlib length: " << length << std::endl;
                auto zlib_data = std::vector<uint8_t>(length);
                frame_network_byte_count += length;
                try{
                parent::rfb_read(zlib_data);
                }
                catch (connection_closed&) {
                    throw;
                }
                catch (std::exception& e) {
                    throw std::runtime_error("framebuffer update rectangle zrle zlib data read fail");
                }
                parent::zrle_decode(zlib_data, x, y, width, height, internal_frame);
            }
            else if (encoding_type == encoding::h264) {
                std::array<uint8_t, 8> message;
                parent::rfb_read(message);
                auto length = from_big_endian_bytes(message[0], message[1], message[2], message[3]);
                auto flags = from_big_endian_bytes(message[4], message[5], message[6], message[7]);
                auto h264_data = std::vector<uint8_t>(length);
                frame_network_byte_count += length;
                parent::rfb_read(h264_data);
                parent::h264_decode(h264_data, x, y, width, height, internal_frame);
            }
            else {
                throw std::runtime_error(std::format("encoding not support: {}", encoding_type));
            }
        }
        frame_updated = true;
        last_update_time = std::chrono::steady_clock::now();
    }
    void get_frame(std::span<uint8_t> frame) {
        std::copy(internal_frame.begin(), internal_frame.end(), frame.begin());
    }
    auto get_frame() {
        return internal_frame;
    }
    bool is_frame_updated() {
        return frame_updated;
    }
    void reset_frame_updated() {
        frame_updated = false;
    }
    auto get_encoding() {
        return encoding_type;
    }
    auto get_frame_network_byte_count() {
        return frame_network_byte_count;
    }
    // How long ago the last FramebufferUpdate finished arriving. This is the
    // one number that tells a stalled stream apart from a static screen: a
    // static remote desktop still has this stay small if the client keeps
    // asking (RFB just answers "nothing changed" quickly), while a genuinely
    // stuck request loop makes it grow without bound.
    double ms_since_last_update() const {
        return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - last_update_time).count();
    }
private:
    std::vector<uint8_t> internal_frame;
    bool frame_updated;
    encoding encoding_type;
    uint32_t frame_network_byte_count;
    std::chrono::steady_clock::time_point last_update_time{
        std::chrono::steady_clock::now()};
};

template<typename T>
concept contain_ip_address = requires (T t) {
    t.address;
    t.port;
};

template<typename T>
class set_address : public T {
public:
    using parent = T;
    template<configure Configure>
        requires contain_ip_address<Configure>
    set_address(const Configure& conf) : parent{conf},
        address{conf.address} {
    }
    set_address(const configure auto& conf) : parent{conf} {
    }
    auto get_address() {
        return address;
    }
    const char* address;
};
template<typename T>
class set_port : public T {
public:
    using parent = T;
    template<configure Configure>
        requires contain_ip_address<Configure>
    set_port(const Configure& conf) : parent{conf},
        port{conf.port} {
    }
    set_port(const configure auto& conf) : parent{conf} {
    }
    auto get_port() {
        return port;
    }
    uint16_t port;
};

template<typename T>
class add_connection : public T {
public:
    using parent = T;
    add_connection(const configure auto& conf) : parent{conf},
        connection{conf}
    {
    }
    // A zero-length read means the peer performed an orderly shutdown. The
    // previous code only treated negative returns as failure, so at EOF this
    // loop spun forever without ever advancing s, wedging the caller's event
    // loop and making the window impossible to close.
    void rfb_read(auto& buf) {
        size_t s = 0;
        size_t buf_bytes_count = buf.size()*sizeof(buf[0]);
        while (s < buf_bytes_count) {
            auto len = socket_helper::read_socket(
                    connection.get_socket(),
                    reinterpret_cast<char*>(buf.data())+s,
                    buf_bytes_count-s);
            if (len == 0)
                throw connection_closed{"server closed the connection"};
            if (len < 0)
                throw std::runtime_error{
                    std::format("read fail: {}", socket_helper::last_error())};
            s += len;
        }
    }
    void rfb_write(const auto& buf) {
        size_t s = 0;
        size_t buf_bytes_count = buf.size()*sizeof(buf[0]);
        while (s < buf_bytes_count) {
            auto len = socket_helper::write_socket(
                    connection.get_socket(),
                    reinterpret_cast<const char*>(buf.data())+s,
                    buf_bytes_count-s);
            if (len == 0)
                throw connection_closed{"server closed the connection"};
            if (len < 0)
                throw std::runtime_error{
                    std::format("write fail: {}", socket_helper::last_error())};
            s += len;
        }
    }
    auto get_socket() {
        return connection.get_socket();
    }
private:
    using connection_t =
        socket_helper::connect_address_port<
        set_port<
        set_address<
        socket_helper::add_socket<
        empty_configurable_class
        >>>>;
    connection_t connection;
};

// Buffers post-handshake server traffic on a nonblocking socket and exposes
// only whole, resource-bounded RFB messages to the existing decoders. Handshake
// reads stay blocking because this layer is enabled explicitly by an event-loop
// client.
template<typename T>
class add_server_message_buffer : public T {
public:
    using parent = T;
    add_server_message_buffer(const configure auto& conf) : parent{conf} {}

    // Bound each event-loop turn even when a peer continuously keeps the socket
    // readable. The next turn resumes from the buffered bytes.
    bool read_available_server_data() {
        if (!socket_helper::set_nonblocking(parent::get_socket()))
            throw std::runtime_error{"could not make RFB socket nonblocking"};
        auto restore_blocking = [&] {
            if (!socket_helper::set_nonblocking(parent::get_socket(), false))
                throw std::runtime_error{"could not restore blocking RFB socket"};
        };
        try {
            std::array<uint8_t, INPUT_SIZE> chunk{};
            for (size_t read = 0; read < max_reads_per_poll; ++read) {
                auto count = socket_helper::read_socket(parent::get_socket(),
                                                         chunk.data(), chunk.size());
                if (count > 0) {
                    if (buffered_size() + static_cast<size_t>(count) > max_buffered_bytes)
                        throw std::runtime_error{"RFB input exceeds resource limit"};
                    m_input.insert(m_input.end(), chunk.begin(), chunk.begin() + count);
                    continue;
                }
                if (count == 0) {
                    m_closed = true;
                    restore_blocking();
                    return false;
                }
                auto error = socket_helper::last_error();
                if (socket_helper::would_block(error)) {
                    restore_blocking();
                    return true;
                }
#ifndef _WIN32
                if (error == EINTR) { --read; continue; }
#endif
                throw std::runtime_error{std::format("read fail: {}", error)};
            }
            restore_blocking();
            return true;
        }
        catch (...) {
            socket_helper::set_nonblocking(parent::get_socket(), false);
            throw;
        }
    }

    bool has_complete_server_message() {
        if (!m_cached_message_size) m_cached_message_size = next_server_message_size();
        return m_cached_message_size.has_value();
    }

    void begin_complete_server_message() {
        if (!has_complete_server_message())
            throw std::logic_error{"incomplete RFB server message"};
        m_message_size = m_cached_message_size;
        m_message_offset = 0;
    }

    void finish_complete_server_message() {
        if (!m_message_size || m_message_offset != *m_message_size)
            throw std::logic_error{"RFB decoder did not consume one message"};
        m_consumed += *m_message_size;
        m_message_size.reset();
        m_cached_message_size.reset();
        m_message_offset = 0;
        compact_input();
    }

    bool server_connection_closed() const { return m_closed; }
    bool has_buffered_server_data() const { return buffered_size() != 0; }

    void rfb_read(auto& buf) {
        if (!m_message_size) {
            parent::rfb_read(buf);
            return;
        }
        auto bytes = buf.size() * sizeof(buf[0]);
        if (bytes > *m_message_size - m_message_offset)
            throw std::logic_error{"RFB decoder read beyond framed message"};
        std::memcpy(buf.data(), m_input.data() + m_consumed + m_message_offset, bytes);
        m_message_offset += bytes;
    }

    void append_server_data(std::span<const uint8_t> bytes) {
        if (buffered_size() + bytes.size() > max_buffered_bytes)
            throw std::runtime_error{"RFB input exceeds resource limit"};
        m_input.insert(m_input.end(), bytes.begin(), bytes.end());
        m_cached_message_size.reset();
    }

private:
    static constexpr size_t max_reads_per_poll = 4;
    static constexpr size_t max_buffered_bytes = 64 * 1024 * 1024;
    static constexpr size_t max_clipboard_bytes = 16 * 1024 * 1024;
    static constexpr size_t max_encoded_rectangle_bytes = 32 * 1024 * 1024;
    static constexpr size_t max_framebuffer_bytes = 256 * 1024 * 1024;
    static constexpr uint16_t max_rectangles = 4096;

    size_t buffered_size() const { return m_input.size() - m_consumed; }
    uint8_t byte_at(size_t offset) const { return m_input[m_consumed + offset]; }

    static size_t checked_add(size_t left, size_t right) {
        if (right > std::numeric_limits<size_t>::max() - left)
            throw std::runtime_error{"RFB server message is too large"};
        return left + right;
    }
    static size_t checked_multiply(size_t left, size_t right) {
        if (left && right > std::numeric_limits<size_t>::max() / left)
            throw std::runtime_error{"RFB server message is too large"};
        return left * right;
    }
    static size_t checked_frame_bytes(uint16_t width, uint16_t height) {
        if (!width || !height) throw std::runtime_error{"invalid framebuffer dimensions"};
        auto bytes = checked_multiply(checked_multiply(width, height), size_t{4});
        if (bytes > max_framebuffer_bytes)
            throw std::runtime_error{"framebuffer exceeds resource limit"};
        return bytes;
    }

    void compact_input() {
        if (!m_consumed) return;
        if (m_consumed == m_input.size()) {
            m_input.clear();
            m_consumed = 0;
            if (m_input.capacity() > max_buffered_bytes / 4) {
                std::vector<uint8_t>{}.swap(m_input);
            }
        }
        else if (m_consumed >= m_input.size() / 2) {
            m_input.erase(m_input.begin(), m_input.begin() + m_consumed);
            m_consumed = 0;
        }
    }

    std::optional<size_t> next_server_message_size() {
        if (!buffered_size()) return {};
        switch (byte_at(0)) {
        case 0: return framebuffer_update_size();
        case 1: {
            if (buffered_size() < 6) return {};
            auto count = from_big_endian_bytes(byte_at(4), byte_at(5));
            auto size = checked_add(size_t{6}, checked_multiply(count, size_t{6}));
            return buffered_size() >= size ? std::optional<size_t>{size} : std::nullopt;
        }
        case 2: return size_t{1};
        case 3: {
            if (buffered_size() < 8) return {};
            auto length = from_big_endian_bytes(byte_at(4), byte_at(5), byte_at(6), byte_at(7));
            if (length > max_clipboard_bytes)
                throw std::runtime_error{"clipboard payload exceeds resource limit"};
            auto size = checked_add(size_t{8}, length);
            return buffered_size() >= size ? std::optional<size_t>{size} : std::nullopt;
        }
        default:
            throw std::runtime_error{std::format(
                "server message type not supported: {}", byte_at(0))};
        }
    }

    std::optional<size_t> framebuffer_update_size() {
        if (buffered_size() < 4) return {};
        auto rectangles = from_big_endian_bytes(byte_at(2), byte_at(3));
        if (rectangles > max_rectangles)
            throw std::runtime_error{"framebuffer rectangle count exceeds resource limit"};
        size_t offset = 4;
        auto fb_width = static_cast<uint16_t>(parent::get_width());
        auto fb_height = static_cast<uint16_t>(parent::get_height());
        for (uint16_t i = 0; i < rectangles; ++i) {
            if (buffered_size() < checked_add(offset, 12)) return {};
            auto x = from_big_endian_bytes(byte_at(offset), byte_at(offset + 1));
            auto y = from_big_endian_bytes(byte_at(offset + 2), byte_at(offset + 3));
            auto width = from_big_endian_bytes(byte_at(offset + 4), byte_at(offset + 5));
            auto height = from_big_endian_bytes(byte_at(offset + 6), byte_at(offset + 7));
            auto type = static_cast<encoding>(from_big_endian_bytes(
                byte_at(offset + 8), byte_at(offset + 9),
                byte_at(offset + 10), byte_at(offset + 11)));
            offset += 12;
            size_t payload{};
            if (type == encoding::scale_ext) {
                checked_frame_bytes(width, height);
                fb_width = width;
                fb_height = height;
            }
            else {
                if (!width || !height || x > fb_width || y > fb_height ||
                    width > fb_width - x || height > fb_height - y)
                    throw std::runtime_error{"framebuffer rectangle is out of bounds"};
                if (type == encoding::raw) {
                    payload = checked_frame_bytes(width, height);
                }
                else if (type == encoding::zrle || type == encoding::h264) {
                    auto prefix = type == encoding::zrle ? size_t{4} : size_t{8};
                    if (buffered_size() < checked_add(offset, prefix)) return {};
                    auto length = from_big_endian_bytes(
                        byte_at(offset), byte_at(offset + 1),
                        byte_at(offset + 2), byte_at(offset + 3));
                    if (length > max_encoded_rectangle_bytes)
                        throw std::runtime_error{"encoded rectangle exceeds resource limit"};
                    payload = checked_add(prefix, length);
                }
                else {
                    throw std::runtime_error{std::format("encoding not support: {}", type)};
                }
            }
            offset = checked_add(offset, payload);
            if (offset > max_buffered_bytes)
                throw std::runtime_error{"RFB server message exceeds resource limit"};
            if (buffered_size() < offset) return {};
        }
        return offset;
    }

    std::vector<uint8_t> m_input;
    size_t m_consumed{};
    std::optional<size_t> m_cached_message_size;
    std::optional<size_t> m_message_size;
    size_t m_message_offset{};
    bool m_closed{};
};

struct pixel_format {
    uint8_t bits_per_pixel;
    uint8_t depth;
    uint8_t big_endian_flag;
    uint8_t true_colour_flag;
    uint16_t red_max;
    uint16_t green_max;
    uint16_t blue_max;
    uint8_t red_shift;
    uint8_t green_shift;
    uint8_t blue_shift;
    uint8_t padding0;
    uint8_t padding1;
    uint8_t padding2;
};
struct server_init_message {
    uint16_t fb_width{}, fb_height{};
    pixel_format server_pixel_format{};
    uint32_t name_length{};
    std::vector<char> name{};
};

template<typename T>
class add_server_init : public T {
public:
    using parent = T;
    add_server_init(const configure auto& conf) : parent{conf} {
    }

    auto server_init()
    {
        uint16_t fb_width{}, fb_height{};
        pixel_format server_pixel_format{};
        uint32_t name_length{};
        std::vector<char> name{};

        std::array<uint8_t, 24> server_init_buf{};
        try{
        parent::rfb_read(server_init_buf);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("server_init_buf read fail: {}", e.what())};
        }
        fb_width = from_big_endian_bytes(server_init_buf[0], server_init_buf[1]);
        fb_height = from_big_endian_bytes(server_init_buf[2], server_init_buf[3]);
        server_pixel_format.bits_per_pixel = server_init_buf[4];
        server_pixel_format.depth = server_init_buf[5];
        server_pixel_format.big_endian_flag = server_init_buf[6];
        server_pixel_format.true_colour_flag = server_init_buf[7];
        server_pixel_format.red_max = from_big_endian_bytes(server_init_buf[8], server_init_buf[9]);
        server_pixel_format.green_max = from_big_endian_bytes(server_init_buf[10], server_init_buf[11]);
        server_pixel_format.blue_max = from_big_endian_bytes(server_init_buf[12], server_init_buf[13]);
        server_pixel_format.red_shift = server_init_buf[14];
        server_pixel_format.green_shift = server_init_buf[15];
        server_pixel_format.blue_shift = server_init_buf[16];
        server_pixel_format.padding0 = server_init_buf[17];
        server_pixel_format.padding1 = server_init_buf[18];
        server_pixel_format.padding2 = server_init_buf[19];

        name_length = from_big_endian_bytes(server_init_buf[20], server_init_buf[21], server_init_buf[22], server_init_buf[23]);
        std::cerr << "name length: " << name_length << std::endl;
        name.resize(name_length);
        try{
        parent::rfb_read(name);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            std::cerr << "name parse fail" << std::endl;
        }
        std::cerr << "fb resolution: " << fb_width << "x" << fb_height << std::endl;
        std::cerr << "bpp: " << (int)server_pixel_format.bits_per_pixel << std::endl;
        std::cerr << "depth: " << (int)server_pixel_format.depth << std::endl;
        std::cerr << "big endian: " << (int)server_pixel_format.big_endian_flag << std::endl;
        std::cerr << "true colour: " << (int)server_pixel_format.true_colour_flag << std::endl;
        std::cerr << "color max: " << server_pixel_format.red_max << ", " << server_pixel_format.green_max << ", " << server_pixel_format.blue_max << std::endl;
        std::cerr << "color shift: " << (int)server_pixel_format.red_shift << ", " << (int)server_pixel_format.green_shift << ", " << (int)server_pixel_format.blue_shift << std::endl;
        name.push_back('\0');
        std::cerr << "server name: " << name.data() << std::endl;
        
        server_init_message m{
            .fb_width = fb_width, .fb_height = fb_height,
            .server_pixel_format = server_pixel_format,
            .name_length = name_length,
            .name = name
        };
        return m;
    }
};

template<typename T>
class add_client_init : public T {
public:
    using parent = T;
    static constexpr uint8_t security_type_none = 1;
    static constexpr uint8_t security_type_vnc_auth = 2;

    add_client_init(const configure auto& conf) : parent{conf} {
    }
    std::pair<uint16_t, uint16_t> parse_protocol_version(const std::array<char,12>& message) {
        bool is_rfb = message[0] == 'R' &&
            message[1] == 'F' &&
            message[2] == 'B' &&
            message[3] == ' ' &&
            message[7] == '.' &&
            message[11] == '\n';
        uint16_t major = parse_3digits(message[4], message[5], message[6]);
        uint16_t minor = parse_3digits(message[8], message[9], message[10]);
        return {major, minor};
    }

    int client_init() {
        std::array<char, 12> buf;
        try{
        parent::rfb_read(buf);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("client init fail: {}", e.what())};
        }
        std::cerr << "get response: "; std::cerr.write(buf.data(), buf.size());
        auto [major, minor] = parse_protocol_version(buf);
        std::cerr << "RFB version: " << major << "." << minor << std::endl;
        if (major != 3 || minor != 8) {
            std::cerr << "version only support 3.8" << std::endl;
            return -1;
        }

        try{
        parent::rfb_write(buf);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("client init write fail: {}", e.what())};
        }

        {
            std::array<uint8_t,1> security_count{};
            try{
            parent::rfb_read(security_count);
            }
            catch (connection_closed&) {
                throw;
            }
            catch (std::exception& e) {
                throw std::runtime_error{std::format("security count read fail: {}", e.what())};
            }
            if (security_count[0] == 0) {
                throw std::runtime_error{std::format("connection rejected by server: {}", read_failure_reason())};
            }
            std::cerr << "Server supportted security count: " << static_cast<int>(security_count[0]) << std::endl;

            std::vector<uint8_t> security_types(security_count[0]);
            try{
            parent::rfb_read(security_types);
            }
            catch (connection_closed&) {
                throw;
            }
            catch (std::exception& e) {
                throw std::runtime_error{std::format("security types read fail: {}", e.what())};
            }
            bool exist_none = false;
            bool exist_vnc_auth = false;
            for (int i = 0; i < security_types.size(); i++) {
                std::cerr << static_cast<int>(security_types[i]) << ",";
                if (security_types[i] == security_type_none) exist_none = true;
                if (security_types[i] == security_type_vnc_auth) exist_vnc_auth = true;
            }
            std::cerr << std::endl;

            // A configured password takes priority: servers that offer both
            // types still expect VncAuth when one was supplied.
            auto password = get_configured_password();
            uint8_t chosen_security_type{};
            if (exist_vnc_auth && !password.empty()) {
                chosen_security_type = security_type_vnc_auth;
            }
            else if (exist_none) {
                chosen_security_type = security_type_none;
            }
            else if (exist_vnc_auth) {
                throw std::runtime_error{"server requires VNC authentication but no password was configured"};
            }
            else {
                throw std::runtime_error{"no mutually supported security type (only None and VNC Authentication are implemented)"};
            }
            std::cerr << "security type: " << static_cast<int>(chosen_security_type) << std::endl;

            try{
            parent::rfb_write(std::array<uint8_t, 1>{chosen_security_type});
            }
            catch (connection_closed&) {
                throw;
            }
            catch (std::exception& e) {
                throw std::runtime_error{std::format("security write fail: {}", e.what())};
            }

            if (chosen_security_type == security_type_vnc_auth) {
                vnc_authenticate(password);
            }

            std::array<uint8_t, 4> security_result_buf{};
            try{
            parent::rfb_read(security_result_buf);
            }
            catch (connection_closed&) {
                throw;
            }
            catch (std::exception& e) {
                throw std::runtime_error{std::format("security result read fail: {}", e.what())};
            }

            uint32_t security_result = from_big_endian_bytes(security_result_buf[0], security_result_buf[1], security_result_buf[2], security_result_buf[3]);
            std::cerr << "security handshaking result: " << security_result << std::endl;
            if (security_result != 0) {
                throw std::runtime_error{std::format("authentication failed: {}", read_failure_reason())};
            }
        }
        std::array<uint8_t,1> is_shared = {true};
        try{
        parent::rfb_write(is_shared);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("send is_shared fail: {}", e.what())};
        }
        return 0;
    }

private:
    // Only present when a set_password_* layer is in the stack; without one the
    // client simply negotiates the "None" security type as before.
    std::string get_configured_password() {
        if constexpr (requires (parent& p) { p.get_password(); }) {
            return std::string{parent::get_password()};
        }
        else {
            return {};
        }
    }

    // RFB 3.8 sends a length-prefixed reason string after a failed handshake.
    std::string read_failure_reason() {
        try {
            std::array<uint8_t, 4> length_buf{};
            parent::rfb_read(length_buf);
            uint32_t length = from_big_endian_bytes(length_buf[0], length_buf[1], length_buf[2], length_buf[3]);
            std::vector<char> reason(length);
            parent::rfb_read(reason);
            return std::string{reason.begin(), reason.end()};
        }
        catch (std::exception&) {
            return "<no reason given>";
        }
    }

    // VNC Authentication: the server sends a 16 byte challenge, which the client
    // DES-encrypts in two 8 byte blocks using the password as key. The password
    // is truncated/zero-padded to 8 bytes and, per the protocol's historical
    // quirk, each key byte has its bits reversed.
    void vnc_authenticate(const std::string& password) {
        std::array<uint8_t, 16> challenge{};
        try{
        parent::rfb_read(challenge);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("vnc auth challenge read fail: {}", e.what())};
        }

        std::array<uint8_t, 8> key{};
        for (size_t i = 0; i < key.size(); i++) {
            uint8_t c = i < password.size() ? static_cast<uint8_t>(password[i]) : 0;
            uint8_t reversed = 0;
            for (int bit = 0; bit < 8; bit++) {
                reversed |= ((c >> bit) & 1) << (7 - bit);
            }
            key[i] = reversed;
        }

        DES_key_schedule key_schedule{};
        DES_set_key_unchecked(reinterpret_cast<const_DES_cblock*>(key.data()), &key_schedule);

        std::array<uint8_t, 16> response{};
        for (int block = 0; block < 2; block++) {
            DES_ecb_encrypt(
                    reinterpret_cast<const_DES_cblock*>(challenge.data() + block*8),
                    reinterpret_cast<DES_cblock*>(response.data() + block*8),
                    &key_schedule, DES_ENCRYPT);
        }

        try{
        parent::rfb_write(response);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("vnc auth response write fail: {}", e.what())};
        }
    }
};

// Supplies the password used by the VNC Authentication security type. Add it to
// the bottom of the stack when connecting to a password protected server.
template<typename T>
class set_password_from_configure : public T {
public:
    using parent = T;
    set_password_from_configure(const configure auto& conf) : parent{conf},
        password{conf.password}
    {
    }
    auto get_password() {
        return password;
    }
private:
    std::string password;
};

template<typename T>
class set_supported_encodings_from_configure : public T {
public:
    using parent = T;
    set_supported_encodings_from_configure(const configure auto& conf) : parent{conf},
        supported_encodings{conf.supported_encodings}
    {
    }
    auto get_supported_encodings() {
        return supported_encodings;
    }
    // Lets a live session re-negotiate its encodings without reconnecting.
    // The state already lives here, so the setter belongs here too.
    void set_supported_encodings(std::vector<uint32_t> encodings) {
        supported_encodings = std::move(encodings);
    }
private:
    std::vector<uint32_t> supported_encodings;
};

template<typename T>
class set_default_supported_encodings : public T {
public:
    using parent = T;
    set_default_supported_encodings(const configure auto& conf) : parent{conf}
    {
    }
    auto get_supported_encodings() {
        return std::to_array<uint32_t>({
                to_big_endian(16),// ZRLE
                to_big_endian(0), // Raw
                //to_big_endian(1), // Copy-Rect
                //to_big_endian(6), // Zlib
                //to_big_endian(7), // Tight
                //to_big_endian(15),// TRLE
                //to_big_endian(21),// JPEG
                //to_big_endian(50),// H264
        });

    }
};

// Client half of the private scale extension. Sends "serve me at this
// fraction of the captured size" on the existing connection, so nothing
// reconnects and no second port is needed.
//
// Degrades gracefully by construction: the request is only ever written after
// the server has advertised support by sending a scale_ext rectangle, so a
// standard server is never handed a message type it cannot parse. Guarding on
// the peer's advertisement, rather than on our own, is what keeps this from
// wedging anything that does not speak it.
inline constexpr uint8_t client_message_set_scale = 180;

template<typename T>
class add_request_scale : public T {
public:
    using parent = T;
    add_request_scale(const configure auto& conf) : parent{conf} {}
    // permille: 1000 = native size, 500 = half, 250 = quarter.
    bool request_scale(int permille) {
        if (!parent::is_scale_ext_supported()) {
            return false;
        }
        permille = std::clamp(permille, 10, 1000);
        auto message = std::to_array<uint8_t>({
            client_message_set_scale, 0,
            to_big_endian_byte(static_cast<uint16_t>(permille), 0),
            to_big_endian_byte(static_cast<uint16_t>(permille), 1),
        });
        parent::rfb_write(message);
        return true;
    }
};

template<typename T>
class add_set_encodings : public T {
public:
    using parent = T;
    add_set_encodings(const configure auto& conf) : parent{conf} {
    }
    int set_encodings()
    {
        // order is a priority hint
        auto supported_encodings = parent::get_supported_encodings();
        uint16_t count = cpp_helper::checked_cast<uint16_t>(supported_encodings.size());
        auto set_encodings = std::to_array<uint8_t>({
            2, 0, // SetEncoding, padding
            to_big_endian_byte(count, 0), to_big_endian_byte(count,1), // Number of encoding
        });
        try{
        parent::rfb_write(set_encodings);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("set encoding send fail: {}", e.what())};
        }
        try{
        parent::rfb_write(supported_encodings);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("supported_encodings send fail: {}", e.what())};
        }
        return 0;
    }
};

template<typename T>
class add_set_format : public T {
public:
    using parent = T;
    add_set_format(const configure auto& conf) : parent{conf} {
    }
    int set_format()
    {
        std::array<uint8_t, 20> set_format = {
            0, 0, 0, 0,
            32, 24, 0, 1,
            0, 255, 0, 255, 0, 255,
            16, 8, 0, 0, 0, 0,
        };
        try{
        parent::rfb_write(set_format);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("set format send fail: {}", e.what())};
        }
        return 0;
    }
};

template<typename T>
class add_client_cut_text : public T {
public:
    using parent = T;
    add_client_cut_text(const configure auto& conf) : parent{conf} {
    }

    void client_cut_text(auto& str) {
        uint32_t length = str.size();
        std::array<uint8_t, 8> client_cut_text = {
            6, 0, 0, 0,
            to_big_endian_byte(length, 0), to_big_endian_byte(length, 1),
            to_big_endian_byte(length, 2), to_big_endian_byte(length, 3),
        };
        try {
            parent::rfb_write(client_cut_text);
            parent::rfb_write(str);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("client cut text send fail: {}", e.what())};
        }
    }
};

template<typename T>
class init_rfb : public T {
public:
    using parent = T;
    init_rfb(const configure auto& conf) : parent{conf} {
        parent::client_init();
        server_info = parent::server_init();
        parent::set_format();
        parent::set_encodings();
    }
    auto get_width() {
        return server_info.fb_width;
    }
    auto get_height() {
        return server_info.fb_height;
    }
    // The served size changes when the peer rescales, which is the whole
    // point of the scale extension, so it cannot stay fixed at ServerInit.
    void set_framebuffer_size(uint16_t width, uint16_t height) {
        server_info.fb_width = width;
        server_info.fb_height = height;
    }
private:
    server_init_message server_info;
};

template<typename T>
concept server_cut_text_processable = requires (T t) {
    t.process_server_cut_text(std::string{});
};

// Default handling for a ServerCutText message: drop it. The remote clipboard
// is data the user did not ask for, so a tool that does not opt in by defining
// its own process_server_cut_text must not have it appear on stdout, where it
// corrupts the tool's real output (and quietly exposes clipboard contents).
// rfb_clipboard is the tool that does opt in.
template<typename T>
class add_process_server_cut_text_if_not : public T {
public:
    using parent = T;
    add_process_server_cut_text_if_not(const configure auto& conf) : parent{conf}
    {}
    void process_server_cut_text(const auto& text) {
        (void)text;
    }
};
template<server_cut_text_processable T>
class add_process_server_cut_text_if_not<T> : public T {
public:
    using parent = T;
    add_process_server_cut_text_if_not(const configure auto& conf) : parent{conf}
    {}
};

template<typename T>
class add_server_cut_text : public add_process_server_cut_text_if_not<T> {
public:
    using parent = add_process_server_cut_text_if_not<T>;
    add_server_cut_text(const configure auto& conf) : parent{conf}
    {}
    void process_server_cut_text() {
        std::array<uint8_t, 4> length_buf{};
        try{
        parent::rfb_read(length_buf);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error("server cut text length read fail");
        }

        uint32_t length = from_big_endian_bytes(length_buf[0], length_buf[1], length_buf[2], length_buf[3]);

        std::vector<char> text(length);
        try{
        parent::rfb_read(text);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error("server cut text read fail");
        }
        parent::process_server_cut_text(text);
    }
};

template <typename T>
class add_rfb : public T {
public:
    using parent = T;
    add_rfb(const configure auto& conf) : parent{conf}
    {
    }
    void framebuffer_update_request(uint16_t x, uint16_t y, uint16_t width, uint16_t height, bool incremental_update = true) {
        std::array<uint8_t, 10> framebuffer_update_request = {
            3, incremental_update,
            to_big_endian_byte(x, 0), to_big_endian_byte(x, 1),
            to_big_endian_byte(y, 0), to_big_endian_byte(y, 1),
            to_big_endian_byte(width, 0), to_big_endian_byte(width, 1),
            to_big_endian_byte(height, 0), to_big_endian_byte(height, 1),
        };
        try{
        parent::rfb_write(framebuffer_update_request);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("framebuffer update request send fail: {}", e.what())};
        }
    }

    void key_event(uint32_t key, uint8_t down_flag) {
        std::array<uint8_t, 8> key_event = {
            4, down_flag, // message-type(4), down-flag
            0, 0, // padding
            to_big_endian_byte(key, 0), to_big_endian_byte(key, 1),
            to_big_endian_byte(key, 2), to_big_endian_byte(key, 3),
        };
        try{
        parent::rfb_write(key_event);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("key event send fail: {}", e.what())};
        }
    }

    void pointer_event(uint8_t button_mask, uint16_t x, uint16_t y) {
        std::array<uint8_t, 6> pointer_event = {
            5, button_mask, // message-type(5), button-mask
            to_big_endian_byte(x, 0), to_big_endian_byte(x, 1),
            to_big_endian_byte(y, 0), to_big_endian_byte(y, 1),
        };
        try{
        parent::rfb_write(pointer_event);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error{std::format("pointer event send fail: {}", e.what())};
        }
    }
    void process_colour_map_entries() {
        std::array<uint8_t, 2> colour_count_buf{};
        try{
        parent::rfb_read(colour_count_buf);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error("colour count read fail");
        }

        uint16_t colour_count = from_big_endian_bytes(colour_count_buf[0], colour_count_buf[1]);
        std::vector<uint8_t> colour_map_buf(colour_count*6);
        try{
        parent::rfb_read(colour_map_buf);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            throw std::runtime_error("colour map read fail");
        }
    }

    void process_server_message() {
        std::array<uint8_t, 1> type{};
        try{
            parent::rfb_read(type);
        }
        catch (connection_closed&) {
            throw;
        }
        catch (std::exception&) {
            throw std::runtime_error("server message type read fail");
        }
        if (type[0] == 0) {
            std::array<uint8_t, 4> framebuffer_update_head{type[0], 0, 0, 0};
            std::span<uint8_t> remainder{framebuffer_update_head.data() + 1, 3};
            parent::rfb_read(remainder);
            parent::process_framebuffer_update(framebuffer_update_head);
        }
        else if (type[0] == 1) {
            std::array<uint8_t, 3> padding_and_first_colour{};
            parent::rfb_read(padding_and_first_colour);
            process_colour_map_entries();
        }
        else if (type[0] == 2) {
            // Bell has no payload.
        }
        else if (type[0] == 3) {
            std::array<uint8_t, 3> padding{};
            parent::rfb_read(padding);
            parent::process_server_cut_text();
        }
        else {
            std::cerr << "not implemented message type: " << (int)type[0] << std::endl;
        }
    }

    bool process_buffered_server_message()
        requires requires(parent& p) {
            p.has_complete_server_message();
            p.begin_complete_server_message();
            p.finish_complete_server_message();
        }
    {
        if (!parent::has_complete_server_message()) return false;
        parent::begin_complete_server_message();
        process_server_message();
        parent::finish_complete_server_message();
        return true;
    }
}; // class rfb

using rfb = add_rfb<
    add_process_framebuffer_update<
    add_decode_h264<
    add_yuv_to_rgb<
    add_zrle<
    init_rfb<
    add_server_cut_text<
    add_client_cut_text<
    add_set_encodings<
    set_default_supported_encodings<
    add_set_format<
    add_server_init<
    add_client_init<
    add_connection<
    set_port<
    set_address<
    empty_configurable_class
    >>>>>>>>>>>>>>>>
;
struct config : cpp_helper::empty_configure {
    const char* address;
    uint16_t port;
};
static int rfb_process(auto host, auto port) {
    rfb rfb{config{.address=host, .port=port}};
    while (true) {
        rfb.framebuffer_update_request(0, 0, rfb.get_width(), rfb.get_height());
        rfb.process_server_message();
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}

} // namespace rfb

