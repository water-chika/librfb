#define NOMINMAX

#include <xkb_helper.hpp>
#include <wayland_helper.hpp>
#include <hip_helper.hpp>
#include <drm_helper.hpp>
#include <posix.hpp>

#include "linux/input-event-codes.h"

#include "rfb.hpp"
#include "control.hpp"
#include "trace.hpp"

#include <memory>
#include <deque>
#include <set>
#include <thread>
#include <fstream>

#include <dirent.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <vector>

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
        m_server_cut_text.assign(text.begin(), text.end());
        parent::set_selection_source_content(text);
    }
    std::string_view server_cut_text() const { return m_server_cut_text; }
private:
    std::string m_server_cut_text;
};

template<typename T>
using add_rfb_parent =
    rfb::add_rfb<
    rfb::add_request_scale<
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
    rfb::set_password_from_configure<
    T
    >>>>>>>>>>>>>>>>>>>
;
template<class T>
class add_rfb : public add_rfb_parent<T> {
public:
    using parent = add_rfb_parent<T>;
    add_rfb(const configure auto& conf) : parent{conf},
        view_only{conf.view_only}
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
        if (view_only) return;
        parent::key_event(key, state);
        if (state) pressed_keys.insert(key); else pressed_keys.erase(key);
    }
    void send_pointer_event(uint32_t button_mask, int x, int y) {
        if (view_only) return;
        parent::pointer_event(button_mask, x, y);
        pointer_mask = button_mask;
        pointer_x = x;
        pointer_y = y;
    }
    const std::set<int>& held_remote_keys() const { return pressed_keys; }
    auto held_remote_pointer() const {
        struct state { uint32_t mask; int x; int y; };
        return state{pointer_mask, pointer_x, pointer_y};
    }
    void release_remote_input() {
        for (auto key : pressed_keys) parent::key_event(key, 0);
        pressed_keys.clear();
        if (pointer_mask) parent::pointer_event(0, pointer_x, pointer_y);
        pointer_mask = 0;
    }
    void send_cut_text(auto& str) {
        if (view_only) return;
        parent::client_cut_text(str);
    }
    void request_framebuffer_update(int x, int y, int width, int height, bool increment_update=true) {
        parent::framebuffer_update_request(x, y, width, height, increment_update);
    }
    void request_framebuffer_update(int x=0, int y=0) {
        request_framebuffer_update(x, y, get_fb_width(), get_fb_height(), true);
    }
    bool get_view_only() const { return view_only; }
    void set_view_only(bool v) { view_only = v; }
private:
    bool view_only;
    std::set<int> pressed_keys;
    uint32_t pointer_mask{};
    int pointer_x{}, pointer_y{};
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
        update_pointer_position();
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
        // Motion is only flushed once per poll iteration, so the press would
        // otherwise be the first event carrying the new position: the server
        // sees the pointer teleport and the button go down in one atomic
        // event. X11 servers deliver that as a press at the new position with
        // no motion before it, and toolkits that pick their click target from
        // the preceding motion/enter simply drop the click. Send the move on
        // its own first.
        update_pointer_position();
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
                button_mask &= ~(1<<1);
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

enum class presentation_mode {
    legacy,
    release_gated,
};

template<typename T>
class add_presentation_copy : public T {
public:
    using parent = T;

    add_presentation_copy(const configure auto& conf) : parent{conf},
        mode{conf.presentation}
    {
        select_hip_device();
        ensure_upload();
        if (mode == presentation_mode::release_gated &&
                parent::has_linux_drm_syncobj_manager() &&
                drm_helper::supports_timeline_syncobj(parent::get_drm_fd())) {
            sync_surface = wp_linux_drm_syncobj_manager_v1_get_surface(
                    parent::get_linux_drm_syncobj_manager(),
                    parent::get_wayland_surface());
            explicit_sync = sync_surface != nullptr;
        }
        std::clog << "presentation: "
                  << (mode == presentation_mode::legacy ? "legacy" : "release-gated")
                  << ", synchronization: "
                  << (explicit_sync ? "linux-drm-syncobj-v1" : "implicit")
                  << std::endl;
        if (mode == presentation_mode::legacy) {
            create_legacy_buffer();
        } else {
            create_generation();
        }
    }

