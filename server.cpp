#include "server.hpp"

#include <charconv>
#include <cstdlib>

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
        "  --quality <n>       H264 quality as a QP, 0 best .. 51 worst"
        " (default 18)\n"
        "  --no-adapt-quality  keep that quality even when updates cannot be\n"
        "                      written in time. By default a client that asks\n"
        "                      for no quality level of its own gets a coarser\n"
        "                      picture instead of a laggy one\n"
        "  --bandwidth <kbit>  cap what each client is written at, in kbit/s,\n"
        "                      to imitate a slow link (default: no cap). A\n"
        "                      test knob, not a fair-sharing policy\n"
        "  --scale <factor>    serve the desktop downscaled by this factor,\n"
        "                      e.g. 0.5 for half width and half height, i.e.\n"
        "                      a quarter of the pixels (default 1.0, no\n"
        "                      scaling). The image is shrunk before it is\n"
        "                      encoded, so the saving is real bytes on the\n"
        "                      wire, not just a smaller window\n"
        "  --max-width <n>     never serve wider than this many pixels,\n"
        "                      shrinking proportionally if the desktop is\n"
        "                      bigger (default: no limit). Combines with\n"
        "                      --scale; the stricter of the two wins\n"
        "  --scale-filter <f>  box (default) or nearest. box averages and\n"
        "                      keeps text legible; nearest preserves flat\n"
        "                      colour runs, which ZRLE compresses far\n"
        "                      better on poster-like content\n"
        "  --all-monitors      capture the whole virtual desktop\n"
        "  --no-cursor         do not draw the mouse cursor into the image\n"
        "  --console-launcher  (Windows) do not serve anything directly;\n"
        "                      instead launch the server into the session that\n"
        "                      is attached to the console, and keep it there.\n"
        "                      Run this as LocalSystem from a boot-time task:\n"
        "                      it is the only way to have a desktop server\n"
        "                      running before anyone logs in\n"
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

// std::from_chars for floating point is not available everywhere this builds,
// so the scale factor is parsed with strtod and then checked to have consumed
// the whole argument, which rejects "0.5x" the same way parse_int would.
double parse_double(const std::string& text, const char* what) {
    const char* begin = text.c_str();
    char* end = nullptr;
    double value = std::strtod(begin, &end);
    if (end != begin + text.size() || text.empty()) {
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

        // Collected so the launcher can hand the very same options to the
        // worker it starts in the console session.
        bool console_launcher = false;
        std::vector<std::string> worker_args{};

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
            else if (argument == "--quality") {
                options.h264_quality =
                    parse_int(require_value(argc, argv, i), "--quality");
                if (options.h264_quality < 0 || options.h264_quality > 51) {
                    throw std::runtime_error{
                        "--quality must be between 0 and 51"};
                }
            }
            else if (argument == "--no-adapt-quality") {
                options.adapt_quality = false;
            }
            else if (argument == "--bandwidth") {
                options.bandwidth_kbit =
                    parse_int(require_value(argc, argv, i), "--bandwidth");
                if (options.bandwidth_kbit < 0) {
                    throw std::runtime_error{
                        "--bandwidth must not be negative"};
                }
            }
            else if (argument == "--scale") {
                options.scale =
                    parse_double(require_value(argc, argv, i), "--scale");
                // Upscaling would cost bandwidth rather than save it, and an
                // arbitrarily small factor collapses the image to nothing.
                if (!(options.scale > 0.0) || options.scale > 1.0) {
                    throw std::runtime_error{
                        "--scale must be greater than 0 and at most 1.0"};
                }
            }
            else if (argument == "--max-width") {
                options.max_width =
                    parse_int(require_value(argc, argv, i), "--max-width");
                if (options.max_width < 0) {
                    throw std::runtime_error{
                        "--max-width must not be negative"};
                }
            }
            else if (argument == "--scale-filter") {
                std::string filter = require_value(argc, argv, i);
                if (filter == "box") {
                    options.scale_box_filter = true;
                }
                else if (filter == "nearest") {
                    options.scale_box_filter = false;
                }
                else {
                    throw std::runtime_error{
                        "--scale-filter must be box or nearest"};
                }
            }
            else if (argument == "--all-monitors") {
                options.capture.all_monitors = true;
            }
            else if (argument == "--no-cursor") {
                options.capture.draw_cursor = false;
            }
            else if (argument == "--console-launcher") {
                console_launcher = true;
            }
            else {
                throw std::runtime_error{
                    std::format("unknown option: {}", argument)};
            }
        }

        if (console_launcher) {
            // The worker gets every option except the one that made this
            // process a launcher, so both sides always agree on the port,
            // password and capture settings.
            for (int i = 1; i < argc; i++) {
                if (std::string{argv[i]} != "--console-launcher") {
                    worker_args.emplace_back(argv[i]);
                }
            }
            return run_console_launcher(worker_args);
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
