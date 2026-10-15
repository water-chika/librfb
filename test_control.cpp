#include "control_server.hpp"
#include "viewer_client.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace {
struct viewer_config : cpp_helper::empty_configure {
    const char* address;
    std::uint16_t port;
    std::vector<std::uint32_t> supported_encodings;
    bool view_only;
    std::string password;
};
static_assert(std::is_constructible_v<
    rfb::viewer_client<cpp_helper::empty_configurable_class>,
    const viewer_config&>);

struct fake_session {
    int width{800}, height{600};
    bool view_only{}, released{};
    int pointer_x{}, pointer_y{};
    std::uint32_t pointer_mask{};
    int key{}, key_state{};
    std::string record_dir;
    std::string sent_clipboard;
    std::string received_clipboard{"remote text"};
    int get_fb_width() const { return width; }
    int get_fb_height() const { return height; }
    void get_rfb(std::span<std::uint8_t>) {}
    void send_pointer_event(std::uint32_t mask, int x, int y) {
        pointer_mask = mask; pointer_x = x; pointer_y = y;
    }
    void send_key_event(int value, int state) { key = value; key_state = state; }
    void send_cut_text(std::string_view text) { sent_clipboard = text; }
    std::string_view server_cut_text() const { return received_clipboard; }
    std::string record_start(const std::string& dir, int) { record_dir = dir; return "ok trace"; }
    std::string record_stop() { return "ok trace"; }
    std::string record_mark(const std::string&) { return "ok"; }
    std::string record_frame(const std::string&) { return "ok"; }
    void set_supported_encodings(std::vector<std::uint32_t>) {}
    void set_encodings() {}
    void request_framebuffer_update(int, int, int, int, bool) {}
    rfb::encoding get_encoding() const { return rfb::encoding::zrle; }
    std::uint32_t get_frame_network_byte_count() const { return 42; }
    double ms_since_last_update() const { return 3.5; }
    bool get_view_only() const { return view_only; }
    void release_remote_input() { released = true; }
    void set_view_only(bool value) { view_only = value; }
    bool request_scale(int) { return true; }
};

bool check(bool value, const char* expression) {
    if (!value) std::cerr << "check failed: " << expression << '\n';
    return value;
}
#define CHECK(expr) do { if (!check((expr), #expr)) return 1; } while (false)
}

int main() {
    fake_session session;
    using rfb_control::run_command;
    CHECK(run_command(session, "host", 5900, "size") == "ok 800 600");
    CHECK(run_command(session, "host", 5900, "pointer 12 34 2") == "ok");
    CHECK(session.pointer_x == 12 && session.pointer_y == 34 && session.pointer_mask == 2);
    CHECK(run_command(session, "host", 5900, "pointer 799 599 0") == "ok");
    CHECK(run_command(session, "host", 5900, "pointer -1 0 0") ==
          "error pointer coordinates out of bounds");
    CHECK(run_command(session, "host", 5900, "pointer 800 0 0") ==
          "error pointer coordinates out of bounds");
    CHECK(run_command(session, "host", 5900, "click 0 600 0") ==
          "error click coordinates out of bounds");
    CHECK(run_command(session, "host", 5900, "key 65293 1") == "ok");
    CHECK(session.key == 65293 && session.key_state == 1);
    CHECK(run_command(session, "host", 5900, "pointer bad 2 1") == "error pointer needs x y buttons");
    CHECK(run_command(session, "host", 5900, "pointer 1 2 256") == "error pointer needs x y buttons");
    CHECK(run_command(session, "host", 5900, "key 65 3") == "error key needs keysym state");
    CHECK(run_command(session, "host", 5900, "clipboard-set aGVsbG8A") == "ok");
    CHECK(session.sent_clipboard.size() == 6);
    CHECK((session.sent_clipboard == std::string_view{"hello\0", 6}));
    CHECK(run_command(session, "host", 5900, "clipboard-read") ==
          "ok cmVtb3RlIHRleHQ=");
    CHECK(run_command(session, "host", 5900, "clipboard-set bad!") ==
          "error clipboard-set needs valid bounded base64");
    CHECK(run_command(session, "host", 5900,
        "record start \"C:\\\\Program Files\\\\rfb trace\" 50") == "ok trace");
    CHECK(session.record_dir == "C:\\Program Files\\rfb trace");
    CHECK(run_command(session, "host", 5900, "view-only on") == "ok on");
    CHECK(session.released && session.view_only);
    CHECK(run_command(session, "host", 5900, "click 5 6 9") == "error click needs x y button");
    CHECK(run_command(session, "host", 5900, "quit") == "ok quit");
}
