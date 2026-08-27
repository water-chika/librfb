#pragma once

// An RFB (VNC) server: it exports the local desktop and replays the input a
// viewer sends back. The rest of librfb is the client half of the protocol;
// this is the mirror image of it, and shares only the socket helpers.
//
// Protocol versions 3.3, 3.7 and 3.8 are accepted, with the None and VNC
// Authentication security types, and Raw, ZRLE and (when built with
// -DRFB_SERVER_H264=ON) H264 encodings.

#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/des.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <socket_helper.hpp>

#include "server_encode.hpp"
#include "server_h264.hpp"
#include "server_platform.hpp"

namespace rfb::server {

struct server_options {
    uint16_t port{5900};
    // Empty means every interface. "127.0.0.1" restricts to loopback, which
    // is the safe default when the connection is tunnelled over SSH.
    std::string listen_address{};
    // Empty means the None security type, i.e. no authentication at all.
    std::string password{};
    std::string desktop_name{"librfb"};
    bool view_only{false};
    int max_fps{20};
    // H264 constant-quality level, in H.264 QP units (0 lossless .. 51 worst).
    // Screen content is mostly flat colour and text, so it wants a far lower
    // QP than camera video does before the text stops shimmering.
    int h264_quality{18};
    // Lets the server raise the QP above h264_quality when updates take
    // longer to write than the capture interval, i.e. when the link cannot
    // carry the stream. Only ever applies when the client did not ask for a
    // quality level itself.
    bool adapt_quality{true};
    // Artificial cap on how fast a session may write to its socket, in kbit/s.
    // Zero means no limit. Purely a test knob: it makes a fast loopback
    // connection behave like a slow link.
    int bandwidth_kbit{0};
    capture_options capture{};
};

inline std::string default_desktop_name() {
#ifdef _WIN32
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = sizeof(name);
    if (::GetComputerNameA(name, &size)) {
        return std::string{name};
    }
#endif
    return "librfb";
}

// Captures the desktop at most max_fps times a second and shares the result
// with every connected session. Frames are handed out as shared_ptr so a
// slow client can keep encoding an older frame while a new one is captured.
class screen_source {
public:
    screen_source(const capture_options& options, int max_fps)
        : m_capture{options},
          m_interval{std::chrono::microseconds{1000000 / std::max(1, max_fps)}} {}

    int width() const { return m_capture.width(); }
    int height() const { return m_capture.height(); }
    int origin_x() const { return m_capture.origin_x(); }
    int origin_y() const { return m_capture.origin_y(); }

    // Returns the newest frame, capturing a fresh one if the previous is
    // older than the frame interval. generation changes whenever the pixels
    // were re-read.
    std::shared_ptr<const frame_buffer> frame(uint64_t& generation) {
        std::lock_guard lock{m_mutex};
        auto now = std::chrono::steady_clock::now();
        if (m_current == nullptr || now - m_captured_at >= m_interval) {
            auto next = acquire_buffer();
            if (m_capture.capture(*next)) {
                m_current = next;
                m_generation++;
                m_failures = 0;
            }
            // A failed capture (the secure desktop is in front, or the
            // session was locked) keeps the last good frame, so the client
            // sees a frozen screen rather than an error. Never failing
            // silently matters though: with no first frame the viewer just
            // waits forever.
            else if (m_failures++ % 100 == 0) {
                std::cerr << std::format(
                    "screen capture failed ({} times): {}{}\n", m_failures,
                    m_capture.diagnostic().empty() ? "no diagnostic"
                                                   : m_capture.diagnostic(),
                    m_current == nullptr
                        ? "; no frame has ever been captured, so viewers will "
                          "see nothing"
                        : "");
            }
            m_captured_at = now;
        }
        generation = m_generation;
        return m_current;
    }

    // How long until frame() would read the screen again. A session that
    // already owes its client an update has nothing useful to do before then,
    // and nothing to gain by waiting any longer.
    //
    // Deliberately not special-cased on m_current == nullptr. frame() stamps
    // m_captured_at on every attempt, failures included, so a screen that
    // cannot be captured at all still reports a full interval and the caller
    // polls at the frame rate instead of spinning. Before the first attempt
    // m_captured_at is its epoch default, which correctly yields zero: the
    // very first frame should be read immediately.
    std::chrono::steady_clock::duration time_to_next_capture() const {
        std::lock_guard lock{m_mutex};
        auto elapsed = std::chrono::steady_clock::now() - m_captured_at;
        if (elapsed >= m_interval) {
            return std::chrono::steady_clock::duration::zero();
        }
        return m_interval - elapsed;
    }

private:
    // Reuses buffers no session still holds, instead of allocating several
    // megabytes per captured frame.
    std::shared_ptr<frame_buffer> acquire_buffer() {
        for (auto& buffer : m_pool) {
            if (buffer.use_count() == 1 && buffer != m_current) {
                return buffer;
            }
        }
        auto buffer = std::make_shared<frame_buffer>();
        m_pool.push_back(buffer);
        return buffer;
    }

