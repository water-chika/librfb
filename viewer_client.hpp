#pragma once

#include "rfb.hpp"

#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rfb {

// Platform-neutral client stack used by native viewers. Platform front ends
// provide only windowing, presentation and input translation.
template<typename T>
class add_client_clipboard_sink : public T {
public:
    using parent = T;
    add_client_clipboard_sink(const configure auto& conf) : parent{conf} {}

    void process_server_cut_text(const auto& text) {
        m_server_cut_text.assign(text.begin(), text.end());
        m_has_server_cut_text = true;
    }

    bool take_server_cut_text(std::string& text) {
        if (!m_has_server_cut_text) {
            return false;
        }
        text = std::move(m_server_cut_text);
        m_server_cut_text.clear();
        m_has_server_cut_text = false;
        return true;
    }

private:
    std::string m_server_cut_text;
    bool m_has_server_cut_text{};
};

template<typename T>
using viewer_client_protocol =
    add_rfb<
    add_request_scale<
    add_process_framebuffer_update<
    add_decode_h264<
    add_yuv_to_rgb<
    add_zrle<
    init_rfb<
    add_server_cut_text<
    add_client_clipboard_sink<
    add_client_cut_text<
    add_set_encodings<
    set_supported_encodings_from_configure<
    add_set_format<
    add_server_init<
    add_client_init<
    add_connection<
    set_port<
    set_address<
    set_password_from_configure<
    T
    >>>>>>>>>>>>>>>>>>>;

template<typename T>
class viewer_client : public viewer_client_protocol<T> {
public:
    using parent = viewer_client_protocol<T>;
    viewer_client(const configure auto& conf)
        : parent{conf}, m_view_only{conf.view_only} {}

    void get_rfb(std::span<std::uint8_t> frame) { parent::get_frame(frame); }
    int get_fb_width() { return parent::get_width(); }
    int get_fb_height() { return parent::get_height(); }

    void send_key_event(int key, int state) {
        if (m_view_only) return;
        parent::key_event(key, state);
        if (state) m_pressed_keys.insert(key); else m_pressed_keys.erase(key);
    }
    void send_pointer_event(std::uint32_t mask, int x, int y) {
        if (m_view_only) return;
        send_pointer_event_unchecked(mask, x, y);
        m_pointer_mask = mask;
        m_pointer_x = x;
        m_pointer_y = y;
    }
    void send_pointer_event_unchecked(std::uint32_t mask, int x, int y) {
        parent::pointer_event(static_cast<std::uint8_t>(mask),
            static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
    }
    void send_cut_text(std::string_view text) {
        if (m_view_only) return;
        std::string copy{text};
        parent::client_cut_text(copy);
    }
    void request_framebuffer_update(int x, int y, int width, int height,
                                    bool incremental = true) {
        parent::framebuffer_update_request(
            static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y),
            static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height),
            incremental);
    }
    void request_framebuffer_update(int x = 0, int y = 0) {
        request_framebuffer_update(x, y, get_fb_width(), get_fb_height(), true);
    }

    bool get_view_only() const { return m_view_only; }
    void set_view_only(bool value) { m_view_only = value; }
    const std::set<int>& held_remote_keys() const { return m_pressed_keys; }
    auto held_remote_pointer() const {
        struct state { std::uint32_t mask; int x; int y; };
        return state{m_pointer_mask, m_pointer_x, m_pointer_y};
    }
    void release_remote_input() {
        for (auto key : m_pressed_keys) parent::key_event(key, 0);
        m_pressed_keys.clear();
        if (m_pointer_mask) send_pointer_event_unchecked(0, m_pointer_x, m_pointer_y);
        m_pointer_mask = 0;
    }

private:
    bool m_view_only{};
    std::set<int> m_pressed_keys;
    std::uint32_t m_pointer_mask{};
    int m_pointer_x{}, m_pointer_y{};
};

} // namespace rfb
