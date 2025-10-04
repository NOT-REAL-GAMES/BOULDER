
#include "main.h"
#include "boulder_cgo.h"
#include <iostream>
#include <memory>
#include <unordered_map>
#include <SDL3/SDL.h>
#include <flecs.h>
#include <glm/glm.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "volk.h"

// Global state for the engine
static struct {
    bool initialized = false;
    bool swapchainNeedsRecreate = false;
    bool isRecreatingSwapchain = false;
    bool resizeEventDuringRecreate = false;
    bool shouldClose = false;
    SDL_Window* window = nullptr;
    VkInstance instance = nullptr;
    VkSurfaceKHR surface = nullptr;
    VkPhysicalDevice physicalDevice = nullptr;
    VkDevice device = nullptr;
    VkQueue graphicsQueue = nullptr;
    VkSwapchainKHR swapchain = nullptr;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    VkFormat swapchainFormat;
    VkExtent2D swapchainExtent;
    VkCommandPool commandPool = nullptr;
    std::vector<VkCommandBuffer> commandBuffers;
    VkSemaphore imageAvailableSemaphore = nullptr;
    VkSemaphore renderFinishedSemaphore = nullptr;
    VkFence inFlightFence = nullptr;
    uint32_t graphicsQueueFamily = UINT32_MAX;
    flecs::world* ecs = nullptr;
    std::unique_ptr<Assimp::Importer> importer;
} g_engine;

// Transform component
struct Transform {
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;
};

// Physics component
struct PhysicsBody {
    float mass;
    glm::vec3 velocity;
    glm::vec3 acceleration;
};

// Model component
struct Model {
    std::string path;
    const aiScene* scene;
};

extern "C" {

int boulder_init(const char* appName, uint version) {
    if (g_engine.initialized) {
        return 0;
    }
    
    // Try to initialize SDL with just events first
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        Logger::get().error( "SDL_Init EVENTS failed: {}", SDL_GetError());
        return -1;
    }

    // Try to add video subsystem
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        Logger::get().error("SDL_InitSubSystem VIDEO failed: {}", SDL_GetError());
        Logger::get().info("Continuing without video subsystem...");
        // Don't return -1, continue without video
    }
    

    g_engine.ecs = new flecs::world();
    g_engine.importer = std::make_unique<Assimp::Importer>();

    VkResult err;

    if ( (err = volkInitialize()) != VK_SUCCESS) {
        Logger::get().error("Volk initialization failed! {}", (int)err);
        return -1;
    } else {
        Logger::get().info("Volk initialization successful!");
    }

    VkApplicationInfo appInfo{};

    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = appName;
    appInfo.applicationVersion = version;
    appInfo.pEngineName = "Boulder Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    appInfo.apiVersion = VK_API_VERSION_1_4;  // Targeting Vulkan 1.4

    unsigned int sdlExtensionCount = 0;
    auto e = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);    
    
    ScopedAlloc<const char*> instanceExtensions(sdlExtensionCount+5);

    Uint32 additionalExtensionCount = 0;


    if (sdlExtensionCount == 0) {
        Logger::get().error("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
        return -1;
    }
    else {
        for (int i; i < sdlExtensionCount; ++i){
            Logger::get().info("Instance Extension {}: {}",i,e[i]);
            
            instanceExtensions[i] = e[i];
        
        }
    }


    uint32_t availableExtensionCount = 0;
    err = vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr);
    if (err != VK_SUCCESS) {
        Logger::get().error("Failed to query instance extension count");
        return -1;
    }

    bool hasSurfaceCapabilities2 = false;

    if (availableExtensionCount > 0) {
        auto extensionProps = ScopedAlloc<VkExtensionProperties>(availableExtensionCount);

        err = vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, extensionProps.get());
        if (err != VK_SUCCESS) {
            Logger::get().error("Failed to enumerate instance extensions");
            return false;
            }

        for (uint32_t i = 0; i < availableExtensionCount; ++i) {
            const char* extName = extensionProps[i].extensionName;

            Logger::get().info("Instance Extension Property {}: {}", i, extName);

            if (strcmp(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, extName) == 0) {
                hasSurfaceCapabilities2 = true;
                instanceExtensions[sdlExtensionCount + additionalExtensionCount++] =
                    VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
                Logger::get().info("Device has surface capabilites 2!");
            }
        }

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = sdlExtensionCount + additionalExtensionCount;
        createInfo.ppEnabledExtensionNames = instanceExtensions.get();

        // Create the Vulkan instance now while pointers are valid
        err = vkCreateInstance(&createInfo, nullptr, &g_engine.instance);
        if (err != VK_SUCCESS) {
            Logger::get().error("Failed to create Vulkan instance: {}", (int)err);
            return -1;
        }
        Logger::get().info("Vulkan instance created!");

        volkLoadInstance(g_engine.instance);
    }

    g_engine.initialized = true;

    return 0;
}

