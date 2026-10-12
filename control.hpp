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
#include "trace.hpp"

#include <socket_helper.hpp>

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rfb_control {

inline constexpr size_t max_pending_bytes = 64 * 1024;

// Accepts one controller at a time. A second connection would interleave
// commands with the first for no benefit, since the commands act on one shared
// session, and serialising them keeps the response line unambiguously the
// answer to the command just sent.
template<typename T>
class add_control_port : public T {
public:
    using parent = T;

    add_control_port(const cpp_helper::configure auto& conf) : parent{conf},
        m_port{conf.control_port},
        m_host{conf.address}, m_rfb_port{conf.port}
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
                .fd = m_port != 0 ? m_listener.get()
                                  : socket_helper::invalid_socket,
                .events = POLLIN,
                });
        fds.emplace_back(pollfd{
                .fd = socket_helper::is_valid(m_client.get())
                    ? m_client.get() : socket_helper::invalid_socket,
                .events = POLLIN,
                });
        return fds;
    }

    void process_events(auto& fds) {
        if (m_port != 0) {
            auto& client_poll = fds[FDS_INDEX + 1];
            // These events describe the controller that was present when poll
            // ran. Drain it before accepting a replacement, and process readable
            // bytes before HUP so a final complete command is not discarded.
            if (client_poll.revents & POLLIN) {
                read_commands();
            }
            if (socket_helper::is_valid(m_client.get()) &&
                (client_poll.revents & (POLLHUP | POLLERR | POLLNVAL))) {
                drop_controller();
            }
            if (fds[FDS_INDEX].revents & POLLIN) {
                accept_controller();
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
        if (!socket_helper::set_nonblocking(m_client.get())) {
            drop_controller();
            return;
        }
        m_pending.clear();
    }

    void drop_controller() {
        m_client = socket_helper::unique_socket{};
        m_pending.clear();
    }

    void read_commands() {
        char buffer[4096];
        bool eof{};
        for (;;) {
            auto count = socket_helper::read_socket(
                    m_client.get(), buffer, sizeof(buffer));
            if (count > 0) {
                m_pending.append(buffer, static_cast<size_t>(count));
                process_pending_commands();
                if (!socket_helper::is_valid(m_client.get())) return;
                if (m_pending.size() > max_pending_bytes) {
                    respond("error command too long");
                    drop_controller();
                    return;
                }
                continue;
            }
            if (count == 0) eof = true;
            else if (!socket_helper::would_block(socket_helper::last_error())) eof = true;
            break;
        }
        process_pending_commands();
        if (eof) drop_controller();
    }

    void process_pending_commands() {
        for (auto newline = m_pending.find('\n');
             newline != std::string::npos;
             newline = m_pending.find('\n')) {
            auto line = m_pending.substr(0, newline);
            m_pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            respond(run_command(line));
            if (!socket_helper::is_valid(m_client.get())) return;
        }
    }

    void respond(const std::string& reply) {
        auto line = reply + "\n";
        size_t sent{};
        while (sent < line.size()) {
            auto count = socket_helper::write_socket(
                m_client.get(), line.data() + sent, line.size() - sent);
            if (count > 0) { sent += static_cast<size_t>(count); continue; }
            // The GUI event loop must never wait for a controller that stopped
            // reading. Drop it on back-pressure or any send error.
            drop_controller();
            return;
        }
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
                in >> std::quoted(path);
                if (path.empty()) {
                    return "error capture needs a file path";
                }
                return capture(path);
            }
            if (command == "pointer") {
                int x = 0, y = 0, mask = 0;
                if (!(in >> x >> y >> mask) || mask < 0 || mask > 255) {
                    return "error pointer needs x y buttons";
                }
                parent::send_pointer_event(
                        static_cast<uint32_t>(mask), x, y);
                return "ok";
            }
            if (command == "click") {
                int x = 0, y = 0, button = 0;
                if (!(in >> x >> y >> button) || button < 0 || button > 7) {
                    return "error click needs x y button";
                }
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
                if (!(in >> keysym >> state) || (state != 0 && state != 1)) {
                    return "error key needs keysym state";
                }
                parent::send_key_event(keysym, state);
                return "ok";
            }
            if (command == "record") {
                std::string action;
                in >> action;
                if (action == "start") {
                    std::string dir;
                    int interval_ms = 0;
                    in >> std::quoted(dir) >> interval_ms;
                    if (dir.empty()) {
                        return "error record start needs a directory";
                    }
                    return parent::record_start(dir, interval_ms);
                }
                if (action == "stop") {
                    return parent::record_stop();
                }
                return "error record needs start or stop";
            }
            if (command == "mark") {
                std::string text;
                std::getline(in, text);
                if (!text.empty() && text.front() == ' ') {
                    text.erase(0, 1);
                }
                return parent::record_mark(text);
            }
            if (command == "frame") {
                std::string note;
                std::getline(in, note);
                if (!note.empty() && note.front() == ' ') {
                    note.erase(0, 1);
                }
                return parent::record_frame(note);
            }
            if (command == "encoding") {
                std::string spec;
                in >> spec;
                return set_encoding(spec);
            }
            if (command == "refresh") {
                // Forces a full, non-incremental repaint. Cheap way to prove
                // (or disprove) that the request loop is still alive: if the
                // viewer is genuinely stuck, this does nothing either, and
                // that is itself useful evidence, not a failure of the verb.
                parent::request_framebuffer_update(
                        0, 0, parent::get_fb_width(), parent::get_fb_height(),
                        false);
                return "ok";
            }
            if (command == "status") {
                return std::format(
                        "ok host={} port={} encoding={} bytes_received={} "
                        "ms_since_last_update={:.1f} view_only={}",
                        m_host, m_rfb_port,
                        static_cast<int>(parent::get_encoding()),
                        parent::get_frame_network_byte_count(),
                        parent::ms_since_last_update(),
                        parent::get_view_only() ? "true" : "false");
            }
            if (command == "view-only") {
                std::string state;
                in >> state;
                if (state == "on") {
                    // Release through an unsuppressed path before input is
                    // disabled, otherwise held remote keys/buttons can stick.
                    if constexpr (requires { parent::release_remote_input(); }) {
                        parent::release_remote_input();
                    }
                    parent::set_view_only(true);
                }
                else if (state == "off") {
                    parent::set_view_only(false);
                }
                else {
                    return "error view-only needs on or off";
                }
                return std::format("ok {}", state);
            }
            if (command == "quit") {
                // Reuses the same exception the viewer already treats as a
                // clean shutdown when the server hangs up, so main()'s
                // existing "not a failure" handling applies here too.
                throw rfb::connection_closed{"quit requested over control port"};
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
    std::string m_host;
    uint16_t m_rfb_port;
    socket_helper::unique_socket m_listener{};
    socket_helper::unique_socket m_client{};
    std::string m_pending;
    std::vector<uint8_t> m_pixels;
};

} // namespace rfb_control
