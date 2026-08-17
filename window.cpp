#define NOMINMAX

#include <xkb_helper.hpp>
#include <wayland_helper.hpp>
#include <hip_helper.hpp>
#include <drm_helper.hpp>
#include <posix.hpp>

#include "linux/input-event-codes.h"

#include "rfb.hpp"

#include <memory>
#include <thread>

using cpp_helper::empty_class;
using cpp_helper::configure;
using cpp_helper::empty_configure;
using cpp_helper::empty_configurable_class;
using namespace std::literals;

template<typename T>
class add_rfb_process_server_cut_text : public T {
public:
    using parent = T;
    add_rfb_process_server_cut_text(const configure auto& conf) : parent{conf}
    {}
    void process_server_cut_text(const auto& text) {
        parent::set_selection_source_content(text);
    }
};

template<typename T>
using add_rfb_parent =
    rfb::add_rfb<
    rfb::add_process_framebuffer_update<
    rfb::add_decode_h264<
    rfb::add_yuv_to_rgb<
    rfb::add_zrle<
    rfb::init_rfb<
    rfb::add_server_cut_text<
    add_rfb_process_server_cut_text<
    rfb::add_client_cut_text<
    rfb::add_set_encodings<
    rfb::set_supported_encodings_from_configure<
    rfb::add_set_format<
    rfb::add_server_init<
    rfb::add_client_init<
    rfb::add_connection<
    rfb::set_port<
    rfb::set_address<
    T
    >>>>>>>>>>>>>>>>>
;
template<class T>
class add_rfb : public add_rfb_parent<T> {
public:
    using parent = add_rfb_parent<T>;
    add_rfb(const configure auto& conf) : parent{conf}
    {}
    auto get_rfb(std::span<uint8_t> frame) {
        parent::get_frame(frame);
    }
    auto get_fb_width() {
        return parent::get_width();
    }
    auto get_fb_height() {
        return parent::get_height();
    }
    void send_key_event(int key, int state) {
        parent::key_event(key, state);
    }
    void send_pointer_event(uint32_t button_mask, int x, int y) {
        parent::pointer_event(button_mask, x, y);
    }
    void send_cut_text(auto& str) {
        parent::client_cut_text(str);
    }
    void request_framebuffer_update(int x, int y, int width, int height, bool increment_update=true) {
        parent::framebuffer_update_request(x, y, width, height, increment_update);
    }
    void request_framebuffer_update(int x=0, int y=0) {
        request_framebuffer_update(x, y, get_fb_width(), get_fb_height(), true);
    }
};

template<typename T>
class add_rfb_process_keysym : public T {
public:
    using parent = T;
    add_rfb_process_keysym(const configure auto& conf) : parent{conf}
    {}
    void update_keysyms() {
        for (auto [keysym, state] : keysyms) {
            if (state == WL_KEYBOARD_KEY_STATE_REPEATED) {
                parent::send_key_event(keysym, WL_KEYBOARD_KEY_STATE_PRESSED);
            }
            else {
                parent::send_key_event(keysym, state);
            }
        }
        keysyms.clear();
    }
    auto process_keysym_event(int keysym, int state) {
        keysyms.emplace_back(keysym, state);
    }
private:
    std::vector<std::pair<int,int>> keysyms;
};

