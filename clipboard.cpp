// rfb_clipboard: reads and writes the clipboard of an RFB/VNC server.
//
// The RFB protocol carries the clipboard in two messages: ServerCutText, which
// the server pushes whenever the remote clipboard changes, and ClientCutText,
// which the client sends to set it. Neither is tied to the framebuffer, so this
// runs headlessly and never requests a single pixel.
//
// Note the asymmetry that follows from the protocol: there is no "read the
// clipboard now" request. A --get therefore waits for the server to announce
// the clipboard, which servers do shortly after a client connects and again on
// every change; if the remote clipboard is empty, nothing is ever sent and
// --get times out.

#include "rfb.hpp"

#include <chrono>
#include <deque>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using cpp_helper::configure;
using cpp_helper::empty_configurable_class;

namespace {

// Opting in to process_server_cut_text keeps rfb.hpp's discarding default out
// of the stack, so clipboard text ends up in this queue instead of nowhere.
template<typename T>
class add_collect_server_cut_text : public T {
public:
    using parent = T;
    add_collect_server_cut_text(const configure auto& conf) : parent{conf} {}

    void process_server_cut_text(const auto& text) {
        m_texts.emplace_back(text.data(), text.size());
    }

    bool take_text(std::string& out) {
        if (m_texts.empty()) {
            return false;
        }
        out = std::move(m_texts.front());
        m_texts.pop_front();
        return true;
    }

private:
    std::deque<std::string> m_texts{};
};

// The framebuffer decoding layers are present only because
// add_process_framebuffer_update requires them; this tool never sends a
// FramebufferUpdateRequest, so no pixels are ever decoded.
template<typename T>
using add_clipboard_rfb =
    rfb::add_rfb<
    rfb::add_process_framebuffer_update<
    rfb::add_decode_h264<
    rfb::add_yuv_to_rgb<
    rfb::add_zrle<
    rfb::init_rfb<
    rfb::add_server_cut_text<
    add_collect_server_cut_text<
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
    >>>>>>>>>>>>>>>>>>
;

struct config : public cpp_helper::empty_configure {
    const char* address;
    uint16_t port;
    std::vector<uint32_t> supported_encodings;
    std::string password;
};

using clipboard_rfb = add_clipboard_rfb<empty_configurable_class>;

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

std::string read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::logic_error(std::format("could not open file: {}", path));
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string read_whole_stdin() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    return buffer.str();
}

// RFB's cut text is defined as latin-1 with lines separated by a bare LF, so
// CRLF pairs from a Windows file would arrive as stray carriage returns.
std::string to_cut_text(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                continue; // the '\n' is emitted on the next iteration
            }
            out.push_back('\n');
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

void write_text(const std::string& text, const std::string& output_path) {
    if (output_path.empty()) {
        std::cout << text << std::endl;
        return;
    }
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error(std::format("could not open output file: {}", output_path));
    }
    out << text;
    if (!out) {
        throw std::runtime_error(std::format("could not write output file: {}", output_path));
    }
    std::cerr << std::format("wrote {} byte(s) to {}", text.size(), output_path) << std::endl;
}

// Pumps server messages until the collecting layer has some clipboard text, or
// the deadline passes. poll() is needed because the underlying reads block
// indefinitely, which would make the timeout unenforceable.
bool wait_for_cut_text(auto& client, std::string& text, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    if (client.take_text(text)) {
        return true;
    }
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd fd{};
        fd.fd = client.get_socket();
        fd.events = POLLIN;
        int ready = socket_helper::poll_sockets(&fd, 1, static_cast<int>(remaining));
        if (ready < 0) {
            throw std::runtime_error("poll on rfb socket failed");
        }
        if (ready == 0) {
            return false;
        }
        client.process_server_message();
        if (client.take_text(text)) {
            return true;
        }
    }
}

struct action {
    enum class kind { get, watch, set, wait } type;
    std::string value{};
    long milliseconds{};
};

} // namespace

