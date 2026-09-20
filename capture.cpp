// rfb_capture: connects to an RFB/VNC server and writes its framebuffer to a
// PNG file.
//
// Split out of rfb_input, which used to carry a --capture action. The two jobs
// only looked related: sending a KeyEvent needs no pixels at all, whereas a
// capture needs the whole decode stack, and interleaving them in one process
// encouraged the mistake of capturing immediately after a click, before the
// remote screen had repainted. Two commands make that delay explicit:
//
//   rfb_input   127.0.0.1 5900 --click 200 200
//   rfb_capture 127.0.0.1 5900 --wait 800 --output /tmp/screen.png
//
// It runs headlessly - no window is opened, and no local image library is
// needed, since libavcodec already provides a PNG encoder.

#include "rfb.hpp"
#include "png.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using cpp_helper::empty_configurable_class;

namespace {

// The same headless protocol stack rfb_input and rfb_recorder use, plus a
// password layer for servers that require VNC Authentication. Unlike those
// two, every decoding layer here is load bearing: this tool exists to turn
// framebuffer updates into pixels.
template<typename T>
using add_capture_rfb =
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

using capture_rfb = add_capture_rfb<empty_configurable_class>;

// Pumps server messages until a framebuffer update has been decoded, so the
// captured image reflects the current screen rather than a blank buffer.
//
// A full-screen update is normally split across several FramebufferUpdate
// messages, and each one only covers part of the screen, so capturing after the
// first message yields a mostly empty image. Instead keep reading until the
// server goes quiet for quiet_ms, which means the screen has been fully sent.
// poll() is used because the underlying reads block indefinitely, which would
// otherwise make the timeout unenforceable.
void wait_for_frame(auto& client, int width, int height, int timeout_ms, int quiet_ms = 400) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    client.reset_frame_updated();
    client.framebuffer_update_request(0, 0, width, height, false);

    bool got_update = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd{};
        fd.fd = client.get_socket();
        fd.events = POLLIN;
        int ready = socket_helper::poll_sockets(&fd, 1, quiet_ms);
        if (ready < 0) {
            throw std::runtime_error("poll on rfb socket failed");
        }
        if (ready == 0) {
            // Server went quiet: everything it wanted to send has arrived.
            if (got_update) {
                return;
            }
            continue;
        }
        client.process_server_message();
        if (client.is_frame_updated()) {
            got_update = true;
        }
    }
    if (!got_update) {
        throw std::runtime_error("timed out waiting for a framebuffer update");
    }
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

} // namespace

int main(int argc, const char* argv[]) {
  try {
    auto usage_string =
        "Usage: rfb_capture <address> <port> [options] --output <path.png>\n"
        "\n"
        "Saves one framebuffer of an RFB/VNC server as a PNG. Runs headlessly.\n"
        "\n"
        "Options:\n"
        "  --output <path.png>      where to write the image (required)\n"
        "  --password <pw>          password for VNC Authentication\n"
        "  --password-file <path>   read the password from the first line of <path>\n"
        "  --encodings <list>       comma separated RFB encodings, by name or ID,\n"
        "                           e.g. zrle,raw or 16,0 (default: zrle,raw)\n"
        "  --wait <ms>              settle before capturing (default: 0)\n"
        "  --timeout <ms>           framebuffer capture timeout (default: 10000)\n"
        "\n"
        "Use --wait after driving the screen with rfb_input: a click is only\n"
        "delivered when it is sent, and the application still has to repaint,\n"
        "so an immediate capture returns the screen as it was before the click.\n"
        "\n"
        "Example:\n"
        "  rfb_input   127.0.0.1 5900 --password mvtest --key super\n"
        "  rfb_capture 127.0.0.1 5900 --password mvtest --wait 800 \\\n"
        "      --output /tmp/screen.png\n"
        ;
    if (argc < 3) {
        throw std::logic_error(usage_string);
    }

    const char* address = nullptr;
    uint16_t port = 5900;
    std::vector<uint32_t> encodings{};
    std::string password{};
    std::string output{};
    int wait_ms = 0;
    int timeout_ms = 10000;

    auto require_args = [&](int i, int count, const char* option) {
        if (i + count >= argc) {
            throw std::logic_error(std::format("{} needs {} argument(s)\n\n{}", option, count, usage_string));
        }
    };

    for (int i = 1, pos_arg = 0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            std::string option = &argv[i][2];
            if (option == "output") {
                require_args(i, 1, "--output");
                output = argv[++i];
            }
            else if (option == "password") {
                require_args(i, 1, "--password");
                password = argv[++i];
            }
            else if (option == "password-file") {
                require_args(i, 1, "--password-file");
                password = read_password_file(argv[++i]);
            }
            else if (option == "wait") {
                require_args(i, 1, "--wait");
                wait_ms = strtol(argv[++i], nullptr, 10);
            }
            else if (option == "timeout") {
                require_args(i, 1, "--timeout");
                timeout_ms = strtol(argv[++i], nullptr, 10);
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
    if (output.empty()) {
        throw std::logic_error(std::format("no --output given\n\n{}", usage_string));
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
    capture_rfb client{conf};

    int width = client.get_width();
    int height = client.get_height();
    std::cerr << std::format("connected: {}x{}", width, height) << std::endl;

    // Waiting before the first request, not after it, so the whole exchange
    // that produces the saved image happens once the screen has settled.
    if (wait_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    }

    // The first update a server sends after connecting often covers only part
    // of the screen, so drain one throwaway cycle before the one that is kept.
    try {
        wait_for_frame(client, width, height, timeout_ms);
    }
    catch (std::exception& e) {
        std::cerr << "warning: initial framebuffer sync failed: " << e.what() << std::endl;
    }

    wait_for_frame(client, width, height, timeout_ms);
    auto frame_data = client.get_frame();
    rfb_png::write_png(output.c_str(), frame_data.data(), width, height);
    std::cerr << "captured: " << output << std::endl;
  }
  catch (std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
  return 0;
}
