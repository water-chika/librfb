#include "server.hpp"

#ifndef _WIN32

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace rfb::server;

class mutable_source final : public frame_source {
public:
    mutable_source() { resize(2, 1); }

    int width() const override { std::lock_guard lock{m_mutex}; return m_frame->width; }
    int height() const override { std::lock_guard lock{m_mutex}; return m_frame->height; }
    std::shared_ptr<const frame_buffer> frame(uint64_t& generation) override {
        std::lock_guard lock{m_mutex};
        generation = m_generation;
        return m_frame;
    }
    std::chrono::steady_clock::duration time_to_next_capture() const override {
        return std::chrono::steady_clock::duration::zero();
    }
    void set_scale(double) override { resize(3, 2); }
    void resize(int width, int height) {
        std::lock_guard lock{m_mutex};
        auto frame = std::make_shared<frame_buffer>();
        frame->resize(width, height);
        for (size_t i = 0; i < frame->pixels.size(); ++i)
            frame->pixels[i] = static_cast<uint8_t>(i);
        m_frame = std::move(frame);
        ++m_generation;
    }
private:
    mutable std::mutex m_mutex;
    std::shared_ptr<frame_buffer> m_frame;
    uint64_t m_generation{};
};

void read_exact(socket_helper::socket_t socket, void* data, size_t size) {
    auto* out = static_cast<uint8_t*>(data);
    while (size) {
        auto count = socket_helper::read_socket(socket, out, size);
        assert(count > 0);
        out += count;
        size -= static_cast<size_t>(count);
    }
}

void write_all(socket_helper::socket_t socket, const auto& bytes) {
    auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    size_t size = bytes.size() * sizeof(bytes[0]);
    while (size) {
        auto count = socket_helper::write_socket(socket, data, size);
        assert(count > 0);
        data += count;
        size -= static_cast<size_t>(count);
    }
}

void handshake(socket_helper::socket_t socket) {
    std::array<char, 12> version{};
    read_exact(socket, version.data(), version.size());
    assert(std::string_view{version.data(), version.size()} == "RFB 003.008\n");
    write_all(socket, version);
    std::array<uint8_t, 2> security{};
    read_exact(socket, security.data(), security.size());
    assert(security[0] == 1 && security[1] == 1);
    write_all(socket, std::array<uint8_t, 1>{1});
    std::array<uint8_t, 4> result{};
    read_exact(socket, result.data(), result.size());
    write_all(socket, std::array<uint8_t, 1>{1});
    std::array<uint8_t, 24> init{};
    read_exact(socket, init.data(), init.size());
    uint32_t name_size = (uint32_t{init[20]} << 24) | (uint32_t{init[21]} << 16) |
                         (uint32_t{init[22]} << 8) | init[23];
    std::vector<uint8_t> name(name_size);
    read_exact(socket, name.data(), name.size());
}

std::vector<uint8_t> read_update(socket_helper::socket_t socket, size_t payload) {
    std::vector<uint8_t> update(16 + payload);
    read_exact(socket, update.data(), update.size());
    assert(update[0] == 0 && update[2] == 0 && update[3] == 1);
    return update;
}

void dynamic_resize_is_announced_before_full_pixels() {
    socket_helper::socket_t sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    mutable_source source;
    server_options options{};
    options.desktop_name = "test";
    session server{socket_helper::unique_socket{sockets[0]}, "test", source, options};
    std::thread worker{[&] { server.run(); }};
    socket_helper::unique_socket client{sockets[1]};
    handshake(client.get());

    const std::array<uint8_t, 12> encodings{
        2, 0, 0, 2, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x21};
    write_all(client.get(), encodings);
    const std::array<uint8_t, 10> request{3, 0, 0, 0, 0, 0, 0, 2, 0, 1};
    write_all(client.get(), request);
    auto initial = read_update(client.get(), 8);
    assert(initial[8] == 0 && initial[9] == 2 && initial[10] == 0 && initial[11] == 1);

    source.resize(3, 2);
    write_all(client.get(), request); // stale 2x1 coordinates must be discarded.
    auto announcement = read_update(client.get(), 0);
    assert(announcement[8] == 0 && announcement[9] == 3);
    assert(announcement[10] == 0 && announcement[11] == 2);
    assert(announcement[12] == 0xff && announcement[13] == 0xff &&
           announcement[14] == 0xff && announcement[15] == 0x21);
    auto full = read_update(client.get(), 24);
    assert(full[8] == 0 && full[9] == 3 && full[10] == 0 && full[11] == 2);

    client.reset();
    worker.join();
}

