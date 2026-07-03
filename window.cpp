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
using vulkan_hpp_helper::configure;

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
              .setBufferRowLength(1920)
              .setBufferImageHeight(1080)
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
        .setSrcOffsets(std::array<vk::Offset3D,2>{vk::Offset3D{}, vk::Offset3D{(int)image_extent.width, (int)image_extent.height, 0}})
        .setDstSubresource(subresource_layers)
        .setDstOffsets(std::array<vk::Offset3D,2>{vk::Offset3D{}, vk::Offset3D{(int)swapchain_image_extent.width, (int)swapchain_image_extent.height, 0}})
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
    using tcp = boost::asio::ip::tcp;
    add_rfb(const configure auto& conf) : parent{conf},
        m_io_context{},
        m_resolver{m_io_context},
        m_socket{m_io_context}
    {
        auto endpoints = m_resolver.resolve(parent::get_address(), parent::get_port());
        boost::asio::connect(m_socket, endpoints);

        rfb::rfb_init(m_socket);
        m_server_init_message = rfb::server_init(m_socket);
        rfb::set_format(m_socket);
        rfb::set_encodings(m_socket);
    }
    add_rfb(add_rfb&&) : parent{empty_configure{}},
        m_io_context{},
        m_resolver{m_io_context},
        m_socket{m_io_context}
        {
        std::cerr << "move construct should not be called" << std::endl;
    }
    auto get_rfb() {
        while (true) {
            if (pointer_sended_x != pointer_x || pointer_sended_y != pointer_y) {
                rfb::pointer_event(m_socket, 0, pointer_x, pointer_y);
                pointer_sended_x = pointer_x;
                pointer_sended_y = pointer_y;
            }
            rfb::framebuffer_update_request(m_socket, 0, 0, m_server_init_message.fb_width, m_server_init_message.fb_height);
            auto frame = rfb::process_server_message(m_socket, m_server_init_message.server_pixel_format);
            if (frame.size() > 0) {
                return frame;
            }
        }
    }
    auto& get_socket() {
        return m_socket;
    }
    auto get_socket_fd() {
        return m_socket.native_handle();
    }
    auto& get_server_pixel_format() {
        return m_server_init_message.server_pixel_format;
    }
    auto get_fb_width() {
        return m_server_init_message.fb_width;
    }
    auto get_fb_height() {
        return m_server_init_message.fb_height;
    }
    auto process_key_event(int key, int state) {
        std::cout << "key event processing" << std::endl;
        auto keysym = parent::get_keysym(key);
        std::cout << keysym << std::endl;
        rfb::key_event(m_socket, keysym, state);
    }
    auto process_keysym_event(int keysym, int state) {
        std::cout << "keysym event processing" << std::endl;
        std::cout << keysym << std::endl;
        rfb::key_event(m_socket, keysym, state);
    }
    void process_pointer_motion_event(int x, int y) {
        auto fb_width = get_fb_width();
        auto fb_height = get_fb_height();
        auto [surface_width, surface_height] = parent::get_surface_resolution();
        x = x * fb_width / surface_width;
        y = y * fb_height / surface_height;
        pointer_x = x;
        pointer_y = y;
    }
    void process_pointer_button_event(int button, int button_state) {
        std::cout << "pointer button event processing" << std::endl;
        int button_mask = 0;
        if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
            if (button == BTN_LEFT) {
                button_mask = (1<<0);
            }
            else if (button == BTN_MIDDLE) {
                button_mask = (1<<2);
            }
            else if (button == BTN_RIGHT) {
                button_mask = (1<<2);
            }
            else {
                std::cerr << "unknown pointer button" << std::endl;
            }
        }
        rfb::pointer_event(m_socket, button_mask, pointer_x, pointer_y);
    }
private:
    boost::asio::io_context m_io_context;
    tcp::resolver m_resolver;
    tcp::socket m_socket;
    rfb::server_init_message m_server_init_message;

    int pointer_sended_x;
    int pointer_sended_y;
    int pointer_x;
    int pointer_y;
};

template<typename T>
class add_rfb_pollfd : public T {
public:
    using parent = T;
    static constexpr int FD_INDEX = parent::FDS_SIZE;
    static constexpr int FDS_SIZE = parent::FDS_SIZE+1;
    add_rfb_pollfd(const configure auto& conf) : parent{conf} {
    }
    std::array<pollfd, FDS_SIZE> get_fds() {
        std::array<pollfd, FDS_SIZE> res{};
        auto fds = parent::get_fds();
        std::copy(fds.begin(), fds.end(), res.begin());
        res.back() = pollfd{
            .fd = parent::get_socket_fd(),
            .revents = POLLIN
        };
        return res;
    }

