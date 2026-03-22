#include "platform/wayland_subsurface.h"
#include "logging.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstring>
#include <vector>

namespace {
const std::vector<const char*> sRequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
};

const std::vector<const char*> sOptionalDeviceExtensions = {
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_EXT_HDR_METADATA_EXTENSION_NAME,
};

// Image description listener for display profile query
struct ImageDescContext {
    bool ready = false;
};

void descFailed(void* /*unused*/, struct wp_image_description_v1* /*unused*/, uint32_t /*unused*/, const char* /*unused*/) { }

void descReady(void* d, struct wp_image_description_v1* /*unused*/, uint32_t /*unused*/) {
    ((ImageDescContext*)d)->ready = true;
}
void descReady2(void* d, struct wp_image_description_v1* /*unused*/, uint32_t /*unused*/, uint32_t /*unused*/) {
    ((ImageDescContext*)d)->ready = true;
}
const struct wp_image_description_v1_listener sDescListener = {
    .failed = descFailed,
    .ready  = descReady,
#if HAVE_WAYLAND_PROTOCOLS_1_47
    .ready2 = desc_ready2,
#endif
};

// Output info listener
struct OutputInfoCtx {
    bool     done    = false;
    uint32_t max_lum = 0;
    uint32_t min_lum = 0;
    uint32_t ref_lum = 0;
};

const struct wp_image_description_info_v1_listener sInfoListener = {
    .done     = [](void* d, struct wp_image_description_info_v1*) { ((OutputInfoCtx*)d)->done = true; },
    .icc_file = [](void*, struct wp_image_description_info_v1*, int32_t, uint32_t) { },
    .primaries = [](void*, struct wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { },
    .primaries_named = [](void*, struct wp_image_description_info_v1*, uint32_t) { },
    .tf_power        = [](void*, struct wp_image_description_info_v1*, uint32_t) { },
    .tf_named        = [](void*, struct wp_image_description_info_v1*, uint32_t) { },
    .luminances =
        [](void* d, struct wp_image_description_info_v1*, uint32_t min, uint32_t max, uint32_t ref) {
            auto* c    = (OutputInfoCtx*)d;
            c->min_lum = min;
            c->max_lum = max;
            c->ref_lum = ref;
        },
    .target_primaries
    = [](void*, struct wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { },
    .target_luminance = [](void*, struct wp_image_description_info_v1*, uint32_t, uint32_t) { },
    .target_max_cll   = [](void*, struct wp_image_description_info_v1*, uint32_t) { },
    .target_max_fall  = [](void*, struct wp_image_description_info_v1*, uint32_t) { },
};

const wl_registry_listener sRegistryListener = {
    .global        = WaylandSubsurface::registryGlobal,
    .global_remove = WaylandSubsurface::registryGlobalRemove,
};
} // namespace

;

WaylandSubsurface::WaylandSubsurface() = default;
WaylandSubsurface::~WaylandSubsurface() {
    cleanup();
}

void WaylandSubsurface::registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* self = static_cast<WaylandSubsurface*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        self->m_wl_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        self->m_wl_subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
        self->m_color_manager
            = static_cast<wp_color_manager_v1*>(wl_registry_bind(registry, name, &wp_color_manager_v1_interface, std::min(version, 1u)));
    } else if (strcmp(interface, wl_output_interface.name) == 0 && !self->m_wl_output) {
        self->m_wl_output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 1));
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        self->m_viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    }
}

void WaylandSubsurface::registryGlobalRemove(void* /*unused*/, wl_registry* /*unused*/, uint32_t /*unused*/) { }

bool WaylandSubsurface::initWayland(SDL_Window* window) {
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props)
        return false;

    m_wl_display = static_cast<wl_display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto* parent = static_cast<wl_surface*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (!m_wl_display || !parent)
        return false;

    wl_registry* reg = wl_display_get_registry(m_wl_display);
    wl_registry_add_listener(reg, &sRegistryListener, this);
    wl_display_roundtrip(m_wl_display);
    wl_registry_destroy(reg);

    if (!m_wl_compositor || !m_wl_subcompositor)
        return false;
    return createSubsurface(parent);
}