    mutable std::mutex m_mutex{};
    screen_capture m_capture;
    std::chrono::steady_clock::duration m_interval;
    std::chrono::steady_clock::time_point m_captured_at{};
    std::shared_ptr<frame_buffer> m_current{};
    std::vector<std::shared_ptr<frame_buffer>> m_pool{};
    uint64_t m_generation{0};
    uint64_t m_failures{0};
};

struct rectangle {
    int x{}, y{}, width{}, height{};
};

// Caps how fast a session writes to its socket, so that a slow link can be
// reproduced on a loopback connection. A token bucket: tokens accrue at the
// configured rate, a write spends one per byte, and overdrawing sleeps off
// the debt. Disabled -- and then free -- when no limit was asked for.
class rate_limiter {
public:
    rate_limiter() = default;

    explicit rate_limiter(int kbit_per_second) {
        if (kbit_per_second <= 0) {
            return;
        }
        m_bytes_per_second = kbit_per_second * 1000.0 / 8.0;
        // One 20ms burst, so that short protocol messages still go out in one
        // piece instead of being paced a packet at a time. Floored at a
        // typical MSS so even a very low limit makes progress.
        m_burst = std::max(1460.0, m_bytes_per_second / 50.0);
        m_tokens = m_burst;
        m_refilled_at = std::chrono::steady_clock::now();
    }

    bool enabled() const { return m_bytes_per_second > 0.0; }

    // The most that should be handed to the socket at once: writing more than
    // a burst in one call would hand the kernel a backlog this class can no
    // longer pace.
    size_t chunk() const { return static_cast<size_t>(m_burst); }

    void consume(size_t count) {
        if (!enabled()) {
            return;
        }
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - m_refilled_at).count();
        m_tokens = std::min(m_burst, m_tokens + elapsed * m_bytes_per_second);
        m_refilled_at = now;
        m_tokens -= static_cast<double>(count);
        if (m_tokens >= 0.0) {
            return;
        }
        // The debt is left on the account rather than cleared here: the sleep
        // pays for it in elapsed time, which the next refill picks up.
        auto debt = std::chrono::duration<double>(-m_tokens / m_bytes_per_second);
        std::this_thread::sleep_for(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(debt));
    }

private:
    double m_bytes_per_second{0.0};
    double m_burst{0.0};
    double m_tokens{0.0};
    std::chrono::steady_clock::time_point m_refilled_at{};
};

// The encodings the server can produce, in RFB's numbering.
inline constexpr uint32_t encoding_raw = 0;
inline constexpr uint32_t encoding_zrle = 16;
inline constexpr uint32_t encoding_h264 = 50;

// Quality level pseudo-encodings: -32 is level 0 and -23 is level 9, sent as
// two's complement in the 32 bit encoding field.
inline constexpr uint32_t encoding_quality_min =
    static_cast<uint32_t>(-32);
inline constexpr uint32_t encoding_quality_max =
    static_cast<uint32_t>(-23);

// Thrown when the viewer goes away, which is an ordinary end of session
// rather than a failure.
struct client_gone : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One connected viewer. Runs on its own thread: reads client messages and
// pushes framebuffer updates, both driven by a poll with the frame interval
// as its timeout.
class session {
public:
    session(socket_helper::unique_socket socket, std::string peer,
            screen_source& screen, const server_options& options)
        : m_socket{std::move(socket)},
          m_peer{std::move(peer)},
          m_screen{screen},
          m_options{options},
          m_input{screen.origin_x(), screen.origin_y()} {
        socket_helper::set_no_delay(m_socket.get());
        m_limit = rate_limiter{m_options.bandwidth_kbit};
        m_base_qp = std::clamp(m_options.h264_quality, 0, 51);
        m_quality_qp = m_base_qp;
        m_h264.set_quality(m_quality_qp);
    }

    void run() {
        try {
            handshake();
            serve();
        }
        catch (client_gone& e) {
            std::cout << std::format("[{}] disconnected: {}\n", m_peer, e.what());
        }
        catch (std::exception& e) {
            std::cerr << std::format("[{}] session error: {}\n", m_peer, e.what());
        }
        if (!m_options.view_only) {
            m_input.release_all();
        }
    }

private:
    // -- transport ---------------------------------------------------------

    void read_exact(void* buffer, size_t count) {
        auto* out = static_cast<uint8_t*>(buffer);
        size_t done = 0;
        while (done < count) {
            auto length = socket_helper::read_socket(m_socket.get(), out + done,
                                                     count - done);
            if (length == 0) {
                throw client_gone{"client closed the connection"};
            }
            if (length < 0) {
                throw client_gone{
                    std::format("read fail: {}", socket_helper::last_error())};
            }
            done += static_cast<size_t>(length);
        }
    }

