#include "trace.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <tuple>

namespace {
struct config : cpp_helper::empty_configure {};
struct fake_client {
    fake_client(const cpp_helper::configure auto&) {}
    void send_key_event(int key, int state) { if (state) keys.insert(key); else keys.erase(key); }
    void send_pointer_event(std::uint32_t mask, int x, int y) { pointer = {mask, x, y}; }
    const std::set<int>& held_remote_keys() const { return keys; }
    auto held_remote_pointer() const { return pointer; }
    void release_remote_input() { keys.clear(); pointer = {0, std::get<1>(pointer), std::get<2>(pointer)}; }
    int get_fb_width() const { return 0; }
    int get_fb_height() const { return 0; }
    void get_frame(std::span<std::uint8_t>) {}
    std::set<int> keys;
    std::tuple<std::uint32_t, int, int> pointer{};
};
bool check(bool value, const char* expression) {
    if (!value) std::cerr << "check failed: " << expression << '\n';
    return value;
}
#define CHECK(expr) do { if (!check((expr), #expr)) return 1; } while (false)
}

int main() {
    auto dir = std::filesystem::temp_directory_path() / "librfb-trace-release-test";
    std::filesystem::remove_all(dir);
    rfb_trace::add_trace_recorder<fake_client> client{config{}};
    CHECK(client.record_start(dir.string(), 0).starts_with("ok "));
    client.send_key_event(65, 1);
    client.send_pointer_event(1, 12, 34);
    client.release_remote_input();
    CHECK(client.record_stop().starts_with("ok "));
    std::ifstream in{dir / "trace.jsonl"};
    std::string text{std::istreambuf_iterator<char>{in}, {}};
    CHECK(text.find("\"keysym\":65,\"state\":0") != std::string::npos);
    CHECK(text.find("\"x\":12,\"y\":34,\"buttons\":0") != std::string::npos);
    std::filesystem::remove_all(dir);
}
