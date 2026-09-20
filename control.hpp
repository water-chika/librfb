#pragma once

// A line-oriented control port for an already-connected RFB session.
//
// Why this exists: every new RFB connection pays a TCP connect, the RFB
// handshake, authentication, encoding negotiation and then a FULL framebuffer
// update before the first useful pixel arrives. Tools that open their own
// connection per action (rfb_input, rfb_capture) therefore pay that cost on
// every single action, which is exactly backwards for the way they are used
// most: many small interactions in a row, driving a GUI click by click or
// grabbing frames in a loop. Reusing one live viewer session pays the startup
// cost once and ships only incremental updates afterwards.
//
// This is written as a policy mixin, in the same style as the rest of the
// codebase, so it is not tied to rfb_window_demo: any tool whose stack
// provides the operations listed below can compose it.
//
// Required from the parent stack:
//   FDS_SIZE, get_fds(), process_events(fds)   - the posix poll-loop protocol
//   get_fb_width(), get_fb_height()            - framebuffer size
//   get_frame(std::span<uint8_t>)              - copy out decoded BGRA pixels
//   send_pointer_event(mask, x, y)             - honours --view-only
//   send_key_event(keysym, state)              - honours --view-only
//   set_supported_encodings(v), set_encodings()- live encoding re-negotiation
//
// Required from the configure object:
//   control_port  - TCP port to listen on, 0 disables the whole layer
//
// Transport is a loopback TCP socket rather than a unix socket or a fifo
// because those do not exist on Windows and colleagues run Windows. The
// protocol is one text command per line, one line of response ("ok ..." or
// "error ..."), so any language can drive it with no bindings. Captures are
// returned as a file path, never as bytes on the control line, which keeps the
// line protocol trivially parseable.
//
// Security: this is remote control of a desktop, so it is off unless a port is
// given explicitly and the listener is bound to loopback only.

#include "rfb.hpp"
#include "png.hpp"

#include <socket_helper.hpp>

#include <charconv>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rfb_control {

// Accepts one controller at a time. A second connection would interleave
// commands with the first for no benefit, since the commands act on one shared
// session, and serialising them keeps the response line unambiguously the
// answer to the command just sent.
template<typename T>
class add_control_port : public T {
public:
    using parent = T;

    add_control_port(const cpp_helper::configure auto& conf) : parent{conf},
        m_port{conf.control_port}
    {
        if (m_port != 0) {
            m_listener = socket_helper::listen_on_port(m_port, "127.0.0.1");
            std::clog << "control port listening on 127.0.0.1:"
                      << m_port << std::endl;
        }
    }

    static constexpr int FDS_INDEX = parent::FDS_SIZE;
    static constexpr int FDS_SIZE = parent::FDS_SIZE + 2;

    std::vector<pollfd> get_fds() {
        auto fds = parent::get_fds();
        // A negative fd is ignored by poll(), which is how both slots stay
        // present in the fixed-size array while disabled or unconnected.
        fds.emplace_back(pollfd{
                .fd = m_port != 0 ? static_cast<int>(m_listener.get()) : -1,
                .events = POLLIN,
                });
        fds.emplace_back(pollfd{
                .fd = socket_helper::is_valid(m_client.get())
                    ? static_cast<int>(m_client.get()) : -1,
                .events = POLLIN,
                });
        return fds;
    }

    void process_events(auto& fds) {
        if (m_port != 0) {
            if (fds[FDS_INDEX].revents & POLLIN) {
                accept_controller();
            }
            auto& client_poll = fds[FDS_INDEX + 1];
            if (client_poll.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                drop_controller();
            }
            else if (client_poll.revents & POLLIN) {
                read_commands();
            }
        }
        parent::process_events(fds);
    }

private:
    void accept_controller() {
        auto sock = socket_helper::accept_connection(m_listener.get());
        if (!socket_helper::is_valid(sock.get())) {
            return;
        }
        // Replace rather than refuse: a controller that died without closing
        // cleanly would otherwise lock the port out until the viewer restarts.
        m_client = std::move(sock);
        m_pending.clear();
    }

    void drop_controller() {
        m_client = socket_helper::unique_socket{};
        m_pending.clear();
    }