    ~add_presentation_copy() {
        shutting_down = true;
        legacy_slot.reset();
        slots.clear();
        if (sync_surface != nullptr) {
            wp_linux_drm_syncobj_surface_v1_destroy(sync_surface);
        }
        if (upload_ptr != nullptr) {
            (void)hipHostFree(upload_ptr);
        }
    }

    void draw() {
        presentation_pending = true;
        present_latest();
    }

    void recreate_surface() {
        presentation_pending = true;
        if (mode == presentation_mode::legacy) {
            create_legacy_buffer();
            present_latest();
            return;
        }
        auto retired_busy = std::ranges::count_if(slots, [](const auto& slot) {
            return slot->retired && slot->busy;
        });
        if (retired_busy >= slot_count) {
            resize_deferred = true;
            return;
        }
        create_generation();
        present_latest();
    }

private:
    void select_hip_device() {
        drmDevicePtr device = nullptr;
        if (drmGetDevice2(parent::get_drm_fd(), 0, &device) != 0 ||
                device == nullptr || !(device->bustype == DRM_BUS_PCI) ||
                device->businfo.pci == nullptr) {
            if (device != nullptr) drmFreeDevice(&device);
            throw std::runtime_error{"cannot map selected DRM device to HIP PCI device"};
        }
        auto* pci = device->businfo.pci;
        std::string bus_id = std::format("{:04x}:{:02x}:{:02x}.{}",
                pci->domain, pci->bus, pci->dev, pci->func);
        drmFreeDevice(&device);
        int hip_device = -1;
        int ret = hipDeviceGetByPCIBusId(&hip_device, bus_id.c_str());
        if (ret != hipSuccess || hipSetDevice(hip_device) != hipSuccess) {
            throw std::runtime_error{std::format(
                    "HIP cannot select presentation GPU {}", bus_id)};
        }
        std::clog << "HIP device: " << hip_device << " (" << bus_id << ")"
                  << std::endl;
    }

    struct presentation_slot {
        add_presentation_copy* owner{};
        uint64_t generation{};
        uint32_t width{};
        uint32_t height{};
        uint32_t stride{};
        size_t size{};
        amdgpu_bo_handle bo{};
        hipExternalMemory_t external_memory{};
        void* mapped{};
        wl_buffer* buffer{};
        std::optional<drm_helper::syncobj_timeline> timeline;
        wp_linux_drm_syncobj_timeline_v1* wayland_timeline{};
        uint64_t next_point{1};
        uint64_t release_point{};
        bool busy{};
        bool retired{};

        ~presentation_slot() {
            if (wayland_timeline != nullptr) {
                wp_linux_drm_syncobj_timeline_v1_destroy(wayland_timeline);
            }
            if (buffer != nullptr) {
                wl_buffer_destroy(buffer);
            }
            if (mapped != nullptr) {
                auto ret = hipFree(mapped);
                if (ret != hipSuccess) {
                    std::cerr << std::format("hipFree presentation buffer failed: {}\n", static_cast<int>(ret));
                }
            }
            if (external_memory != nullptr) {
                auto ret = hipDestroyExternalMemory(external_memory);
                if (ret != hipSuccess) {
                    std::cerr << std::format("hipDestroyExternalMemory failed: {}\n", static_cast<int>(ret));
                }
            }
            if (bo != nullptr) {
                int ret = amdgpu_bo_free(bo);
                if (ret < 0) {
                    std::cerr << std::format("amdgpu_bo_free failed: {}\n", ret);
                }
            }
        }
    };

    static void buffer_release(void* data, wl_buffer*) {
        auto* slot = static_cast<presentation_slot*>(data);
        assert(slot != nullptr && slot->owner != nullptr);
        slot->owner->release_slot(slot);
    }

    static constexpr wl_buffer_listener buffer_listener{
        .release = buffer_release,
    };