    void write_exact(const void* buffer, size_t count) {
        const auto* in = static_cast<const uint8_t*>(buffer);
        size_t done = 0;
        while (done < count) {
            size_t want = count - done;
            if (m_limit.enabled()) {
                want = std::min(want, m_limit.chunk());
            }
            auto length = socket_helper::write_socket(m_socket.get(), in + done,
                                                      want);
            if (length <= 0) {
                throw client_gone{
                    std::format("write fail: {}", socket_helper::last_error())};
            }
            done += static_cast<size_t>(length);
            // Paid for after the fact, on what actually went out: a partial
            // write must not be charged for bytes still queued here.
            m_limit.consume(static_cast<size_t>(length));
        }
    }

    void write_bytes(const std::vector<uint8_t>& buffer) {
        write_exact(buffer.data(), buffer.size());
    }

    static void push_u16(std::vector<uint8_t>& out, uint16_t value) {
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value));
    }
    static void push_u32(std::vector<uint8_t>& out, uint32_t value) {
        out.push_back(static_cast<uint8_t>(value >> 24));
        out.push_back(static_cast<uint8_t>(value >> 16));
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value));
    }
    static uint16_t read_u16(const uint8_t* data) {
        return static_cast<uint16_t>((data[0] << 8) | data[1]);
    }
    static uint32_t read_u32(const uint8_t* data) {
        return (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) |
               static_cast<uint32_t>(data[3]);
    }

    // -- handshake ---------------------------------------------------------

    void handshake() {
        write_exact("RFB 003.008\n", 12);

        std::array<char, 12> version{};
        read_exact(version.data(), version.size());
        int minor = 8;
        if (version[0] == 'R' && version[1] == 'F' && version[2] == 'B' &&
            version[7] == '.') {
            minor = (version[8] - '0') * 100 + (version[9] - '0') * 10 +
                    (version[10] - '0');
        }
        // Anything newer than 3.8 is required by the spec to be treated as
        // 3.8, and 3.4/3.5/3.6 never existed as anything but 3.3.
        if (minor >= 8) m_minor = 8;
        else if (minor == 7) m_minor = 7;
        else m_minor = 3;
        std::cout << std::format("[{}] client protocol 3.{}\n", m_peer, m_minor);

        bool needs_password = !m_options.password.empty();
        if (m_minor == 3) {
            // 3.3 has no negotiation: the server dictates the security type.
            std::vector<uint8_t> chosen{};
            push_u32(chosen, needs_password ? 2u : 1u);
            write_bytes(chosen);
            // With None, 3.3 skips SecurityResult entirely and goes
            // straight to ClientInit.
            if (needs_password) {
                authenticate();
                send_security_result(true);
            }
        }
        else {
            std::vector<uint8_t> types{1, static_cast<uint8_t>(needs_password ? 2 : 1)};
            write_bytes(types);
            uint8_t chosen{};
            read_exact(&chosen, 1);
            uint8_t expected = needs_password ? 2 : 1;
            if (chosen != expected) {
                send_failure(std::format("unsupported security type {}", chosen));
                throw client_gone{"client picked an unsupported security type"};
            }
            if (needs_password) {
                authenticate();
            }
            send_security_result(true);
        }

        uint8_t shared_flag{};
        read_exact(&shared_flag, 1);
        send_server_init();
    }

    void send_security_result(bool ok) {
        std::vector<uint8_t> result{};
        push_u32(result, ok ? 0u : 1u);
        write_bytes(result);
    }

    void send_failure(const std::string& reason) {
        try {
            if (m_minor >= 7) {
                send_security_result(false);
                std::vector<uint8_t> message{};
                push_u32(message, static_cast<uint32_t>(reason.size()));
                write_bytes(message);
                write_exact(reason.data(), reason.size());
            }
        }
        catch (std::exception&) {
            // The peer is already gone; the original error is what matters.
        }
    }

    // VNC Authentication: DES-encrypt a random 16 byte challenge with the
    // password as key. The key bytes are bit reversed, a quirk of the
    // original implementation that the protocol froze in place.
    void authenticate() {
        std::array<uint8_t, 16> challenge{};
        if (1 != RAND_bytes(challenge.data(), static_cast<int>(challenge.size()))) {
            throw std::runtime_error{"challenge generation fail"};
        }
        write_exact(challenge.data(), challenge.size());

        std::array<uint8_t, 16> response{};
        read_exact(response.data(), response.size());

        std::array<uint8_t, 8> key{};
        for (size_t i = 0; i < key.size(); i++) {
            uint8_t c = i < m_options.password.size()
                            ? static_cast<uint8_t>(m_options.password[i])
                            : 0;
            uint8_t reversed = 0;
            for (int bit = 0; bit < 8; bit++) {
                reversed |= static_cast<uint8_t>(((c >> bit) & 1) << (7 - bit));
            }
            key[i] = reversed;
        }
        DES_key_schedule schedule{};
        DES_set_key_unchecked(reinterpret_cast<const_DES_cblock*>(key.data()), &schedule);

        std::array<uint8_t, 16> expected{};
        for (int block = 0; block < 2; block++) {
            DES_ecb_encrypt(
                reinterpret_cast<const_DES_cblock*>(challenge.data() + block * 8),
                reinterpret_cast<DES_cblock*>(expected.data() + block * 8),
                &schedule, DES_ENCRYPT);
        }
        if (expected != response) {
            std::cerr << std::format("[{}] authentication failed\n", m_peer);
            send_failure("authentication failed");
            throw client_gone{"bad password"};
        }
    }

    void send_server_init() {
        auto format = native_pixel_format();
        std::vector<uint8_t> message{};
        push_u16(message, static_cast<uint16_t>(m_screen.width()));
        push_u16(message, static_cast<uint16_t>(m_screen.height()));
        message.push_back(format.bits_per_pixel);
        message.push_back(format.depth);
        message.push_back(format.big_endian_flag);
        message.push_back(format.true_colour_flag);
        push_u16(message, format.red_max);
        push_u16(message, format.green_max);
        push_u16(message, format.blue_max);
        message.push_back(format.red_shift);
        message.push_back(format.green_shift);
        message.push_back(format.blue_shift);
        message.insert(message.end(), 3, 0); // padding
        push_u32(message, static_cast<uint32_t>(m_options.desktop_name.size()));
        message.insert(message.end(), m_options.desktop_name.begin(),
                       m_options.desktop_name.end());
        write_bytes(message);
    }

    // -- main loop ---------------------------------------------------------

    void serve() {
        std::cout << std::format("[{}] session started ({}x{})\n", m_peer,
                                 m_screen.width(), m_screen.height());
        int frame_interval_ms = std::max(1, 1000 / std::max(1, m_options.max_fps));
        while (true) {
            pollfd fd{m_socket.get(), POLLIN, 0};
            int ret = socket_helper::poll_sockets(&fd, 1, poll_timeout(frame_interval_ms));
            if (ret < 0) {
#ifndef _WIN32
                if (socket_helper::last_error() == EINTR) continue;
#endif
                throw client_gone{
                    std::format("poll fail: {}", socket_helper::last_error())};
            }
            if (ret > 0) {
                if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    throw client_gone{"connection dropped"};
                }
                // Drain what has already arrived before spending time
                // encoding, but cap the burst: a viewer streaming pointer
                // motion must not starve the framebuffer updates.
                for (int i = 0; i < 64 && (fd.revents & POLLIN); i++) {
                    handle_client_message();
                    fd.revents = 0;
                    if (socket_helper::poll_sockets(&fd, 1, 0) <= 0) {
                        break;
                    }
                }
            }
            send_pending_update();
            send_pending_clipboard();
        }
    }

    // With no update owed, sleeping a whole frame interval costs nothing: the
    // client is not waiting for anything. With one owed, that same sleep is
    // pure added latency, because the update cannot go out until the screen is
    // read again and the loop would keep sleeping past that moment. Waking
    // exactly when the next capture falls due removes, on average, half a
    // frame interval from every update -- 25ms at the default 20fps, far more
    // than the whole encode costs.
    int poll_timeout(int frame_interval_ms) const {
        if (!m_update_pending) {
            return frame_interval_ms;
        }
        auto wait = m_screen.time_to_next_capture();
        // Rounded up, so a sub-millisecond wait sleeps 1ms rather than
        // spinning the loop until the clock catches up.
        auto wait_ms = std::chrono::duration_cast<
            std::chrono::duration<int64_t, std::milli>>(
            wait + std::chrono::microseconds{999}).count();
        return static_cast<int>(
            std::clamp<int64_t>(wait_ms, 0, frame_interval_ms));
    }

    void handle_client_message() {
        uint8_t type{};
        read_exact(&type, 1);
        switch (type) {
        case 0: set_pixel_format(); break;
        case 2: set_encodings(); break;
        case 3: framebuffer_update_request(); break;
        case 4: key_event(); break;
        case 5: pointer_event(); break;
        case 6: client_cut_text(); break;
        default:
            // Message lengths are not self describing, so an unknown type
            // makes the rest of the stream unparseable.
            throw client_gone{
                std::format("unsupported client message type {}", type)};
        }
    }

    void set_pixel_format() {
        std::array<uint8_t, 19> message{};
        read_exact(message.data(), message.size());
        pixel_format format{};
        format.bits_per_pixel = message[3];
        format.depth = message[4];
        format.big_endian_flag = message[5];
        format.true_colour_flag = message[6];
        format.red_max = read_u16(&message[7]);
        format.green_max = read_u16(&message[9]);
        format.blue_max = read_u16(&message[11]);
        format.red_shift = message[13];
        format.green_shift = message[14];
        format.blue_shift = message[15];
        m_writer.set_format(format);
        // The client's idea of the screen is gone, so the next update has to
        // be a full one whatever it asks for.
        m_previous.pixels.clear();
        std::cout << std::format("[{}] pixel format: {} bpp, depth {}\n", m_peer,
                                 static_cast<int>(format.bits_per_pixel),
                                 static_cast<int>(format.depth));
    }

    void set_encodings() {
        std::array<uint8_t, 3> header{};
        read_exact(header.data(), header.size());
        uint16_t count = read_u16(&header[1]);
        std::vector<uint8_t> body(static_cast<size_t>(count) * 4);
        if (!body.empty()) {
            read_exact(body.data(), body.size());
        }
        // The order the client sends is a priority hint, so the first entry
        // the server can produce wins. Raw needs no announcing, but a client
        // that lists it explicitly gets to rank it against the others.
        // Pseudo-encodings carry no priority and may appear anywhere in the
        // list, so the whole list is always scanned.
        auto chosen = encoding_raw;
        bool picked = false;
        int quality_level = -1;
        for (uint16_t i = 0; i < count; i++) {
            auto encoding = read_u32(&body[static_cast<size_t>(i) * 4]);
            if (encoding >= encoding_quality_min &&
                encoding <= encoding_quality_max) {
                // -32 is level 0 (worst) and -23 is level 9 (best).
                quality_level =
                    static_cast<int>(encoding - encoding_quality_min);
                continue;
            }
            if (picked) {
                continue;
            }
            if (encoding == encoding_h264 && h264_encoder_available) {
                chosen = encoding_h264;
                picked = true;
            }
            else if (encoding == encoding_zrle) {
                chosen = encoding_zrle;
                picked = true;
            }
            else if (encoding == encoding_raw) {
                picked = true;
            }
        }
        if (quality_level >= 0) {
            // An explicit request wins outright: the client is stating what it
            // wants to look at, so the server stops second-guessing it.
            m_client_set_quality = true;
            m_base_qp = quality_to_qp(quality_level);
            m_quality_qp = m_base_qp;
            m_h264.set_quality(m_quality_qp);
        }
        set_encoding(chosen);
        std::cout << std::format("[{}] encodings: {} offered, using {}\n", m_peer,
                                 count, encoding_name(m_encoding));
        if (quality_level >= 0) {
            std::cout << std::format("[{}] quality level {} (qp {})\n", m_peer,
                                     quality_level,
                                     quality_to_qp(quality_level));
        }
        else if (adapting_quality()) {
            std::cout << std::format(
                "[{}] no quality level asked for, adapting between qp {} and "
                "{}\n",
                m_peer, m_base_qp, adaptive_worst_qp);
        }
    }

    // The ten quality levels are spread over the useful half of the QP scale:
    // level 9 is visually lossless for screen content, level 0 is as coarse as
    // is worth sending at all.
    static int quality_to_qp(int level) {
        constexpr int best_qp = 12;
        constexpr int worst_qp = 39;
        level = std::clamp(level, 0, 9);
        return worst_qp - (worst_qp - best_qp) * level / 9;
    }

    // As coarse as the adaptive controller is allowed to get. Past this the
    // picture is too blocky to read, and a link that still cannot keep up is
    // beyond what picture quality alone can fix.
    static constexpr int adaptive_worst_qp = 45;

    bool adapting_quality() const {
        return m_options.adapt_quality && !m_client_set_quality &&
               h264_encoder_available;
    }

    // A client that never asked for a quality level has left the trade-off to
    // the server, so the server spends it on latency. The signal is how long
    // the update took to write: with a blocking socket that is the time the
    // link needed to swallow the payload, and anything beyond one capture
    // interval means updates are queueing and the viewer is falling behind.
    //
    // Deliberately not the encode time, which is also part of the latency but
    // barely moves with the QP -- degrading the picture to fix a slow CPU
    // would cost quality and buy nothing.
    //
    // H264 bitrate is roughly geometric in the QP, near enough half the bits
    // per six steps, so the correction is computed in that unit: being 4x over
    // budget asks for +12, capped to keep one bad update from blanking the
    // picture. Recovery is one step at a time and on a longer timer, because
    // overshooting downwards immediately re-floods the link.
    void adapt_quality(std::chrono::steady_clock::duration write_time) {
        if (!adapting_quality()) {
            return;
        }
        double seconds = std::chrono::duration<double>(write_time).count();
        double budget = 1.0 / std::max(1, m_options.max_fps);
        int target = m_quality_qp;
        if (seconds > budget) {
            m_updates_in_budget = 0;
            int steps = static_cast<int>(
                std::lround(6.0 * std::log2(seconds / budget)));
            target = m_quality_qp + std::clamp(steps, 1, 12);
        }
        else if (seconds * 4.0 < budget) {
            // Easing off needs a run of comfortable updates, not one: a
            // single quick update is as likely to mean the screen barely
            // changed as it is to mean the link has room, and acting on it
            // produces a permanent one-step oscillation around the true
            // setting.
            if (++m_updates_in_budget < 4) {
                return;
            }
            m_updates_in_budget = 0;
            target = m_quality_qp - 1;
        }
        target = std::clamp(target, m_base_qp, adaptive_worst_qp);
        if (target == m_quality_qp) {
            return;
        }
        // Reopening the encoder costs an IDR, which is itself a burst of
        // bytes on a link that is already struggling, so changes are rationed.
        // Recovery waits four times as long as degradation: latency is the
        // thing being defended, and quality can afford to come back slowly.
        auto now = std::chrono::steady_clock::now();
        auto cooldown = target > m_quality_qp ? std::chrono::milliseconds{250}
                                              : std::chrono::milliseconds{1000};
        if (m_quality_changed_at != std::chrono::steady_clock::time_point{} &&
            now - m_quality_changed_at < cooldown) {
            return;
        }
        std::cout << std::format(
            "[{}] adaptive quality: qp {} -> {} (update took {:.0f} ms, "
            "budget {:.0f} ms)\n",
            m_peer, m_quality_qp, target, seconds * 1000.0, budget * 1000.0);
        m_quality_qp = target;
        m_quality_changed_at = now;
        m_h264.set_quality(m_quality_qp);
    }

    static const char* encoding_name(uint32_t encoding) {
        switch (encoding) {
        case encoding_zrle: return "ZRLE";
        case encoding_h264: return "H264";
        default: return "Raw";
        }
    }

    void set_encoding(uint32_t encoding) {
        if (encoding == m_encoding) {
            return;
        }
        m_encoding = encoding;
        // Switching encoders invalidates whatever the client has: ZRLE and
        // H264 both carry state the other one never fed.
        m_previous.pixels.clear();
        m_force_keyframe = true;
    }

    void framebuffer_update_request() {
        std::array<uint8_t, 9> message{};
        read_exact(message.data(), message.size());
        bool incremental = message[0] != 0;
        rectangle request{
            read_u16(&message[1]), read_u16(&message[3]),
            read_u16(&message[5]), read_u16(&message[7])};

        request = clip(request);
        if (m_update_pending) {
            // Merge with the outstanding request rather than dropping it.
            m_requested = merge(m_requested, request);
            m_incremental = m_incremental && incremental;
        }
        else {
            m_requested = request;
            m_incremental = incremental;
            m_update_pending = true;
        }
    }

    void key_event() {
        std::array<uint8_t, 7> message{};
        read_exact(message.data(), message.size());
        if (m_options.view_only) return;
        m_input.key_event(read_u32(&message[3]), message[0] != 0);
    }

    void pointer_event() {
        std::array<uint8_t, 5> message{};
        read_exact(message.data(), message.size());
        if (m_options.view_only) return;
        m_input.pointer_event(message[0], read_u16(&message[1]),
                              read_u16(&message[3]));
    }

    void client_cut_text() {
        std::array<uint8_t, 7> header{};
        read_exact(header.data(), header.size());
        uint32_t length = read_u32(&header[3]);
        // Extended clipboard negotiation encodes a huge or negative length;
        // it is optional, so refuse politely rather than allocating.
        if (length > (1u << 20)) {
            throw client_gone{"client cut text too large"};
        }
        std::string text(length, '\0');
        if (length != 0) {
            read_exact(text.data(), length);
        }
        if (m_options.view_only) return;
        m_clipboard.set_text(text);
    }

    // -- updates -----------------------------------------------------------

    rectangle clip(rectangle rect) const {
        int right = std::min(rect.x + rect.width, m_screen.width());
        int bottom = std::min(rect.y + rect.height, m_screen.height());
        rect.x = std::clamp(rect.x, 0, m_screen.width());
        rect.y = std::clamp(rect.y, 0, m_screen.height());
        rect.width = std::max(0, right - rect.x);
        rect.height = std::max(0, bottom - rect.y);
        return rect;
    }

    static rectangle merge(rectangle a, rectangle b) {
        if (a.width == 0 || a.height == 0) return b;
        if (b.width == 0 || b.height == 0) return a;
        int x = std::min(a.x, b.x);
        int y = std::min(a.y, b.y);
        int right = std::max(a.x + a.width, b.x + b.width);
        int bottom = std::max(a.y + a.height, b.y + b.height);
        return rectangle{x, y, right - x, bottom - y};
    }

    void send_pending_update() {
        if (!m_update_pending) {
            return;
        }
        uint64_t generation = 0;
        auto frame = m_screen.frame(generation);
        if (frame == nullptr || frame->width == 0) {
            return;
        }
        bool full = !m_incremental || m_previous.pixels.empty() ||
                    m_previous.width != frame->width ||
                    m_previous.height != frame->height;
        if (!full && generation == m_sent_generation) {
            return; // Nothing new was captured, keep the request pending.
        }
        if (m_encoding == encoding_h264) {
            try {
                send_h264_update(*frame, generation, full);
                return;
            }
            catch (const std::exception& e) {
                // Whether an H264 encoder can be opened is only known once
                // there is a frame to encode, which is long after encodings
                // were negotiated. Rectangles carry their own encoding, so
                // the session can simply continue in ZRLE rather than
                // dropping a viewer that asked for something this machine
                // turns out not to have.
                std::cerr << std::format("[{}] h264 unavailable ({}),"
                                         " falling back to ZRLE\n",
                                         m_peer, e.what());
                set_encoding(encoding_zrle);
                full = true;
            }
        }

        std::vector<rectangle> rects{};
        if (full) {
            rects.push_back(m_requested);
        }
        else {
            collect_dirty(*frame, m_requested, rects);
            if (rects.empty()) {
                m_sent_generation = generation;
                return;
            }
        }

        std::vector<uint8_t> message{};
        message.push_back(0); // FramebufferUpdate
        message.push_back(0); // padding
        push_u16(message, static_cast<uint16_t>(rects.size()));
        for (const auto& rect : rects) {
            push_u16(message, static_cast<uint16_t>(rect.x));
            push_u16(message, static_cast<uint16_t>(rect.y));
            push_u16(message, static_cast<uint16_t>(rect.width));
            push_u16(message, static_cast<uint16_t>(rect.height));
            if (m_encoding == encoding_zrle) {
                push_u32(message, encoding_zrle);
                m_zrle.append_rect(message, m_writer, *frame, rect.x, rect.y,
                                   rect.width, rect.height);
            }
            else {
                push_u32(message, encoding_raw);
                m_writer.append_raw(message, *frame, rect.x, rect.y, rect.width,
                                    rect.height);
            }
        }
        write_bytes(message);

        remember(*frame);
        m_sent_generation = generation;
        m_update_pending = false;
    }

    // H264 cannot use the 64x64 dirty rectangles: the encoder needs a stream
    // of one constant size, so every update is one full screen rectangle and
    // dirty detection only decides whether a frame is worth encoding at all.
    void send_h264_update(const frame_buffer& frame, uint64_t generation,
                          bool full) {
        if (!full && !frame_changed(frame)) {
            m_sent_generation = generation;
            return;
        }

        std::vector<uint8_t> payload{};
        uint32_t flags = 0;
        bool up_to_date = false;
        // A client that has just switched encoding, or whose screen changed
        // size, has nothing to predict from, so start with an IDR.
        if (!m_h264.encode(frame, m_force_keyframe || full, payload, flags,
                           up_to_date)) {
            return; // Nothing came out; keep the request pending.
        }
        // Only an IDR that actually reached the wire retires the request for
        // one: claiming otherwise would leave a client decoding nothing.
        if ((flags & h264_flag_keyframe) != 0) {
            m_force_keyframe = false;
        }
        if (m_h264_reported != m_h264.encoder_name()) {
            m_h264_reported = m_h264.encoder_name();
            std::cout << std::format("[{}] h264 encoder: {}\n", m_peer,
                                     m_h264_reported);
        }

        std::vector<uint8_t> message{};
        message.push_back(0); // FramebufferUpdate
        message.push_back(0); // padding
        push_u16(message, 1);
        push_u16(message, 0);
        push_u16(message, 0);
        push_u16(message, static_cast<uint16_t>(frame.width));
        push_u16(message, static_cast<uint16_t>(frame.height));
        push_u32(message, encoding_h264);
        push_u32(message, static_cast<uint32_t>(payload.size()));
        push_u32(message, flags);
        message.insert(message.end(), payload.begin(), payload.end());
        auto write_started = std::chrono::steady_clock::now();
        write_bytes(message);
        adapt_quality(std::chrono::steady_clock::now() - write_started);

        if (!up_to_date) {
            // The encoder answered with an older picture, so this frame is
            // still owed to the client: leave the request pending and do not
            // pretend the client has seen these pixels.
            return;
        }
        remember(frame);
        m_sent_generation = generation;
        m_update_pending = false;
    }

    // Compares against the copy the client already has, in the same 64x64
    // grid ZRLE uses, then merges changed tiles into rows of rectangles.
    void collect_dirty(const frame_buffer& frame, const rectangle& region,
                       std::vector<rectangle>& rects) const {
        constexpr int tile = 64;
        int region_right = region.x + region.width;
        int region_bottom = region.y + region.height;
        // Tiles are cut on the global 64 pixel grid, so a partial request
        // still produces bands that line up between updates.
        for (int y = region.y; y < region_bottom;) {
            int height = std::min(tile - y % tile, region_bottom - y);
            int run_start = -1;
            for (int x = region.x; x < region_right;) {
                int width = std::min(tile - x % tile, region_right - x);
                if (tile_changed(frame, x, y, width, height)) {
                    if (run_start < 0) run_start = x;
                }
                else if (run_start >= 0) {
                    rects.push_back(rectangle{run_start, y, x - run_start, height});
                    run_start = -1;
                }
                x += width;
            }
            if (run_start >= 0) {
                rects.push_back(rectangle{run_start, y, region_right - run_start,
                                          height});
            }
            y += height;
        }
        merge_rows(rects);
    }

    // Rectangles directly above one another with the same span are one
    // rectangle, which matters for full-screen changes: 1920x1080 turns into
    // 30 rows of one rect each instead of 510 tiles.
    static void merge_rows(std::vector<rectangle>& rects) {
        std::vector<rectangle> merged{};
        for (const auto& rect : rects) {
            if (!merged.empty()) {
                auto& last = merged.back();
                if (last.x == rect.x && last.width == rect.width &&
                    last.y + last.height == rect.y) {
                    last.height += rect.height;
                    continue;
                }
            }
            merged.push_back(rect);
        }
        rects.swap(merged);
    }

    bool tile_changed(const frame_buffer& frame, int x, int y, int width,
                      int height) const {
        auto row_bytes = static_cast<size_t>(width) * 4;
        for (int row = 0; row < height; row++) {
            auto offset = static_cast<size_t>(y + row) * frame.stride() +
                          static_cast<size_t>(x) * 4;
            if (0 != std::memcmp(frame.pixels.data() + offset,
                                 m_previous.pixels.data() + offset, row_bytes)) {
                return true;
            }
        }
        return false;
    }

    // H264 only needs to know whether anything moved, not where, so this stops
    // at the first difference instead of walking the whole screen and building
    // a rectangle list that send_h264_update would throw away.
    bool frame_changed(const frame_buffer& frame) const {
        if (m_previous.width != frame.width ||
            m_previous.height != frame.height ||
            m_previous.pixels.size() != frame.pixels.size()) {
            return true;
        }
        return 0 != std::memcmp(frame.pixels.data(), m_previous.pixels.data(),
                                frame.pixels.size());
    }

    void remember(const frame_buffer& frame) {
        if (m_previous.width != frame.width || m_previous.height != frame.height) {
            m_previous.resize(frame.width, frame.height);
        }
        m_previous.pixels = frame.pixels;
    }

    void send_pending_clipboard() {
        std::string text{};
        if (!m_clipboard.poll_text(text) || text.empty()) {
            return;
        }
        std::vector<uint8_t> message{};
        message.push_back(3); // ServerCutText
        message.insert(message.end(), 3, 0);
        push_u32(message, static_cast<uint32_t>(text.size()));
        message.insert(message.end(), text.begin(), text.end());
        write_bytes(message);
    }

    socket_helper::unique_socket m_socket;
    std::string m_peer;
    screen_source& m_screen;
    server_options m_options;
    input_injector m_input;
    clipboard m_clipboard{};
    pixel_writer m_writer{};
    zrle_encoder m_zrle{};
    h264_encoder m_h264{};
    std::string m_h264_reported{};
    frame_buffer m_previous{};
    rectangle m_requested{};
    bool m_update_pending{false};
    bool m_incremental{false};
    uint32_t m_encoding{encoding_raw};
    bool m_force_keyframe{true};
    uint64_t m_sent_generation{0};
    int m_minor{8};
    rate_limiter m_limit{};
    // The QP the client asked for, or the one the command line configured:
    // the adaptive controller never produces a better picture than this, only
    // a cheaper one.
    int m_base_qp{18};
    int m_quality_qp{18};
    bool m_client_set_quality{false};
    int m_updates_in_budget{0};
    std::chrono::steady_clock::time_point m_quality_changed_at{};
};