bool WaylandSubsurface::createSubsurface(wl_surface* parent) {
    m_mpv_surface = wl_compositor_create_surface(m_wl_compositor);
    if (!m_mpv_surface)
        return false;

    m_mpv_subsurface = wl_subcompositor_get_subsurface(m_wl_subcompositor, m_mpv_surface, parent);
    if (!m_mpv_subsurface)
        return false;

    wl_subsurface_set_position(m_mpv_subsurface, 0, 0);
    wl_subsurface_place_below(m_mpv_subsurface, parent);
    wl_subsurface_set_desync(m_mpv_subsurface);

    wl_region* empty = wl_compositor_create_region(m_wl_compositor);
    wl_surface_set_input_region(m_mpv_surface, empty);
    wl_region_destroy(empty);

    if (m_viewporter)
        m_viewport = wp_viewporter_get_viewport(m_viewporter, m_mpv_surface);

    wl_surface_commit(m_mpv_surface);
    wl_display_roundtrip(m_wl_display);
    return true;
}

bool WaylandSubsurface::init(SDL_Window* window,
                             VkInstance /*instance*/,
                             VkPhysicalDevice /*physicalDevice*/,
                             VkDevice /*device*/,
                             uint32_t /*queueFamily*/,
                             const char* const* /*extensions*/,
                             int /*numExtensions*/,
                             const VkPhysicalDeviceFeatures2* /*features*/) {
    if (!initWayland(window))
        return false;

    // Query display HDR profile (for mpv's libplacebo rendering target)
    queryDisplayProfile();

    // No color management surface — Mesa creates one via the swapchain.
    // This matches standalone mpv where only Mesa's WSI handles color management.

    const std::array<char*, 5> instanceExts{{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    }};

    VkApplicationInfo appInfo{};
    appInfo.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instInfo{};
    instInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo        = &appInfo;
    instInfo.enabledExtensionCount   = 5;
    instInfo.ppEnabledExtensionNames = reinterpret_cast<const char* const*>(instanceExts.data());

    if (vkCreateInstance(&instInfo, nullptr, &m_instance) != VK_SUCCESS)
        return false;

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &gpuCount, nullptr);
    if (!gpuCount)
        return false;
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(m_instance, &gpuCount, gpus.data());
    m_physical_device = gpus[0];

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> avail(extCount);
    vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &extCount, avail.data());

    auto has = [&](const char* n) {
        for (auto& e : avail)
            if (strcmp(e.extensionName, n) == 0)
                return true;
        return false;
    };

    m_enabled_extensions.clear();

    for (auto& e : sRequiredDeviceExtensions) {
        if (!has(e)) {
            LOG_ERROR(LOG_PLATFORM, "Missing: %s", e);
            return false;
        }
        m_enabled_extensions.push_back(e);
    }
    for (auto& e : sOptionalDeviceExtensions)
        if (has(e))
            m_enabled_extensions.push_back(e);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &qfCount, qfs.data());
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_queue_family = i;
            break;
        }
    }

    float                   prio = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = m_queue_family;
    qi.queueCount       = 1;
    qi.pQueuePriorities = &prio;

    m_vk11_features                        = {};
    m_vk11_features.sType                  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    m_vk11_features.samplerYcbcrConversion = VK_TRUE;
    m_vk12_features                        = {};
    m_vk12_features.sType                  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    m_vk12_features.pNext                  = &m_vk11_features;
    m_vk12_features.timelineSemaphore      = VK_TRUE;
    m_vk12_features.hostQueryReset         = VK_TRUE;
    m_features2                            = {};
    m_features2.sType                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    m_features2.pNext                      = &m_vk12_features;

    VkDeviceCreateInfo di{};
    di.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.pNext                   = &m_features2;
    di.queueCreateInfoCount    = 1;
    di.pQueueCreateInfos       = &qi;
    di.enabledExtensionCount   = (uint32_t)m_enabled_extensions.size();
    di.ppEnabledExtensionNames = m_enabled_extensions.data();

    if (vkCreateDevice(m_physical_device, &di, nullptr, &m_device) != VK_SUCCESS)
        return false;
    vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

    VkWaylandSurfaceCreateInfoKHR si{};
    si.sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    si.display = m_wl_display;
    si.surface = m_mpv_surface;
    auto fn    = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(vkGetInstanceProcAddr(m_instance, "vkCreateWaylandSurfaceKHR"));
    if (!fn || fn(m_instance, &si, nullptr, &m_vk_surface) != VK_SUCCESS)
        return false;

    LOG_INFO(LOG_PLATFORM, "Vulkan subsurface initialized (libplacebo swapchain mode)");
    return true;
}

bool WaylandSubsurface::createSwapchain(int width, int height) {
    m_swapchain_extent = {.width = (uint32_t)width, .height = (uint32_t)height};
    return true;
}

