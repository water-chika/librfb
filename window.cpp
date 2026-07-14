#ifdef WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#include "vulkan_start.hpp"
#include "xkb_helper.hpp"
#include <posix.hpp>

#include "linux/input-event-codes.h"

#include "rfb.hpp"

#include <memory>

using namespace vulkan_start;
using namespace vulkan_hpp_helper;
using cpp_helper::configure;
using cpp_helper::empty_configurable_class;

#ifdef WIN32
constexpr auto PLATFORM = vulkan_start::platform::win32;
#else
constexpr auto PLATFORM = vulkan_start::platform::wayland;
#endif

template <class T> class record_swapchain_command_buffers : public T {
public:
  using parent = T;
  record_swapchain_command_buffers(const configure auto& conf) : parent{conf} {
      create();
  }
  void create() {
    auto buffers = parent::get_swapchain_command_buffers();
    auto swapchain_images = parent::get_swapchain_images();
    auto queue_family_index = parent::get_queue_family_index();
    auto image_buffers = parent::get_buffer_vector();
    auto images = parent::get_images();

    vk::Extent2D swapchain_image_extent =
        parent::get_swapchain_image_extent();
    vk::Extent3D image_extent = parent::get_image_extent();

    if (buffers.size() != swapchain_images.size()) {
      throw std::runtime_error{
          "swapchain images count != command buffers count"};
    }
    uint32_t index = 0;
    for (uint32_t index = 0; index < buffers.size(); index++) {
      vk::Image swapchain_image = swapchain_images[index];
      vk::CommandBuffer cmd = buffers[index];
      vk::Image image = images[index];

      cmd.begin(vk::CommandBufferBeginInfo{});
      auto render_area = vk::Rect2D{}
                             .setOffset(vk::Offset2D{0, 0})
                             .setExtent(swapchain_image_extent);
      auto subresource_range = vk::ImageSubresourceRange{}.setAspectMask(vk::ImageAspectFlagBits::eColor).setLevelCount(1).setLayerCount(1);
      auto subresource_layers = vk::ImageSubresourceLayers{}.setAspectMask(vk::ImageAspectFlagBits::eColor).setLayerCount(1);
      {
          auto image_memory_barrier = vk::ImageMemoryBarrier2{}
                .setOldLayout(vk::ImageLayout::eUndefined).setNewLayout(vk::ImageLayout::eTransferDstOptimal)
                .setImage(image)
                .setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe)
                .setSrcAccessMask(vk::AccessFlagBits2::eNone)
                .setDstStageMask(vk::PipelineStageFlagBits2::eTransfer)
                .setDstAccessMask(vk::AccessFlagBits2::eTransferWrite)
                .setSubresourceRange(subresource_range);
          cmd.pipelineBarrier2(
                  vk::DependencyInfo{}.setImageMemoryBarriers(image_memory_barrier)
                  );
      }
      auto image_buffer = image_buffers[index];
      auto buffer_image_copy = vk::BufferImageCopy{}
              .setBufferOffset(0)
              .setBufferRowLength(image_extent.width)
              .setBufferImageHeight(image_extent.height)
              .setImageSubresource(subresource_layers)
              .setImageExtent(vk::Extent3D{image_extent.width, image_extent.height, 1});
      cmd.copyBufferToImage(image_buffer, image, vk::ImageLayout::eTransferDstOptimal, 1,
              &buffer_image_copy
              );
      {
          auto image_memory_barriers = std::array<vk::ImageMemoryBarrier2, 2>{
              vk::ImageMemoryBarrier2{}
                .setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
                .setImage(image)
                .setSrcStageMask(vk::PipelineStageFlagBits2::eTransfer)
                .setSrcAccessMask(vk::AccessFlagBits2::eTransferWrite)
                .setDstStageMask(vk::PipelineStageFlagBits2::eTransfer)
                .setDstAccessMask(vk::AccessFlagBits2::eTransferRead)
                .setSubresourceRange(subresource_range),
              vk::ImageMemoryBarrier2{}
                .setOldLayout(vk::ImageLayout::eUndefined).setNewLayout(vk::ImageLayout::eTransferDstOptimal)
                .setImage(swapchain_image)
                .setSrcStageMask(vk::PipelineStageFlagBits2::eTopOfPipe)
                .setSrcAccessMask(vk::AccessFlagBits2::eNone)
                .setDstStageMask(vk::PipelineStageFlagBits2::eTransfer)
                .setDstAccessMask(vk::AccessFlagBits2::eTransferWrite)
                .setSubresourceRange(subresource_range),
          };
          cmd.pipelineBarrier2(
                  vk::DependencyInfo{}.setImageMemoryBarriers(image_memory_barriers)
                  );
      }
      auto image_blit = vk::ImageBlit{}
        .setSrcSubresource(subresource_layers)
        .setSrcOffsets(std::array<vk::Offset3D,2>{vk::Offset3D{}, vk::Offset3D{(int)image_extent.width, (int)image_extent.height, 1}})
        .setDstSubresource(subresource_layers)
        .setDstOffsets(std::array<vk::Offset3D,2>{vk::Offset3D{}, vk::Offset3D{(int)swapchain_image_extent.width, (int)swapchain_image_extent.height, 1}})
      ;
      cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image, vk::ImageLayout::eTransferDstOptimal,
              1, &image_blit, vk::Filter::eNearest);
      {
          auto image_memory_barrier = vk::ImageMemoryBarrier2{}
                .setOldLayout(vk::ImageLayout::eTransferDstOptimal).setNewLayout(vk::ImageLayout::ePresentSrcKHR)
                .setImage(swapchain_image)
                .setSrcStageMask(vk::PipelineStageFlagBits2::eTransfer)
                .setSrcAccessMask(vk::AccessFlagBits2::eTransferWrite)
                .setDstStageMask(vk::PipelineStageFlagBits2::eBottomOfPipe)
                .setDstAccessMask(vk::AccessFlagBits2::eNone)
                .setSubresourceRange(subresource_range);
          cmd.pipelineBarrier2(
                  vk::DependencyInfo{}.setImageMemoryBarriers(image_memory_barrier)
                  );
      }
      cmd.end();
    }
  }
  void destroy() {}
}; // class record_swapchain_command_buffers in use_app<app::cube>

