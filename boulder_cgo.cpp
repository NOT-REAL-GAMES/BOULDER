
#include "main.h"
#include "boulder_cgo.h"
#include <iostream>
#include <memory>
#include <unordered_map>
#include <SDL3/SDL.h>
#include <flecs.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "volk.h"
#include <shaderc/shaderc.hpp>

// Maximum frames that can be processed simultaneously
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

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

    // Per-frame synchronization (fixed size for frames in flight)
    VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT];
    VkFence inFlightFences[MAX_FRAMES_IN_FLIGHT];

    // Per-swapchain-image semaphores (must be separate per image for present)
    std::vector<VkSemaphore> renderFinishedSemaphores;

    uint32_t graphicsQueueFamily = UINT32_MAX;
    VkPipelineLayout pipelineLayout = nullptr;
    VkPipeline cubePipeline = nullptr;
    VkShaderModule meshShaderModule = nullptr;
    VkShaderModule fragShaderModule = nullptr;
    flecs::world* ecs = nullptr;
    std::unique_ptr<Assimp::Importer> importer;

    // Modular rendering state
    std::unordered_map<uint64_t, VkShaderModule> shaderModules;
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    std::unordered_map<uint64_t, VkPipelineLayout> pipelineLayouts;
    uint64_t nextShaderModuleId = 1;
    uint64_t nextPipelineId = 1;
    VkPipeline boundPipeline = nullptr;
    VkCommandBuffer activeCommandBuffer = nullptr;
    uint32_t currentFrameIndex = 0;
    VkClearColorValue clearColor = {{0.1f, 0.2f, 0.3f, 1.0f}};
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

// Shader compilation helper
static std::vector<uint32_t> compileShader(const std::string& source, shaderc_shader_kind kind, const char* name) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    // Use Vulkan 1.2 for better compatibility with glslang
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);

    auto result = compiler.CompileGlslToSpv(source, kind, name, options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        Logger::get().error("Shader compilation failed for {}: {}", name, result.GetErrorMessage());
        return {};
    }

    Logger::get().info("Shader {} compiled successfully", name);

    return {result.cbegin(), result.cend()};
}

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

        // Enable validation layers for debugging
        const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = sdlExtensionCount + additionalExtensionCount;
        createInfo.ppEnabledExtensionNames = instanceExtensions.get();
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = validationLayers;

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
        // Cleanup pipeline and shaders
        if (g_engine.cubePipeline) {
            vkDestroyPipeline(g_engine.device, g_engine.cubePipeline, nullptr);
            g_engine.cubePipeline = nullptr;
        }
        if (g_engine.pipelineLayout) {
            vkDestroyPipelineLayout(g_engine.device, g_engine.pipelineLayout, nullptr);
            g_engine.pipelineLayout = nullptr;
        }
        if (g_engine.meshShaderModule) {
            vkDestroyShaderModule(g_engine.device, g_engine.meshShaderModule, nullptr);
            g_engine.meshShaderModule = nullptr;
        }
        if (g_engine.fragShaderModule) {
            vkDestroyShaderModule(g_engine.device, g_engine.fragShaderModule, nullptr);
            g_engine.fragShaderModule = nullptr;
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(g_engine.device, g_engine.imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(g_engine.device, g_engine.inFlightFences[i], nullptr);
        }

        for (auto sem : g_engine.renderFinishedSemaphores) {
            vkDestroySemaphore(g_engine.device, sem, nullptr);
        }
        g_engine.renderFinishedSemaphores.clear();
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
    vkWaitForFences(g_engine.device, MAX_FRAMES_IN_FLIGHT, g_engine.inFlightFences, VK_TRUE, UINT64_MAX);

    // Clean up old swapchain resources
    for (auto imageView : g_engine.swapchainImageViews) {
        vkDestroyImageView(g_engine.device, imageView, nullptr);
    }
    g_engine.swapchainImageViews.clear();

    // Clean up old per-image semaphores
    for (auto sem : g_engine.renderFinishedSemaphores) {
        vkDestroySemaphore(g_engine.device, sem, nullptr);
    }
    g_engine.renderFinishedSemaphores.clear();

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

    // Recreate per-image semaphores
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    g_engine.renderFinishedSemaphores.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        if (vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.renderFinishedSemaphores[i]) != VK_SUCCESS) {
            Logger::get().error("Failed to recreate render finished semaphore for image {}", i);
            g_engine.isRecreatingSwapchain = false;
            return -1;
        }
    }

    // Command buffers are per-frame-in-flight (MAX_FRAMES_IN_FLIGHT), not per-image, so no recreation needed

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