// Accepts connections and runs one session per client on its own thread.
class rfb_server {
public:
    explicit rfb_server(const server_options& options)
        : m_options{options},
          m_screen{options.capture, options.max_fps} {}

    int width() const { return m_screen.width(); }
    int height() const { return m_screen.height(); }

    void run() {
        warn_if_not_interactive();
        auto listener = socket_helper::listen_on_port(
            m_options.port,
            m_options.listen_address.empty() ? nullptr
                                             : m_options.listen_address.c_str());
        std::cout << std::format(
            "serving {}x{} on {}:{} ({}, {})\n", m_screen.width(),
            m_screen.height(),
            m_options.listen_address.empty() ? "*" : m_options.listen_address,
            m_options.port,
            m_options.password.empty() ? "no authentication" : "VNC authentication",
            m_options.view_only ? "view only" : "input enabled");
        if (m_options.bandwidth_kbit > 0) {
            std::cout << std::format("each client is capped at {} kbit/s\n",
                                     m_options.bandwidth_kbit);
        }

        while (true) {
            std::string peer{};
            auto client = socket_helper::accept_connection(listener.get(), &peer);
            std::cout << std::format("[{}] connected\n", peer);
            // Detached: a session ends on its own when the viewer leaves,
            // and the accept loop must never block waiting for one.
            std::thread{[this, socket = std::move(client), peer]() mutable {
                session current{std::move(socket), peer, m_screen, m_options};
                current.run();
            }}.detach();
        }
    }

private:
    // A process started over SSH, or by a service, lives in session 0, whose
    // desktop is not the one the user sees. Capturing there yields either
    // nothing or a blank screen, and injected input goes nowhere, so say so
    // rather than let the viewer stare at a black rectangle.
    static void warn_if_not_interactive() {
#ifdef _WIN32
        DWORD session_id = 0;
        if (::ProcessIdToSessionId(::GetCurrentProcessId(), &session_id) &&
            session_id == 0) {
            std::cerr <<
                "warning: running in session 0, which has no visible desktop. "
                "Start the server inside the interactive session instead, for "
                "example with \"psexec -i 1\" or a scheduled task created "
                "with /it.\n";
        }
#endif
    }

    server_options m_options;
    screen_source m_screen;
};

} // namespace rfb::server