    void read_commands() {
        char buffer[4096];
        auto count = socket_helper::read_socket(
                m_client.get(), buffer, sizeof(buffer));
        if (count <= 0) {
            drop_controller();
            return;
        }
        m_pending.append(buffer, static_cast<size_t>(count));
        for (auto newline = m_pending.find('\n');
             newline != std::string::npos;
             newline = m_pending.find('\n')) {
            auto line = m_pending.substr(0, newline);
            m_pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            respond(run_command(line));
        }
    }

    void respond(const std::string& reply) {
        auto line = reply + "\n";
        socket_helper::write_socket(m_client.get(), line.data(), line.size());
    }

    // Commands arrive as text at runtime, so this one function dispatches on a
    // string. That is deliberately the only runtime dispatch in the layer:
    // there are no virtual functions, and the surrounding composition stays
    // the same compile-time policy design as the rest of the stack.
    std::string run_command(const std::string& line) {
        std::istringstream in{line};
        std::string command;
        in >> command;
        try {
            if (command.empty()) {
                return "ok";
            }
            if (command == "ping") {
                return "ok";
            }
            if (command == "size") {
                return std::format("ok {} {}",
                        parent::get_fb_width(), parent::get_fb_height());
            }
            if (command == "capture") {
                std::string path;
                in >> path;
                if (path.empty()) {
                    return "error capture needs a file path";
                }
                return capture(path);
            }
            if (command == "pointer") {
                int x = 0, y = 0, mask = 0;
                in >> x >> y >> mask;
                parent::send_pointer_event(
                        static_cast<uint32_t>(mask), x, y);
                return "ok";
            }
            if (command == "click") {
                int x = 0, y = 0, button = 0;
                in >> x >> y >> button;
                // A press carrying a new position looks to many toolkits like
                // a teleport with no motion before it, and the click is
                // dropped, so move first exactly as the pointer layer does.
                uint32_t mask = 1u << button;
                parent::send_pointer_event(0, x, y);
                parent::send_pointer_event(mask, x, y);
                parent::send_pointer_event(0, x, y);
                return "ok";
            }
            if (command == "key") {
                int keysym = 0, state = 1;
                in >> keysym >> state;
                parent::send_key_event(keysym, state);
                return "ok";
            }
            if (command == "encoding") {
                std::string spec;
                in >> spec;
                return set_encoding(spec);
            }
            if (command == "scale") {
                double factor = 0;
                in >> factor;
                if (!(factor > 0) || factor > 1.0) {
                    return "error scale must be >0 and <=1.0";
                }
                // Server-side resample over the private extension, on this
                // same connection. False means the peer never advertised it.
                if (!parent::request_scale(
                            static_cast<int>(factor * 1000 + 0.5))) {
                    return "error peer does not support the scale extension";
                }
                return std::format("ok {}", factor);
            }
            return std::format("error unknown command: {}", command);
        }
        catch (rfb::connection_closed&) {
            throw;
        }
        catch (std::exception& e) {
            return std::format("error {}", e.what());
        }
    }

    std::string capture(const std::string& path) {
        int width = parent::get_fb_width();
        int height = parent::get_fb_height();
        if (width <= 0 || height <= 0) {
            return "error no framebuffer yet";
        }
        m_pixels.resize(static_cast<size_t>(width) * height * 4);
        parent::get_frame(std::span<uint8_t>{m_pixels});
        rfb_png::write_png(path.c_str(), m_pixels.data(), width, height);
        return std::format("ok {}", path);
    }

    std::string set_encoding(const std::string& spec) {
        auto parsed = rfb::parse_encodings(spec);
        if (!parsed) {
            return std::format("error unknown encoding: {}", spec);
        }
        std::vector<uint32_t> encodings;
        for (auto value : *parsed) {
            encodings.emplace_back(
                    rfb::to_big_endian(static_cast<uint32_t>(value)));
        }
        parent::set_supported_encodings(std::move(encodings));
        parent::set_encodings();
        return std::format("ok {}", spec);
    }

    uint16_t m_port;
    socket_helper::unique_socket m_listener{};
    socket_helper::unique_socket m_client{};
    std::string m_pending;
    std::vector<uint8_t> m_pixels;
};

} // namespace rfb_control