template<class T>
class add_rfb : public T {
public:
    using parent = T;
    add_rfb(const configure auto& conf) : parent{cpp_helper::increment_configure_log_index_count(conf)},
        rfb{conf}
    {
    }
    constexpr uint32_t get_log_index() {
        return parent::get_log_index() + 1;
    }
    void log(const auto& v) {
        parent::log(get_log_index(), v);
    }
    auto get_rfb(std::span<uint8_t> frame) {
        rfb.get_frame(frame);
    }
    auto process_keysym_event(int keysym, int state) {
        log(std::format("keysym event processing: {}, {}\n", keysym, state));
        rfb.key_event(keysym, state);
    }
    auto& get_rfb() {
        return rfb;
    }
    auto get_fb_width() {
        return rfb.get_width();
    }
    auto get_fb_height() {
        return rfb.get_height();
    }
    void send_pointer_event(uint32_t button_mask, int x, int y) {
        rfb.pointer_event(button_mask, x, y);
    }
    void request_framebuffer_update(int x, int y, int width, int height) {
        rfb.framebuffer_update_request(x, y, width, height);
    }
    void request_framebuffer_update(int x=0, int y=0) {
        request_framebuffer_update(x, y, get_fb_width(), get_fb_height());
    }
    void process_rfb_server_message() {
        rfb.process_server_message();
    }
    auto get_encoding() {
        return rfb.get_encoding();
    }
    auto get_frame_network_byte_count() {
        return rfb.get_frame_network_byte_count();
    }
private:
    using rfb_env =
        rfb::add_rfb<
        rfb::add_process_framebuffer_update<
        rfb::add_decode_h264<
        rfb::add_yuv_to_rgb<
        rfb::add_zrle<
        rfb::init_rfb<
        rfb::add_set_encodings<
        rfb::set_supported_encodings_from_configure<
        rfb::add_set_format<
        rfb::add_server_init<
        rfb::add_client_init<
        rfb::add_connection<
        rfb::set_port<
        rfb::set_address<
        empty_configurable_class
        >>>>>>>>>>>>>>
    ;
    rfb_env rfb;
    int pointer_sended_x;
    int pointer_sended_y;
    int pointer_x;
    int pointer_y;
    int pointer_button_mask;
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
    void request_framebuffer_update(int x, int y, int width, int height) {
        framebuffer_update_request_time = clock::now();
        return parent::request_framebuffer_update(x, y, width, height);
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

template <class T> class add_dynamic_draw : public T {
public:
  using parent = T;
  add_dynamic_draw(const configure auto& conf) : parent{conf} {
  }
  add_dynamic_draw(const add_dynamic_draw&) = delete;
  add_dynamic_draw(add_dynamic_draw&&) = default;
  void draw() {
    vk::Device device = parent::get_device();
    vk::SwapchainKHR swapchain = parent::get_swapchain();
    vk::Queue queue = parent::get_queue();
    vk::Semaphore acquire_image_semaphore =
        parent::get_acquire_next_image_semaphore();
    bool need_recreate_surface = false;

    auto [res, index] =
        device.acquireNextImage2KHR(vk::AcquireNextImageInfoKHR{}
                                        .setSwapchain(swapchain)
                                        .setSemaphore(acquire_image_semaphore)
                                        .setTimeout(UINT64_MAX)
                                        .setDeviceMask(1));
    if (res == vk::Result::eSuboptimalKHR) {
      need_recreate_surface = true;
    } else if (res != vk::Result::eSuccess) {
      throw std::runtime_error{"acquire next image != success"};
    }
    parent::free_acquire_next_image_semaphore(index);

    vk::Fence acquire_next_image_semaphore_fence =
        parent::get_acquire_next_image_semaphore_fence(index);
    {
      vk::Result res = device.waitForFences(acquire_next_image_semaphore_fence,
                                            true, UINT64_MAX);
      if (res != vk::Result::eSuccess) {
        throw std::runtime_error{"failed to wait fences"};
      }
    }
    device.resetFences(acquire_next_image_semaphore_fence);

    std::vector<void*> upload_memory_ptrs = parent::get_buffer_memory_ptr_vector();
    uint8_t* upload_ptr = reinterpret_cast<uint8_t*>(upload_memory_ptrs[index]);
    auto upload_buffer_size = parent::get_buffer_size();
    parent::get_rfb(std::span{upload_ptr, upload_buffer_size});
    auto fb_width = parent::get_fb_width();
    auto fb_height = parent::get_fb_height();
    auto upload_memory_vector = parent::get_buffer_memory_vector();
    auto upload_memory = upload_memory_vector[index];
    device.flushMappedMemoryRanges(vk::MappedMemoryRange{}
            .setMemory(upload_memory)
            .setOffset(0)
            .setSize(vk::WholeSize));

    auto time = parent::get_time();
    auto time_in_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time);
    uint64_t frame_index = time_in_ms.count();

    vk::Semaphore draw_image_semaphore =
        parent::get_draw_image_semaphore(index);
    vk::CommandBuffer buffer = parent::get_swapchain_command_buffer(index);
    vk::PipelineStageFlags wait_stage_mask{
        vk::PipelineStageFlagBits::eTopOfPipe};
    queue.submit(vk::SubmitInfo{}
                     .setCommandBuffers(buffer)
                     .setWaitSemaphores(acquire_image_semaphore)
                     .setWaitDstStageMask(wait_stage_mask)
                     .setSignalSemaphores(draw_image_semaphore),
                 acquire_next_image_semaphore_fence);
    try {
      auto res = queue.presentKHR(vk::PresentInfoKHR{}
                                      .setImageIndices(index)
                                      .setSwapchains(swapchain)
                                      .setWaitSemaphores(draw_image_semaphore));
      if (res == vk::Result::eSuboptimalKHR) {
        need_recreate_surface = true;
      } else if (res != vk::Result::eSuccess) {
        throw std::runtime_error{"present return != success"};
      }
    } catch (vk::OutOfDateKHRError e) {
      need_recreate_surface = true;
    }
    if (need_recreate_surface) {
      parent::process_suboptimal_image();
    }
  }
  ~add_dynamic_draw() {
    vk::Device device = parent::get_device();
    vk::Queue queue = parent::get_queue();
    queue.waitIdle();
  }
};

template <class T> class set_vector_size_to_swapchain_image_count : public T {
public:
  using parent = T;
  set_vector_size_to_swapchain_image_count(const configure auto& conf) : parent{conf} {
  }
  auto get_vector_size() { return parent::get_swapchain_images().size(); }
};


template<typename T>
class set_image_extent_equal_to_fb_extent : public T {
public:
    using parent = T;
    set_image_extent_equal_to_fb_extent(const configure auto& conf) : parent{conf} {
    }
    auto get_image_extent() {
        return vk::Extent3D{parent::get_fb_width(), parent::get_fb_height(), 1};
    }
};

template<typename T>
using add_image_used_to_scale =
    map_image_memory_vector<
    add_images_memories<
    add_image_memory_property<vk::MemoryPropertyFlagBits::eHostVisible,
    add_empty_image_memory_properties<
    add_images<
    add_image_format<vk::Format::eB8G8R8A8Unorm,
    add_image_type<vk::ImageType::e2D,
    add_image_usage<vk::ImageUsageFlagBits::eTransferSrc,
    add_image_usage<vk::ImageUsageFlagBits::eTransferDst,
    add_empty_image_usages<
    set_image_samples<vk::SampleCountFlagBits::e1,
    set_image_tiling<vk::ImageTiling::eLinear,
    set_image_extent_equal_to_fb_extent<
    add_image_count_equal_swapchain_image_count<
    T
    >>>>>>>>>>>>>>
;

template<typename T>
class set_buffer_size_equal_to_fb_size : public T {
public:
    using parent = T;
    set_buffer_size_equal_to_fb_size(const configure auto& conf) : parent{conf} {
    }
    auto get_buffer_size() { return parent::get_fb_width() * parent::get_fb_height() * 4; }
};

template <class T>
using add_swapchain_and_pipeline_layout =
    map_buffer_memory_vector<
    add_buffer_memory_vector<
    set_buffer_memory_properties<vk::MemoryPropertyFlagBits::eHostVisible,
    add_buffer_vector<
    add_buffer_usage<vk::BufferUsageFlagBits::eTransferSrc,
    empty_buffer_usage<
    set_buffer_size_equal_to_fb_size<
    set_vector_size_to_swapchain_image_count<
    add_image_used_to_scale<
    add_rfb_process_pointer<
    add_rfb<
    rfb::set_address<
    rfb::set_port<
	add_recreate_surface_for<
	add_swapchain_images_views<
	add_recreate_surface_for<
	add_swapchain_images<
	add_recreate_surface_for<
	add_swapchain<
	add_swapchain_image_format<
  T
  >>>>>>>>>>>>>>>>>>>>
;

template<class T>
class add_queue_wait_idle_to_recreate_surface : public T {
public:
    using parent = T;
    add_queue_wait_idle_to_recreate_surface(const configure auto& conf) : parent{conf} {
    }
    void recreate_surface() {
        auto queue = parent::get_queue();
        queue.waitIdle();
        parent::recreate_surface();
    }
};

template <class F, class T> class add_process_suboptimal_image : public T {
public:
    using parent = T;
    add_process_suboptimal_image(const configure auto& conf) : parent{conf} {
    }
    void process_suboptimal_image() {
        F f;
        f(this);
    }
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
class add_device_nexts : public T {
public:
    using parent = T;
    add_device_nexts(const configure auto& conf) : parent{conf} {
    }
    using structure_chain = vk::StructureChain<vk::PhysicalDeviceSynchronization2Features>;
    void set_structure_chain(structure_chain& chain) {
        chain.get().setSynchronization2(true);
    }
};

template<class T>
using
add_physical_device_and_device_and_draw =
    add_frame_time_analyser<
    add_dynamic_draw <
    add_get_time <
    add_process_suboptimal_image<
        decltype([](auto* p) {p->recreate_surface();std::cout << "recreate surface" << std::endl;}),
    add_queue_wait_idle_to_recreate_surface<
    add_acquire_next_image_semaphores <
    add_acquire_next_image_semaphore_fences <
    add_draw_semaphores <
    add_recreate_surface_for<
    record_swapchain_command_buffers<
    add_get_format_clear_color_value_type <
    add_recreate_surface_for<
    add_swapchain_command_buffers <
	add_swapchain_and_pipeline_layout<
    typename use_platform_add_swapchain_image_extent<PLATFORM>::template add_swapchain_image_extent<
	add_command_pool <
	add_queue <
	add_device <
    add_device_nexts<
	add_swapchain_extension <
	add_empty_extensions <
	add_find_properties <
	cache_physical_device_memory_properties<
	add_recreate_surface_for<
	cache_surface_capabilities<
	add_recreate_surface_for<
	test_physical_device_support_surface<
	add_queue_family_index <
    vulkan_hpp_helper::add_physical_device<
    add_recreate_surface<
    typename use_platform<PLATFORM>::template add_vulkan_surface<
    T
  >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
;

template<typename T>
class add_rfb_socket_pollfd : public T {
public:
    using parent = T;
    add_rfb_socket_pollfd(const configure auto& conf) : parent{conf} {
        parent::request_framebuffer_update();
    }
    static constexpr int FDS_INDEX = parent::FDS_SIZE;
    static constexpr int FDS_SIZE = parent::FDS_SIZE+1;
    void process_events(auto& fds) {
        auto& rfb = parent::get_rfb();
        assert(fds[FDS_INDEX].fd == rfb.get_socket());
        auto now = std::chrono::steady_clock::now();
        if (fds[FDS_INDEX].revents & POLLIN) {
            parent::request_framebuffer_update();
            parent::update_pointer_position();
            parent::process_rfb_server_message();
            if (rfb.is_frame_updated()) {
                rfb.reset_frame_updated();
                parent::draw();
            }
            previous_time = now;
        }
        else if (now - previous_time > 30ms) {
            parent::update_pointer_position();
            previous_time = now;
        }
        parent::process_events(fds);
    }
    std::vector<pollfd> get_fds() {
        auto fds = parent::get_fds();
        auto& rfb = parent::get_rfb();
        fds.emplace_back(pollfd{
                .fd = rfb.get_socket(),
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
            auto cpu_frame_time = parent::get_cpu_frame_time();
            auto cpu_frame_time_ms = cpu_frame_time / 1000000ns;
            float fps = cpu_frame_time > 0ns ? (100s / cpu_frame_time)/100.0 : 0;
            auto encoding_type = parent::get_encoding();
            auto frame_network_byte_count = parent::get_frame_network_byte_count();
            std::clog << std::format("encoding: {}, bytes: {}, cpu frame time: {:5}ms, fps: {:6}\r",
                    encoding_type, frame_network_byte_count, cpu_frame_time_ms, fps);
        }
        parent::process_events(fds);
    }
private:
    clock::time_point previous_time;
};

using draw_app =
    use_platform<PLATFORM>::template add_pollfds_loop<
    add_info_printer<
    add_rfb_socket_pollfd<
    add_rfb_latency_analyser<
    add_physical_device_and_device_and_draw<
    posix::add_empty_pollfd_array<
    add_instance<
    use_platform<PLATFORM>::template add_platform_needed_extensions<
    add_surface_extension<
    add_empty_extensions<
    use_platform<PLATFORM>::template add_window<
    cpp_helper::add_logger<
    empty_class
    >>>>>>>>>>>>
;

using namespace std::literals;

struct config : public empty_configure {
    const char* address;
    uint16_t port;
    std::vector<uint32_t> supported_encodings;
    const char* enabled_logs;
};

int main(int argc, const char* argv[]) {
  try {
    if (argc < 3) {
        throw std::logic_error("Usage: rfb_window_demo <address> <port> <encoding> --log <enabled_logs>");
    }
    const char* address = "127.0.0.1";
    uint16_t port = 5900;
    uint32_t encoding = 16;
    const char* enabled_logs = "";
    for (int i = 1, pos_arg=0; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (strcmp(&argv[i][2], "log") == 0) {
                enabled_logs = argv[i+1];
                i += 1;
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
                encoding = strtol(argv[i], NULL, 10);
            }
            pos_arg += 1;
        }
    }
    auto conf = config{
        .address=address, .port=port,
        .supported_encodings = { rfb::to_big_endian(encoding) },
        .enabled_logs = enabled_logs
    };
    auto app = draw_app{conf};
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
  }
  return 0;
}