void boulder_shutdown() {
    if (!g_engine.initialized) {
        return;
    }

    Logger::get().info("Shutting down engine...");

    // Wait for device to be idle before cleanup
    if (g_engine.device) {
        vkDeviceWaitIdle(g_engine.device);
    }

    // Cleanup Vulkan resources
    if (g_engine.device) {
        if (g_engine.imageAvailableSemaphore) {
            vkDestroySemaphore(g_engine.device, g_engine.imageAvailableSemaphore, nullptr);
            g_engine.imageAvailableSemaphore = nullptr;
        }
        if (g_engine.renderFinishedSemaphore) {
            vkDestroySemaphore(g_engine.device, g_engine.renderFinishedSemaphore, nullptr);
            g_engine.renderFinishedSemaphore = nullptr;
        }
        if (g_engine.inFlightFence) {
            vkDestroyFence(g_engine.device, g_engine.inFlightFence, nullptr);
            g_engine.inFlightFence = nullptr;
        }
        if (g_engine.commandPool) {
            vkDestroyCommandPool(g_engine.device, g_engine.commandPool, nullptr);
            g_engine.commandPool = nullptr;
        }
        for (auto imageView : g_engine.swapchainImageViews) {
            vkDestroyImageView(g_engine.device, imageView, nullptr);
        }
        g_engine.swapchainImageViews.clear();
        if (g_engine.swapchain) {
            vkDestroySwapchainKHR(g_engine.device, g_engine.swapchain, nullptr);
            g_engine.swapchain = nullptr;
        }
        vkDestroyDevice(g_engine.device, nullptr);
        g_engine.device = nullptr;
    }

    if (g_engine.instance) {
        if (g_engine.surface) {
            vkDestroySurfaceKHR(g_engine.instance, g_engine.surface, nullptr);
            g_engine.surface = nullptr;
        }
        vkDestroyInstance(g_engine.instance, nullptr);
        g_engine.instance = nullptr;
    }

    if (g_engine.window) {
        SDL_DestroyWindow(g_engine.window);
        g_engine.window = nullptr;
    }

    delete g_engine.ecs;
    g_engine.ecs = nullptr;

    g_engine.importer.reset();

    SDL_Quit();
    g_engine.initialized = false;
}

int boulder_update(float deltaTime) {
    if (!g_engine.initialized || !g_engine.ecs) {
        return -1;
    }

    // Update physics system
    // In Flecs v4, we need to create a query first
    auto query = g_engine.ecs->query<Transform, PhysicsBody>();
    query.each([deltaTime](Transform& t, PhysicsBody& pb) {
        t.position += pb.velocity * deltaTime;
        pb.velocity += pb.acceleration * deltaTime;
    });

    return 0;
}

