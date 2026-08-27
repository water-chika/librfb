// rfb_input: sends keyboard and pointer events to an RFB/VNC server.
//
// Unlike driving a VNC *viewer* window with local input injection tools
// (ydotool/wtype), everything here is spoken directly in the RFB protocol:
// KeyEvent carries an X keysym and PointerEvent carries absolute framebuffer
// coordinates, so the local keyboard layout, window focus and the user's real
// cursor are never involved. It runs headlessly - no window is opened.
//
// Reading the screen back is rfb_capture's job. Sending input needs no pixels,
// so this tool never sends a FramebufferUpdateRequest and decodes nothing.

#include "rfb.hpp"

#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using cpp_helper::empty_configurable_class;

namespace {

// Same headless protocol stack as rfb_recorder, plus a password layer so we can
// talk to servers that require VNC Authentication.
template<typename T>
using add_input_rfb =
    rfb::add_rfb<
    rfb::add_process_framebuffer_update<
    rfb::add_decode_h264<
    rfb::add_yuv_to_rgb<
    rfb::add_zrle<
    rfb::init_rfb<
    rfb::add_server_cut_text<
    rfb::add_client_cut_text<
    rfb::add_set_encodings<
    rfb::set_supported_encodings_from_configure<
    rfb::add_set_format<
    rfb::add_server_init<
    rfb::add_client_init<
    rfb::add_connection<
    rfb::set_port<
    rfb::set_address<
    rfb::set_password_from_configure<
    T
    >>>>>>>>>>>>>>>>>
;

struct config : public cpp_helper::empty_configure {
    const char* address;
    uint16_t port;
    std::vector<uint32_t> supported_encodings;
    std::string password;
};

using input_rfb = add_input_rfb<empty_configurable_class>;

// X11 keysyms for the non-printable keys worth naming. Printable ASCII needs no
// table: for 0x20..0x7e the keysym value is the character code itself.
const std::map<std::string, uint32_t> named_keysyms = {
    {"backspace", 0xff08}, {"tab", 0xff09}, {"return", 0xff0d}, {"enter", 0xff0d},
    {"escape", 0xff1b}, {"esc", 0xff1b}, {"space", 0x0020}, {"delete", 0xffff},
    {"insert", 0xff63}, {"home", 0xff50}, {"end", 0xff57},
    {"pageup", 0xff55}, {"pagedown", 0xff56},
    {"left", 0xff51}, {"up", 0xff52}, {"right", 0xff53}, {"down", 0xff54},
    {"f1", 0xffbe}, {"f2", 0xffbf}, {"f3", 0xffc0}, {"f4", 0xffc1},
    {"f5", 0xffc2}, {"f6", 0xffc3}, {"f7", 0xffc4}, {"f8", 0xffc5},
    {"f9", 0xffc6}, {"f10", 0xffc7}, {"f11", 0xffc8}, {"f12", 0xffc9},
    {"shift", 0xffe1}, {"ctrl", 0xffe3}, {"control", 0xffe3},
    {"alt", 0xffe9}, {"super", 0xffeb}, {"win", 0xffeb}, {"meta", 0xffe7},
    {"capslock", 0xffe5}, {"printscreen", 0xff61}, {"pause", 0xff13},
};

std::string to_lower(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

uint32_t parse_keysym(const std::string& name) {
    if (name.size() == 1 && static_cast<unsigned char>(name[0]) >= 0x20 && static_cast<unsigned char>(name[0]) <= 0x7e) {
        return static_cast<uint32_t>(static_cast<unsigned char>(name[0]));
    }
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        return static_cast<uint32_t>(strtoul(name.c_str(), nullptr, 16));
    }
    auto it = named_keysyms.find(to_lower(name));
    if (it != named_keysyms.end()) {
        return it->second;
    }
    throw std::logic_error(std::format("unknown key name: {} (use a single character, a name like 'enter', or a raw keysym like 0xff0d)", name));
}

// Splits "ctrl+alt+delete" into its component key names.
std::vector<std::string> split_combo(const std::string& combo) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        auto plus = combo.find('+', start);
        // A trailing '+' as the actual key (e.g. "ctrl++") must not split.
        if (plus == std::string::npos || plus + 1 == combo.size()) {
            parts.push_back(combo.substr(start));
            break;
        }
        parts.push_back(combo.substr(start, plus - start));
        start = plus + 1;
    }
    return parts;
}