template<class T>
class add_rfb_process_pointer : public T {
public:
    using parent = T;
    add_rfb_process_pointer(const configure auto& conf) : parent{conf},
        sended_x{},
        sended_y{},
        latest_x{},
        latest_y{},
        button_mask{}
    {
    }
    void update_pointer(uint32_t mask, int x, int y) {
        parent::send_pointer_event(mask, x, y);
        sended_x = x;
        sended_y = y;
    }
    void update_pointer_position() {
        if (sended_x != latest_x || sended_y != latest_y) {
            update_pointer(button_mask, latest_x, latest_y);
        }
    }
    void process_pointer_axis_event(uint32_t axis, int value) {
        int mask = button_mask;
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            if (value < 0) {
                mask |= (1<<3);
            }
            else if (value > 0) {
                mask |= (1<<4);
            }
        }
        std::cout << "pointer axis value: " << axis << " " << value << std::endl;
        update_pointer(mask, latest_x, latest_y);
        update_pointer(button_mask, latest_x, latest_y);
    }
    void process_pointer_motion_event(int x, int y) {
        auto fb_width = parent::get_fb_width();
        auto fb_height = parent::get_fb_height();
        auto [surface_width, surface_height] = parent::get_surface_resolution();
        x = x * fb_width / surface_width;
        y = y * fb_height / surface_height;
        latest_x = x;
        latest_y = y;
    }
    void process_pointer_button_event(int button, int button_state) {
        std::cout << "pointer button event processing" << std::endl;
        if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (button == BTN_LEFT) {
                button_mask |= (1<<0);
            }
            else if (button == BTN_MIDDLE) {
                button_mask |= (1<<1);
            }
            else if (button == BTN_RIGHT) {
                button_mask |= (1<<2);
            }
            else {
                std::cerr << "unknown pointer button" << std::endl;
            }
        }
        else if (button_state == WL_POINTER_BUTTON_STATE_RELEASED) {
            if (button == BTN_LEFT) {
                button_mask &= ~(1<<0);
            }
            else if (button == BTN_MIDDLE) {
                button_mask &= ~(1<<2);
            }
            else if (button == BTN_RIGHT) {
                button_mask &= ~(1<<2);
            }
            else {
                std::cerr << "unknown pointer button" << std::endl;
            }
        }
        update_pointer(button_mask, latest_x, latest_y);
    }
private:
    int sended_x;
    int sended_y;
    int latest_x;
    int latest_y;
    int button_mask;
};

template<typename T>
class register_size_change_callback : public T {
public:
    using parent = T;
    using this_type = register_size_change_callback<T>;
    register_size_change_callback(const configure auto& conf) : parent{conf} {
        parent::set_size_changed_callback(size_changed_callback, this);
    }
    static void size_changed_callback(int width, int height, void* data) {
        std::cout << "size changed" << std::endl;
        auto t = reinterpret_cast<this_type*>(data);
        t->size_changed(width, height);
    }
    void size_changed(int width, int height) {
        parent::recreate_surface();
    }
};

template<typename T>
class add_recreate_surface_for : public T {
public:
    using parent = T;
    add_recreate_surface_for(const configure auto& conf) : parent{conf} {
    }
    void recreate_surface() {
        parent::destroy();
        parent::recreate_surface();
        parent::create();
    }
};
template<typename T>
class add_dummy_recreate_surface : public T {
public:
    using parent = T;
    add_dummy_recreate_surface(const configure auto& conf) : parent{conf} {
    }
    void recreate_surface() {
    }
};

template<typename T>
class set_bo_alloc_size : public T {
public:
    using parent = T;
    set_bo_alloc_size(const configure auto& conf) : parent{conf}
    {}
    auto get_bo_alloc_size() {
        auto [width,height] = parent::get_surface_resolution();
        return width*height*sizeof(uint32_t);
    }
    auto get_dma_buf_size() {
        return get_bo_alloc_size();
    }
};

template<typename T>
class attach_dma_buf_fd_to_surface : public T {
public:
    using parent = T;
    attach_dma_buf_fd_to_surface(const configure auto& conf) : parent{conf}
    {
        create();
    }
    ~attach_dma_buf_fd_to_surface() {
        destroy();
    }

    void create() {
        auto [width,height] = parent::get_surface_resolution();
        uint32_t dma_buf_fd = parent::get_dma_buf_fd();
        auto dmabuf = parent::get_dmabuf();
        buffer_params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        zwp_linux_buffer_params_v1_add(buffer_params, dma_buf_fd, 0, 0, width, 0, 0);
        buffer = zwp_linux_buffer_params_v1_create_immed(buffer_params, width, height, DRM_FORMAT_XRGB8888, 0);
        assert(buffer != nullptr);
        auto surface = parent::get_wayland_surface();
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_commit(surface);
    }
    void destroy() {
        wl_buffer_destroy(buffer);
        zwp_linux_buffer_params_v1_destroy(buffer_params);
    }
private:
    zwp_linux_buffer_params_v1* buffer_params;
    wl_buffer* buffer;
};

