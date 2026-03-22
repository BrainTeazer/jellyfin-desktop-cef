#pragma once

#define VK_USE_PLATFORM_WAYLAND_KHR
#include "video_surface.h"
#include "wayland-protocols/color-management-v1-client.h"
#include "wayland-protocols/viewporter-client.h"
#include <atomic>
#include <mpv/render_vk.h>
#include <vector>
#include <vulkan/vulkan.h>
#include <wayland-client.h>

struct SDL_Window;

// Wayland subsurface with libplacebo-managed swapchain.
// We create VkInstance/VkDevice/VkSurface and pass the VkSurface to mpv.
// mpv's internal libplacebo creates the swapchain (identical to standalone mpv),
// handling format selection, color management, and display profile negotiation.
// We do NOT create our own swapchain or color management surface.
class WaylandSubsurface : public VideoSurface {
public:
    WaylandSubsurface();
    ~WaylandSubsurface() override;

    [[nodiscard]] bool init(SDL_Window*                      window,
              VkInstance                       instance,
              VkPhysicalDevice                 physicalDevice,
              VkDevice                         device,
              uint32_t                         queueFamily,
              const char* const*               extensions,
              int                              numExtensions,
              const VkPhysicalDeviceFeatures2* features) override;
    [[nodiscard]] bool createSwapchain(int width, int height) override;
    [[nodiscard]] bool recreateSwapchain(int width, int height) override;
    void cleanup() override;

    // Not used — mpv handles frame acquisition/presentation via pl_swapchain
    [[nodiscard]] bool startFrame(VkImage* /*outImage*/, VkImageView* /*outView*/, VkFormat* /*outFormat*/) override { return false; }
    void submitFrame() override {}

    // Accessors
    [[nodiscard]] wl_display* display() const { return m_wl_display; }
    [[nodiscard]] wl_surface* surface() const { return m_mpv_surface; }
    [[nodiscard]] VkFormat    swapchainFormat() const override { return VK_FORMAT_UNDEFINED; }
    [[nodiscard]] VkExtent2D  swapchainExtent() const override { return m_swapchain_extent; }
    [[nodiscard]] bool        isHdr() const override { return true; }
    [[nodiscard]] uint32_t    width() const override { return m_swapchain_extent.width; }
    [[nodiscard]] uint32_t    height() const override { return m_swapchain_extent.height; }

    // Vulkan handles for mpv
    [[nodiscard]] VkInstance                       vkInstance() const override { return m_instance; }
    [[nodiscard]] VkPhysicalDevice                 vkPhysicalDevice() const override { return m_physical_device; }
    [[nodiscard]] VkDevice                         vkDevice() const override { return m_device; }
    [[nodiscard]] VkQueue                          vkQueue() const override;
    [[nodiscard]] uint32_t                         vkQueueFamily() const override;
    [[nodiscard]] PFN_vkGetInstanceProcAddr        vkGetProcAddr() const override { return vkGetInstanceProcAddr; }
    [[nodiscard]] const VkPhysicalDeviceFeatures2* features() const override { return &m_features2; }
    [[nodiscard]] const char* const*               deviceExtensions() const override;
    [[nodiscard]] int                              deviceExtensionCount() const override;

    [[nodiscard]] VkSurfaceKHR               vkSurface() const { return m_vk_surface; }
    [[nodiscard]] const mpv_display_profile& displayProfile() const { return m_display_profile; }

    void commit();
    void hide() override;
    void setColorspace() override {} // Mesa handles via swapchain
    void setDestinationSize(int width, int height) override;
    void initDestinationSize(int width, int height);

    static void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    static void registryGlobalRemove(void* data, wl_registry* registry, uint32_t name);

private:
    [[nodiscard]] bool initWayland(SDL_Window* window);
    [[nodiscard]] bool createSubsurface(wl_surface* parentSurface);
    void queryDisplayProfile();

    wl_display*       m_wl_display       = nullptr;
    wl_compositor*    m_wl_compositor    = nullptr;
    wl_subcompositor* m_wl_subcompositor = nullptr;
    wl_surface*       m_mpv_surface      = nullptr;
    wl_subsurface*    m_mpv_subsurface   = nullptr;

    wp_viewporter* m_viewporter = nullptr;
    wp_viewport*   m_viewport   = nullptr;

    VkInstance       m_instance        = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice         m_device          = VK_NULL_HANDLE;
    VkQueue          m_queue           = VK_NULL_HANDLE;
    uint32_t         m_queue_family    = 0;
    VkSurfaceKHR     m_vk_surface      = VK_NULL_HANDLE;

    VkPhysicalDeviceVulkan11Features m_vk11_features{};
    VkPhysicalDeviceVulkan12Features m_vk12_features{};
    VkPhysicalDeviceFeatures2        m_features2{};
    std::vector<const char*>         m_enabled_extensions;

    wp_color_manager_v1* m_color_manager   = nullptr;
    wl_output*           m_wl_output       = nullptr;
    mpv_display_profile  m_display_profile = {};

    VkExtent2D m_swapchain_extent = {.width=0, .height=0};

    std::atomic<int>  m_pending_dest_width{0};
    std::atomic<int>  m_pending_dest_height{0};
    std::atomic<bool> m_dest_pending{false};
};
