#define NOMINMAX

#include <xkb_helper.hpp>
#include <wayland_helper.hpp>
#include <hip_helper.hpp>
#include <drm_helper.hpp>
#include <posix.hpp>

#include "linux/input-event-codes.h"

#include <memory>
#include <thread>

using cpp_helper::empty_class;
using cpp_helper::configure;
using cpp_helper::empty_configure;
using cpp_helper::empty_configurable_class;
using namespace std::literals;

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
    auto get_buffer() {
        return buffer;
    }
private:
    zwp_linux_buffer_params_v1* buffer_params;
    wl_buffer* buffer;
};

template<typename T>
class add_hip_draw : public T {
public:
    using parent = T;
    using this_type = add_hip_draw<T>;
    using clock = std::chrono::steady_clock;
    struct repeat_draw {
        this_type* t;

        void operator()() {
            t->draw();
            t->add_time_point_callback(clock::now()+30ms, *this);
        }
    };
    add_hip_draw(const configure auto& conf) : parent{conf}
    {
        parent::add_time_point_callback(
                clock::now()+30ms,
                repeat_draw{this}
                );
    }
    ~add_hip_draw() {
        auto& callbacks = parent::get_time_point_callbacks();
        callbacks.clear();
    }
    void draw() {
        auto [width,height] = parent::get_surface_resolution();
        auto hip_memory = parent::get_external_memory();
        auto frame_ptr = reinterpret_cast<uint32_t*>(parent::get_external_memory_buffer());

        draw_frame<<<dim3(8,8,1), dim3(32,1,1),0>>>(
                frame_ptr, width, height,
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
    static void draw_frame(
            uint32_t* frame_ptr, uint32_t frame_width, uint32_t frame_height,
            uint32_t tile_width, uint32_t tile_height) {
        for (int y_ = 0; y_ < tile_height; y_++) {
            int y = y_ + (threadIdx.y+blockIdx.y*blockDim.y)*tile_height;
            if (y >= frame_height) break;
            for (int x_ = 0; x_ < tile_width; x_++) {
                int x = x_ + (threadIdx.x+blockIdx.x*blockDim.x)*tile_width;
                if (x >= frame_width) break;
                frame_ptr[y*frame_width+x] = 0x000000ff * y / frame_height;
            }
        }
    }
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
    wayland_helper::register_keyboard_modifiers_callback<
    wayland_helper::register_keymap_callback<
    xkb_helper::add_process_key_event<
    xkb_helper::add_process_keyboard_modifiers<
    xkb_helper::add_process_keymap<
    xkb_helper::add_state<
    xkb_helper::add_keymap<
    xkb_helper::add_context<
    add_hip_draw<
    posix::add_time_point_callbacks<
    wayland_helper::register_size_change_callback<
    wayland_helper::add_recreate_surface_for<
    attach_dma_buf_fd_to_surface<
    wayland_helper::add_recreate_surface_for<
    hip_helper::add_external_memory_buffer<
    wayland_helper::add_recreate_surface_for<
    hip_helper::add_external_memory<
    wayland_helper::add_recreate_surface_for<
    drm_helper::add_dma_buf_fd_with_amdgpu_bo<
    wayland_helper::add_recreate_surface_for<
    drm_helper::add_amdgpu_bo<
    set_bo_alloc_size<
    wayland_helper::add_dummy_recreate_surface<
    drm_helper::add_amdgpu_device<
    drm_helper::add_drm_fd<
    posix::add_empty_pollfd_array<
    wayland_helper::add_wayland_surface<
    cpp_helper::add_logger<
    empty_class
    >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
;

using namespace std::literals;

struct config : public empty_configure {
    int32_t width;
    int32_t height;
    const char* drm_device_path;
};

int main(int argc, const char* argv[]) {
  try {
    auto usage_string =
                "Usage: test_hip\n"
                "--width <width>\n"
                "--height <height>\n"
                "--gpu <drm_device_path>\n"
                ;
    if (argc < 3) {
        throw std::logic_error(
                usage_string
                );
    }
    int32_t width = 1920, height = 1080;
    const char* drm_device_path = "";
    for (int i = 1, pos_arg=0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (strcmp(&argv[i][2], "width") == 0) {
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
            throw std::logic_error(
                    usage_string
                    );
            pos_arg += 1;
        }
    }
    auto conf = config{
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