bool WaylandSubsurface::recreateSwapchain(int width, int height) {
    m_swapchain_extent = {.width = (uint32_t)width, .height = (uint32_t)height};
    if (m_viewport && m_dest_pending.exchange(false, std::memory_order_acquire)) {
        wp_viewport_set_destination(m_viewport,
                                    m_pending_dest_width.load(std::memory_order_relaxed),
                                    m_pending_dest_height.load(std::memory_order_relaxed));
    }
    return true;
}

void WaylandSubsurface::queryDisplayProfile() {
    if (!m_color_manager || !m_wl_output)
        return;

    auto* cmOut = wp_color_manager_v1_get_output(m_color_manager, m_wl_output);
    if (!cmOut)
        return;

    auto* desc = wp_color_management_output_v1_get_image_description(cmOut);
    if (!desc) {
        wp_color_management_output_v1_destroy(cmOut);
        return;
    }

    ImageDescContext dc{};
    wp_image_description_v1_add_listener(desc, &sDescListener, &dc);
    wl_display_roundtrip(m_wl_display);
    if (!dc.ready) {
        wp_image_description_v1_destroy(desc);
        wp_color_management_output_v1_destroy(cmOut);
        return;
    }

    auto* info = wp_image_description_v1_get_information(desc);
    if (!info) {
        wp_image_description_v1_destroy(desc);
        wp_color_management_output_v1_destroy(cmOut);
        return;
    }

    OutputInfoCtx ic{};
    wp_image_description_info_v1_add_listener(info, &sInfoListener, &ic);
    wl_display_roundtrip(m_wl_display);

    if (ic.max_lum > 0 && ic.ref_lum > 0) {
        m_display_profile.max_luma = (float)ic.max_lum;
        m_display_profile.min_luma = (float)ic.min_lum / 10000.0f;
        m_display_profile.ref_luma = (float)ic.ref_lum;

        LOG_INFO(LOG_PLATFORM,
                 "Display: max=%.0f min=%.4f ref=%.0f nits",
                 m_display_profile.max_luma,
                 m_display_profile.min_luma,
                 m_display_profile.ref_luma);
    }

    wp_image_description_v1_destroy(desc);
    wp_color_management_output_v1_destroy(cmOut);
}

void WaylandSubsurface::commit() {
    wl_surface_commit(m_mpv_surface);
    wl_display_flush(m_wl_display);
}

void WaylandSubsurface::hide() {
    if (!m_mpv_surface)
        return;
    wl_surface_attach(m_mpv_surface, nullptr, 0, 0);
    wl_surface_commit(m_mpv_surface);
    wl_display_flush(m_wl_display);
}

void WaylandSubsurface::initDestinationSize(int w, int h) {
    if (m_viewport && w > 0 && h > 0)
        wp_viewport_set_destination(m_viewport, w, h);
}

void WaylandSubsurface::setDestinationSize(int w, int h) {
    if (m_viewport && w > 0 && h > 0) {
        m_pending_dest_width.store(w, std::memory_order_relaxed);
        m_pending_dest_height.store(h, std::memory_order_relaxed);
        m_dest_pending.store(true, std::memory_order_release);
    }
}

void WaylandSubsurface::cleanup() {
    if (m_color_manager) {
        wp_color_manager_v1_destroy(m_color_manager);
        m_color_manager = nullptr;
    }
    if (m_wl_output) {
        wl_output_destroy(m_wl_output);
        m_wl_output = nullptr;
    }
    if (m_vk_surface && m_instance) {
        vkDestroySurfaceKHR(m_instance, m_vk_surface, nullptr);
        m_vk_surface = VK_NULL_HANDLE;
    }
    if (m_device) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
    if (m_viewport) {
        wp_viewport_destroy(m_viewport);
        m_viewport = nullptr;
    }
    if (m_viewporter) {
        wp_viewporter_destroy(m_viewporter);
        m_viewporter = nullptr;
    }
    if (m_mpv_subsurface) {
        wl_subsurface_destroy(m_mpv_subsurface);
        m_mpv_subsurface = nullptr;
    }
    if (m_mpv_surface) {
        wl_surface_destroy(m_mpv_surface);
        m_mpv_surface = nullptr;
    }
    m_wl_compositor    = nullptr;
    m_wl_subcompositor = nullptr;
    m_wl_display       = nullptr;
}

VkQueue WaylandSubsurface::vkQueue() const {
    return m_queue;
}
uint32_t WaylandSubsurface::vkQueueFamily() const {
    return m_queue_family;
}
const char* const* WaylandSubsurface::deviceExtensions() const {
    return m_enabled_extensions.data();
}
int WaylandSubsurface::deviceExtensionCount() const {
    return (int)m_enabled_extensions.size();
}
