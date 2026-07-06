#pragma once

#include <array>
#include <iostream>
#include <boost/asio.hpp>

#include "zlib.h"

namespace rfb {
enum class encoding : uint32_t {
    raw = 0,
    zrle = 16,
};
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
        std::string str;
        if (encoding == rfb::encoding::raw) {
            str = "raw";
        }
        else if (encoding == rfb::encoding::zrle) {
            str = "ZRLE";
        }
        else {
            str = "unknow";
        }
        return std::copy(str.begin(), str.end(), ctx.out());
    }
};

namespace rfb {

using boost::asio::ip::tcp;

uint16_t parse_3digits(char d100, char d10, char d) {
    return (d100-'0')*100 + (d10-'0')*10 + (d-'0');
}

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

class rfb {
public:
    auto get_width() {
        return m_server_init_message.fb_width;
    }
    auto get_height() {
        return m_server_init_message.fb_height;
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

    void framebuffer_update_request(uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
        boost::system::error_code error;
        std::array<uint8_t, 10> framebuffer_update_request = {
            3, 0,
            to_big_endian_byte(x, 0), to_big_endian_byte(x, 1),
            to_big_endian_byte(y, 0), to_big_endian_byte(y, 1),
            to_big_endian_byte(width, 0), to_big_endian_byte(width, 1),
            to_big_endian_byte(height, 0), to_big_endian_byte(height, 1),
        };
        auto len = write(socket, boost::asio::buffer(framebuffer_update_request), error);
        if (error == boost::asio::error::eof)
            return;
        else if (error)
            throw boost::system::system_error(error);
        if (len != framebuffer_update_request.size()) {
            std::cerr << "framebuffer update request send fail" << std::endl;
        }
    }

    void key_event(uint32_t key, uint8_t down_flag) {
        boost::system::error_code error;
        std::array<uint8_t, 8> key_event = {
            4, down_flag, // message-type(4), down-flag
            0, 0, // padding
            to_big_endian_byte(key, 0), to_big_endian_byte(key, 1),
            to_big_endian_byte(key, 2), to_big_endian_byte(key, 3),
        };
        auto len = write(socket, boost::asio::buffer(key_event), error);
        if (error == boost::asio::error::eof)
            return;
        else if (error)
            throw boost::system::system_error(error);
        if (len != key_event.size()) {
            std::cerr << "key event send fail" << std::endl;
        }
    }

    void pointer_event(uint8_t button_mask, uint16_t x, uint16_t y) {
        boost::system::error_code error;
        std::array<uint8_t, 6> pointer_event = {
            5, button_mask, // message-type(5), button-mask
            to_big_endian_byte(x, 0), to_big_endian_byte(x, 1),
            to_big_endian_byte(y, 0), to_big_endian_byte(y, 1),
        };
        auto len = write(socket, boost::asio::buffer(pointer_event), error);
        if (error == boost::asio::error::eof)
            return;
        else if (error)
            throw boost::system::system_error(error);
        if (len != pointer_event.size()) {
            std::cerr << "key event send fail" << std::endl;
        }
    }

    void process_server_cut_text() {
        boost::system::error_code error;
        std::array<uint8_t, 4> length_buf{};
        auto len = read(socket, boost::asio::buffer(length_buf), error);
        if (error == boost::asio::error::eof)
        {
            std::cout << "eof" << std::endl;
            return;
        }
        else if (error)
            throw boost::system::system_error(error);
        if (len != length_buf.size()) {
            throw std::runtime_error("server cut text length read fail");
        }

        uint32_t length = from_big_endian_bytes(length_buf[0], length_buf[1], length_buf[2], length_buf[3]);

        std::vector<char> text(length);
        len = read(socket, boost::asio::buffer(text), error);
        if (error == boost::asio::error::eof)
        {
            std::cout << "eof" << std::endl;
            return;
        }
        else if (error)
            throw boost::system::system_error(error);
        if (len != text.size()) {
            throw std::runtime_error("server cut text read fail");
        }
        text.push_back('\0');
        std::cout << text.data() << std::endl;
    }

    void process_colour_map_entries() {
        boost::system::error_code error;
        std::array<uint8_t, 2> colour_count_buf{};
        auto len = read(socket, boost::asio::buffer(colour_count_buf), error);
        if (error == boost::asio::error::eof)
        {
            std::cout << "eof" << std::endl;
            return;
        }
        else if (error)
            throw boost::system::system_error(error);
        if (len != colour_count_buf.size()) {
            throw std::runtime_error("colour count read fail");
        }

        uint16_t colour_count = from_big_endian_bytes(colour_count_buf[0], colour_count_buf[1]);
        std::vector<uint8_t> colour_map_buf(colour_count*6);
        len = read(socket, boost::asio::buffer(colour_map_buf), error);
        if (error == boost::asio::error::eof)
        {
            std::cout << "eof" << std::endl;
            return;
        }
        else if (error)
            throw boost::system::system_error(error);
        if (len != colour_map_buf.size()) {
            throw std::runtime_error("colour map read fail");
        }
    }

    auto process_framebuffer_update(auto& framebuffer_update_head) {
        boost::system::error_code error;
        uint16_t rectangles_count = from_big_endian_bytes(framebuffer_update_head[2], framebuffer_update_head[3]);
        //std::cout << "rectangles count: " << rectangles_count << std::endl;
        std::vector<uint8_t> last_update{};
        for (uint16_t i = 0; i < rectangles_count; i++) {
            std::array<uint8_t, 12> rectangle{};
            auto len = read(socket, boost::asio::buffer(rectangle), error);
            if (error == boost::asio::error::eof)
                throw std::runtime_error("eof");
            else if (error)
                throw boost::system::system_error(error);
            if (len != rectangle.size()) {
                throw std::runtime_error("framebuffer update rectangle read fail");
            }
            uint16_t x = from_big_endian_bytes(rectangle[0], rectangle[1]);
            uint16_t y = from_big_endian_bytes(rectangle[2], rectangle[3]);
            uint16_t width = from_big_endian_bytes(rectangle[4], rectangle[5]);
            uint16_t height = from_big_endian_bytes(rectangle[6], rectangle[7]);
            auto encoding_type = static_cast<encoding>(from_big_endian_bytes(rectangle[8], rectangle[9], rectangle[10], rectangle[11]));
            if (false) {
                std::cout << "x: " << x << std::endl;
                std::cout << "y: " << y << std::endl;
                std::cout << "width: " << width << std::endl;
                std::cout << "height: " << height << std::endl;
            }
            if (encoding_type == encoding::raw) {
                auto pixels = frame;
                len = read(socket, boost::asio::buffer(pixels), error);
                if (error == boost::asio::error::eof)
                    throw std::runtime_error("eof");
                else if (error)
                    throw boost::system::system_error(error);
                if (len != pixels.size()) {
                    throw std::runtime_error("framebuffer update rectangle pixels read fail");
                }
            }
            else if (encoding_type == encoding::zrle) {
                std::array<uint8_t, 4> length_buf;
                auto len = read(socket, boost::asio::buffer(length_buf), error);
                if (error == boost::asio::error::eof)
                    throw std::runtime_error("eof");
                else if (error)
                    throw boost::system::system_error(error);
                if (len != length_buf.size()) {
                    throw std::runtime_error("framebuffer update rectangle zrle length read fail");
                }
                auto length = from_big_endian_bytes(length_buf[0], length_buf[1], length_buf[2], length_buf[3]);
                //std::cout << "zlib length: " << length << std::endl;
                auto zlib_data = std::vector<uint8_t>(length);
                len = read(socket, boost::asio::buffer(zlib_data), error);
                if (error == boost::asio::error::eof)
                    throw std::runtime_error("eof");
                else if (error)
                    throw boost::system::system_error(error);
                if (len != zlib_data.size()) {
                    throw std::runtime_error("framebuffer update rectangle zrle zlib data read fail");
                }

                constexpr int CHUNK = 1024;

                zlib_stream.avail_in = zlib_data.size();
                zlib_stream.next_in = zlib_data.data();

                std::vector<uint8_t> data{};

                do {
                    auto data_previous_size = data.size();
                    data.resize(data.size() + CHUNK);
                    zlib_stream.avail_out = CHUNK;
                    zlib_stream.next_out = data.data() + data_previous_size;

                    auto ret = inflate(&zlib_stream, Z_NO_FLUSH);
                    if (ret == Z_STREAM_ERROR) {
                        throw std::runtime_error{"zlib deflate error"};
                    }
                    auto have = CHUNK - zlib_stream.avail_out;
                } while (zlib_stream.avail_out == 0);
                data.resize(data.size() - zlib_stream.avail_out);

                int data_offset = 0;
                auto fb_width = get_width();
                auto fb_height = get_height();
                for (int x = 0; x < fb_width; x+=64) {
                    for (int y = 0; y < fb_height; y+=64) {
                        width = x+64 < fb_width? 64 : fb_width - x;
                        height = y+64 < fb_height? 64 : fb_height - y;
                        auto subencoding = data[data_offset];
                        data_offset++;
                        int bytes_per_cpixel = 3;
                        if (subencoding == 0) {
                            for (int x_ = 0; x_ < width; x_++) {
                                for (int y_ = 0; y_ < height; y_++) {
                                    auto ptr = &data[data_offset + (y_*width + x_)*3];
                                    frame[(y+y_)*fb_width + (x+x_)] = from_big_endian_bytes(ptr[0], ptr[1], ptr[2], 0);
                                }
                            }
                            data_offset += width*height*bytes_per_cpixel;
                        }
                        else if (subencoding == 1) {
                            auto ptr = &data[data_offset];
                            data_offset+=bytes_per_cpixel;
                            uint32_t pixel_value = from_big_endian_bytes(ptr[0], ptr[1], ptr[2], 0);
                            for (int x_ = 0; x_ < width; x_++) {
                                for (int y_ = 0; y_ < height; y_++) {
                                    frame[(y+y_)*fb_width + (x+x_)] = from_big_endian_bytes(ptr[0], ptr[1], ptr[2], 0);
                                }
                            }
                        }
                        else if (subencoding > 1 && subencoding < 16) {
                            int palette_size = subencoding;
                            auto palette = std::span{&data[data_offset], palette_size*bytes_per_cpixel};
                            data_offset += palette_size*bytes_per_cpixel;
                            int bits_per_packed_pixels = 1;
                            if (palette_size > 2) bits_per_packed_pixels = 2;
                            if (palette_size > 4) bits_per_packed_pixels = 4;
                            int palette_index_mask = 0b1;
                            if (palette_size > 2) palette_index_mask = 0b11;
                            if (palette_size > 4) palette_index_mask = 0b1111;
                            auto packed_pixels = std::span{&data[data_offset], (width*bits_per_packed_pixels+7)/8*height};
                            data_offset += (width*bits_per_packed_pixels+7)/8*height;

                            for (int y_ = 0; y_ < height; y_++) {
                                auto packed_row_pixels = std::span{&packed_pixels[(y_*width*bits_per_packed_pixels+7)/8], (width*bits_per_packed_pixels+7)/8};
                                for (int x_ = 0; x_ < width; x_++) {
                                    auto palette_index = (packed_row_pixels[x_*bits_per_packed_pixels/8] >> ((x_*bits_per_packed_pixels)%8)) & palette_index_mask;
                                    if (palette_index >= palette_size) palette_index = 0;
                                    auto ptr = &palette[palette_index*3];
                                    frame[(y+y_)*fb_width + (x+x_)] = from_big_endian_bytes(ptr[0], ptr[1], ptr[2], 0);
                                }
                            }
                        }
                        else if (subencoding == 128) {
                            int run = 0;
                            while (run < width*height) {
                                auto ptr = &data[data_offset];
                                uint32_t pixel_value = from_big_endian_bytes(ptr[0], ptr[1], ptr[2], 0);
                                data_offset += bytes_per_cpixel;
                                ptr = &data[data_offset];
                                int count_255 = 0;
                                while (ptr[count_255] == 255) count_255++;
                                int run_length = 255*count_255 + ptr[count_255] + 1;
                                run += run_length;
                                data_offset += count_255 + 1;

                                for (int i = 0; i < run_length; i++) {
                                    int y_ = (run+i)/64, x_ = (run+i)%64;
                                    frame[(y+y_)*fb_width + (x+x_)] = pixel_value;
                                }
                            }
                        }
                        else if (subencoding >= 130 && subencoding <= 255) {
                            int palette_size = subencoding - 128;
                            auto palette = std::span{&data[data_offset], palette_size*bytes_per_cpixel};
                            data_offset += palette_size*bytes_per_cpixel;
                            int run = 0;
                            while (run < width*height) {
                                auto ptr = &data[data_offset];
                                data_offset++;
                                if (ptr[0] < 128) {
                                    int palette_index = ptr[0];
                                    run++;
                                }
                                else {
                                    int palette_index = ptr[0]-128;
                                    if (palette_index*3+2 >= palette_size) palette_index = 0;
                                    uint32_t pixel_value = from_big_endian_bytes(palette[palette_index*3], palette[palette_index*3+1], palette[palette_index*3+2], 0);
                                    ptr = &data[data_offset];
                                    int count_255 = 0;
                                    while (ptr[count_255] == 255) count_255++;
                                    int run_length = 255*count_255 + ptr[count_255] + 1;
                                    run += 255*count_255 + ptr[count_255] + 1;
                                    data_offset += count_255+1;

                                    for (int i = 0; i < run_length; i++) {
                                        int y_ = (run+i)/64, x_ = (run+i)%64;
                                        frame[(y+y_)*fb_width + (x+x_)] = pixel_value;
                                    }
                                }
                            }
                        }
                        else {
                            break;
                            //throw std::runtime_error(std::format("framebuffer update rectangle zrle subencoding not supportted: {}", subencoding));
                        }
                    }
                }
            }
            else {
                throw std::runtime_error(std::format("encoding not support: {}", encoding_type));
            }
            frame_updated = true;
        }
    }

    void process_server_message() {
        boost::system::error_code error;
        std::array<uint8_t, 4> framebuffer_update_head{};
        auto len = read(socket, boost::asio::buffer(framebuffer_update_head), error);
        if (error == boost::asio::error::eof)
        {
            throw std::runtime_error("eof");
        }
        else if (error)
            throw boost::system::system_error(error);
        if (len != framebuffer_update_head.size()) {
            throw std::runtime_error("framebuffer update head read fail");
        }
        //std::cout << "server message: " << (int)framebuffer_update_head[0] << std::endl;
        if (framebuffer_update_head[0] == 0) {
            process_framebuffer_update(framebuffer_update_head);
        }
        else if (framebuffer_update_head[0] == 1) {
            process_colour_map_entries();
        }
        else if (framebuffer_update_head[0] == 3) {
            process_server_cut_text();
        }
        else {
            std::cout << "not implemented message type: " << (int)framebuffer_update_head[0] << std::endl;
        }
    }

    struct server_init_message {
        uint16_t fb_width{}, fb_height{};
        pixel_format server_pixel_format{};
        uint32_t name_length{};
        std::vector<char> name{};
    };

    auto server_init()
    {
        uint16_t fb_width{}, fb_height{};
        pixel_format server_pixel_format{};
        uint32_t name_length{};
        std::vector<char> name{};

        boost::system::error_code error;
        std::array<uint8_t, 24> server_init_buf{};
        auto len = read(socket, boost::asio::buffer(server_init_buf), error);
        if (error == boost::asio::error::eof)
            throw std::runtime_error("eof");
        else if (error)
            throw boost::system::system_error(error);
        if (len != server_init_buf.size()) {
            std::cerr << "server_init_buf read fail with len: " << len << std::endl;
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
        std::cout << "name length: " << name_length << std::endl;
        name.resize(name_length);
        len = read(socket, boost::asio::buffer(name), error);
        if (error == boost::asio::error::eof)
            throw std::runtime_error("eof");
        else if (error)
            throw boost::system::system_error(error);
        if (len != name_length) {
            std::cerr << "name parse fail" << std::endl;
        }
        std::cout << "fb resolution: " << fb_width << "x" << fb_height << std::endl;
        std::cout << "bpp: " << (int)server_pixel_format.bits_per_pixel << std::endl;
        std::cout << "depth: " << (int)server_pixel_format.depth << std::endl;
        std::cout << "big endian: " << (int)server_pixel_format.big_endian_flag << std::endl;
        std::cout << "true colour: " << (int)server_pixel_format.true_colour_flag << std::endl;
        std::cout << "color max: " << server_pixel_format.red_max << ", " << server_pixel_format.green_max << ", " << server_pixel_format.blue_max << std::endl;
        std::cout << "color shift: " << (int)server_pixel_format.red_shift << ", " << (int)server_pixel_format.green_shift << ", " << (int)server_pixel_format.blue_shift << std::endl;
        name.push_back('\0');
        std::cout << "server name: " << name.data() << std::endl;
        
        server_init_message m{
            .fb_width = fb_width, .fb_height = fb_height,
            .server_pixel_format = server_pixel_format,
            .name_length = name_length,
            .name = name
        };
        return m;
    }

    int set_format()
    {
        boost::system::error_code error;
        std::array<uint8_t, 20> set_format = {
            0, 0, 0, 0,
            32, 24, 0, 1,
            0, 255, 0, 255, 0, 255,
            16, 8, 0, 0, 0, 0,
        };
        auto len = write(socket, boost::asio::buffer(set_format), error);
        if (error == boost::asio::error::eof)
            return 0;
        else if (error)
            throw boost::system::system_error(error);
        if (len != set_format.size()) {
            std::cerr << "set format send fail" << std::endl;
        }
        return 0;
    }

    int set_encodings()
    {
        boost::system::error_code error;
        // order is a priority hint
        auto supported_encodings = std::to_array<uint32_t>({
                to_big_endian(0), // Raw
                to_big_endian(16),// ZRLE
                to_big_endian(1), // Copy-Rect
                to_big_endian(6), // Zlib
                to_big_endian(7), // Tight
                to_big_endian(15),// TRLE
                to_big_endian(21),// JPEG
                to_big_endian(50),// H264
        });
        auto set_encodings = std::to_array<uint8_t>({
            2, 0, // SetEncoding, padding
            0, supported_encodings.size(), // Number of encoding
        });
        auto len = write(socket, boost::asio::buffer(set_encodings), error);
        if (error == boost::asio::error::eof)
            return 0;
        else if (error)
            throw boost::system::system_error(error);
        if (len != set_encodings.size()) {
            std::cerr << "set encoding send fail" << std::endl;
        }
        len = write(socket, boost::asio::buffer(supported_encodings), error);
        if (error == boost::asio::error::eof)
            return 0;
        else if (error)
            throw boost::system::system_error(error);
        if (len != supported_encodings.size()*sizeof(supported_encodings[0])) {
            std::cerr << "supported_encodings send fail" << std::endl;
        }
        return 0;
    }

    int rfb_init() {
        std::array<char, 12> buf;
        boost::system::error_code error;
        size_t len = read(socket, boost::asio::buffer(buf), error);
        if (error == boost::asio::error::eof)
            return 0;
        else if (error)
            throw boost::system::system_error(error);
        std::cout << "get response: "; std::cout.write(buf.data(), len);
        auto [major, minor] = parse_protocol_version(buf);
        std::cout << "RFB version: " << major << "." << minor << std::endl;
        if (major != 3 || minor != 8) {
            std::cerr << "version only support 3.8" << std::endl;
            return -1;
        }

        len = write(socket, boost::asio::buffer(buf), error);
        if (error == boost::asio::error::eof)
            return 0;
        else if (error)
            throw boost::system::system_error(error);
        if (len != buf.size()) {
            std::cerr << "write support buf failed" << std::endl;
            return -1;
        }

        {
            std::array<uint8_t,1> security_count{};
            size_t len = socket.read_some(boost::asio::buffer(security_count), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);
            std::cout << "Server supportted security count: " << static_cast<int>(security_count[0]) << std::endl;

            std::vector<uint8_t> security_types(security_count[0]);
            len = socket.read_some(boost::asio::buffer(security_types), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);
            bool exist_none = false;
            for (int i = 0; i < security_types.size(); i++) {
                std::cout << static_cast<int>(security_types[i]) << ",";
                if (i == 1) exist_none = true;
            }
            std::cout << std::endl;

            write(socket, boost::asio::buffer(std::array<uint8_t,1>{1}), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);

            std::array<uint8_t, 4> security_result_buf{};
            len = socket.read_some(boost::asio::buffer(security_result_buf), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);

            uint32_t security_result = from_big_endian_bytes(security_result_buf[0], security_result_buf[1], security_result_buf[2], security_result_buf[3]);
            std::cout << "security handshaking result: " << security_result << std::endl;
        }
        std::array<uint8_t,1> is_shared = {true};
        {
            boost::system::error_code error;
            write(socket, boost::asio::buffer(is_shared), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);
        }
        return 0;
    }

    static int rfb_process(auto host, auto port) {
        rfb rfb{host, port};
        while (true) {
            rfb.framebuffer_update_request(0, 0, rfb.get_width(), rfb.get_height());
            rfb.process_server_message();
            sleep(1);
        }
    }

    rfb(const char* host, const char* port)
        : 
        io_context{},
        resolver{io_context},
        socket{io_context},
        zlib_stream{}
    {
        auto endpoints = resolver.resolve(host, port);
        boost::asio::connect(socket, endpoints);

        rfb::rfb_init();
        m_server_init_message = server_init();
        set_format();
        set_encodings();

        int ret = inflateInit(&zlib_stream);
        if (ret != Z_OK) {
            throw std::runtime_error("deflateInit error");
        }
    }
    ~rfb() {
        inflateEnd(&zlib_stream);
    }
    void set_frame(std::span<uint8_t> f) {
        frame = f;
    }
    auto get_frame() {
        return frame;
    }
    bool is_frame_updated() {
        return frame_updated;
    }
    void reset_frame_updated() {
        frame_updated = false;
    }
private:
    boost::asio::io_context io_context;
    tcp::resolver resolver;
    tcp::socket socket;
    server_init_message m_server_init_message;
    z_stream zlib_stream;
    std::span<uint8_t> frame;
    bool frame_updated;
}; // class rfb

} // namespace rfb
