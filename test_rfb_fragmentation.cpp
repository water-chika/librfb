#include "rfb.hpp"

#include <array>
#include <cassert>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

struct test_config : cpp_helper::empty_configure {};

class no_socket : public cpp_helper::empty_configurable_class {
public:
    no_socket(const cpp_helper::configure auto& conf)
        : cpp_helper::empty_configurable_class{conf} {}
    socket_helper::socket_t get_socket() { return socket_helper::invalid_socket; }
    int get_width() const { return 7680; }
    int get_height() const { return 4320; }
    void rfb_read(auto&) { assert(false && "buffered reads must not reach socket"); }
};

using buffer = rfb::add_server_message_buffer<no_socket>;

void append(buffer& input, const std::vector<uint8_t>& message,
            size_t begin, size_t end) {
    input.append_server_data(std::span<const uint8_t>{message}.subspan(begin,
                                                                       end - begin));
}

void fragmented_raw_update() {
    buffer input{test_config{}};
    std::vector<uint8_t> message{
        0, 0, 0, 1,                 // FramebufferUpdate, one rectangle
        0, 0, 0, 0, 0, 2, 0, 1,    // x, y, width=2, height=1
        0, 0, 0, 0,                 // raw encoding
        1, 2, 3, 4, 5, 6, 7, 8,
    };

    for (size_t i = 0; i + 1 < message.size(); ++i) {
        append(input, message, i, i + 1);
        assert(!input.has_complete_server_message());
    }
    append(input, message, message.size() - 1, message.size());
    assert(input.has_complete_server_message());

    input.begin_complete_server_message();
    std::vector<uint8_t> decoded(message.size());
    input.rfb_read(decoded);
    assert(decoded == message);
    input.finish_complete_server_message();
    assert(!input.has_complete_server_message());
}

void fragmented_variable_messages() {
    buffer input{test_config{}};
    const std::vector<uint8_t> cut{3, 0, 0, 0, 0, 0, 0, 5,
                                   'h', 'e', 'l', 'l', 'o'};
    append(input, cut, 0, 7);
    assert(!input.has_complete_server_message());
    append(input, cut, 7, cut.size() - 1);
    assert(!input.has_complete_server_message());
    append(input, cut, cut.size() - 1, cut.size());
    assert(input.has_complete_server_message());

    input.begin_complete_server_message();
    std::vector<uint8_t> decoded(cut.size());
    input.rfb_read(decoded);
    input.finish_complete_server_message();
    assert(decoded == cut);

    const std::vector<uint8_t> colours{
        1, 0, 0, 0, 0, 2,          // two colour entries
        0, 1, 0, 2, 0, 3,
        0, 4, 0, 5, 0, 6,
    };
    append(input, colours, 0, colours.size() - 1);
    assert(!input.has_complete_server_message());
    append(input, colours, colours.size() - 1, colours.size());
    assert(input.has_complete_server_message());
}

void complete_messages_are_separated() {
    buffer input{test_config{}};
    const std::array<uint8_t, 2> bells{2, 2};
    input.append_server_data(bells);
    assert(input.has_complete_server_message());
    input.begin_complete_server_message();
    std::array<uint8_t, 1> one{};
    input.rfb_read(one);
    input.finish_complete_server_message();
    assert(one[0] == 2);
    assert(input.has_complete_server_message());
}