template<typename T>
class add_hip_draw : public T {
public:
    using parent = T;
    add_hip_draw(const configure auto& conf) : parent{conf},
        upload_ptr{}
    {
        auto fb_width = parent::get_fb_width();
        auto fb_height = parent::get_fb_height();
        hipHostMalloc(&upload_ptr, fb_width*fb_height*sizeof(uint32_t));
        assert(upload_ptr != nullptr);
    }
    void draw() {
        auto fb_width = parent::get_fb_width();
        auto fb_height = parent::get_fb_height();
        auto [width,height] = parent::get_surface_resolution();
        std::cout << std::format("resolution={}x{}", width, height) << std::endl;
        auto hip_memory = parent::get_external_memory();
        auto frame_ptr = reinterpret_cast<uint32_t*>(parent::get_external_memory_buffer());

        parent::get_rfb(std::span{reinterpret_cast<uint8_t*>(upload_ptr), fb_width*fb_height*sizeof(uint32_t)});
        copy_frame<<<dim3(8,8,1), dim3(32,1,1),0>>>(
                frame_ptr, width, height,
                upload_ptr, fb_width, fb_height,
                (width+32*8-1)/(32*8), (height+1*8-1)/(1*8));
        int ret = hipDeviceSynchronize();
        if (ret != hipSuccess) {
            throw std::runtime_error{std::format("hipDeviceSynchronize failed: {}", ret)};
        }

        auto surface = parent::get_wayland_surface();
        auto buffer = parent::get_buffer();
        wl_surface_damage(surface, 0, 0, width, height);
        wl_surface_commit(surface);
        auto display = parent::get_display();
        wl_display_flush(display);
    }
    __global__
    static void copy_frame(
            uint32_t* frame_ptr, uint32_t frame_width, uint32_t frame_height,
            uint32_t* src, uint32_t width, uint32_t height,
            uint32_t tile_width, uint32_t tile_height) {
        for (int y_ = 0; y_ < tile_height; y_++) {
            int y = y_ + (threadIdx.y+blockIdx.y*blockDim.y)*tile_height;
            if (y >= frame_height) break;
            for (int x_ = 0; x_ < tile_width; x_++) {
                int x = x_ + (threadIdx.x+blockIdx.x*blockDim.x)*tile_width;
                if (x >= frame_width) break;
                frame_ptr[y*frame_width+x] = src[(y*height/frame_height)*width+x*width/frame_width];
            }
        }
    }
private:
    uint32_t* upload_ptr;
};

template<class T>
class add_rfb_latency_analyser : public T {
public:
    using parent = T;
    add_rfb_latency_analyser(const configure auto& conf) : parent{conf},
        framebuffer_update_request_time{std::chrono::steady_clock::now()},
        latency{}
    {
    }
    using clock = std::chrono::steady_clock;
    auto draw() {
        auto draw_time = clock::now();
        latency = draw_time - framebuffer_update_request_time; // This ignores render and present latency
        return parent::draw();
    }
    void request_framebuffer_update(int x, int y, int width, int height, bool increment_update=true) {
        framebuffer_update_request_time = clock::now();
        return parent::request_framebuffer_update(x, y, width, height, increment_update);
    }
    void request_framebuffer_update(int x=0, int y=0) {
        framebuffer_update_request_time = clock::now();
        return parent::request_framebuffer_update(x, y);
    }
    auto get_rfb_latency() {
        return latency;
    }
private:
    clock::time_point framebuffer_update_request_time;
    clock::duration latency;
};

template<typename T>
class set_buffer_size_equal_to_fb_size : public T {
public:
    using parent = T;
    set_buffer_size_equal_to_fb_size(const configure auto& conf) : parent{conf} {
    }
    auto get_buffer_size() { return parent::get_fb_width() * parent::get_fb_height() * 4; }
};

template <class T> class add_get_time : public T {
public:
    using parent = T;
    add_get_time(const configure auto& conf) : parent{conf}, m_start_time{std::chrono::steady_clock::now()}{
    }
    auto get_time() {
        return std::chrono::steady_clock::now() - m_start_time;
    }
private:
    std::chrono::steady_clock::time_point m_start_time;
};

