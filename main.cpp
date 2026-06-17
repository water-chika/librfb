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

int main(void) {
    try {
        boost::asio::io_context io_context;
        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve("127.0.0.1", "5901");
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

        socket.write_some(boost::asio::buffer(buf), error);
        if (error == boost::asio::error::eof)
            return 0;
        else if (error)
            throw boost::system::system_error(error);

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

            socket.write_some(boost::asio::buffer({1}), error);
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
            socket.write_some(boost::asio::buffer({is_shared}), error);
        }

        uint16_t fb_width{}, fb_height{};
        pixel_format server_pixel_format{};
        uint32_t name_length{};
        std::string name{};

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

            name_length = server_init_buf[20];
            std::cout << "name length: " << name_length << std::endl;
            name.resize(name_length);
            len = read(socket, boost::asio::buffer(name), error);
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
            std::cout << "server name: " << name << std::endl;
        }
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    return 0;
}
