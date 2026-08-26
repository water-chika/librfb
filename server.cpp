#include "server.hpp"

#include <charconv>

namespace {

void print_usage() {
    std::cout <<
        "Usage: rfb_server [options]\n"
        "\n"
        "Serves the local desktop over the RFB (VNC) protocol.\n"
        "\n"
        "  --port <n>          port to listen on (default 5900, i.e. display :0)\n"
        "  --listen <address>  bind to one address, e.g. 127.0.0.1 for SSH\n"
        "                      tunnels only (default: every interface)\n"
        "  --password <text>   require VNC authentication with this password\n"
        "                      (default: no authentication at all)\n"
        "  --name <text>       desktop name shown by the viewer\n"
        "  --view-only         ignore keyboard and pointer events\n"
        "  --fps <n>           maximum capture rate (default 20)\n"
        "  --all-monitors      capture the whole virtual desktop\n"
        "  --no-cursor         do not draw the mouse cursor into the image\n"
        "  --help              show this message\n";
}

// VNC authentication keys are DES keys: only the first 8 characters ever
// reach the cipher, so silently truncating would be a security surprise.
constexpr size_t max_password_length = 8;

std::string require_value(int argc, char** argv, int& index) {
    if (index + 1 >= argc) {
        throw std::runtime_error{
            std::format("{} needs a value", argv[index])};
    }
    return std::string{argv[++index]};
}

int parse_int(const std::string& text, const char* what) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error{std::format("{} is not a number: {}", what, text)};
    }
    return value;
}

} // namespace

int main(int argc, char** argv) {
    using namespace rfb::server;
    // The log is usually redirected to a file or a pipe, where stdout is
    // block buffered: without this, nothing shows up until several kilobytes
    // of log have accumulated, which for a quiet server can be never.
    std::cout << std::unitbuf;
    try {
        server_options options{};
        options.desktop_name = default_desktop_name();

        for (int i = 1; i < argc; i++) {
            std::string argument{argv[i]};
            if (argument == "--help" || argument == "-h") {
                print_usage();
                return 0;
            }
            else if (argument == "--port") {
                auto port = parse_int(require_value(argc, argv, i), "--port");
                if (port <= 0 || port > 65535) {
                    throw std::runtime_error{"--port is out of range"};
                }
                options.port = static_cast<uint16_t>(port);
            }
            else if (argument == "--listen") {
                options.listen_address = require_value(argc, argv, i);
            }
            else if (argument == "--password") {
                options.password = require_value(argc, argv, i);
                if (options.password.size() > max_password_length) {
                    throw std::runtime_error{
                        "--password is limited to 8 characters by the VNC "
                        "authentication scheme"};
                }
            }
            else if (argument == "--name") {
                options.desktop_name = require_value(argc, argv, i);
            }
            else if (argument == "--view-only") {
                options.view_only = true;
            }
            else if (argument == "--fps") {
                options.max_fps = parse_int(require_value(argc, argv, i), "--fps");
                if (options.max_fps < 1 || options.max_fps > 120) {
                    throw std::runtime_error{"--fps must be between 1 and 120"};
                }
            }
            else if (argument == "--all-monitors") {
                options.capture.all_monitors = true;
            }
            else if (argument == "--no-cursor") {
                options.capture.draw_cursor = false;
            }
            else {
                throw std::runtime_error{
                    std::format("unknown option: {}", argument)};
            }
        }

        if (options.password.empty()) {
            std::cout << "warning: no --password given, anyone who can reach "
                         "the port gets full control\n";
        }

        rfb_server server{options};
        server.run();
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