// Helper function to recreate swapchain
static int recreate_swapchain() {

    if (!g_engine.device || !g_engine.window || !g_engine.physicalDevice || !g_engine.surface) {
        return -1;
    }

    // Prevent re-entry
    if (g_engine.isRecreatingSwapchain) {
        Logger::get().error("recreate_swapchain is already recreating swapchain! Aborting...");
        return 0;
    }
    g_engine.isRecreatingSwapchain = true;
    g_engine.resizeEventDuringRecreate = false;

    // Wait for in-flight rendering to complete before destroying resources
    if (g_engine.inFlightFence) {
        vkWaitForFences(g_engine.device, 1, &g_engine.inFlightFence, VK_TRUE, 1000);
    }

    // Clean up old swapchain resources
    for (auto imageView : g_engine.swapchainImageViews) {
        vkDestroyImageView(g_engine.device, imageView, nullptr);
    }
    g_engine.swapchainImageViews.clear();

    VkSwapchainKHR oldSwapchain = g_engine.swapchain;

    // Get current window size
    int width, height;
    SDL_GetWindowSize(g_engine.window, &width, &height);

    // Handle minimization (width/height = 0) - just skip recreation
    if (width == 0 || height == 0) {
        Logger::get().info("Window minimized, skipping swapchain recreation");
        g_engine.isRecreatingSwapchain = false;
        return 0;
    }

    Logger::get().info("Recreating swapchain with size: {}x{}", width, height);

    // Get surface capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_engine.physicalDevice, g_engine.surface, &capabilities);

    // Update swapchain extent
    g_engine.swapchainExtent = capabilities.currentExtent;
    if (g_engine.swapchainExtent.width == UINT32_MAX) {
        g_engine.swapchainExtent.width = std::clamp(static_cast<uint32_t>(width),
                                                     capabilities.minImageExtent.width,
                                                     capabilities.maxImageExtent.width);
        g_engine.swapchainExtent.height = std::clamp(static_cast<uint32_t>(height),
                                                      capabilities.minImageExtent.height,
                                                      capabilities.maxImageExtent.height);
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = g_engine.surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = g_engine.swapchainFormat;
    swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainInfo.imageExtent = g_engine.swapchainExtent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = oldSwapchain;

    if (vkCreateSwapchainKHR(g_engine.device, &swapchainInfo, nullptr, &g_engine.swapchain) != VK_SUCCESS) {
        Logger::get().error("Failed to recreate swapchain");
        g_engine.isRecreatingSwapchain = false;
        return -1;
    }

    // Destroy old swapchain
    if (oldSwapchain) {
        vkDestroySwapchainKHR(g_engine.device, oldSwapchain, nullptr);
    }

    // Get new swapchain images
    vkGetSwapchainImagesKHR(g_engine.device, g_engine.swapchain, &imageCount, nullptr);
    g_engine.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(g_engine.device, g_engine.swapchain, &imageCount, g_engine.swapchainImages.data());

    // Recreate image views
    g_engine.swapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = g_engine.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g_engine.swapchainFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(g_engine.device, &viewInfo, nullptr, &g_engine.swapchainImageViews[i]) != VK_SUCCESS) {
            Logger::get().error("Failed to recreate image view");
            g_engine.isRecreatingSwapchain = false;
            return -1;
        }
    }

    // Recreate command buffers if count changed
    if (g_engine.commandBuffers.size() != imageCount) {
        if (!g_engine.commandBuffers.empty()) {
            vkFreeCommandBuffers(g_engine.device, g_engine.commandPool,
                               static_cast<uint32_t>(g_engine.commandBuffers.size()),
                               g_engine.commandBuffers.data());
        }

        g_engine.commandBuffers.resize(imageCount);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = g_engine.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = imageCount;

        if (vkAllocateCommandBuffers(g_engine.device, &allocInfo, g_engine.commandBuffers.data()) != VK_SUCCESS) {
            Logger::get().error("Failed to reallocate command buffers");
            g_engine.isRecreatingSwapchain = false;
            return -1;
        }
    }

    Logger::get().info("Swapchain recreated successfully!");

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_engine.physicalDevice, g_engine.surface, &capabilities);

    if (g_engine.swapchainExtent.height != capabilities.currentExtent.height || g_engine.swapchainExtent.width != capabilities.currentExtent.width){
        g_engine.resizeEventDuringRecreate = true;
    }

    // Only clear the recreate flag if no resize event happened during recreation
    if (!g_engine.resizeEventDuringRecreate) {
        g_engine.swapchainNeedsRecreate = false;
    } else {
        Logger::get().info("Resize event occurred during recreation, will recreate again");
        g_engine.resizeEventDuringRecreate = false;
    }

    g_engine.isRecreatingSwapchain = false;

    return 0;
}