int main(int argc, const char* argv[]) {
  try {
    auto usage_string =
        "Usage: rfb_clipboard <address> <port> [options] <actions...>\n"
        "\n"
        "Reads and writes the clipboard of an RFB/VNC server. No framebuffer\n"
        "updates are requested, so it is cheap even over a slow tunnel.\n"
        "\n"
        "Connection options:\n"
        "  --password <pw>          password for VNC Authentication\n"
        "  --password-file <path>   read the password from the first line of <path>\n"
        "  --encodings <list>       comma separated RFB encoding IDs (default: 16,0)\n"
        "  --timeout <ms>           how long --get waits for the server (default: 10000)\n"
        "  --output <path>          write --get/--watch text here instead of stdout\n"
        "\n"
        "Actions (executed in the order given):\n"
        "  --get                    print the next clipboard text the server sends\n"
        "  --watch                  keep printing every clipboard update, forever\n"
        "  --set <text>             set the remote clipboard to <text>\n"
        "  --set-file <path>        set the remote clipboard to the contents of <path>\n"
        "  --set-stdin              set the remote clipboard to everything on stdin\n"
        "  --wait <ms>              sleep before the next action\n"
        "\n"
        "The protocol has no 'read clipboard now' request: --get waits for the\n"
        "server to announce it, which happens shortly after connecting and on\n"
        "every change. An empty remote clipboard is never announced, so --get\n"
        "times out with exit status 2 in that case.\n"
        "\n"
        "Examples:\n"
        "  rfb_clipboard 127.0.0.1 5901 --get\n"
        "  echo -n 'hello' | rfb_clipboard 127.0.0.1 5901 --set-stdin\n"
        "  rfb_clipboard 127.0.0.1 5901 --set 'text' --wait 500 --get\n"
        ;
    if (argc < 3) {
        throw std::logic_error(usage_string);
    }

    const char* address = nullptr;
    uint16_t port = 5900;
    std::vector<uint32_t> encodings{};
    std::string password{};
    std::string output_path{};
    int timeout_ms = 10000;
    std::vector<action> actions{};

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
            else if (option == "timeout") {
                require_args(i, 1, "--timeout");
                timeout_ms = strtol(argv[++i], nullptr, 10);
            }
            else if (option == "output") {
                require_args(i, 1, "--output");
                output_path = argv[++i];
            }
            else if (option == "encodings") {
                require_args(i, 1, "--encodings");
                auto str = argv[++i];
                char* next;
                while (*str != '\0') {
                    auto v = strtol(str, &next, 10);
                    if (next == str) {
                        break;
                    }
                    str = next + (*next == '\0' ? 0 : 1);
                    encodings.emplace_back(rfb::to_big_endian(static_cast<uint32_t>(v)));
                    if (*next == '\0') {
                        break;
                    }
                }
            }
            else if (option == "get" || option == "watch") {
                action a{};
                a.type = option == "get" ? action::kind::get : action::kind::watch;
                actions.push_back(a);
            }
            else if (option == "set") {
                require_args(i, 1, "--set");
                action a{};
                a.type = action::kind::set;
                a.value = to_cut_text(argv[++i]);
                actions.push_back(a);
            }
            else if (option == "set-file") {
                require_args(i, 1, "--set-file");
                action a{};
                a.type = action::kind::set;
                a.value = to_cut_text(read_whole_file(argv[++i]));
                actions.push_back(a);
            }
            else if (option == "set-stdin") {
                action a{};
                a.type = action::kind::set;
                a.value = to_cut_text(read_whole_stdin());
                actions.push_back(a);
            }
            else if (option == "wait") {
                require_args(i, 1, "--wait");
                action a{};
                a.type = action::kind::wait;
                a.milliseconds = strtol(argv[++i], nullptr, 10);
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
        encodings = {rfb::to_big_endian(uint32_t{16}), 0};
    }

    auto conf = config{
        .address = address, .port = port,
        .supported_encodings = std::move(encodings),
        .password = std::move(password),
    };
    clipboard_rfb client{conf};

    std::cerr << std::format("connected: {}x{}", client.get_width(), client.get_height())
              << std::endl;

    bool timed_out = false;
    for (auto& a : actions) {
        switch (a.type) {
        case action::kind::set:
            client.client_cut_text(a.value);
            std::cerr << std::format("sent {} byte(s) to the remote clipboard", a.value.size())
                      << std::endl;
            break;
        case action::kind::get: {
            std::string text{};
            if (!wait_for_cut_text(client, text, timeout_ms)) {
                std::cerr << "timed out waiting for clipboard text from the server"
                          << std::endl;
                timed_out = true;
                break;
            }
            write_text(text, output_path);
            break;
        }
        case action::kind::watch: {
            // Deliberately never returns: the caller stops it with a signal.
            while (true) {
                std::string text{};
                if (wait_for_cut_text(client, text, timeout_ms)) {
                    write_text(text, output_path);
                }
            }
        }
        case action::kind::wait:
            std::this_thread::sleep_for(std::chrono::milliseconds(a.milliseconds));
            break;
        }
    }
    return timed_out ? 2 : 0;
  }
  catch (std::logic_error& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
  catch (std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }
}
