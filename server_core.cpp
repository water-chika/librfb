#include "server_core.hpp"

#include <stdexcept>

namespace rfb::server {
namespace {

using remote_media::KeyCode;

std::optional<KeyCode> letter_key(std::uint32_t keysym) noexcept {
    if (keysym >= 'A' && keysym <= 'Z') {
        keysym += 'a' - 'A';
    }
    if (keysym >= 'a' && keysym <= 'z') {
        return static_cast<KeyCode>(static_cast<std::uint16_t>(KeyCode::a) +
                                    keysym - 'a');
    }
    return std::nullopt;
}

} // namespace

std::optional<KeyCode> keysym_to_key_code(std::uint32_t keysym) noexcept {
    if (auto letter = letter_key(keysym)) {
        return letter;
    }
    if (keysym >= '1' && keysym <= '9') {
        return static_cast<KeyCode>(static_cast<std::uint16_t>(KeyCode::digit1) +
                                    keysym - '1');
    }
    if (keysym == '0') return KeyCode::digit0;
    if (keysym >= 0xffbe && keysym <= 0xffc9) {
        return static_cast<KeyCode>(static_cast<std::uint16_t>(KeyCode::f1) +
                                    keysym - 0xffbe);
    }

    switch (keysym) {
    case 0xff08: return KeyCode::backspace;
    case 0xff09: return KeyCode::tab;
    case 0xff0d: return KeyCode::enter;
    case 0xff13: return KeyCode::pause;
    case 0xff14: return KeyCode::scroll_lock;
    case 0xff1b: return KeyCode::escape;
    case 0xff50: return KeyCode::home;
    case 0xff51: return KeyCode::left;
    case 0xff52: return KeyCode::up;
    case 0xff53: return KeyCode::right;
    case 0xff54: return KeyCode::down;
    case 0xff55: return KeyCode::page_up;
    case 0xff56: return KeyCode::page_down;
    case 0xff57: return KeyCode::end;
    case 0xff61: return KeyCode::print_screen;
    case 0xff63: return KeyCode::insert;
    case 0xffff: return KeyCode::delete_forward;
    case 0xff7f: return KeyCode::num_lock;
    case 0xffe1: return KeyCode::left_shift;
    case 0xffe2: return KeyCode::right_shift;
    case 0xffe3: return KeyCode::left_control;
    case 0xffe4: return KeyCode::right_control;
    case 0xffe7:
    case 0xffeb: return KeyCode::left_meta;
    case 0xffe8:
    case 0xffec: return KeyCode::right_meta;
    case 0xffe9: return KeyCode::left_alt;
    case 0xffea: return KeyCode::right_alt;
    case ' ': return KeyCode::space;
    case '-': return KeyCode::minus;
    case '=': return KeyCode::equal;
    case '[': return KeyCode::left_bracket;
    case ']': return KeyCode::right_bracket;
    case '\\': return KeyCode::backslash;
    case ';': return KeyCode::semicolon;
    case '\'': return KeyCode::apostrophe;
    case '`': return KeyCode::grave;
    case ',': return KeyCode::comma;
    case '.': return KeyCode::period;
    case '/': return KeyCode::slash;
    default: return std::nullopt;
    }
}

input_forwarder::input_forwarder(std::shared_ptr<remote_media::InputSink> sink)
    : sink_{std::move(sink)} {
    if (!sink_) {
        throw std::invalid_argument{"input sink must not be null"};
    }
}

void input_forwarder::key_event(std::uint32_t keysym, bool down) {
    auto code = keysym_to_key_code(keysym);
    if (!code) return;

    if (down) {
        auto [_, inserted] = held_keys_.insert(*code);
        sink_->submit(remote_media::KeyInput{
            *code, inserted ? remote_media::KeyAction::pressed
                            : remote_media::KeyAction::repeated});
    } else {
        held_keys_.erase(*code);
        sink_->submit(remote_media::KeyInput{*code,
                                             remote_media::KeyAction::released});
    }
}

void input_forwarder::submit_button(std::uint8_t bit,
                                    remote_media::PointerButton button,
                                    bool down) {
    const bool was_down = (button_mask_ & bit) != 0;
    if (was_down == down) return;
    sink_->submit(remote_media::PointerButtonInput{
        button, down ? remote_media::ButtonAction::pressed
                     : remote_media::ButtonAction::released});
}

void input_forwarder::pointer_event(std::uint8_t button_mask, int x, int y) {
    sink_->submit(remote_media::PointerMoveInput{x, y});
    submit_button(0x01, remote_media::PointerButton::primary,
                  (button_mask & 0x01) != 0);
    submit_button(0x02, remote_media::PointerButton::middle,
                  (button_mask & 0x02) != 0);
    submit_button(0x04, remote_media::PointerButton::secondary,
                  (button_mask & 0x04) != 0);

    const auto pressed = static_cast<std::uint8_t>(button_mask & ~button_mask_);
    if (pressed & 0x08) sink_->submit(remote_media::ScrollInput{0, 1});
    if (pressed & 0x10) sink_->submit(remote_media::ScrollInput{0, -1});
    if (pressed & 0x20) sink_->submit(remote_media::ScrollInput{-1, 0});
    if (pressed & 0x40) sink_->submit(remote_media::ScrollInput{1, 0});
    button_mask_ = button_mask;
}

void input_forwarder::release_all() {
    for (auto code : held_keys_) {
        sink_->submit(remote_media::KeyInput{code,
                                             remote_media::KeyAction::released});
    }
    held_keys_.clear();
    submit_button(0x01, remote_media::PointerButton::primary, false);
    submit_button(0x02, remote_media::PointerButton::middle, false);
    submit_button(0x04, remote_media::PointerButton::secondary, false);
    button_mask_ = 0;
}

} // namespace rfb::server