int boulder_render() {
    
    if (!g_engine.initialized || !g_engine.window || !g_engine.device) {
        return -1;
    }

    // Skip rendering if currently recreating swapchain
    if (g_engine.isRecreatingSwapchain) {
        return 0;
    }

    // Recreate swapchain if needed (loop in case size changed during recreation)
    int recreateAttempts = 0;
    while (g_engine.swapchainNeedsRecreate && !g_engine.isRecreatingSwapchain && recreateAttempts < 5) {
        if (recreate_swapchain() != 0) {
            return -1;
        }
        recreateAttempts++;
    }

    // If we exhausted attempts, clear the flag to avoid infinite loop
    if (recreateAttempts >= 5) {
        Logger::get().error("Too many swapchain recreation attempts, giving up");
        g_engine.swapchainNeedsRecreate = false;
    }

    // Wait for previous frame
    vkWaitForFences(g_engine.device, 1, &g_engine.inFlightFence, VK_TRUE, UINT64_MAX);
    vkResetFences(g_engine.device, 1, &g_engine.inFlightFence);

    // Acquire next image
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(g_engine.device, g_engine.swapchain, UINT64_MAX,
                                            g_engine.imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        g_engine.swapchainNeedsRecreate = true;
        return 0;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        Logger::get().error("Failed to acquire swapchain image");
        return -1;
    }

    // Record command buffer
    VkCommandBuffer cmd = g_engine.commandBuffers[imageIndex];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        Logger::get().error("Failed to begin command buffer");
        return -1;
    }

    // Transition image to color attachment optimal
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_engine.swapchainImages[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Begin dynamic rendering
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = g_engine.swapchainImageViews[imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.1f, 0.2f, 0.3f, 1.0f}}; // Clear color: dark blue

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = g_engine.swapchainExtent;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    // Rendering commands would go here

    vkCmdEndRendering(cmd);

    // Transition to present
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        Logger::get().error("Failed to end command buffer");
        return -1;
    }

    // Submit command buffer
    VkSemaphore waitSemaphores[] = {g_engine.imageAvailableSemaphore};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {g_engine.renderFinishedSemaphore};

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(g_engine.graphicsQueue, 1, &submitInfo, g_engine.inFlightFence) != VK_SUCCESS) {
        Logger::get().error("Failed to submit command buffer");
        return -1;
    }

    // Present
    VkSwapchainKHR swapchains[] = {g_engine.swapchain};
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(g_engine.graphicsQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_engine.swapchainNeedsRecreate = true;
    } else if (result != VK_SUCCESS) {
        Logger::get().error("Failed to present swapchain image");
        return -1;
    }

    return 0;
}