void scale_ext_only_resize_stays_compatible() {
    socket_helper::socket_t sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    mutable_source source;
    server_options options{};
    options.desktop_name = "test";
    session server{socket_helper::unique_socket{sockets[0]}, "test", source, options};
    std::thread worker{[&] { server.run(); }};
    socket_helper::unique_socket client{sockets[1]};
    handshake(client.get());

    const std::array<uint8_t, 12> encodings{
        2, 0, 0, 2, 0, 0, 0, 0, 0xff, 0xff, 0xf9, 0x3f};
    write_all(client.get(), encodings);
    const std::array<uint8_t, 10> request{3, 0, 0, 0, 0, 0, 0, 2, 0, 1};
    write_all(client.get(), request);
    auto advertisement = read_update(client.get(), 0);
    assert(advertisement[12] == 0xff && advertisement[13] == 0xff &&
           advertisement[14] == 0xf9 && advertisement[15] == 0x3f);
    (void)read_update(client.get(), 8);

    const std::array<uint8_t, 4> scale{client_message_set_scale, 0, 1, 0xf4};
    write_all(client.get(), scale);
    write_all(client.get(), request);
    auto resized = read_update(client.get(), 0);
    assert(resized[8] == 0 && resized[9] == 3 && resized[10] == 0 && resized[11] == 2);
    assert(resized[12] == 0xff && resized[13] == 0xff &&
           resized[14] == 0xf9 && resized[15] == 0x3f);
    auto full = read_update(client.get(), 24);
    assert(full[8] == 0 && full[9] == 3 && full[10] == 0 && full[11] == 2);

    client.reset();
    worker.join();
}

void capability_renegotiation_clears_scale_announcement() {
    socket_helper::socket_t sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    mutable_source source;
    server_options options{};
    options.desktop_name = "test";
    session server{socket_helper::unique_socket{sockets[0]}, "test", source, options};
    std::thread worker{[&] { server.run(); }};
    socket_helper::unique_socket client{sockets[1]};
    handshake(client.get());
    const std::array<uint8_t, 12> with_scale{
        2, 0, 0, 2, 0, 0, 0, 0, 0xff, 0xff, 0xf9, 0x3f};
    const std::array<uint8_t, 8> raw_only{2, 0, 0, 1, 0, 0, 0, 0};
    write_all(client.get(), with_scale);
    write_all(client.get(), raw_only);
    const std::array<uint8_t, 10> request{3, 0, 0, 0, 0, 0, 0, 2, 0, 1};
    write_all(client.get(), request);
    auto pixels = read_update(client.get(), 8);
    assert(pixels[12] == 0 && pixels[13] == 0 && pixels[14] == 0 && pixels[15] == 0);
    client.reset();
    worker.join();
}

void unsupported_client_is_disconnected() {
    socket_helper::socket_t sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    mutable_source source;
    server_options options{};
    options.desktop_name = "test";
    auto server = std::make_unique<session>(
        socket_helper::unique_socket{sockets[0]}, "test", source, options);
    std::thread worker{[&] { server->run(); }};
    socket_helper::unique_socket client{sockets[1]};
    handshake(client.get());
    const std::array<uint8_t, 8> encodings{2, 0, 0, 1, 0, 0, 0, 0};
    write_all(client.get(), encodings);
    const std::array<uint8_t, 10> request{3, 0, 0, 0, 0, 0, 0, 2, 0, 1};
    write_all(client.get(), request);
    (void)read_update(client.get(), 8);
    source.resize(3, 2);
    write_all(client.get(), request);
    worker.join();
    server.reset(); // session owns the server socket; destruction closes it.
    std::array<uint8_t, 1> byte{};
    assert(socket_helper::read_socket(client.get(), byte.data(), byte.size()) == 0);
}

} // namespace

int main() {
    dynamic_resize_is_announced_before_full_pixels();
    scale_ext_only_resize_stays_compatible();
    capability_renegotiation_clears_scale_announcement();
    unsupported_client_is_disconnected();
}

#else
int main() { return 0; }
#endif