constexpr uint8_t key_down = 1;
constexpr uint8_t key_up = 0;

// Presses a (possibly modified) key: modifiers go down in order, the final key
// is tapped, then the modifiers are released in reverse order.
void send_key_combo(auto& client, const std::string& combo) {
    auto parts = split_combo(combo);
    std::vector<uint32_t> keysyms;
    for (auto& part : parts) {
        keysyms.push_back(parse_keysym(part));
    }
    for (size_t i = 0; i + 1 < keysyms.size(); i++) {
        client.key_event(keysyms[i], key_down);
    }
    client.key_event(keysyms.back(), key_down);
    client.key_event(keysyms.back(), key_up);
    for (size_t i = keysyms.size() - 1; i-- > 0;) {
        client.key_event(keysyms[i], key_up);
    }
}

// Types a literal string. The keysym of a printable character is its own code
// point, and the server applies whatever shift state that character implies, so
// no modifier handling is needed for uppercase or symbols.
void send_text(auto& client, const std::string& text, int key_delay_ms) {
    for (unsigned char c : text) {
        uint32_t keysym = c;
        if (c == '\n' || c == '\r') {
            keysym = 0xff0d;
        }
        else if (c == '\t') {
            keysym = 0xff09;
        }
        else if (c < 0x20 || c > 0x7e) {
            throw std::logic_error(std::format("cannot type non-ASCII/control byte 0x{:02x}; use --key with a keysym", c));
        }
        client.key_event(keysym, key_down);
        client.key_event(keysym, key_up);
        if (key_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(key_delay_ms));
        }
    }
}

uint8_t button_mask_for(long button) {
    if (button < 1 || button > 8) {
        throw std::logic_error("button must be 1..8 (1=left, 2=middle, 3=right, 4/5=wheel up/down)");
    }
    return static_cast<uint8_t>(1u << (button - 1));
}

std::string read_password_file(const char* path) {
    std::ifstream in(path);
    if (!in) {
        throw std::logic_error(std::format("could not open password file: {}", path));
    }
    std::string password;
    std::getline(in, password);
    while (!password.empty() && (password.back() == '\r' || password.back() == '\n')) {
        password.pop_back();
    }
    return password;
}

// One queued input step. Steps are executed in the order given on the
// command line, so a whole interaction can be scripted in a single connection.
struct action {
    enum class kind { move, click, press, release, drag, key, text, wait } type;
    long x{}, y{}, x2{}, y2{};
    long button{1};
    std::string value;
};

} // namespace

