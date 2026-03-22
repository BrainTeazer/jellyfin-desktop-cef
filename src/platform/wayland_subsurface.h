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
    [[nodiscard]] bool startFrame(VkImage*, VkImageView*, VkFormat*) override { return false; }
    void submitFrame() override {}

    // Accessors
    [[nodiscard]] wl_display* display() const { return wl_display_; }
    [[nodiscard]] wl_surface* surface() const { return mpv_surface_; }
    [[nodiscard]] VkFormat    swapchainFormat() const override { return VK_FORMAT_UNDEFINED; }
    [[nodiscard]] VkExtent2D  swapchainExtent() const override { return swapchain_extent_; }
    [[nodiscard]] bool        isHdr() const override { return true; }
    [[nodiscard]] uint32_t    width() const override { return swapchain_extent_.width; }
    [[nodiscard]] uint32_t    height() const override { return swapchain_extent_.height; }

    // Vulkan handles for mpv
    [[nodiscard]] VkInstance                       vkInstance() const override { return instance_; }
    [[nodiscard]] VkPhysicalDevice                 vkPhysicalDevice() const override { return physical_device_; }
    [[nodiscard]] VkDevice                         vkDevice() const override { return device_; }
    [[nodiscard]] VkQueue                          vkQueue() const override;
    [[nodiscard]] uint32_t                         vkQueueFamily() const override;
    [[nodiscard]] PFN_vkGetInstanceProcAddr        vkGetProcAddr() const override { return vkGetInstanceProcAddr; }
    [[nodiscard]] const VkPhysicalDeviceFeatures2* features() const override { return &features2_; }
    [[nodiscard]] const char* const*               deviceExtensions() const override;
    [[nodiscard]] int                              deviceExtensionCount() const override;

    [[nodiscard]] VkSurfaceKHR               vkSurface() const { return vk_surface_; }
    [[nodiscard]] const mpv_display_profile& displayProfile() const { return display_profile_; }

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

    wl_display*       wl_display_       = nullptr;
    wl_compositor*    wl_compositor_    = nullptr;
    wl_subcompositor* wl_subcompositor_ = nullptr;
    wl_surface*       mpv_surface_      = nullptr;
    wl_subsurface*    mpv_subsurface_   = nullptr;

    wp_viewporter* viewporter_ = nullptr;
    wp_viewport*   viewport_   = nullptr;

    VkInstance       instance_        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice         device_          = VK_NULL_HANDLE;
    VkQueue          queue_           = VK_NULL_HANDLE;
    uint32_t         queue_family_    = 0;
    VkSurfaceKHR     vk_surface_      = VK_NULL_HANDLE;

    VkPhysicalDeviceVulkan11Features vk11_features_{};
    VkPhysicalDeviceVulkan12Features vk12_features_{};
    VkPhysicalDeviceFeatures2        features2_{};
    std::vector<const char*>         enabled_extensions_;

    wp_color_manager_v1* color_manager_   = nullptr;
    wl_output*           wl_output_       = nullptr;
    mpv_display_profile  display_profile_ = {};

    VkExtent2D swapchain_extent_ = {0, 0};

    std::atomic<int>  pending_dest_width_{0};
    std::atomic<int>  pending_dest_height_{0};
    std::atomic<bool> dest_pending_{false};
};