    void process_events(auto& fds) {
        std::cout << "process" << std::endl;
        auto frame = rfb::process_server_message(parent::get_socket(), parent::get_server_pixel_format());
        parent::draw(frame);
        if (fds[FD_INDEX].revents & POLLIN) {
            auto frame = rfb::process_server_message(parent::get_socket(), parent::get_server_pixel_format());
            parent::draw(frame);
        }
        rfb::framebuffer_update_request(parent::get_socket(), 0, 0, parent::get_fb_width(), parent::get_fb_height());
        parent::process_events(fds);
    }
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
    uint32_t* upload_ptr = reinterpret_cast<uint32_t*>(upload_memory_ptrs[index]);
    auto frame = parent::get_rfb();
    for (int y = 0; y < 1080; y++) {
        for (int x = 0; x < 1920; x++) {
            upload_ptr[y*1920+x] = reinterpret_cast<uint32_t*>(frame.data())[y*1920+x];
        }
    }
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
using add_image_used_to_scale =
    map_image_memory_vector<
    add_images_memories<
    add_image_memory_property<vk::MemoryPropertyFlagBits::eHostVisible,
    add_empty_image_memory_properties<
    add_images<
    add_image_format<vk::Format::eB8G8R8A8Unorm,
    add_image_type<vk::ImageType::e2D,
    add_image_usage<vk::ImageUsageFlagBits::eTransferSrc,
    add_empty_image_usages<
    set_image_samples<vk::SampleCountFlagBits::e1,
    set_image_tiling<vk::ImageTiling::eLinear,
    set_image_extent<vk::Extent2D{1920,1080},
    add_image_count_equal_swapchain_image_count<
    T
    >>>>>>>>>>>>>
;

template <class T>
using add_swapchain_and_pipeline_layout =
    map_buffer_memory_vector<
    add_buffer_memory_vector<
    set_buffer_memory_properties<vk::MemoryPropertyFlagBits::eHostVisible,
    add_buffer_vector<
    add_buffer_usage<vk::BufferUsageFlagBits::eTransferSrc,
    empty_buffer_usage<
    set_buffer_size<1920*1080*4,
    set_vector_size_to_swapchain_image_count<
    add_image_used_to_scale<
	add_recreate_surface_for<
	add_swapchain_images_views<
	add_recreate_surface_for<
	add_swapchain_images<
	add_recreate_surface_for<
	add_swapchain<
	add_swapchain_image_format<
  T
  >>>>>>>>>>>>>>>>
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
concept contain_ip_address = requires (T t) {
    t.address;
    t.port;
};

template<typename T>
class set_address : public T {
public:
    using parent = T;
    template<configure Configure>
        requires contain_ip_address<Configure>
    set_address(const Configure& conf) : parent{conf},
        address{conf.address} {
    }
    set_address(const configure auto& conf) : parent{conf} {
    }
    auto get_address() {
        return address;
    }
    const char* address;
};
template<typename T>
class set_port : public T {
public:
    using parent = T;
    template<configure Configure>
        requires contain_ip_address<Configure>
    set_port(const Configure& conf) : parent{conf},
        port{conf.port} {
    }
    set_port(const configure auto& conf) : parent{conf} {
    }
    auto get_port() {
        return port;
    }
    const char* port;
};

template<class T>
using
add_physical_device_and_device_and_draw =
    add_dynamic_draw <
    add_rfb<
    set_address<
    set_port<
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
  >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
;

struct config {
    const char* address;
    const char* port;
};
template<>
struct cpp_helper::is_configure_structure<config> {
    static constexpr bool value = true;
};

using draw_app =
    use_platform<PLATFORM>::template add_event_loop<
    add_physical_device_and_device_and_draw<
    add_instance<
    use_platform<PLATFORM>::template add_platform_needed_extensions<
    add_surface_extension<
    add_empty_extensions<
    use_platform<PLATFORM>::template add_window<
    empty_class
    >>>>>>>
;

using namespace std::literals;

int main(int argc, const char* argv[]) {
  try {
    if (argc < 3) {
        throw std::logic_error("Usage: rfb_window_demo <address> <port>");
    }
    auto address = argv[1];
    auto port = argv[2];
    auto conf = config{.address=address, .port=port};
    auto app = draw_app{conf};
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
  }
  return 0;
}