int main(int argc, const char* argv[]) {
  try {
    auto usage_string =
        "Usage: rfb_input <address> <port> [options] <actions...>\n"
        "\n"
        "Connection options:\n"
        "  --password <pw>          password for VNC Authentication\n"
        "  --password-file <path>   read the password from the first line of <path>\n"
        "  --encodings <list>       comma separated RFB encodings, by name or ID,\n"
        "                           e.g. zrle,raw or 16,0 (default: zrle,raw)\n"
        "  --key-delay <ms>         delay between keystrokes of --type (default: 12)\n"
        "\n"
        "Actions (executed in the order given):\n"
        "  --move <x> <y>           move the pointer\n"
        "  --click <x> <y>          press+release at <x> <y> (see --button)\n"
        "  --press <x> <y>          press and hold at <x> <y>\n"
        "  --release <x> <y>        release at <x> <y>\n"
        "  --drag <x1> <y1> <x2> <y2>  press at 1, move to 2, release\n"
        "  --button <n>             button for the next click/press/drag (1=left,\n"
        "                           2=middle, 3=right, 4/5=wheel; default 1)\n"
        "  --key <combo>            tap a key, e.g. 'enter', 'a', 'ctrl+alt+delete',\n"
        "                           'super', or a raw keysym like 0xff0d\n"
        "  --type <text>            type a literal ASCII string\n"
        "  --wait <ms>              sleep before the next action\n"
        "\n"
        "Reading the screen back is rfb_capture's job.\n"
        "\n"
        "Example:\n"
        "  rfb_input 127.0.0.1 5900 --password mvtest \\\n"
        "      --key super --wait 800 --type notepad --key enter\n"
        ;
    if (argc < 3) {
        throw std::logic_error(usage_string);
    }

    const char* address = nullptr;
    uint16_t port = 5900;
    std::vector<uint32_t> encodings{};
    std::string password{};
    int key_delay_ms = 12;
    std::vector<action> actions{};
    long current_button = 1;

    auto require_args = [&](int i, int count, const char* option) {
        if (i + count >= argc) {
            throw std::logic_error(std::format("{} needs {} argument(s)\n\n{}", option, count, usage_string));
        }
    };

    for (int i = 1, pos_arg = 0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            std::string option = &argv[i][2];
            if (option == "password") {
                require_args(i, 1, "--password");
                password = argv[++i];
            }
            else if (option == "password-file") {
                require_args(i, 1, "--password-file");
                password = read_password_file(argv[++i]);
            }
            else if (option == "key-delay") {
                require_args(i, 1, "--key-delay");
                key_delay_ms = strtol(argv[++i], nullptr, 10);
            }
            else if (option == "button") {
                require_args(i, 1, "--button");
                current_button = strtol(argv[++i], nullptr, 10);
                button_mask_for(current_button); // validate early
            }
            else if (option == "encodings") {
                require_args(i, 1, "--encodings");
                auto parsed = rfb::parse_encodings(argv[++i]);
                if (!parsed) {
                    throw std::runtime_error{
                        std::string{"unknown encoding in --encodings: "} +
                        argv[i]};
                }
                for (auto v : *parsed) {
                    encodings.emplace_back(
                        rfb::to_big_endian(static_cast<uint32_t>(v)));
                }
            }
            else if (option == "move" || option == "click" || option == "press" || option == "release") {
                require_args(i, 2, argv[i]);
                action a{};
                a.type = option == "move" ? action::kind::move
                    : option == "click" ? action::kind::click
                    : option == "press" ? action::kind::press
                    : action::kind::release;
                a.x = strtol(argv[++i], nullptr, 10);
                a.y = strtol(argv[++i], nullptr, 10);
                a.button = current_button;
                actions.push_back(a);
            }
            else if (option == "drag") {
                require_args(i, 4, "--drag");
                action a{};
                a.type = action::kind::drag;
                a.x = strtol(argv[++i], nullptr, 10);
                a.y = strtol(argv[++i], nullptr, 10);
                a.x2 = strtol(argv[++i], nullptr, 10);
                a.y2 = strtol(argv[++i], nullptr, 10);
                a.button = current_button;
                actions.push_back(a);
            }
            else if (option == "key" || option == "type") {
                require_args(i, 1, argv[i]);
                action a{};
                a.type = option == "key" ? action::kind::key
                    : action::kind::text;
                a.value = argv[++i];
                if (a.type == action::kind::key) {
                    // Fail before connecting if a key name is bogus.
                    for (auto& part : split_combo(a.value)) {
                        parse_keysym(part);
                    }
                }
                actions.push_back(a);
            }
            else if (option == "wait") {
                require_args(i, 1, "--wait");
                action a{};
                a.type = action::kind::wait;
                a.x = strtol(argv[++i], nullptr, 10);
                actions.push_back(a);
            }
            else {
                throw std::logic_error(std::format("unknown option: {}\n\n{}", argv[i], usage_string));
            }
        }
        else {
            if (pos_arg == 0) {
                address = argv[i];
            }
            else if (pos_arg == 1) {
                port = strtol(argv[i], nullptr, 10);
            }
            else {
                throw std::logic_error(usage_string);
            }
            pos_arg += 1;
        }
    }

    if (address == nullptr) {
        throw std::logic_error(usage_string);
    }
    if (actions.empty()) {
        throw std::logic_error(std::format("no actions given\n\n{}", usage_string));
    }
    if (encodings.empty()) {
        // ZRLE first, Raw as fallback. A Raw-only 1920x1080 frame is ~8 MB and
        // several of them are sent per update, which takes tens of seconds over
        // an SSH tunnel and looks like a hang; ZRLE brings that down to ~3 s.
        encodings = {rfb::to_big_endian(uint32_t{16}), 0};
    }

    auto conf = config{
        .address = address, .port = port,
        .supported_encodings = std::move(encodings),
        .password = std::move(password),
    };
    input_rfb client{conf};

    int width = client.get_width();
    int height = client.get_height();
    std::cerr << std::format("connected: {}x{}", width, height) << std::endl;

    // Pointer position is tracked so that button press/release pairs and drags
    // report a consistent location even when only a button changes.
    uint16_t pointer_x = 0;
    uint16_t pointer_y = 0;
    auto clamp_coord = [](long v, int max) -> uint16_t {
        if (v < 0) return 0;
        if (v >= max) return static_cast<uint16_t>(max - 1);
        return static_cast<uint16_t>(v);
    };

    for (auto& a : actions) {
        switch (a.type) {
        case action::kind::move:
            pointer_x = clamp_coord(a.x, width);
            pointer_y = clamp_coord(a.y, height);
            client.pointer_event(0, pointer_x, pointer_y);
            break;
        case action::kind::click:
            pointer_x = clamp_coord(a.x, width);
            pointer_y = clamp_coord(a.y, height);
            client.pointer_event(0, pointer_x, pointer_y);
            client.pointer_event(button_mask_for(a.button), pointer_x, pointer_y);
            client.pointer_event(0, pointer_x, pointer_y);
            break;
        case action::kind::press:
            pointer_x = clamp_coord(a.x, width);
            pointer_y = clamp_coord(a.y, height);
            client.pointer_event(0, pointer_x, pointer_y);
            client.pointer_event(button_mask_for(a.button), pointer_x, pointer_y);
            break;
        case action::kind::release:
            pointer_x = clamp_coord(a.x, width);
            pointer_y = clamp_coord(a.y, height);
            client.pointer_event(button_mask_for(a.button), pointer_x, pointer_y);
            client.pointer_event(0, pointer_x, pointer_y);
            break;
        case action::kind::drag: {
            auto mask = button_mask_for(a.button);
            auto from_x = clamp_coord(a.x, width);
            auto from_y = clamp_coord(a.y, height);
            auto to_x = clamp_coord(a.x2, width);
            auto to_y = clamp_coord(a.y2, height);
            client.pointer_event(0, from_x, from_y);
            client.pointer_event(mask, from_x, from_y);
            // Interpolated intermediate motion: applications that implement
            // drag-and-drop generally ignore a single teleporting move.
            constexpr int steps = 16;
            for (int step = 1; step <= steps; step++) {
                auto ix = static_cast<uint16_t>(from_x + (to_x - from_x) * step / steps);
                auto iy = static_cast<uint16_t>(from_y + (to_y - from_y) * step / steps);
                client.pointer_event(mask, ix, iy);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            client.pointer_event(0, to_x, to_y);
            pointer_x = to_x;
            pointer_y = to_y;
            break;
        }
        case action::kind::key:
            send_key_combo(client, a.value);
            break;
        case action::kind::text:
            send_text(client, a.value, key_delay_ms);
            break;
        case action::kind::wait:
            std::this_thread::sleep_for(std::chrono::milliseconds(a.x));
            break;
        }
    }
  }
  catch (std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
  return 0;
}
