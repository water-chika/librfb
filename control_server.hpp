#pragma once

#include "png.hpp"
#include "rfb.hpp"

#include <socket_helper.hpp>

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rfb_control {

inline constexpr std::size_t max_control_line = 64 * 1024;

inline bool read_argument(std::istringstream& in, std::string& value) {
    in >> std::ws;
    if (in.peek() == '"') {
        in >> std::quoted(value);
    } else {
        in >> value;
    }
    return !value.empty();
}

template<class Session>
std::string run_command(Session& session, std::string_view host,
                        std::uint16_t rfb_port, const std::string& line) {
    std::istringstream in{line};
    std::string command;
    in >> command;
    try {
        if (command.empty() || command == "ping") return "ok";
        if (command == "size") return std::format(
            "ok {} {}", session.get_fb_width(), session.get_fb_height());
        if (command == "capture") {
            std::string path;
            if (!read_argument(in, path)) return "error capture needs a file path";
            int width = session.get_fb_width(), height = session.get_fb_height();
            if (width <= 0 || height <= 0) return "error no framebuffer yet";
            std::vector<std::uint8_t> pixels(
                static_cast<std::size_t>(width) * height * 4);
            session.get_rfb(pixels);
            rfb_png::write_png(path.c_str(), pixels.data(), width, height);
            return std::format("ok {}", path);
        }
        if (command == "pointer") {
            int x{}, y{}, mask{};
            if (!(in >> x >> y >> mask) || mask < 0 || mask > 255)
                return "error pointer needs x y buttons";
            session.send_pointer_event(static_cast<std::uint32_t>(mask), x, y);
            return "ok";
        }
        if (command == "click") {
            int x{}, y{}, button{};
            if (!(in >> x >> y >> button) || button < 0 || button > 7)
                return "error click needs x y button";
            auto mask = 1u << button;
            session.send_pointer_event(0, x, y);
            session.send_pointer_event(mask, x, y);
            session.send_pointer_event(0, x, y);
            return "ok";
        }
        if (command == "key") {
            int keysym{}, state{1};
            if (!(in >> keysym >> state) || (state != 0 && state != 1))
                return "error key needs keysym state";
            session.send_key_event(keysym, state);
            return "ok";
        }
        if (command == "record") {
            std::string action;
            in >> action;
            if (action == "start") {
                std::string dir;
                int interval_ms{};
                if (!read_argument(in, dir)) return "error record start needs a directory";
                if (!(in >> interval_ms)) interval_ms = 0;
                return session.record_start(dir, interval_ms);
            }
            if (action == "stop") return session.record_stop();
            return "error record needs start or stop";
        }
        if (command == "mark" || command == "frame") {
            std::string text;
            std::getline(in >> std::ws, text);
            return command == "mark" ? session.record_mark(text)
                                     : session.record_frame(text);
        }
        if (command == "encoding") {
            std::string spec;
            in >> spec;
            auto parsed = rfb::parse_encodings(spec);
            if (!parsed) return std::format("error unknown encoding: {}", spec);
            std::vector<std::uint32_t> encodings;
            for (auto value : *parsed)
                encodings.push_back(rfb::to_big_endian(static_cast<std::uint32_t>(value)));
            encodings.push_back(rfb::to_big_endian(
                static_cast<std::uint32_t>(rfb::encoding::scale_ext)));
            session.set_supported_encodings(std::move(encodings));
            session.set_encodings();
            return std::format("ok {}", spec);
        }
        if (command == "refresh") {
            session.request_framebuffer_update(0, 0, session.get_fb_width(),
                                               session.get_fb_height(), false);
            return "ok";
        }
        if (command == "status") return std::format(
            "ok host={} port={} encoding={} bytes_received={} "
            "ms_since_last_update={:.1f} view_only={}", host, rfb_port,
            static_cast<int>(session.get_encoding()),
            session.get_frame_network_byte_count(), session.ms_since_last_update(),
            session.get_view_only() ? "true" : "false");
        if (command == "view-only") {
            std::string state;
            in >> state;
            if (state != "on" && state != "off")
                return "error view-only needs on or off";
            // Releases must bypass view-only suppression or keys/buttons held at
            // the instant it is enabled remain stuck on the remote desktop.
            if (state == "on") {
                if constexpr (requires { session.release_remote_input(); })
                    session.release_remote_input();
                session.set_view_only(true);
            } else {
                session.set_view_only(false);
            }
            return std::format("ok {}", state);
        }
        if (command == "scale") {
            double factor{};
            if (!(in >> factor) || !(factor > 0) || factor > 1.0)
                return "error scale must be >0 and <=1.0";
            if (!session.request_scale(static_cast<int>(factor * 1000 + .5)))
                return "error peer does not support the scale extension";
            return std::format("ok {}", factor);
        }
        if (command == "quit") return "ok quit";
        return std::format("error unknown command: {}", command);
    } catch (const std::exception& e) {
        return std::format("error {}", e.what());
    }
}

template<class Session>
class control_server {
public:
    control_server(std::uint16_t port, std::string host,
                   std::uint16_t rfb_port, Session& session)
        : m_port{port}, m_host{std::move(host)}, m_rfb_port{rfb_port},
          m_session{session} {
        if (m_port) m_listener = socket_helper::listen_on_port(m_port, "127.0.0.1");
    }

    bool poll_once() {
        if (!m_port) return true;
        pollfd fds[2]{{m_listener.get(), POLLIN, 0},
                      {m_client.get(), POLLIN, 0}};
        auto count = m_client ? 2u : 1u;
        if (socket_helper::poll_sockets(fds, count, 0) <= 0) return true;
        // Drain the old controller before accepting its replacement. Otherwise
        // fds[1] describes the old socket but is applied to the fresh handle.
        if (m_client && (fds[1].revents & POLLIN)) read_commands();
        if (m_client && (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))) drop();
        if (fds[0].revents & POLLIN) {
            m_client = socket_helper::accept_connection(m_listener.get());
            if (!socket_helper::set_nonblocking(m_client.get())) {
                drop();
                return !m_quit;
            }
            m_pending.clear();
        }
        return !m_quit;
    }

private:
    void drop() { m_client.reset(); m_pending.clear(); }
    void send_all(std::string_view text) {
        std::size_t sent{};
        while (sent < text.size()) {
            auto n = socket_helper::write_socket(
                m_client.get(), text.data() + sent, text.size() - sent);
            if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
            if (n < 0 && socket_helper::would_block(socket_helper::last_error())) {
                drop(); // Never let a non-reading controller block the GUI.
                return;
            }
            drop(); return;
        }
    }
    void read_commands() {
        char buffer[4096];
        bool eof{};
        for (;;) {
            auto n = socket_helper::read_socket(m_client.get(), buffer, sizeof buffer);
            if (n > 0) {
                m_pending.append(buffer, static_cast<std::size_t>(n));
                process_pending();
                if (!m_client) return;
                if (m_pending.size() > max_control_line) {
                    send_all("error command too long\n"); drop(); return;
                }
                continue;
            }
            if (n == 0) eof = true;
            else if (!socket_helper::would_block(socket_helper::last_error())) eof = true;
            break;
        }
        process_pending();
        if (eof) drop();
    }
    void process_pending() {
        for (auto pos = m_pending.find('\n'); pos != std::string::npos;
             pos = m_pending.find('\n')) {
            auto line = m_pending.substr(0, pos);
            m_pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto reply = run_command(m_session, m_host, m_rfb_port, line);
            if (reply == "ok quit") { send_all("ok\n"); m_quit = true; return; }
            send_all(reply + "\n");
            if (!m_client) return;
        }
    }

    std::uint16_t m_port{};
    std::string m_host;
    std::uint16_t m_rfb_port{};
    Session& m_session;
    socket_helper::unique_socket m_listener, m_client;
    std::string m_pending;
    bool m_quit{};
};

} // namespace rfb_control