    std::unique_ptr<presentation_slot> create_slot(uint64_t generation,
            uint32_t width, uint32_t height, bool listen_for_release = true,
            bool legacy_stride = false) {
        auto slot = std::make_unique<presentation_slot>();
        slot->owner = this;
        slot->generation = generation;
        slot->width = width;
        slot->height = height;
        slot->stride = width * sizeof(uint32_t);
        slot->size = static_cast<size_t>(slot->stride) * height;

        amdgpu_bo_alloc_request request{
            .alloc_size = slot->size,
            .phys_alignment = 4096,
            .preferred_heap = AMDGPU_GEM_DOMAIN_VRAM,
            .flags = AMDGPU_GEM_CREATE_CPU_GTT_USWC,
        };
        int ret = amdgpu_bo_alloc(parent::get_amdgpu_device(), &request, &slot->bo);
        if (ret < 0) {
            throw std::runtime_error{"amdgpu_bo_alloc failed"};
        }

        uint32_t exported_fd = 0;
        ret = amdgpu_bo_export(slot->bo, amdgpu_bo_handle_type_dma_buf_fd,
                &exported_fd);
        if (ret < 0) {
            throw std::runtime_error{"amdgpu_bo_export failed"};
        }
        int hip_fd = static_cast<int>(exported_fd);
        int wayland_fd = dup(hip_fd);
        if (wayland_fd < 0) {
            close(hip_fd);
            throw std::runtime_error{"dup dma-buf fd failed"};
        }

        hipExternalMemoryHandleDesc external_desc{};
        external_desc.type = hipExternalMemoryHandleTypeOpaqueFd;
        external_desc.handle.fd = hip_fd;
        external_desc.size = slot->size;
        ret = hipImportExternalMemory(&slot->external_memory, &external_desc);
        if (ret != hipSuccess) {
            close(hip_fd);
            close(wayland_fd);
            throw std::runtime_error{std::format("hipImportExternalMemory failed: {}", ret)};
        }
        // A successful opaque-fd import transfers ownership of hip_fd to HIP.

        hipExternalMemoryBufferDesc buffer_desc{};
        buffer_desc.size = slot->size;
        ret = hipExternalMemoryGetMappedBuffer(&slot->mapped,
                slot->external_memory, &buffer_desc);
        if (ret != hipSuccess) {
            close(wayland_fd);
            throw std::runtime_error{std::format(
                    "hipExternalMemoryGetMappedBuffer failed: {}", ret)};
        }

        auto* params = zwp_linux_dmabuf_v1_create_params(parent::get_dmabuf());
        if (params == nullptr) {
            close(wayland_fd);
            throw std::runtime_error{"zwp_linux_dmabuf_v1_create_params failed"};
        }
        zwp_linux_buffer_params_v1_add(params, wayland_fd, 0, 0,
                legacy_stride ? width : slot->stride, 0, 0);
        slot->buffer = zwp_linux_buffer_params_v1_create_immed(params,
                width, height, DRM_FORMAT_XRGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        close(wayland_fd);
        if (slot->buffer == nullptr) {
            throw std::runtime_error{"zwp_linux_buffer_params_v1_create_immed failed"};
        }
        if (explicit_sync && listen_for_release) {
            slot->timeline.emplace(parent::get_drm_fd());
            int timeline_fd = slot->timeline->export_fd();
            slot->wayland_timeline =
                wp_linux_drm_syncobj_manager_v1_import_timeline(
                    parent::get_linux_drm_syncobj_manager(), timeline_fd);
            close(timeline_fd);
            if (slot->wayland_timeline == nullptr) {
                throw std::runtime_error{"syncobj timeline import failed"};
            }
        } else if (listen_for_release &&
                wl_buffer_add_listener(slot->buffer, &buffer_listener, slot.get()) < 0) {
            throw std::runtime_error{"wl_buffer_add_listener failed"};
        }
        return slot;
    }

    std::optional<std::pair<uint32_t, uint32_t>> validated_surface_size() {
        auto [width_value, height_value] = parent::get_surface_resolution();
        if (width_value <= 0 || height_value <= 0) {
            return std::nullopt;
        }
        auto width = static_cast<uint32_t>(width_value);
        auto height = static_cast<uint32_t>(height_value);
        constexpr size_t pixel_size = sizeof(uint32_t);
        if (width > UINT32_MAX / pixel_size ||
                width > rfb::max_framebuffer_bytes / pixel_size / height) {
            std::cerr << std::format(
                    "presentation size {}x{} exceeds the {} byte limit\n",
                    width, height, rfb::max_framebuffer_bytes);
            return std::nullopt;
        }
        return std::pair{width, height};
    }

    void create_legacy_buffer() {
        auto size = validated_surface_size();
        if (!size) {
            return;
        }
        auto [width, height] = *size;
        auto replacement = create_slot(0, width, height, false);
        slots.clear();
        legacy_slot = std::move(replacement);
        auto* surface = parent::get_wayland_surface();
        wl_surface_attach(surface, legacy_slot->buffer, 0, 0);
        wl_surface_commit(surface);
        wl_display_flush(parent::get_display());
    }

    void create_generation() {
        auto size = validated_surface_size();
        if (!size) {
            return;
        }
        auto [width, height] = *size;
        uint64_t new_generation = generation + 1;
        std::vector<std::unique_ptr<presentation_slot>> created;
        created.reserve(slot_count);
        for (size_t i = 0; i < slot_count; ++i) {
            created.push_back(create_slot(new_generation, width, height));
        }

        for (auto& slot : slots) {
            slot->retired = true;
        }
        generation = new_generation;
        for (auto& slot : created) {
            slots.push_back(std::move(slot));
        }
        std::erase_if(slots, [](const auto& slot) {
            return slot->retired && !slot->busy;
        });
    }

    void ensure_upload() {
        size_t needed = static_cast<size_t>(parent::get_fb_width())
                * parent::get_fb_height() * sizeof(uint32_t);
        if (needed <= upload_size) {
            return;
        }
        if (upload_ptr != nullptr) {
            (void)hipHostFree(upload_ptr);
            upload_ptr = nullptr;
            upload_size = 0;
        }
        int ret = hipHostMalloc(&upload_ptr, needed);
        if (ret != hipSuccess || upload_ptr == nullptr) {
            throw std::runtime_error{std::format("hipHostMalloc failed: {}", ret)};
        }
        upload_size = needed;
    }

    void poll_explicit_releases() {
        if (!explicit_sync) {
            return;
        }
        for (auto& slot : slots) {
            if (slot->busy && slot->release_point != 0 &&
                    slot->timeline->reached(slot->release_point)) {
                slot->busy = false;
                slot->release_point = 0;
            }
        }
        std::erase_if(slots, [](const auto& slot) {
            return slot->retired && !slot->busy;
        });
        if (resize_deferred && std::ranges::none_of(slots, [](const auto& slot) {
                return slot->retired && slot->busy;
            })) {
            resize_deferred = false;
            create_generation();
        }
    }

    presentation_slot* free_current_slot() {
        poll_explicit_releases();
        for (auto& slot : slots) {
            if (!slot->retired && slot->generation == generation && !slot->busy) {
                return slot.get();
            }
        }
        return nullptr;
    }

    void present_latest() {
        if (shutting_down || !presentation_pending) {
            return;
        }
        presentation_slot* slot = nullptr;
        if (mode == presentation_mode::legacy) {
            slot = legacy_slot.get();
        } else {
            slot = free_current_slot();
        }
        if (slot == nullptr) {
            return;
        }

        ensure_upload();
        auto fb_width = parent::get_fb_width();
        auto fb_height = parent::get_fb_height();
        parent::get_rfb(std::span{reinterpret_cast<uint8_t*>(upload_ptr),
                static_cast<size_t>(fb_width) * fb_height * sizeof(uint32_t)});
        copy_frame<<<dim3(8,8,1), dim3(32,1,1),0>>>(
                static_cast<uint32_t*>(slot->mapped), slot->width, slot->height,
                upload_ptr, fb_width, fb_height,
                (slot->width+32*8-1)/(32*8), (slot->height+1*8-1)/(1*8));
        int ret = hipDeviceSynchronize();
        if (ret != hipSuccess) {
            throw std::runtime_error{std::format("hipDeviceSynchronize failed: {}", ret)};
        }

        auto* surface = parent::get_wayland_surface();
        if (mode == presentation_mode::release_gated) {
            slot->busy = true;
            if (explicit_sync) {
                uint64_t acquire_point = slot->next_point++;
                slot->release_point = slot->next_point++;
                slot->timeline->signal(acquire_point);
                wp_linux_drm_syncobj_surface_v1_set_acquire_point(
                        sync_surface, slot->wayland_timeline,
                        acquire_point >> 32, acquire_point & UINT32_MAX);
                wp_linux_drm_syncobj_surface_v1_set_release_point(
                        sync_surface, slot->wayland_timeline,
                        slot->release_point >> 32,
                        slot->release_point & UINT32_MAX);
            }
            wl_surface_attach(surface, slot->buffer, 0, 0);
        }
        wl_surface_damage(surface, 0, 0, slot->width, slot->height);
        wl_surface_commit(surface);
        wl_display_flush(parent::get_display());
        presentation_pending = false;
    }

    void release_slot(presentation_slot* released) {
        released->busy = false;
        if (released->retired) {
            std::erase_if(slots, [released](const auto& slot) {
                return slot.get() == released;
            });
        }
        if (resize_deferred) {
            auto retired_busy = std::ranges::count_if(slots, [](const auto& slot) {
                return slot->retired && slot->busy;
            });
            if (retired_busy < slot_count) {
                resize_deferred = false;
                create_generation();
            }
        }
        present_latest();
    }

    __global__ static void copy_frame(
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

    static constexpr size_t slot_count = 3;
    presentation_mode mode;
    bool explicit_sync{};
    wp_linux_drm_syncobj_surface_v1* sync_surface{};
    std::unique_ptr<presentation_slot> legacy_slot;
    std::vector<std::unique_ptr<presentation_slot>> slots;
    uint32_t* upload_ptr{};
    size_t upload_size{};
    uint64_t generation{};
    bool presentation_pending{};
    bool resize_deferred{};
    bool shutting_down{};
};
template<class T>
class add_rfb_latency_analyser : public T {
public:
    using parent = T;
    add_rfb_latency_analyser(const configure auto& conf) : parent{conf},
        latency{}
    {
    }
    using clock = std::chrono::steady_clock;
    // Summarises the frames drawn since the last read. A reporter sampling
    // get_rfb_latency() on its own schedule would show whichever single frame
    // happened to land last, which says little when the spread is what hurts.
    struct latency_stats {
        int count;
        double mean_ms;
        double worst_ms;
    };
    // The event loop pipelines: it asks for the next framebuffer before
    // reading the one already sitting in the socket. Timing a draw against the
    // most recent request therefore measures how long the local decode took
    // rather than how long the client waited -- it reported under 2ms against
    // a server measured at 28ms. Requests and framebuffer updates are one to
    // one in RFB, so this queue pairs each drawn frame with the request that
    // actually asked for it.
    auto draw() {
        if (!m_pending.empty()) {
            latency = clock::now() - m_pending.front();
            m_pending.pop_front();
            auto milliseconds =
                std::chrono::duration<double, std::milli>(latency).count();
            total_ms += milliseconds;
            worst_ms = std::max(worst_ms, milliseconds);
            count++;
        }
        return parent::draw();
    }
    void request_framebuffer_update(int x, int y, int width, int height, bool increment_update=true) {
        record_request();
        return parent::request_framebuffer_update(x, y, width, height, increment_update);
    }
    void request_framebuffer_update(int x=0, int y=0) {
        record_request();
        return parent::request_framebuffer_update(x, y);
    }
    auto get_rfb_latency() {
        return latency;
    }
    latency_stats take_rfb_latency_stats() {
        latency_stats stats{count, count > 0 ? total_ms / count : 0.0, worst_ms};
        count = 0;
        total_ms = 0.0;
        worst_ms = 0.0;
        return stats;
    }
private:
    void record_request() {
        // A server that answers none of them must not grow this without bound.
        // Dropping the oldest also resynchronises the pairing rather than
        // reporting an ever growing latency for the rest of the session.
        if (m_pending.size() >= 64) {
            m_pending.pop_front();
        }
        m_pending.push_back(clock::now());
    }
    std::deque<clock::time_point> m_pending;
    clock::duration latency;
    int count{};
    double total_ms{};
    double worst_ms{};
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
    ~add_rfb_reduce_update() {
        if (frame_callback != nullptr) {
            wl_callback_destroy(frame_callback);
        }
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
        t->frame_event(callback);
    }
    void set_callback() {
        assert(frame_callback == nullptr);
        auto wayland_surface = parent::get_wayland_surface();
        frame_callback = wl_surface_frame(wayland_surface);
        static struct wl_callback_listener callback_listener = {
            .done = static_frame_event,
        };
        wl_callback_add_listener(frame_callback, &callback_listener, this);
    }
    void frame_event(wl_callback* callback) {
        assert(callback == frame_callback);
        wl_callback_destroy(frame_callback);
        frame_callback = nullptr;
        if (ignored_update) {
            parent::request_framebuffer_update();
            ignored_update = false;
        }
        wayland_frame = true;
        set_callback();
        // A wl_surface frame callback only fires again after a SUBSEQUENT
        // commit -- but the only other commit in this program is inside
        // draw(), which only runs when a new framebuffer update actually
        // arrived. A new update only arrives in response to a request, and a
        // request is only sent once this callback fires: on an idle remote
        // desktop (nothing to draw) that chain has nothing left to close it,
        // so set_callback() above would register a callback the compositor
        // then never delivers, and the RFB request loop would stop firing
        // FOREVER, not just slow down -- looking exactly like the reported
        // stall (it just takes however long normal post-connect screen
        // activity lasts to first go idle). Committing here, independent of
        // whether new pixels arrived, guarantees this callback keeps
        // completing the loop even when the remote screen never changes.
        wl_surface_commit(parent::get_wayland_surface());
    }
    bool wayland_frame;
    bool ignored_update;
    wl_callback* frame_callback{};
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
        // The server can drop the connection without anything left to read, in
        // which case only POLLHUP/POLLERR is set and the POLLIN branch below
        // would never notice.
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
        if (fds[FDS_INDEX].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            throw rfb::connection_closed{"server closed the connection"};
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
            auto encoding_type = parent::get_encoding();
            auto frame_network_byte_count = parent::get_frame_network_byte_count();
            auto latency = parent::take_rfb_latency_stats();
            // Fixed widths: the line is rewritten in place with a carriage
            // return, so a shorter line would leave the tail of the previous
            // one on screen.
            auto mean = latency.count > 0
                ? std::format("{:6.1f}", latency.mean_ms)
                : std::string{"    --"};
            auto worst = latency.count > 0
                ? std::format("{:6.1f}", latency.worst_ms)
                : std::string{"    --"};
            // Raw at three megabytes a frame can go a whole window without
            // completing one, and printing 0.0 there would read as instant
            // rather than as nothing to report.
            std::clog << std::format(
                    "encoding: {}, bytes: {:8}, latency: {} ms"
                    " (worst {}, {:3} frames)\r",
                    encoding_type, frame_network_byte_count,
                    mean, worst, latency.count);
        }
        parent::process_events(fds);
    }
private:
    clock::time_point previous_time;
};

using app_transport =
    rfb::set_address<
    rfb::set_port<
    posix::add_empty_pollfd_array<
    wayland_helper::add_wayland_surface<
    cpp_helper::add_logger<
    empty_class
    >>>>>;

using app_rfb =
    rfb_trace::add_trace_recorder<
    add_rfb<
    app_transport
    >>;

using app_presentation =
    add_rfb_process_keysym<
    add_rfb_process_pointer<
    wayland_helper::register_size_change_callback<
    add_presentation_copy<
    drm_helper::add_amdgpu_device<
    drm_helper::add_drm_fd<
    app_rfb
    >>>>>>;

using app_protocol =
    add_info_printer<
    rfb_control::add_control_port<
    add_rfb_socket_pollfd<
    add_rfb_reduce_update<
    add_rfb_latency_analyser<
    app_presentation
    >>>>>;

using app_keyboard =
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
    app_protocol
    >>>>>>>>>>;

using app_input =
    wayland_helper::register_pointer_axis_callback<
    wayland_helper::register_pointer_button_callback<
    wayland_helper::register_pointer_motion_callback<
    wayland_helper::register_key_callback<
    wayland_helper::register_keyboard_leave_callback<
    app_keyboard
    >>>>>;

using draw_app =
    wayland_helper::run_wayland_event_loop<
    wayland_helper::add_wayland_pollfds_loop<
    posix::add_poll_events<
    wayland_helper::add_wayland_pollfd<
    app_input
    >>>>;


using namespace std::literals;

// Default DRM device selection rule.
//
// This demo gets its window surface from Wayland (wl_compositor/xdg_wm_base
// etc.); the DRM fd it opens is used for exactly one thing, amdgpu buffer
// allocation/export for the dma-buf it hands to the compositor (see
// add_amdgpu_device/add_drm_fd in drm_helper.hpp and their use in draw_app
// below). That only works on a node we are authorized to do GEM/accel
// ioctls on without being DRM master. A /dev/dri/cardN (primary) node
// requires DRM master or explicit auth - normally held by the Wayland
// compositor already - so amdgpu_device_initialize on a card node fails
// with EACCES for an ordinary client, which is exactly the "needs
// /dev/dri/render" failure this function exists to avoid. A
// /dev/dri/renderDN node needs no master/auth for GEM/accel, which is what
// this demo actually needs, so that is what gets probed and picked here:
// enumerate /dev/dri/renderD* in numeric order, try amdgpu_device_initialize
// on each, and return the first path that succeeds. An explicit --gpu path
// always takes priority and is never probed here.
std::string pick_default_drm_device() {
    std::vector<std::string> candidates;
    if (DIR* dir = opendir("/dev/dri")) {
        while (dirent* entry = readdir(dir)) {
            std::string name = entry->d_name;
            if (name.rfind("renderD", 0) == 0) {
                candidates.push_back("/dev/dri/" + name);
            }
        }
        closedir(dir);
    }
    std::sort(candidates.begin(), candidates.end());

    std::string probe_log;
    for (auto& path : candidates) {
        int fd = open(path.c_str(), O_RDWR);
        if (fd == -1) {
            probe_log += std::format("  {}: open failed: {}\n", path, strerror(errno));
            continue;
        }
        amdgpu_device_handle handle{};
        uint32_t major_version{}, minor_version{};
        int ret = amdgpu_device_initialize(fd, &major_version, &minor_version, &handle);
        if (ret < 0) {
            probe_log += std::format(
                "  {}: amdgpu_device_initialize failed: {}\n", path, strerror(-ret));
            close(fd);
            continue;
        }
        amdgpu_device_deinitialize(handle);
        close(fd);
        return path;
    }

    if (candidates.empty()) {
        throw std::runtime_error{
            "no DRM render nodes (/dev/dri/renderD*) found; pass --gpu "
            "<path> to select a device explicitly, or check that the "
            "amdgpu kernel driver is loaded"};
    }
    throw std::runtime_error{std::format(
        "no usable DRM render node found among {} candidate(s) under "
        "/dev/dri:\n{}"
        "pass --gpu <path> to select one explicitly, or check permissions "
        "(you may need to be in the 'render' group)",
        candidates.size(), probe_log)};
}

std::string read_password_file(const char* path) {
    std::ifstream in{path};
    if (!in) {
        throw std::logic_error(std::format("could not open password file: {}", path));
    }
    std::string password;
    std::getline(in, password);
    while (!password.empty() && (password.back() == '\r' || password.back() == '\n')) {
        password.pop_back();
    }
    return password;
}

struct config : public empty_configure {
    const char* address;
    uint16_t port;
    std::vector<uint32_t> supported_encodings;
    const char* enabled_logs;
    int32_t width;
    int32_t height;
    const char* drm_device_path;
    bool view_only;
    std::string password;
    uint16_t control_port;
    presentation_mode presentation;
};

int main(int argc, const char* argv[]) {
  try {
    auto usage_string =
                "Usage: rfb_window_demo <address> <port> <encodings>\n"
                "<encodings> is comma separated, by name or ID, e.g.\n"
                "  h264,zrle,raw,quality9   or   50,16,0,-23\n"
                "  quality0..quality9 set the H264 quality level\n"
                "--log <enabled_logs>\n"
                "--width <width>\n"
                "--height <height>\n"
                "--gpu <drm_device_path>  override DRM device probing;\n"
                "                        default is to probe\n"
                "                        /dev/dri/renderD* and pick the\n"
                "                        first that initializes amdgpu\n"
                "--view-only\n"
                "--password <password>\n"
                "--password-file <path>\n"
                "--control-port <port>   accept line-oriented control\n"
                "                        commands on 127.0.0.1:<port>\n"
                "                        (off unless given)\n"
                "--presentation-mode <legacy|release-gated>\n"
                "  legacy               repeatedly update one attached buffer\n"
                "                        (default; fullscreen compatibility)\n"
                "  release-gated        rotate three buffers and reuse only\n"
                "                        after wl_buffer.release\n"
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
    bool view_only = false;
    std::string password{};
    uint16_t control_port = 0;
    auto presentation = presentation_mode::release_gated;
    auto require_value = [&](int i, std::string_view option) -> const char* {
        if (i + 1 >= argc) {
            throw std::logic_error{std::format("{} requires a value", option)};
        }
        return argv[i + 1];
    };
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
            else if (strcmp(&argv[i][2], "view-only") == 0) {
                view_only = true;
            }
            else if (strcmp(&argv[i][2], "password") == 0) {
                password = argv[i+1];
                i += 1;
            }
            else if (strcmp(&argv[i][2], "control-port") == 0) {
                control_port = strtol(argv[i+1], NULL, 10);
                i += 1;
            }
            else if (strcmp(&argv[i][2], "password-file") == 0) {
                password = read_password_file(argv[i+1]);
                i += 1;
            }
            else if (strcmp(&argv[i][2], "presentation-mode") == 0) {
                std::string_view value = require_value(i, "--presentation-mode");
                if (value == "legacy") {
                    presentation = presentation_mode::legacy;
                } else if (value == "release-gated") {
                    presentation = presentation_mode::release_gated;
                } else {
                    throw std::logic_error{std::format(
                            "unknown presentation mode: {}", value)};
                }
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
                auto parsed = rfb::parse_encodings(argv[i]);
                if (!parsed) {
                    throw std::runtime_error{
                        std::string{"unknown encoding: "} + argv[i]};
                }
                for (auto v : *parsed) {
                    encodings.emplace_back(
                        rfb::to_big_endian(static_cast<uint32_t>(v)));
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
    // Advertise the private scale extension. A server that does not know it
    // ignores it like any other unsupported pseudo-encoding, and this viewer
    // then never sends a scale request, so nothing is wedged either way.
    encodings.emplace_back(rfb::to_big_endian(
            static_cast<uint32_t>(rfb::encoding::scale_ext)));
    encodings.emplace_back(rfb::to_big_endian(
            static_cast<uint32_t>(rfb::encoding::desktop_size)));
    auto conf = config{
        .address=address, .port=port,
        .supported_encodings = std::move(encodings),
        .enabled_logs = enabled_logs,
        .width = width,
        .height = height,
        .drm_device_path = drm_device_path,
        .view_only = view_only,
        .password = std::move(password),
        .control_port = control_port,
        .presentation = presentation,
    };
    auto app = draw_app{conf};
  } catch (rfb::connection_closed &e) {
    // Normal termination, not a failure: the server went away, so there is
    // nothing left to display.
    std::cerr << e.what() << std::endl;
    return 0;
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
  return 0;
}