// Legacy function - use begin_frame/end_frame instead
int boulder_render() {
    uint32_t imageIndex;
    int result = boulder_begin_frame(&imageIndex);

    if (result == -2) {
        // Swapchain needs recreation
        if (recreate_swapchain() != 0) {
            return -1;
        }
        return 0;
    } else if (result != 0) {
        return result;
    }

    // Draw cube with mesh shader
    if (g_engine.cubePipeline) {
        vkCmdBindPipeline(g_engine.activeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_engine.cubePipeline);

        // Set dynamic viewport and scissor
        boulder_set_viewport(0.0f, 0.0f, (float)g_engine.swapchainExtent.width, (float)g_engine.swapchainExtent.height, 0.0f, 1.0f);
        boulder_set_scissor(0, 0, g_engine.swapchainExtent.width, g_engine.swapchainExtent.height);

        // Set up view-projection matrix (simple perspective)
        float aspect = (float)g_engine.swapchainExtent.width / (float)g_engine.swapchainExtent.height;
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        proj[1][1] *= -1; // Flip Y for Vulkan

        glm::mat4 view = glm::lookAt(
            glm::vec3(2.0f, 2.0f, 2.0f),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );

        glm::mat4 viewProj = proj * view;

        // Get current time for animation
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

        // Push constants
        struct PushConstants {
            glm::mat4 viewProj;
            float time;
        } pushConstants;
        pushConstants.viewProj = viewProj;
        pushConstants.time = time;

        vkCmdPushConstants(g_engine.activeCommandBuffer, g_engine.pipelineLayout, VK_SHADER_STAGE_MESH_BIT_EXT,
                           0, sizeof(PushConstants), &pushConstants);

        // Draw mesh shader (1 workgroup = 1 cube)
        vkCmdDrawMeshTasksEXT(g_engine.activeCommandBuffer, 1, 1, 1);
    }

    return boulder_end_frame(imageIndex);
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

    // Check if mesh shader extension is available
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(g_engine.physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(g_engine.physicalDevice, nullptr, &extensionCount, availableExtensions.data());

    bool meshShaderSupported = false;
    for (const auto& ext : availableExtensions) {
        if (strcmp(ext.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
            meshShaderSupported = true;
            Logger::get().info("Mesh shader extension is supported!");
            break;
        }
    }

    if (!meshShaderSupported) {
        Logger::get().error("Mesh shader extension NOT supported on this device!");
        return -1;
    }

    // Mesh shader features
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
    meshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    meshShaderFeatures.meshShader = VK_TRUE;
    meshShaderFeatures.taskShader = VK_FALSE;

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeature{};
    dynamicRenderingFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamicRenderingFeature.pNext = &meshShaderFeatures;
    dynamicRenderingFeature.dynamicRendering = VK_TRUE;

    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME
    };

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &dynamicRenderingFeature;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
    deviceCreateInfo.enabledExtensionCount = 2;
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

    // Create command buffers (one per frame in flight)
    g_engine.commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_engine.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

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

    // Per-frame-in-flight sync (imageAvailable and fences)
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(g_engine.device, &fenceInfo, nullptr, &g_engine.inFlightFences[i]) != VK_SUCCESS) {
            Logger::get().error("Failed to create sync objects for frame {}", i);
            return -1;
        }
    }

    // Per-swapchain-image semaphores (renderFinished - used by present)
    g_engine.renderFinishedSemaphores.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        if (vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.renderFinishedSemaphores[i]) != VK_SUCCESS) {
            Logger::get().error("Failed to create render finished semaphore for image {}", i);
            return -1;
        }
    }

    // Load and compile shaders at runtime
    std::ifstream meshFile("shaders/cube.mesh");
    std::string meshSource((std::istreambuf_iterator<char>(meshFile)), std::istreambuf_iterator<char>());

    std::ifstream fragFile("shaders/cube.frag");
    std::string fragSource((std::istreambuf_iterator<char>(fragFile)), std::istreambuf_iterator<char>());

    if (meshSource.empty() || fragSource.empty()) {
        Logger::get().error("Failed to read shader source files");
        return -1;
    }

    auto meshSpirv = compileShader(meshSource, shaderc_glsl_default_mesh_shader, "cube.mesh");
    auto fragSpirv = compileShader(fragSource, shaderc_glsl_default_fragment_shader, "cube.frag");

    if (meshSpirv.empty() || fragSpirv.empty()) {
        Logger::get().error("Failed to compile shaders");
        return -1;
    }

    // Create shader modules
    VkShaderModuleCreateInfo meshModuleInfo{};
    meshModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    meshModuleInfo.codeSize = meshSpirv.size() * sizeof(uint32_t);
    meshModuleInfo.pCode = meshSpirv.data();

    VkShaderModuleCreateInfo fragModuleInfo{};
    fragModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragModuleInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);
    fragModuleInfo.pCode = fragSpirv.data();

    if (vkCreateShaderModule(g_engine.device, &meshModuleInfo, nullptr, &g_engine.meshShaderModule) != VK_SUCCESS ||
        vkCreateShaderModule(g_engine.device, &fragModuleInfo, nullptr, &g_engine.fragShaderModule) != VK_SUCCESS) {
        Logger::get().error("Failed to create shader modules");
        return -1;
    }

    // Create pipeline layout with push constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4) + sizeof(float); // viewProj + time

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(g_engine.device, &pipelineLayoutInfo, nullptr, &g_engine.pipelineLayout) != VK_SUCCESS) {
        Logger::get().error("Failed to create pipeline layout");
        return -1;
    }

    // Create graphics pipeline
    VkPipelineShaderStageCreateInfo meshStage{};
    meshStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    meshStage.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    meshStage.module = g_engine.meshShaderModule;
    meshStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = g_engine.fragShaderModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {meshStage, fragStage};

    // Dynamic state for viewport and scissor
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Viewport state (counts only, actual values set dynamically)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; 
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineRenderingCreateInfo pipelineRenderingInfo{};
    pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipelineRenderingInfo.colorAttachmentCount = 1;
    pipelineRenderingInfo.pColorAttachmentFormats = &g_engine.swapchainFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &pipelineRenderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = g_engine.pipelineLayout;

    if (vkCreateGraphicsPipelines(g_engine.device, nullptr, 1, &pipelineInfo, nullptr, &g_engine.cubePipeline) != VK_SUCCESS) {
        Logger::get().error("Failed to create graphics pipeline");
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

// Shader management
ShaderModuleID boulder_compile_shader(const char* source, int shaderKind, const char* name) {
    if (!g_engine.initialized || !g_engine.device || !source || !name) {
        Logger::get().error("Cannot compile shader: engine not initialized or invalid parameters");
        return 0;
    }

    std::string sourceStr(source);
    shaderc_shader_kind kind = static_cast<shaderc_shader_kind>(shaderKind);

    auto spirv = compileShader(sourceStr, kind, name);
    if (spirv.empty()) {
        Logger::get().error("Failed to compile shader: {}", name);
        return 0;
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(uint32_t);
    createInfo.pCode = spirv.data();

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(g_engine.device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        Logger::get().error("Failed to create shader module: {}", name);
        return 0;
    }

    uint64_t id = g_engine.nextShaderModuleId++;
    g_engine.shaderModules[id] = shaderModule;

    Logger::get().info("Shader module {} created with ID {}", name, id);
    return id;
}

void boulder_destroy_shader_module(ShaderModuleID shaderId) {
    if (!g_engine.initialized || !g_engine.device) {
        return;
    }

    auto it = g_engine.shaderModules.find(shaderId);
    if (it != g_engine.shaderModules.end()) {
        vkDestroyShaderModule(g_engine.device, it->second, nullptr);
        g_engine.shaderModules.erase(it);
        Logger::get().info("Destroyed shader module with ID {}", shaderId);
    }
}

ShaderModuleID boulder_reload_shader(ShaderModuleID shaderId, const char* source, int shaderKind, const char* name) {
    if (!g_engine.initialized || !g_engine.device || !source || !name) {
        Logger::get().error("Cannot reload shader: engine not initialized or invalid parameters");
        return 0;
    }

    // Destroy old shader module if it exists
    if (shaderId != 0) {
        boulder_destroy_shader_module(shaderId);
    }

    // Create new shader module
    return boulder_compile_shader(source, shaderKind, name);
}

// Pipeline management
PipelineID boulder_create_graphics_pipeline(ShaderModuleID meshShader, ShaderModuleID fragShader) {
    if (!g_engine.initialized || !g_engine.device) {
        Logger::get().error("Cannot create pipeline: engine not initialized");
        return 0;
    }

    auto meshIt = g_engine.shaderModules.find(meshShader);
    auto fragIt = g_engine.shaderModules.find(fragShader);

    if (meshIt == g_engine.shaderModules.end() || fragIt == g_engine.shaderModules.end()) {
        Logger::get().error("Cannot create pipeline: invalid shader module IDs");
        return 0;
    }

    // Create pipeline layout with push constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = 64; // 64 bytes for transform matrix

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    VkPipelineLayout pipelineLayout;
    if (vkCreatePipelineLayout(g_engine.device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        Logger::get().error("Failed to create pipeline layout");
        return 0;
    }

    // Create shader stages
    VkPipelineShaderStageCreateInfo meshShaderStageInfo{};
    meshShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    meshShaderStageInfo.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
    meshShaderStageInfo.module = meshIt->second;
    meshShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragIt->second;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {meshShaderStageInfo, fragShaderStageInfo};

    // Rasterization state
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color blending
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Dynamic state
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Viewport state (counts only, actual values set dynamically)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Dynamic rendering info
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &g_engine.swapchainFormat;

    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(g_engine.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        Logger::get().error("Failed to create graphics pipeline");
        vkDestroyPipelineLayout(g_engine.device, pipelineLayout, nullptr);
        return 0;
    }

    uint64_t id = g_engine.nextPipelineId++;
    g_engine.pipelines[id] = pipeline;
    g_engine.pipelineLayouts[id] = pipelineLayout;

    Logger::get().info("Graphics pipeline created with ID {}", id);
    return id;
}

void boulder_bind_pipeline(PipelineID pipelineId) {
    if (!g_engine.initialized || !g_engine.activeCommandBuffer) {
        Logger::get().error("Cannot bind pipeline: no active command buffer");
        return;
    }

    auto it = g_engine.pipelines.find(pipelineId);
    if (it == g_engine.pipelines.end()) {
        Logger::get().error("Cannot bind pipeline: invalid pipeline ID {}", pipelineId);
        return;
    }

    vkCmdBindPipeline(g_engine.activeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, it->second);
    g_engine.boundPipeline = it->second;
}

void boulder_destroy_pipeline(PipelineID pipelineId) {
    if (!g_engine.initialized || !g_engine.device) {
        return;
    }

    auto pipelineIt = g_engine.pipelines.find(pipelineId);
    auto layoutIt = g_engine.pipelineLayouts.find(pipelineId);

    if (pipelineIt != g_engine.pipelines.end()) {
        vkDestroyPipeline(g_engine.device, pipelineIt->second, nullptr);
        g_engine.pipelines.erase(pipelineIt);
    }

    if (layoutIt != g_engine.pipelineLayouts.end()) {
        vkDestroyPipelineLayout(g_engine.device, layoutIt->second, nullptr);
        g_engine.pipelineLayouts.erase(layoutIt);
    }

    Logger::get().info("Destroyed pipeline with ID {}", pipelineId);
}

// Rendering control
int boulder_begin_frame(uint32_t* imageIndex) {
    if (!g_engine.initialized || !g_engine.device || !g_engine.swapchain) {
        Logger::get().error("Cannot begin frame: engine not initialized");
        return -1;
    }

    if (g_engine.swapchainNeedsRecreate) {
        Logger::get().info("Swapchain recreation needed before beginning frame");
        return -2;
    }

    // Wait for the fence for this frame
    vkWaitForFences(g_engine.device, 1, &g_engine.inFlightFences[g_engine.currentFrameIndex], VK_TRUE, UINT64_MAX);

    // Acquire next image
    VkResult result = vkAcquireNextImageKHR(
        g_engine.device,
        g_engine.swapchain,
        UINT64_MAX,
        g_engine.imageAvailableSemaphores[g_engine.currentFrameIndex],
        VK_NULL_HANDLE,
        imageIndex
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_engine.swapchainNeedsRecreate = true;
        return -2;
    } else if (result != VK_SUCCESS) {
        Logger::get().error("Failed to acquire swapchain image");
        return -1;
    }

    // Reset fence
    vkResetFences(g_engine.device, 1, &g_engine.inFlightFences[g_engine.currentFrameIndex]);

    // Begin command buffer
    VkCommandBuffer cmd = g_engine.commandBuffers[g_engine.currentFrameIndex];
    g_engine.activeCommandBuffer = cmd;

    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        Logger::get().error("Failed to begin command buffer");
        g_engine.activeCommandBuffer = nullptr;
        return -1;
    }

    // Transition image layout
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_engine.swapchainImages[*imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Begin rendering
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = g_engine.swapchainImageViews[*imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = g_engine.clearColor;

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = g_engine.swapchainExtent;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    return 0;
}

int boulder_end_frame(uint32_t imageIndex) {
    if (!g_engine.initialized || !g_engine.device || !g_engine.activeCommandBuffer) {
        Logger::get().error("Cannot end frame: no active command buffer");
        return -1;
    }

    VkCommandBuffer cmd = g_engine.activeCommandBuffer;

    // End rendering
    vkCmdEndRendering(cmd);

    // Transition image layout for presentation
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_engine.swapchainImages[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = 0;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // End command buffer
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        Logger::get().error("Failed to record command buffer");
        g_engine.activeCommandBuffer = nullptr;
        return -1;
    }

    // Submit command buffer
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore waitSemaphores[] = {g_engine.imageAvailableSemaphores[g_engine.currentFrameIndex]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    // Use imageIndex for renderFinished (per-swapchain-image, not per-frame)
    VkSemaphore signalSemaphores[] = {g_engine.renderFinishedSemaphores[imageIndex]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(g_engine.graphicsQueue, 1, &submitInfo, g_engine.inFlightFences[g_engine.currentFrameIndex]) != VK_SUCCESS) {
        Logger::get().error("Failed to submit draw command buffer");
        g_engine.activeCommandBuffer = nullptr;
        return -1;
    }

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    VkSwapchainKHR swapchains[] = {g_engine.swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = vkQueuePresentKHR(g_engine.graphicsQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_engine.swapchainNeedsRecreate = true;
    } else if (result != VK_SUCCESS) {
        Logger::get().error("Failed to present swapchain image");
    }

    g_engine.activeCommandBuffer = nullptr;
    g_engine.currentFrameIndex = (g_engine.currentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;

    return 0;
}

void boulder_set_clear_color(float r, float g, float b, float a) {
    g_engine.clearColor = {{r, g, b, a}};
}

void boulder_set_viewport(float x, float y, float width, float height, float minDepth, float maxDepth) {
    if (!g_engine.initialized || !g_engine.activeCommandBuffer) {
        Logger::get().error("Cannot set viewport: no active command buffer");
        return;
    }

    VkViewport viewport{};
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.minDepth = minDepth;
    viewport.maxDepth = maxDepth;

    vkCmdSetViewport(g_engine.activeCommandBuffer, 0, 1, &viewport);
}

void boulder_set_scissor(int x, int y, int width, int height) {
    if (!g_engine.initialized || !g_engine.activeCommandBuffer) {
        Logger::get().error("Cannot set scissor: no active command buffer");
        return;
    }

    VkRect2D scissor{};
    scissor.offset = {x, y};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    vkCmdSetScissor(g_engine.activeCommandBuffer, 0, 1, &scissor);
}

// Draw commands
void boulder_draw_mesh(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    if (!g_engine.initialized || !g_engine.activeCommandBuffer) {
        Logger::get().error("Cannot draw mesh: no active command buffer");
        return;
    }

    vkCmdDrawMeshTasksEXT(g_engine.activeCommandBuffer, groupCountX, groupCountY, groupCountZ);
}

void boulder_set_push_constants(const void* data, uint32_t size, uint32_t offset) {
    if (!g_engine.initialized || !g_engine.activeCommandBuffer) {
        Logger::get().error("Cannot set push constants: no active command buffer");
        return;
    }

    if (!data || size == 0) {
        Logger::get().error("Cannot set push constants: invalid data or size");
        return;
    }

    // Find the pipeline layout for the currently bound pipeline
    VkPipelineLayout layout = VK_NULL_HANDLE;
    for (const auto& [id, pipeline] : g_engine.pipelines) {
        if (pipeline == g_engine.boundPipeline) {
            auto layoutIt = g_engine.pipelineLayouts.find(id);
            if (layoutIt != g_engine.pipelineLayouts.end()) {
                layout = layoutIt->second;
                break;
            }
        }
    }

    if (layout == VK_NULL_HANDLE) {
        Logger::get().error("Cannot set push constants: no pipeline layout found for bound pipeline");
        return;
    }

    vkCmdPushConstants(g_engine.activeCommandBuffer, layout, VK_SHADER_STAGE_MESH_BIT_EXT, offset, size, data);
}

// Swapchain management
void boulder_get_swapchain_extent(int* width, int* height) {
    if (width && height) {
        *width = g_engine.swapchainExtent.width;
        *height = g_engine.swapchainExtent.height;
    }
}

int boulder_recreate_swapchain() {
    if (!g_engine.initialized || !g_engine.device) {
        Logger::get().error("Cannot recreate swapchain: engine not initialized");
        return -1;
    }

    g_engine.swapchainNeedsRecreate = true;
    return 0;
}

} // extern "C"