bool rejects(buffer& input, const std::vector<uint8_t>& message) {
    try {
        input.append_server_data(message);
        input.has_complete_server_message();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

void eight_k_edges_and_limits() {
    static_assert(static_cast<std::size_t>(7680) * 4320 * 4 <=
                  rfb::max_encoded_rectangle_bytes);
    static_assert(static_cast<std::size_t>(7680) * 4320 * 4 <=
                  rfb::max_framebuffer_bytes);
    buffer valid{test_config{}};
    // One encoded pixel at the bottom-right edge of 7680x4320 is valid.
    std::vector<uint8_t> edge{
        0, 0, 0, 1,
        0x1d, 0xff, 0x10, 0xdf, 0, 1, 0, 1,
        0, 0, 0, 16,
        0, 0, 0, 1, 0,
    };
    valid.append_server_data(edge);
    assert(valid.has_complete_server_message());

    buffer x_overflow{test_config{}};
    auto bad_x = edge;
    bad_x[4] = 0x1e; bad_x[5] = 0;
    assert(rejects(x_overflow, bad_x));

    buffer y_overflow{test_config{}};
    auto bad_y = edge;
    bad_y[6] = 0x10; bad_y[7] = 0xe0;
    assert(rejects(y_overflow, bad_y));

    buffer too_many{test_config{}};
    std::vector<uint8_t> count{0, 0, 0x10, 0x01};
    assert(rejects(too_many, count));

    buffer too_large{test_config{}};
    auto length = static_cast<uint32_t>(rfb::max_encoded_rectangle_bytes + 1);
    std::vector<uint8_t> encoded{
        0, 0, 0, 1,
        0, 0, 0, 0, 0, 1, 0, 1,
        0, 0, 0, 16,
        rfb::to_big_endian_byte(length, 0), rfb::to_big_endian_byte(length, 1),
        rfb::to_big_endian_byte(length, 2), rfb::to_big_endian_byte(length, 3),
    };
    assert(rejects(too_large, encoded));
}

void fragmented_desktop_size_then_pixels() {
    buffer input{test_config{}};
    const std::vector<uint8_t> resize{
        0, 0, 0, 1,
        0, 0, 0, 0, 0, 3, 0, 2,
        0xff, 0xff, 0xff, 0x21,
    };
    const std::vector<uint8_t> pixels{
        0, 0, 0, 1,
        0, 0, 0, 0, 0, 3, 0, 2,
        0, 0, 0, 0,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    };

    for (size_t i = 0; i < resize.size(); ++i) {
        append(input, resize, i, i + 1);
        assert(input.has_complete_server_message() == (i + 1 == resize.size()));
    }
    input.begin_complete_server_message();
    std::vector<uint8_t> decoded(resize.size());
    input.rfb_read(decoded);
    input.finish_complete_server_message();
    assert(decoded == resize);

    // The framing layer must now validate against 3x2, not ServerInit's size.
    for (size_t i = 0; i < pixels.size(); ++i) {
        append(input, pixels, i, i + 1);
        assert(input.has_complete_server_message() == (i + 1 == pixels.size()));
    }
}

void desktop_size_rejects_invalid_geometry() {
    buffer input{test_config{}};
    const std::vector<uint8_t> zero_width{
        0, 0, 0, 1,
        0, 0, 0, 0, 0, 0, 0, 2,
        0xff, 0xff, 0xff, 0x21,
    };
    assert(rejects(input, zero_width));

    buffer nonzero_origin{test_config{}};
    const std::vector<uint8_t> bad_origin{
        0, 0, 0, 1,
        0, 1, 0, 0, 0, 3, 0, 2,
        0xff, 0xff, 0xff, 0x21,
    };
    assert(rejects(nonzero_origin, bad_origin));

    buffer not_last{test_config{}};
    const std::vector<uint8_t> desktop_size_not_last{
        0, 0, 0, 2,
        0, 0, 0, 0, 0, 3, 0, 2,
        0xff, 0xff, 0xff, 0x21,
        0, 0, 0, 0, 0, 1, 0, 1,
        0, 0, 0, 0,
        1, 2, 3, 4,
    };
    assert(rejects(not_last, desktop_size_not_last));
}

} // namespace

int main() {
    fragmented_raw_update();
    fragmented_variable_messages();
    complete_messages_are_separated();
    eight_k_edges_and_limits();
    fragmented_desktop_size_then_pixels();
    desktop_size_rejects_invalid_geometry();
}