template<typename T>
class add_rfb_reduce_update : public T {
public:
    using parent = T;
    add_rfb_reduce_update(const configure auto& conf) : parent{conf},
        wayland_frame{true},
        ignored_update{false}
    {
        set_callback();
    }
    void request_framebuffer_update(int x, int y, int width, int height, bool increment_update=true) {
        if (wayland_frame) {
            parent::request_framebuffer_update(x, y, width, height, increment_update);

            wayland_frame = false;
        }
        else {
            ignored_update = true;
        }
    }
    void request_framebuffer_update(int x=0, int y=0) {
        request_framebuffer_update(x, y, parent::get_fb_width(), parent::get_fb_height(), true);
    }
private:
    static void static_frame_event(void* p, wl_callback* callback, uint32_t data) {
        auto t = reinterpret_cast<add_rfb_reduce_update*>(p);
        t->frame_event();
    }
    void set_callback() {
        auto wayland_surface = parent::get_wayland_surface();
        auto callback = wl_surface_frame(wayland_surface);
        static struct wl_callback_listener callback_listener = {
            .done = static_frame_event,
        };
        wl_callback_add_listener(callback, &callback_listener, this);
    }
    void frame_event() {
        if (ignored_update) {
            parent::request_framebuffer_update();
            ignored_update = false;
        }
        wayland_frame = true;
        set_callback();
    }
    bool wayland_frame;
    bool ignored_update;
};

template<typename T>
class add_rfb_socket_pollfd : public T {
public:
    using parent = T;
    add_rfb_socket_pollfd(const configure auto& conf) : parent{conf} {
        parent::request_framebuffer_update(0,0, parent::get_fb_width(), parent::get_fb_height(), false);
    }
    static constexpr int FDS_INDEX = parent::FDS_SIZE;
    static constexpr int FDS_SIZE = parent::FDS_SIZE+1;
    void process_events(auto& fds) {
        assert(fds[FDS_INDEX].fd == parent::get_socket());
        auto now = std::chrono::steady_clock::now();
        if (fds[FDS_INDEX].revents & POLLIN) {
            parent::request_framebuffer_update(); // request next framebuffer, decrease delay, increase fps.
            parent::update_pointer_position(); // pointer and key events is after frame request to increase fps.
            parent::update_keysyms();
            auto content_opt = parent::get_selection_content();
            if (content_opt) {
                auto& content = content_opt.value();
                parent::send_cut_text(content);
                write(STDOUT_FILENO, content.data(), content.size());
                std::cout << std::endl;
            }
            parent::process_server_message(); // process current framebuffer request.
            if (parent::is_frame_updated()) {
                parent::reset_frame_updated();
                parent::draw();
            }
            previous_time = now;
        }
        else if (now - previous_time > 30ms) {
            parent::update_pointer_position();
            parent::update_keysyms();
            previous_time = now;
        }
        parent::process_events(fds);
    }
    std::vector<pollfd> get_fds() {
        auto fds = parent::get_fds();
        fds.emplace_back(pollfd{
                .fd = parent::get_socket(),
                .events = POLLIN,
                });
        return fds;
    }
private:
    std::chrono::steady_clock::time_point previous_time;
};

template<typename T>
class add_info_printer: public T {
public:
    using parent = T;
    add_info_printer(const configure auto& conf) : parent{conf} {
    }
    using clock = std::chrono::steady_clock;
    void process_events(auto& fds) {
        auto now = clock::now();
        if (now - previous_time > 500ms) {
            previous_time = now;
            //auto rfb_latency = parent::get_rfb_latency();
            //auto rfb_latency_ms = rfb_latency / 1000000ns;
            //auto cpu_frame_time = parent::get_cpu_frame_time();
            //auto cpu_frame_time_ms = cpu_frame_time / 1000000ns;
            //float fps = cpu_frame_time > 0ns ? (100s / cpu_frame_time)/100.0 : 0;
            auto encoding_type = parent::get_encoding();
            auto frame_network_byte_count = parent::get_frame_network_byte_count();
            //std::clog << std::format("encoding: {}, bytes: {}, cpu frame time: {:5}ms, fps: {:6}\r",
            //        encoding_type, frame_network_byte_count, cpu_frame_time_ms, fps);
        }
        parent::process_events(fds);
    }
private:
    clock::time_point previous_time;
};

