#include <array>
#include <iostream>
#include <boost/asio.hpp>
using boost::asio::ip::tcp;

uint16_t parse_3digits(char d100, char d10, char d) {
    return (d100-'0')*100 + (d10-'0')*10 + (d-'0');
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


void framebuffer_update_request(auto& socket, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    boost::system::error_code error;
    std::array<uint8_t, 10> framebuffer_update_request = {
        3, 0,
        static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x), static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
        static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width), static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height)
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

void process_server_cut_text(auto& socket) {
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

    uint32_t length =
        (static_cast<uint32_t>(length_buf[0]) << (3*8)) |
        (static_cast<uint32_t>(length_buf[1]) << (2*8)) |
        (static_cast<uint32_t>(length_buf[2]) << (1*8)) |
        (static_cast<uint32_t>(length_buf[3]) << (0*8)) |
        0;

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

void process_colour_map_entries(auto& socket) {
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

    uint16_t colour_count = (static_cast<uint16_t>(colour_count_buf[0]) << 8) | colour_count_buf[1];
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

void process_server_message(auto& socket, pixel_format& server_pixel_format) {
    boost::system::error_code error;
    std::array<uint8_t, 4> framebuffer_update_head{};
    auto len = read(socket, boost::asio::buffer(framebuffer_update_head), error);
    if (error == boost::asio::error::eof)
    {
        std::cout << "eof" << std::endl;
        return;
    }
    else if (error)
        throw boost::system::system_error(error);
    if (len != framebuffer_update_head.size()) {
        throw std::runtime_error("framebuffer update head read fail");
    }
    std::cout << "server message: " << (int)framebuffer_update_head[0] << std::endl;
    if (framebuffer_update_head[0] == 1) {
        process_colour_map_entries(socket);
        return;
    }
    if (framebuffer_update_head[0] == 3) {
        process_server_cut_text(socket);
        return;
    }
    if (framebuffer_update_head[0] != 0) {
        std::cout << "not framebuffer_update: " << (int)framebuffer_update_head[0] << std::endl;
        return;
    }
    uint16_t rectangles_count = (static_cast<uint16_t>(framebuffer_update_head[2]) << 8) | (framebuffer_update_head[3]);
    std::cout << "rectangles count: " << rectangles_count << std::endl;
    for (uint16_t i = 0; i < rectangles_count; i++) {
        std::array<uint8_t, 12> rectangle{};
        len = read(socket, boost::asio::buffer(rectangle), error);
        if (error == boost::asio::error::eof)
            return;
        else if (error)
            throw boost::system::system_error(error);
        if (len != rectangle.size()) {
            throw std::runtime_error("framebuffer update rectangle read fail");
        }
        uint16_t x = (static_cast<uint16_t>(rectangle[0]) << 8) | rectangle[1];
        uint16_t y = (static_cast<uint16_t>(rectangle[2]) << 8) | rectangle[3];
        uint16_t width = (static_cast<uint16_t>(rectangle[4]) << 8) | rectangle[5];
        uint16_t height = (static_cast<uint16_t>(rectangle[6]) << 8) | rectangle[7];
        uint16_t encoding_type =
            (static_cast<uint16_t>(rectangle[8]) << (3*8)) |
            (static_cast<uint16_t>(rectangle[9]) << (2*8)) |
            (static_cast<uint16_t>(rectangle[10]) << (1*8)) |
            (static_cast<uint16_t>(rectangle[11]) << (0*8)) |
            0;
        std::cout << "x: " << x << std::endl;
        std::cout << "y: " << y << std::endl;
        std::cout << "width: " << width << std::endl;
        std::cout << "height: " << height << std::endl;
        if (encoding_type != 0) {
            throw std::runtime_error("encoding not support");
        }

        std::vector<uint8_t> pixels(width*height*server_pixel_format.bits_per_pixel/8);
        len = read(socket, boost::asio::buffer(pixels), error);
        if (error == boost::asio::error::eof)
            return;
        else if (error)
            throw boost::system::system_error(error);
        if (len != pixels.size()) {
            throw std::runtime_error("framebuffer update rectangle pixels read fail");
        }
    }
}

int main(int argc, char** argv) {
    try {
        boost::asio::io_context io_context;
        tcp::resolver resolver(io_context);
        char* port;
        if (argc < 2)
            port = "5900";
        else
            port = argv[1];
        auto endpoints = resolver.resolve("127.0.0.1", port);
        tcp::socket socket(io_context);
        boost::asio::connect(socket, endpoints);

        std::array<char, 12> buf;
        boost::system::error_code error;
        size_t len = socket.read_some(boost::asio::buffer(buf), error);
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

            uint32_t security_result =
                static_cast<uint32_t>(security_result_buf[0] << (3*8)) |
                static_cast<uint32_t>(security_result_buf[1] << (2*8)) |
                static_cast<uint32_t>(security_result_buf[2] << (1*8)) |
                static_cast<uint32_t>(security_result_buf[3] << (0*8));
            std::cout << "security handshaking result: " << security_result << std::endl;
        }

        uint8_t is_shared = true;
        {
            write(socket, boost::asio::buffer(std::array<uint8_t,1>{is_shared}), error);
        }

        uint16_t fb_width{}, fb_height{};
        pixel_format server_pixel_format{};
        uint32_t name_length{};
        std::vector<char> name{};

        {
            std::array<uint8_t, 24> server_init_buf{};
            auto len = read(socket, boost::asio::buffer(server_init_buf), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);
            if (len != server_init_buf.size()) {
                std::cerr << "server_init_buf read fail with len: " << len << std::endl;
                //return -1;
            }
            fb_width = (static_cast<uint16_t>(server_init_buf[0]) << 8) | (server_init_buf[1]);
            fb_height = (static_cast<uint16_t>(server_init_buf[2]) << 8) | (server_init_buf[3]);
            server_pixel_format.bits_per_pixel = server_init_buf[4];
            server_pixel_format.depth = server_init_buf[5];
            server_pixel_format.big_endian_flag = server_init_buf[6];
            server_pixel_format.true_colour_flag = server_init_buf[7];
            server_pixel_format.red_max = (static_cast<uint16_t>(server_init_buf[8]) << 8) | (server_init_buf[9]);
            server_pixel_format.green_max = (static_cast<uint16_t>(server_init_buf[10]) << 8) | (server_init_buf[11]);
            server_pixel_format.blue_max = (static_cast<uint16_t>(server_init_buf[12]) << 8) | (server_init_buf[13]);
            server_pixel_format.red_shift = server_init_buf[14];
            server_pixel_format.green_shift = server_init_buf[15];
            server_pixel_format.blue_shift = server_init_buf[16];
            server_pixel_format.padding0 = server_init_buf[17];
            server_pixel_format.padding1 = server_init_buf[18];
            server_pixel_format.padding2 = server_init_buf[19];

            name_length =
                (static_cast<uint32_t>(server_init_buf[20]) << (3*8)) |
                (static_cast<uint32_t>(server_init_buf[21]) << (2*8)) |
                (static_cast<uint32_t>(server_init_buf[22]) << (1*8)) |
                (static_cast<uint32_t>(server_init_buf[23]) << (0*8)) |
                0;
            std::cout << "name length: " << name_length << std::endl;
            name.resize(name_length);
            len = read(socket, boost::asio::buffer(name), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);
            if (len != name_length) {
                std::cerr << "name parse fail" << std::endl;
                return -1;
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
        }

        if (true) {
            std::array<uint8_t, 20> set_format = {
                0, 0, 0, 0,
                32, 24, 0, 1,
                0, 255, 0, 255, 0, 255,
                16, 8, 0, 0, 0, 0,
            };
            len = write(socket, boost::asio::buffer(set_format), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);
            if (len != set_format.size()) {
                std::cerr << "set format send fail" << std::endl;
            }
        }
        if (true) {
            std::array<uint8_t, 8> set_encodings = {
                2, 0, // SetEncoding, padding
                0, 1, // Number of encoding
                0, 0, 0, 0, // Raw encoding
            };
            len = write(socket, boost::asio::buffer(set_encodings), error);
            if (error == boost::asio::error::eof)
                return 0;
            else if (error)
                throw boost::system::system_error(error);
            if (len != set_encodings.size()) {
                std::cerr << "set encoding send fail" << std::endl;
            }
        }

        if (true) {
        }

        while (true) {
            framebuffer_update_request(socket, 0, 0, fb_width, fb_height);
            process_server_message(socket, server_pixel_format);
            sleep(1);
        }
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