int boulder_create_window(int width, int height, const char* title) {

    VkResult err;

    if (!g_engine.initialized || !g_engine.instance) {
        Logger::get().error("Engine not initialized or no Vulkan instance");
        return -1;
    }

    Logger::get().info("Window creation: {} x {} '{}'" , width, height, title);


    if (g_engine.window) {
        SDL_DestroyWindow(g_engine.window);
    }

    g_engine.window = SDL_CreateWindow(
        title,
        width, height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (!g_engine.window) {
        Logger::get().error("Failed to create window: {}", SDL_GetError());
        return -1;
    }

    if(!SDL_Vulkan_CreateSurface(g_engine.window, g_engine.instance, nullptr, &g_engine.surface)){
        Logger::get().error("Failed to create Vulkan surface: {}", SDL_GetError());
        return -1;
    } else {
        Logger::get().info("Vulkan surface created!");
    }

    // Select physical device
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(g_engine.instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        Logger::get().error("No Vulkan physical devices found");
        return -1;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(g_engine.instance, &deviceCount, devices.data());
    g_engine.physicalDevice = devices[0]; // Just use first device for now

    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_engine.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_engine.physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(g_engine.physicalDevice, i, g_engine.surface, &presentSupport);
            if (presentSupport) {
                g_engine.graphicsQueueFamily = i;
                break;
            }
        }
    }

    if (g_engine.graphicsQueueFamily == UINT32_MAX) {
        Logger::get().error("No suitable queue family found");
        return -1;
    }

    // Create logical device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = g_engine.graphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceFeatures deviceFeatures{};

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeature{};
    dynamicRenderingFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamicRenderingFeature.dynamicRendering = VK_TRUE;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &dynamicRenderingFeature;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

    if (vkCreateDevice(g_engine.physicalDevice, &deviceCreateInfo, nullptr, &g_engine.device) != VK_SUCCESS) {
        Logger::get().error("Failed to create logical device");
        return -1;
    }

    volkLoadDevice(g_engine.device);
    vkGetDeviceQueue(g_engine.device, g_engine.graphicsQueueFamily, 0, &g_engine.graphicsQueue);
    Logger::get().info("Vulkan device created!");

    // Create swapchain
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_engine.physicalDevice, g_engine.surface, &capabilities);

    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_engine.physicalDevice, g_engine.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_engine.physicalDevice, g_engine.surface, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = format;
            break;
        }
    }

    g_engine.swapchainFormat = surfaceFormat.format;
    g_engine.swapchainExtent = capabilities.currentExtent;

    if (g_engine.swapchainExtent.width == UINT32_MAX) {
        g_engine.swapchainExtent.width = width;
        g_engine.swapchainExtent.height = height;
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = g_engine.surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = surfaceFormat.format;
    swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainInfo.imageExtent = g_engine.swapchainExtent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(g_engine.device, &swapchainInfo, nullptr, &g_engine.swapchain) != VK_SUCCESS) {
        Logger::get().error("Failed to create swapchain");
        return -1;
    }

    vkGetSwapchainImagesKHR(g_engine.device, g_engine.swapchain, &imageCount, nullptr);
    g_engine.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(g_engine.device, g_engine.swapchain, &imageCount, g_engine.swapchainImages.data());

    // Create image views
    g_engine.swapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = g_engine.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g_engine.swapchainFormat;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(g_engine.device, &viewInfo, nullptr, &g_engine.swapchainImageViews[i]) != VK_SUCCESS) {
            Logger::get().error("Failed to create image view");
            return -1;
        }
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_engine.graphicsQueueFamily;

    if (vkCreateCommandPool(g_engine.device, &poolInfo, nullptr, &g_engine.commandPool) != VK_SUCCESS) {
        Logger::get().error("Failed to create command pool");
        return -1;
    }

    // Create command buffers
    g_engine.commandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_engine.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = imageCount;

    if (vkAllocateCommandBuffers(g_engine.device, &allocInfo, g_engine.commandBuffers.data()) != VK_SUCCESS) {
        Logger::get().error("Failed to allocate command buffers");
        return -1;
    }

    // Create sync objects
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.imageAvailableSemaphore) != VK_SUCCESS ||
        vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.renderFinishedSemaphore) != VK_SUCCESS ||
        vkCreateFence(g_engine.device, &fenceInfo, nullptr, &g_engine.inFlightFence) != VK_SUCCESS) {
        Logger::get().error("Failed to create sync objects");
        return -1;
    }

    Logger::get().info("Vulkan rendering setup complete!");

    return g_engine.window ? 0 : -1;

}

void boulder_set_window_size(int width, int height) {
    if (g_engine.window) {
        SDL_SetWindowSize(g_engine.window, width, height);
        g_engine.swapchainNeedsRecreate = true;
    }
}

void boulder_get_window_size(int* width, int* height) {
    if (g_engine.window && width && height) {
        SDL_GetWindowSize(g_engine.window, width, height);
    }
}

int boulder_should_close() {
    return g_engine.shouldClose ? 1 : 0;
}

void boulder_poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                g_engine.shouldClose = true;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                // Set flag to indicate resize needed
                g_engine.swapchainNeedsRecreate = true;
                // Track if this happened during swapchain recreation
                if (g_engine.isRecreatingSwapchain) {
                    g_engine.resizeEventDuringRecreate = true;
                }
                break;
            default:
                break;
        }
    }
}

