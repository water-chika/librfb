#pragma once

// Records a hand-performed repro in rfb_window_demo: the input water-chika
// generates, and enough of the output to see what the screen did about it.
//
// Scope: this records and stores a trace. It deliberately does NOT generate a
// repro script -- that belongs to whoever consumes the trace.
//
// Frame policy, and why. Writing a PNG on every framebuffer update would be
// enormous and would mostly capture the same screen repeatedly. The frames
// here exist so a human and a script generator can verify STATE at meaningful
// instants, not to make a video, so one is written:
//   * at record start and at record stop  - the before and after of the repro
//   * at every marker                     - the instant he says matters
//   * on an explicit `frame` command      - when he wants one
//   * optionally, thinned by time         - at most one per interval_ms
// The time-thinned frame is taken lazily when the NEXT input event is
// recorded, never on a timer. That is deliberate: it means the frame captures
// the screen as it stood after the previous input had time to take effect,
// so a trace naturally reads as (input, resulting screen, next input).
//
// This is a linear policy mixin in the codebase's existing style (not CRTP):
// it inherits from its parent policy in a compile-time decorator stack. It must
// be composed BELOW the input layers (add_rfb_process_keysym /
// add_rfb_process_pointer) and directly above add_rfb, because those layers
// call downwards into their parent: sitting here is what lets it see every
// key and pointer event, whether it came from the window or from the control
// port, without any of them knowing it exists.
//
// Required from the parent stack:
//   send_key_event(keysym, state), send_pointer_event(mask, x, y)
//   get_frame(std::span<uint8_t>), get_fb_width(), get_fb_height()

#include "png.hpp"

#include <cpp_helper.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace rfb_trace {

// Pause. Chosen as the marker key because virtually no GUI under test binds
// it, so annotating the trace cannot itself perturb the repro. It is consumed
// locally and never forwarded to the server.
inline constexpr int marker_keysym = 0xff13;

inline std::string json_escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            }
            else {
                out += c;
            }
        }
    }
    return out;
}

template<typename T>
class add_trace_recorder : public T {
public:
    using parent = T;
    using clock = std::chrono::steady_clock;

    add_trace_recorder(const cpp_helper::configure auto& conf) : parent{conf} {}

    bool recording() const { return m_out.is_open(); }

    std::string record_start(const std::string& dir, int interval_ms) {
        if (recording()) {
            return "error already recording";
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return std::format("error could not create {}: {}",
                               dir, ec.message());
        }
        m_dir = dir;
        m_interval_ms = interval_ms;
        m_frame_index = 0;
        m_start = clock::now();
        m_last_frame = m_start;
        m_out.open(std::filesystem::path{dir} / "trace.jsonl",
                   std::ios::out | std::ios::trunc);
        if (!m_out) {
            return std::format("error could not open {}/trace.jsonl", dir);
        }
        // Self describing: a reader needs the desktop size to make sense of
        // any pointer coordinate, and it is the first thing in the file.
        write_line(std::format(
                "\"type\":\"start\",\"desktop\":{{\"width\":{},\"height\":{}}}"
                ",\"interval_ms\":{},\"frame\":\"{}\"",
                parent::get_fb_width(), parent::get_fb_height(),
                interval_ms, capture_frame()));
        return std::format("ok {}/trace.jsonl", dir);
    }

    std::string record_stop() {
        if (!recording()) {
            return "error not recording";
        }
        write_line(std::format("\"type\":\"stop\",\"frame\":\"{}\"",
                               capture_frame()));
        m_out.close();
        return std::format("ok {}/trace.jsonl", m_dir);
    }

    std::string record_mark(const std::string& text) {
        if (!recording()) {
            return "error not recording";
        }
        write_line(std::format("\"type\":\"mark\",\"text\":\"{}\",\"frame\":\"{}\"",
                               json_escape(text), capture_frame()));
        return "ok";
    }

    std::string record_frame(const std::string& note) {
        if (!recording()) {
            return "error not recording";
        }
        write_line(std::format("\"type\":\"frame\",\"note\":\"{}\",\"frame\":\"{}\"",
                               json_escape(note), capture_frame()));
        return "ok";
    }

    // Intercepts every key on its way to the server.
    void send_key_event(int keysym, int state) {
        if (recording() && keysym == marker_keysym) {
            // Consumed locally: the key under test must not see it.
            if (state != 0) {
                record_mark("marker key");
            }
            return;
        }
        if (!input_suppressed()) record_key_event(keysym, state);
        parent::send_key_event(keysym, state);
    }

    void send_pointer_event(uint32_t button_mask, int x, int y) {
        if (!input_suppressed()) record_pointer_event(button_mask, x, y);
        parent::send_pointer_event(button_mask, x, y);
    }

    // Synthesized releases must bypass view-only suppression in the parent, but
    // remain visible in the trace so replay cannot leave remote input held.
    void release_remote_input() {
        if constexpr (requires { parent::held_remote_keys(); }) {
            for (auto key : parent::held_remote_keys()) record_key_event(key, 0);
        }
        if constexpr (requires { parent::held_remote_pointer(); }) {
            auto [mask, x, y] = parent::held_remote_pointer();
            if (mask) record_pointer_event(0, x, y);
        }
        parent::release_remote_input();
    }

private:
    bool input_suppressed() const {
        if constexpr (requires { parent::get_view_only(); }) {
            return parent::get_view_only();
        }
        return false;
    }

    void record_key_event(int keysym, int state) {
        if (!recording()) return;
        interval_frame();
        write_line(std::format(
                "\"type\":\"key\",\"keysym\":{},\"state\":{}",
                keysym, state));
    }

    void record_pointer_event(uint32_t button_mask, int x, int y) {
        if (!recording()) return;
        interval_frame();
        write_line(std::format(
                "\"type\":\"pointer\",\"x\":{},\"y\":{},\"buttons\":{}",
                x, y, button_mask));
    }

    int64_t elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - m_start).count();
    }

    void write_line(const std::string& body) {
        m_out << std::format("{{\"t_ms\":{},{}}}\n", elapsed_ms(), body);
        // Flushed per line so a trace survives a viewer that is killed, which
        // is exactly what happens when the thing under test wedges the box.
        m_out.flush();
    }

    std::string capture_frame() {
        int width = parent::get_fb_width();
        int height = parent::get_fb_height();
        if (width <= 0 || height <= 0) {
            m_last_frame = clock::now();
            return "";
        }
        auto name = std::format("frame_{:06}.png", m_frame_index++);
        m_pixels.resize(static_cast<size_t>(width) * height * 4);
        parent::get_frame(std::span<uint8_t>{m_pixels});
        auto path = (std::filesystem::path{m_dir} / name).string();
        try {
            rfb_png::write_png(path.c_str(), m_pixels.data(), width, height);
        }
        catch (const std::exception&) {
            return "";  // A failed frame must not abort the recording.
        }
        m_last_frame = clock::now();
        return name;
    }

    void interval_frame() {
        if (m_interval_ms <= 0) {
            return;
        }
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - m_last_frame).count();
        if (since < m_interval_ms) {
            return;
        }
        write_line(std::format("\"type\":\"frame\",\"note\":\"interval\","
                               "\"frame\":\"{}\"", capture_frame()));
    }

    std::ofstream m_out;
    std::string m_dir;
    int m_interval_ms{0};
    uint32_t m_frame_index{0};
    clock::time_point m_start{};
    clock::time_point m_last_frame{};
    std::vector<uint8_t> m_pixels;
};

} // namespace rfb_trace