using draw_app =
    wayland_helper::run_wayland_event_loop<
    wayland_helper::add_wayland_pollfds_loop<
    posix::add_poll_events<
    wayland_helper::add_wayland_pollfd<
    wayland_helper::register_pointer_axis_callback<
    wayland_helper::register_pointer_button_callback<
    wayland_helper::register_pointer_motion_callback<
    wayland_helper::register_key_callback<
    wayland_helper::register_keyboard_leave_callback<
    wayland_helper::add_repeat_key<
    posix::add_time_point_callbacks<
    wayland_helper::register_keyboard_modifiers_callback<
    wayland_helper::register_keymap_callback<
    xkb_helper::add_process_key_event<
    xkb_helper::add_process_keyboard_modifiers<
    xkb_helper::add_process_keymap<
    xkb_helper::add_state<
    xkb_helper::add_keymap<
    xkb_helper::add_context<
    add_info_printer<
    add_rfb_socket_pollfd<
    add_rfb_reduce_update<
    add_rfb_latency_analyser<
    add_rfb_process_keysym<
    add_rfb_process_pointer<
    add_hip_draw<
    register_size_change_callback<
    add_recreate_surface_for<
    attach_dma_buf_fd_to_surface<
    add_recreate_surface_for<
    hip_helper::add_external_memory_buffer<
    add_recreate_surface_for<
    hip_helper::add_external_memory<
    add_recreate_surface_for<
    drm_helper::add_dma_buf_fd_with_amdgpu_bo<
    add_recreate_surface_for<
    drm_helper::add_amdgpu_bo<
    set_bo_alloc_size<
    add_dummy_recreate_surface<
    drm_helper::add_amdgpu_device<
    drm_helper::add_drm_fd<
    add_rfb<
    rfb::set_address<
    rfb::set_port<
    posix::add_empty_pollfd_array<
    wayland_helper::add_wayland_surface<
    cpp_helper::add_logger<
    empty_class
    >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
;

using namespace std::literals;

struct config : public empty_configure {
    const char* address;
    uint16_t port;
    std::vector<uint32_t> supported_encodings;
    const char* enabled_logs;
    int32_t width;
    int32_t height;
    const char* drm_device_path;
};

int main(int argc, const char* argv[]) {
  try {
    auto usage_string =
                "Usage: rfb_window_demo <address> <port> <encodings>\n"
                "--log <enabled_logs>\n"
                "--width <width>\n"
                "--height <height>\n"
                "--gpu <drm_device_path>\n"
                ;
    if (argc < 3) {
        throw std::logic_error(
                usage_string
                );
    }
    const char* address = "127.0.0.1";
    uint16_t port = 5900;
    std::vector<uint32_t> encodings{};
    const char* enabled_logs = "";
    int32_t width = 1920, height = 1080;
    const char* drm_device_path = "";
    for (int i = 1, pos_arg=0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (strcmp(&argv[i][2], "log") == 0) {
                enabled_logs = argv[i+1];
                i += 1;
            }
            else if (strcmp(&argv[i][2], "width") == 0) {
                width = strtol(argv[i+1], NULL, 10);
                i += 1;
            }
            else if (strcmp(&argv[i][2], "height") == 0) {
                height = strtol(argv[i+1], NULL, 10);
                i += 1;
            }
            else if (strcmp(&argv[i][2], "gpu") == 0) {
                drm_device_path = argv[i+1];
                i += 1;
            }
            else {
                throw std::logic_error(
                        usage_string
                        );
            }
        }
        else {
            if (pos_arg == 0) {
                address = argv[i];
            }
            else if (pos_arg == 1) {
                port = strtol(argv[i], NULL, 10);
            }
            else if (pos_arg == 2) {
                auto str = argv[i];
                char* next;
                while (*str != '\0') {
                    auto v = strtol(str, &next, 10);
                    if (next == str) {
                        break;
                    }
                    str = next + 1;
                    encodings.emplace_back(rfb::to_big_endian(v));
                }
            }
            else {
                throw std::logic_error(
                        usage_string
                        );
            }
            pos_arg += 1;
        }
    }
    if (encodings.size() == 0) {
        encodings = {rfb::to_big_endian(50), rfb::to_big_endian(16), 0};
    }
    auto conf = config{
        .address=address, .port=port,
        .supported_encodings = std::move(encodings),
        .enabled_logs = enabled_logs,
        .width = width,
        .height = height,
        .drm_device_path = drm_device_path,
    };
    auto app = draw_app{conf};
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
  }
  return 0;
}