EntityID boulder_create_entity() {
    if (!g_engine.ecs) {
        return 0;
    }

    flecs::entity e = g_engine.ecs->entity();
    return e.id();
}

void boulder_destroy_entity(EntityID entity) {
    if (!g_engine.ecs) {
        return;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    e.destruct();
}

int boulder_entity_exists(EntityID entity) {
    if (!g_engine.ecs) {
        return 0;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    return e.is_alive() ? 1 : 0;
}

int boulder_add_transform(EntityID entity, float x, float y, float z) {
    if (!g_engine.ecs) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    e.set<Transform>({
        .position = glm::vec3(x, y, z),
        .rotation = glm::vec3(0.0f),
        .scale = glm::vec3(1.0f)
    });

    return 0;
}

int boulder_get_transform(EntityID entity, float* x, float* y, float* z) {
    if (!g_engine.ecs || !x || !y || !z) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    const Transform* t = e.get<Transform>();
    if (!t) {
        return -1;
    }

    *x = t->position.x;
    *y = t->position.y;
    *z = t->position.z;

    return 0;
}

int boulder_set_transform(EntityID entity, float x, float y, float z) {
    if (!g_engine.ecs) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    Transform* t = e.get_mut<Transform>();
    if (!t) {
        return -1;
    }

    t->position = glm::vec3(x, y, z);

    return 0;
}

int boulder_add_physics_body(EntityID entity, float mass) {
    if (!g_engine.ecs) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    e.set<PhysicsBody>({
        .mass = mass,
        .velocity = glm::vec3(0.0f),
        .acceleration = glm::vec3(0.0f, -9.81f, 0.0f) // gravity
    });

    return 0;
}

int boulder_set_velocity(EntityID entity, float vx, float vy, float vz) {
    if (!g_engine.ecs) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    PhysicsBody* pb = e.get_mut<PhysicsBody>();
    if (!pb) {
        return -1;
    }

    pb->velocity = glm::vec3(vx, vy, vz);

    return 0;
}

int boulder_get_velocity(EntityID entity, float* vx, float* vy, float* vz) {
    if (!g_engine.ecs || !vx || !vy || !vz) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    const PhysicsBody* pb = e.get<PhysicsBody>();
    if (!pb) {
        return -1;
    }

    *vx = pb->velocity.x;
    *vy = pb->velocity.y;
    *vz = pb->velocity.z;

    return 0;
}

int boulder_apply_force(EntityID entity, float fx, float fy, float fz) {
    if (!g_engine.ecs) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    PhysicsBody* pb = e.get_mut<PhysicsBody>();
    if (!pb) {
        return -1;
    }

    glm::vec3 force(fx, fy, fz);
    pb->acceleration += force / pb->mass;

    return 0;
}

int boulder_load_model(EntityID entity, const char* path) {
    if (!g_engine.ecs || !g_engine.importer || !path) {
        return -1;
    }

    const aiScene* scene = g_engine.importer->ReadFile(path,
        aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    e.set<Model>({
        .path = std::string(path),
        .scene = scene
    });

    return 0;
}

int boulder_is_key_pressed(int keyCode) {
    const bool* state = SDL_GetKeyboardState(nullptr);
    return state[keyCode] ? 1 : 0;
}

int boulder_is_mouse_button_pressed(int button) {
    Uint32 buttons = SDL_GetMouseState(nullptr, nullptr);
    // SDL3 uses SDL_BUTTON_MASK instead of SDL_BUTTON
    return (buttons & SDL_BUTTON_MASK(button)) ? 1 : 0;
}

void boulder_get_mouse_position(float* x, float* y) {
    if (x && y) {
        SDL_GetMouseState(x, y);
    }
}

void boulder_log_info(const char* message) {
    if (message) {
        Logger::get().info("{}",message);
    }
}

void boulder_log_error(const char* message) {
    if (message) {
        Logger::get().error("{}",message);
    }
}

} // extern "C"