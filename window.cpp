#ifdef WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#else
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#include "vulkan_start.hpp"

using namespace vulkan_start;
using namespace vulkan_hpp_helper;

#ifdef WIN32
constexpr auto PLATFORM = vulkan_start::platform::win32;
#else
constexpr auto PLATFORM = vulkan_start::platform::wayland;
#endif

template <class T> class record_swapchain_command_buffers : public T {
public:
  using parent = T;
  record_swapchain_command_buffers() { create(); }
  void create() {
    auto buffers = parent::get_swapchain_command_buffers();
    auto swapchain_images = parent::get_swapchain_images();
    auto queue_family_index = parent::get_queue_family_index();
    auto image_buffers = parent::get_buffer_vector();

    if (buffers.size() != swapchain_images.size()) {
      throw std::runtime_error{
          "swapchain images count != command buffers count"};
    }
    uint32_t index = 0;
    for (uint32_t index = 0; index < buffers.size(); index++) {
      vk::Image swapchain_image = swapchain_images[index];
      vk::CommandBuffer cmd = buffers[index];

      cmd.begin(vk::CommandBufferBeginInfo{});
      vk::Extent2D swapchain_image_extent =
          parent::get_swapchain_image_extent();
      auto render_area = vk::Rect2D{}
                             .setOffset(vk::Offset2D{0, 0})
                             .setExtent(swapchain_image_extent);
      auto subresource_range = vk::ImageSubresourceRange{}.setAspectMask(vk::ImageAspectFlagBits::eColor).setLevelCount(1).setLayerCount(1);
      auto subresource_layers = vk::ImageSubresourceLayers{}.setAspectMask(vk::ImageAspectFlagBits::eColor).setLayerCount(1);
      {
          auto image_memory_barrier = vk::ImageMemoryBarrier2{}
                .setOldLayout(vk::ImageLayout::eUndefined).setNewLayout(vk::ImageLayout::eTransferDstOptimal)
                .setImage(swapchain_image)
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
              .setImageExtent(vk::Extent3D{swapchain_image_extent.width, swapchain_image_extent.height, 1});
      cmd.copyBufferToImage(image_buffer, swapchain_image, vk::ImageLayout::eTransferDstOptimal, 1,
              &buffer_image_copy
              );
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

template <class T> class add_dynamic_draw : public T {
public:
  using parent = T;
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
    for (int y = 0; y < 1080; y++) {
        for (int x = 0; x < 1920; x++) {
            upload_ptr[y*1920+x] = 0x00ffff00;
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
  auto get_vector_size() { return parent::get_swapchain_images().size(); }
};

template <class T> class add_cube_swapchain_and_pipeline_layout
  : public
    map_buffer_memory_vector<
    add_buffer_memory_vector<
    set_buffer_memory_properties<vk::MemoryPropertyFlagBits::eHostVisible,
    add_buffer_vector<
    add_buffer_usage<vk::BufferUsageFlagBits::eTransferSrc,
    empty_buffer_usage<
    set_buffer_size<1920*1080*4,
    set_vector_size_to_swapchain_image_count<
	add_recreate_surface_for<
	add_swapchain_images_views<
	add_recreate_surface_for<
	add_swapchain_images<
	add_recreate_surface_for<
	add_swapchain<
	add_swapchain_image_format<
  T
  >>>>>>>>>>>>>>>
{};

template<class T>
class add_queue_wait_idle_to_recreate_surface : public T {
public:
    using parent = T;
    void recreate_surface() {
        auto queue = parent::get_queue();
        queue.waitIdle();
        parent::recreate_surface();
    }
};

template <class F, class T> class add_process_suboptimal_image : public T {
public:
    using parent = T;
    void process_suboptimal_image() {
        F f;
        f(this);
    }
};

template <class T> class add_get_time : public T {
public:
    add_get_time() : m_start_time{std::chrono::steady_clock::now()}{
    }
    auto get_time() {
        return std::chrono::steady_clock::now() - m_start_time;
    }
private:
    std::chrono::steady_clock::time_point m_start_time;
};

template<class T>
class add_physical_device_and_device_and_draw
    : public
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
	add_cube_swapchain_and_pipeline_layout<
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
  >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
{};

using draw_app =
	vulkan_start::run_on_platform<
        PLATFORM, add_physical_device_and_device_and_draw
	>
	;

using namespace std::literals;

int main(int argc, const char* argv[]) {
  try {
      auto app = draw_app{};
  } catch (std::exception &e) {
    std::cerr << e.what() << std::endl;
  }
  return 0;
}
