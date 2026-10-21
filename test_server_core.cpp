#include "server.hpp"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

class recording_sink final : public remote_media::InputSink {
public:
    void submit(const remote_media::InputEvent& event) override {
        events.push_back(event);
    }
    std::vector<remote_media::InputEvent> events;
};

} // namespace

int main() {
    using namespace rfb::server;

    auto mailbox = std::make_shared<remote_media::FrameMailbox>();
    auto frame = std::make_shared<remote_media::OwnedFrame>(
        2, 1, remote_media::PixelFormat::rgba8);
    auto pixels = frame->mutable_view();
    auto* row = reinterpret_cast<unsigned char*>(pixels.row(0));
    row[0] = 10; row[1] = 20; row[2] = 30; row[3] = 255;
    row[4] = 40; row[5] = 50; row[6] = 60; row[7] = 255;
    assert(mailbox->publish(frame) == 1);

    remote_frame_source source{mailbox, 120};
    assert(source.width() == 2 && source.height() == 1);
    std::uint64_t generation = 0;
    auto converted = source.frame(generation);
    assert(generation == 1 && converted);
    assert(converted->pixels[0] == 30 && converted->pixels[1] == 20 &&
           converted->pixels[2] == 10);
    assert(converted->pixels[4] == 60 && converted->pixels[5] == 50 &&
           converted->pixels[6] == 40);

    auto resized = std::make_shared<remote_media::OwnedFrame>(
        3, 2, remote_media::PixelFormat::bgra8);
    assert(mailbox->publish(resized) == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    converted = source.frame(generation);
    assert(generation == 2 && converted);
    assert(source.width() == 3 && source.height() == 2);
    assert(converted->width == 3 && converted->height == 2);

    auto sink = std::make_shared<recording_sink>();
    input_forwarder input{sink};
    input.key_event('A', true);
    input.key_event('A', true);
    input.key_event('A', false);
    input.pointer_event(0x09, 7, 9);
    input.pointer_event(0x00, 8, 10);
    assert(std::get<remote_media::KeyInput>(sink->events[0]).code ==
           remote_media::KeyCode::a);
    assert(std::get<remote_media::KeyInput>(sink->events[1]).action ==
           remote_media::KeyAction::repeated);
    assert(std::holds_alternative<remote_media::PointerMoveInput>(sink->events[3]));
    assert(std::get<remote_media::PointerButtonInput>(sink->events[4]).action ==
           remote_media::ButtonAction::pressed);
    assert(std::get<remote_media::ScrollInput>(sink->events[5]).delta_y == 1);
    assert(std::get<remote_media::PointerButtonInput>(sink->events[7]).action ==
           remote_media::ButtonAction::released);

    bool rejected_null = false;
    try {
        input_forwarder invalid{nullptr};
    } catch (const std::invalid_argument&) {
        rejected_null = true;
    }
    assert(rejected_null);
    return 0;
}
