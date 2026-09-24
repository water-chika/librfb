#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>

#include <remote_media/frame.hpp>
#include <remote_media/input.hpp>

namespace rfb::server {

// Convert the X11 keysym carried by RFB into remote-media's stable USB-HID
// vocabulary. Character keysyms are matched independently of case; keysyms
// with no HID equivalent are ignored.
[[nodiscard]] std::optional<remote_media::KeyCode>
keysym_to_key_code(std::uint32_t keysym) noexcept;

// Stateful translation of RFB keyboard and pointer messages into the
// transport-neutral input contract. It emits button edges (RFB sends a mask),
// wheel clicks, and releases held keys/buttons when a session ends.
class input_forwarder {
public:
    explicit input_forwarder(std::shared_ptr<remote_media::InputSink> sink);

    void key_event(std::uint32_t keysym, bool down);
    void pointer_event(std::uint8_t button_mask, int x, int y);
    void release_all();

private:
    void submit_button(std::uint8_t bit, remote_media::PointerButton button,
                       bool down);

    std::shared_ptr<remote_media::InputSink> sink_;
    std::set<remote_media::KeyCode> held_keys_;
    std::uint8_t button_mask_{};
};

// Convenience no-op sink for view-only and tests.
class null_input_sink final : public remote_media::InputSink {
public:
    void submit(const remote_media::InputEvent&) override {}
};

} // namespace rfb::server
