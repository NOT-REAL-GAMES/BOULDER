
#include "main.h"
#include "boulder_cgo.h"
#include "ui_renderer.h"
#include <iostream>
#include <memory>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <thread>
#include <chrono>
#include <SDL3/SDL.h>
#include <flecs.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "volk.h"
#include <shaderc/shaderc.hpp>
#include <steam/steam_api.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

// Maximum frames that can be processed simultaneously
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

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

    // Depth buffer
    VkImage depthImage = nullptr;
    VkImageView depthImageView = nullptr;
    VkDeviceMemory depthImageMemory = nullptr;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;

    VkCommandPool commandPool = nullptr;
    std::vector<VkCommandBuffer> commandBuffers;

    // Per-frame synchronization (fixed size for frames in flight)
    VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore renderFinishedSemaphores[MAX_FRAMES_IN_FLIGHT];
    VkFence inFlightFences[MAX_FRAMES_IN_FLIGHT];

    // Per-image fence tracking (which frame is using which image)
    std::vector<VkFence> imagesInFlight; // Initially VK_NULL_HANDLE, set to frame fence when image is acquired

    uint32_t graphicsQueueFamily = UINT32_MAX;
    VkPipelineLayout pipelineLayout = nullptr;
    VkPipeline cubePipeline = nullptr;
    VkShaderModule meshShaderModule = nullptr;
    VkShaderModule fragShaderModule = nullptr;

    // Model rendering pipeline (mesh + fragment)
    VkPipeline modelPipeline = nullptr;
    VkPipelineLayout modelPipelineLayout = nullptr;
    VkShaderModule modelMeshShader = nullptr;
    VkShaderModule modelFragShader = nullptr;
    VkDescriptorSetLayout modelDescriptorSetLayout = nullptr;
    VkDescriptorPool modelDescriptorPools[MAX_FRAMES_IN_FLIGHT] = {}; // One pool per frame-in-flight
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

    // UI System
    std::unique_ptr<boulder::UIRenderer> uiRenderer;
    std::unordered_map<uint64_t, bool> buttonClickStates;
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

// Vertex structure for loaded models - matches GLSL std430 layout
struct Vertex {
    glm::vec3 position;  // 12 bytes, offset 0
    glm::vec3 normal;    // 12 bytes, offset 12
    glm::vec2 texCoord;  // 8 bytes, offset 24
    // Total: 32 bytes (naturally aligned for std430)
};

// Verify struct layout matches GLSL std430
static_assert(sizeof(Vertex) == 32, "Vertex struct size must be 32 bytes to match GLSL std430 layout");
static_assert(offsetof(Vertex, position) == 0, "position offset must be 0");
static_assert(offsetof(Vertex, normal) == 12, "normal offset must be 12");
static_assert(offsetof(Vertex, texCoord) == 24, "texCoord offset must be 24");

// Mesh structure with GPU buffers
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    VkBuffer drawParamsBuffer = VK_NULL_HANDLE;
    VkDeviceMemory drawParamsBufferMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    ~Mesh() {
        // Cleanup is handled separately to ensure proper Vulkan device context
    }
};

// Model component
struct Model {
    std::string path;
    const aiScene* scene;
    std::vector<Mesh> meshes;
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

    setenv("SDL_VIDEODRIVER", "x11", 1);
    
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
        const char* validationLayers[] = {}; //VK_LAYER_KHRONOS_validation

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = sdlExtensionCount + additionalExtensionCount;
        createInfo.ppEnabledExtensionNames = instanceExtensions.get();
        createInfo.enabledLayerCount = 0;
        createInfo.ppEnabledLayerNames = nullptr;

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

// Forward declaration
static void destroyDepthResources();

void boulder_shutdown() {
    if (!g_engine.initialized) {
        return;
    }

    Logger::get().info("Shutting down engine...");

    // Wait for device to be idle before cleanup
    if (g_engine.device) {
        vkDeviceWaitIdle(g_engine.device);
    }

    // Cleanup UI system after device is idle
    boulder_ui_cleanup();

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

        // Cleanup model rendering pipeline and resources
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (g_engine.modelDescriptorPools[i]) {
                vkDestroyDescriptorPool(g_engine.device, g_engine.modelDescriptorPools[i], nullptr);
                g_engine.modelDescriptorPools[i] = nullptr;
            }
        }
        if (g_engine.modelDescriptorSetLayout) {
            vkDestroyDescriptorSetLayout(g_engine.device, g_engine.modelDescriptorSetLayout, nullptr);
            g_engine.modelDescriptorSetLayout = nullptr;
        }
        if (g_engine.modelPipeline) {
            vkDestroyPipeline(g_engine.device, g_engine.modelPipeline, nullptr);
            g_engine.modelPipeline = nullptr;
        }
        if (g_engine.modelPipelineLayout) {
            vkDestroyPipelineLayout(g_engine.device, g_engine.modelPipelineLayout, nullptr);
            g_engine.modelPipelineLayout = nullptr;
        }
        if (g_engine.modelMeshShader) {
            vkDestroyShaderModule(g_engine.device, g_engine.modelMeshShader, nullptr);
            g_engine.modelMeshShader = nullptr;
        }
        if (g_engine.modelFragShader) {
            vkDestroyShaderModule(g_engine.device, g_engine.modelFragShader, nullptr);
            g_engine.modelFragShader = nullptr;
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(g_engine.device, g_engine.imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(g_engine.device, g_engine.renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(g_engine.device, g_engine.inFlightFences[i], nullptr);
        }
        if (g_engine.commandPool) {
            vkDestroyCommandPool(g_engine.device, g_engine.commandPool, nullptr);
            g_engine.commandPool = nullptr;
        }
        for (auto imageView : g_engine.swapchainImageViews) {
            vkDestroyImageView(g_engine.device, imageView, nullptr);
        }
        g_engine.swapchainImageViews.clear();
        destroyDepthResources();
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

    // Cleanup model buffers from all entities before deleting ECS
    if (g_engine.ecs && g_engine.device) {
        auto query = g_engine.ecs->query<Model>();
        query.each([](Model& model) {
            for (auto& mesh : model.meshes) {
                if (mesh.vertexBuffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_engine.device, mesh.vertexBuffer, nullptr);
                    mesh.vertexBuffer = VK_NULL_HANDLE;
                }
                if (mesh.vertexBufferMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(g_engine.device, mesh.vertexBufferMemory, nullptr);
                    mesh.vertexBufferMemory = VK_NULL_HANDLE;
                }
                if (mesh.indexBuffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_engine.device, mesh.indexBuffer, nullptr);
                    mesh.indexBuffer = VK_NULL_HANDLE;
                }
                if (mesh.indexBufferMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(g_engine.device, mesh.indexBufferMemory, nullptr);
                    mesh.indexBufferMemory = VK_NULL_HANDLE;
                }
                if (mesh.drawParamsBuffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(g_engine.device, mesh.drawParamsBuffer, nullptr);
                    mesh.drawParamsBuffer = VK_NULL_HANDLE;
                }
                if (mesh.drawParamsBufferMemory != VK_NULL_HANDLE) {
                    vkFreeMemory(g_engine.device, mesh.drawParamsBufferMemory, nullptr);
                    mesh.drawParamsBufferMemory = VK_NULL_HANDLE;
                }
            }
        });
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

// Helper function to find suitable memory type
static uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_engine.physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    Logger::get().error("Failed to find suitable memory type");
    return 0;
}

// Helper function to create a Vulkan buffer
static bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                        VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(g_engine.device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        Logger::get().error("Failed to create buffer");
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(g_engine.device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(g_engine.device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        Logger::get().error("Failed to allocate buffer memory");
        return false;
    }

    vkBindBufferMemory(g_engine.device, buffer, bufferMemory, 0);
    return true;
}

// Helper function to copy data to a buffer
static void copyDataToBuffer(VkDeviceMemory bufferMemory, const void* data, size_t size) {
    void* mapped;
    vkMapMemory(g_engine.device, bufferMemory, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(g_engine.device, bufferMemory);
}

// Helper function to process a single Assimp mesh
static Mesh processMesh(aiMesh* mesh) {
    Mesh result;

    // Extract vertices
    for (uint32_t i = 0; i < mesh->mNumVertices; i++) {
        Vertex vertex;

        // Position
        vertex.position = glm::vec3(
            mesh->mVertices[i].x,
            mesh->mVertices[i].y,
            mesh->mVertices[i].z
        );

        // Normal
        if (mesh->HasNormals()) {
            vertex.normal = glm::vec3(
                mesh->mNormals[i].x,
                mesh->mNormals[i].y,
                mesh->mNormals[i].z
            );
        } else {
            vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        }

        // Texture coordinates
        if (mesh->mTextureCoords[0]) {
            vertex.texCoord = glm::vec2(
                mesh->mTextureCoords[0][i].x,
                mesh->mTextureCoords[0][i].y
            );
        } else {
            vertex.texCoord = glm::vec2(0.0f, 0.0f);
        }

        result.vertices.push_back(vertex);
    }

    // Extract indices
    for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for (uint32_t j = 0; j < face.mNumIndices; j++) {
            result.indices.push_back(face.mIndices[j]);
        }
    }

    result.indexCount = static_cast<uint32_t>(result.indices.size());

    // Create GPU storage buffers for mesh shaders
    // NOTE: Mesh shaders read from STORAGE_BUFFER, NOT VERTEX_BUFFER
    if (!result.vertices.empty()) {
        VkDeviceSize vertexBufferSize = sizeof(Vertex) * result.vertices.size();
        createBuffer(vertexBufferSize,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    result.vertexBuffer, result.vertexBufferMemory);

        copyDataToBuffer(result.vertexBufferMemory, result.vertices.data(), vertexBufferSize);
    }

    if (!result.indices.empty()) {
        VkDeviceSize indexBufferSize = sizeof(uint32_t) * result.indices.size();
        createBuffer(indexBufferSize,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    result.indexBuffer, result.indexBufferMemory);

        copyDataToBuffer(result.indexBufferMemory, result.indices.data(), indexBufferSize);
    }

    // Create draw params buffer (indexCount, instanceCount)
    struct DrawParams {
        uint32_t indexCount;
        uint32_t instanceCount;
    };
    DrawParams drawParams{result.indexCount, 1};

    VkDeviceSize drawParamsSize = sizeof(DrawParams);
    createBuffer(drawParamsSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                result.drawParamsBuffer, result.drawParamsBufferMemory);

    copyDataToBuffer(result.drawParamsBufferMemory, &drawParams, drawParamsSize);

    Logger::get().info("Processed mesh: {} vertices, {} indices", result.vertices.size(), result.indices.size());

    return result;
}

// Helper function to recursively process Assimp nodes
static void processNode(aiNode* node, const aiScene* scene, std::vector<Mesh>& meshes) {
    // Process all the node's meshes
    for (uint32_t i = 0; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        meshes.push_back(processMesh(mesh));
    }

    // Process children
    for (uint32_t i = 0; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene, meshes);
    }
}

// Helper function to create depth buffer resources
static int createDepthResources() {
    // Create depth image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = g_engine.swapchainExtent.width;
    imageInfo.extent.height = g_engine.swapchainExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = g_engine.depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(g_engine.device, &imageInfo, nullptr, &g_engine.depthImage) != VK_SUCCESS) {
        Logger::get().error("Failed to create depth image");
        return -1;
    }

    // Allocate memory for depth image
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(g_engine.device, g_engine.depthImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(g_engine.device, &allocInfo, nullptr, &g_engine.depthImageMemory) != VK_SUCCESS) {
        Logger::get().error("Failed to allocate depth image memory");
        vkDestroyImage(g_engine.device, g_engine.depthImage, nullptr);
        g_engine.depthImage = nullptr;
        return -1;
    }

    vkBindImageMemory(g_engine.device, g_engine.depthImage, g_engine.depthImageMemory, 0);

    // Create depth image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = g_engine.depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = g_engine.depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(g_engine.device, &viewInfo, nullptr, &g_engine.depthImageView) != VK_SUCCESS) {
        Logger::get().error("Failed to create depth image view");
        vkFreeMemory(g_engine.device, g_engine.depthImageMemory, nullptr);
        vkDestroyImage(g_engine.device, g_engine.depthImage, nullptr);
        g_engine.depthImage = nullptr;
        g_engine.depthImageMemory = nullptr;
        return -1;
    }

    return 0;
}

// Helper function to destroy depth buffer resources
static void destroyDepthResources() {
    if (g_engine.depthImageView) {
        vkDestroyImageView(g_engine.device, g_engine.depthImageView, nullptr);
        g_engine.depthImageView = nullptr;
    }
    if (g_engine.depthImage) {
        vkDestroyImage(g_engine.device, g_engine.depthImage, nullptr);
        g_engine.depthImage = nullptr;
    }
    if (g_engine.depthImageMemory) {
        vkFreeMemory(g_engine.device, g_engine.depthImageMemory, nullptr);
        g_engine.depthImageMemory = nullptr;
    }
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

    // Wait for ALL device operations to complete (including presentation engine)
    // This ensures semaphores are no longer in use before we destroy them
    vkDeviceWaitIdle(g_engine.device);

    // Clean up old swapchain resources
    for (auto imageView : g_engine.swapchainImageViews) {
        vkDestroyImageView(g_engine.device, imageView, nullptr);
    }
    g_engine.swapchainImageViews.clear();

    // Clean up old depth resources
    destroyDepthResources();

    // Clean up old per-frame-in-flight semaphores
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(g_engine.device, g_engine.imageAvailableSemaphores[i], nullptr);
        vkDestroySemaphore(g_engine.device, g_engine.renderFinishedSemaphores[i], nullptr);
    }

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

    // Query available present modes (same as initial swapchain creation)
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_engine.physicalDevice, g_engine.surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_engine.physicalDevice, g_engine.surface, &presentModeCount, presentModes.data());

    // Prefer Immediate (uncapped framerate) for maximum performance, fallback to FIFO (guaranteed support)
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            Logger::get().info("Using Immediate present mode (uncapped framerate)");
            break;
        }
    }
    if (presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        Logger::get().info("Using FIFO present mode (vsync fallback)");
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
    swapchainInfo.presentMode = presentMode;
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

    // Reset per-image fence tracking (no frame is using any image after recreation)
    // Use assign() to clear ALL elements, not just resize which keeps old values if size unchanged
    g_engine.imagesInFlight.assign(imageCount, VK_NULL_HANDLE);

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

    // Recreate depth resources
    if (createDepthResources() != 0) {
        Logger::get().error("Failed to recreate depth resources");
        g_engine.isRecreatingSwapchain = false;
        return -1;
    }

    // Recreate per-frame-in-flight semaphores (both imageAvailable and renderFinished)
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.renderFinishedSemaphores[i]) != VK_SUCCESS) {
            Logger::get().error("Failed to recreate semaphores for frame {}", i);
            g_engine.isRecreatingSwapchain = false;
            return -1;
        }
    }

    // Reset frame index to start fresh
    g_engine.currentFrameIndex = 0;

    // After vkDeviceWaitIdle, all fences are signaled but we need them in a known state
    // Since we're resetting currentFrameIndex to 0, we should ensure fence[0] will work correctly
    // Actually, let's leave fences as-is (signaled) and let normal flow handle them

    // Command buffers are per-frame-in-flight (MAX_FRAMES_IN_FLIGHT), not per-image, so no recreation needed

    Logger::get().info("Swapchain recreated successfully! Frame index reset to 0");

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

// Render all models with the Model component
void boulder_render_models() {
    if (!g_engine.initialized || !g_engine.activeCommandBuffer || !g_engine.modelPipeline || !g_engine.ecs) {
        return;
    }

    vkCmdBindPipeline(g_engine.activeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_engine.modelPipeline);

    // Ensure all storage buffer writes are visible to mesh shader reads
    VkMemoryBarrier memoryBarrier{};
    memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        g_engine.activeCommandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT,
        0,
        1, &memoryBarrier,
        0, nullptr,
        0, nullptr
    );

    // Set dynamic viewport and scissor
    boulder_set_viewport(0.0f, 0.0f, (float)g_engine.swapchainExtent.width, (float)g_engine.swapchainExtent.height, 0.0f, 1.0f);
    boulder_set_scissor(0, 0, g_engine.swapchainExtent.width, g_engine.swapchainExtent.height);

    // Set up view-projection matrix
    float aspect = (float)g_engine.swapchainExtent.width / (float)g_engine.swapchainExtent.height;
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1; // Flip Y for Vulkan

    glm::mat4 view = glm::lookAt(
        glm::vec3(2.0f, 2.0f, 2.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );

    glm::mat4 viewProj = proj * view;

    // Query all entities with Model and Transform components
    auto query = g_engine.ecs->query_builder<const Model, const Transform>().build();

    static bool logged = false;
    int entityCount = 0;

    query.each([&](flecs::entity e, const Model& model, const Transform& transform) {
        entityCount++;
        // Build model matrix from transform
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        modelMatrix = glm::translate(modelMatrix, transform.position);
        modelMatrix = glm::rotate(modelMatrix, transform.rotation.x, glm::vec3(1, 0, 0));
        modelMatrix = glm::rotate(modelMatrix, transform.rotation.y, glm::vec3(0, 1, 0));
        modelMatrix = glm::rotate(modelMatrix, transform.rotation.z, glm::vec3(0, 0, 1));
        modelMatrix = glm::scale(modelMatrix, transform.scale);

        // Render each mesh in the model
        int meshIndex = 0;
        for (const auto& mesh : model.meshes) {
            if (!logged) {
                Logger::get().info("Processing mesh {}: vbuf={:x} ibuf={:x} indices={}",
                                  meshIndex, (uint64_t)mesh.vertexBuffer, (uint64_t)mesh.indexBuffer, mesh.indexCount);
            }

            if (mesh.vertexBuffer == VK_NULL_HANDLE || mesh.indexBuffer == VK_NULL_HANDLE) {
                if (!logged) {
                    Logger::get().error("Skipping mesh {} - null buffers!", meshIndex);
                }
                meshIndex++;
                continue;
            }

            // Allocate descriptor set for this mesh from current frame's pool
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = g_engine.modelDescriptorPools[g_engine.currentFrameIndex];
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &g_engine.modelDescriptorSetLayout;

            VkDescriptorSet descriptorSet;
            if (vkAllocateDescriptorSets(g_engine.device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
                Logger::get().error("Failed to allocate descriptor set for model mesh");
                continue;
            }

            // Update descriptor set with storage buffer bindings
            VkDescriptorBufferInfo vertexBufferInfo{};
            vertexBufferInfo.buffer = mesh.vertexBuffer;
            vertexBufferInfo.offset = 0;
            vertexBufferInfo.range = VK_WHOLE_SIZE;

            VkDescriptorBufferInfo indexBufferInfo{};
            indexBufferInfo.buffer = mesh.indexBuffer;
            indexBufferInfo.offset = 0;
            indexBufferInfo.range = VK_WHOLE_SIZE;

            VkDescriptorBufferInfo drawParamsInfo{};
            drawParamsInfo.buffer = mesh.drawParamsBuffer;
            drawParamsInfo.offset = 0;
            drawParamsInfo.range = VK_WHOLE_SIZE;

            VkWriteDescriptorSet descriptorWrites[3] = {};

            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = descriptorSet;
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].dstArrayElement = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &vertexBufferInfo;

            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].dstSet = descriptorSet;
            descriptorWrites[1].dstBinding = 1;
            descriptorWrites[1].dstArrayElement = 0;
            descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[1].descriptorCount = 1;
            descriptorWrites[1].pBufferInfo = &indexBufferInfo;

            descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[2].dstSet = descriptorSet;
            descriptorWrites[2].dstBinding = 2;
            descriptorWrites[2].dstArrayElement = 0;
            descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorWrites[2].descriptorCount = 1;
            descriptorWrites[2].pBufferInfo = &drawParamsInfo;

            vkUpdateDescriptorSets(g_engine.device, 3, descriptorWrites, 0, nullptr);

            // Bind descriptor set
            vkCmdBindDescriptorSets(g_engine.activeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   g_engine.modelPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

            // Set push constants
            struct ModelPushConstants {
                glm::mat4 viewProj;
                glm::mat4 model;
                uint32_t vertexOffset;
                uint32_t indexOffset;
            } pushConstants;

            pushConstants.viewProj = viewProj;
            pushConstants.model = modelMatrix;
            pushConstants.vertexOffset = 0;
            pushConstants.indexOffset = 0;

            vkCmdPushConstants(g_engine.activeCommandBuffer, g_engine.modelPipelineLayout,
                             VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(ModelPushConstants), &pushConstants);

            // Draw mesh with mesh shader
            // Calculate workgroups needed (30 indices = 10 triangles per workgroup)
            uint32_t numWorkgroups = (mesh.indexCount + 29) / 30;

            if (!logged) {
                Logger::get().info("Drawing mesh: {} indices, {} workgroups", mesh.indexCount, numWorkgroups);
            }

            vkCmdDrawMeshTasksEXT(g_engine.activeCommandBuffer, numWorkgroups, 1, 1);

            meshIndex++;
        }
    });

    // Debug log on first frame
    if (!logged && entityCount > 0) {
        Logger::get().info("Rendering {} entities with models", entityCount);
        logged = true;
    }
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

    // Draw cube with mesh shader (DISABLED for debugging model rendering)
    if (false && g_engine.cubePipeline) {
        vkCmdBindPipeline(g_engine.activeCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_engine.cubePipeline);

        // Set dynamic viewport and scissor
        boulder_set_viewport(0.0f, 0.0f, (float)g_engine.swapchainExtent.width, (float)g_engine.swapchainExtent.height, 0.0f, 1.0f);
        boulder_set_scissor(0, 0, g_engine.swapchainExtent.width, g_engine.swapchainExtent.height);

        // Set up view-projection matrix (simple perspective)
        float aspect = (float)g_engine.swapchainExtent.width / (float)g_engine.swapchainExtent.height;
        glm::mat4 proj = glm::perspective(glm::radians(90.0f), aspect, 0.1f, 100.0f);
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

    // Render models
    boulder_render_models();

    // Render UI overlay on top of the scene
    boulder_ui_render(imageIndex);

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

    // Query mesh shader features to ensure they're actually supported
    VkPhysicalDeviceMeshShaderFeaturesEXT queriedMeshShaderFeatures{};
    queriedMeshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &queriedMeshShaderFeatures;

    vkGetPhysicalDeviceFeatures2(g_engine.physicalDevice, &features2);

    if (!queriedMeshShaderFeatures.meshShader) {
        Logger::get().error("Mesh shader feature NOT supported on this device!");
        return -1;
    }

    Logger::get().info("Mesh shader feature is supported!");

    // Enable mesh shader features for device creation
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

    // Query available present modes
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_engine.physicalDevice, g_engine.surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_engine.physicalDevice, g_engine.surface, &presentModeCount, presentModes.data());

    // Prefer Immediate (uncapped framerate) for maximum performance, fallback to FIFO (guaranteed support)
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            Logger::get().info("Using Immediate present mode (uncapped framerate)");
            break;
        }
    }
    if (presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        Logger::get().info("Using FIFO present mode (vsync fallback)");
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
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(g_engine.device, &swapchainInfo, nullptr, &g_engine.swapchain) != VK_SUCCESS) {
        Logger::get().error("Failed to create swapchain");
        return -1;
    }

    vkGetSwapchainImagesKHR(g_engine.device, g_engine.swapchain, &imageCount, nullptr);
    g_engine.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(g_engine.device, g_engine.swapchain, &imageCount, g_engine.swapchainImages.data());

    // Initialize per-image fence tracking (initially no frame is using any image)
    g_engine.imagesInFlight.resize(imageCount, VK_NULL_HANDLE);

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

    // Create depth resources
    if (createDepthResources() != 0) {
        Logger::get().error("Failed to create depth resources");
        return -1;
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

    // Per-frame-in-flight sync (imageAvailable, renderFinished, and fences)
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(g_engine.device, &semaphoreInfo, nullptr, &g_engine.renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(g_engine.device, &fenceInfo, nullptr, &g_engine.inFlightFences[i]) != VK_SUCCESS) {
            Logger::get().error("Failed to create sync objects for frame {}", i);
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
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // Disable culling for debugging
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

    // Depth stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineRenderingCreateInfo pipelineRenderingInfo{};
    pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    pipelineRenderingInfo.colorAttachmentCount = 1;
    pipelineRenderingInfo.pColorAttachmentFormats = &g_engine.swapchainFormat;
    pipelineRenderingInfo.depthAttachmentFormat = g_engine.depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &pipelineRenderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = g_engine.pipelineLayout;

    if (vkCreateGraphicsPipelines(g_engine.device, nullptr, 1, &pipelineInfo, nullptr, &g_engine.cubePipeline) != VK_SUCCESS) {
        Logger::get().error("Failed to create graphics pipeline");
        return -1;
    }

    Logger::get().info("Vulkan rendering setup complete!");

    // Create model rendering pipeline for loaded geometry
    Logger::get().info("Creating model rendering pipeline...");

    // Load model shaders
    std::ifstream modelMeshFile("shaders/model.mesh");
    std::string modelMeshSource((std::istreambuf_iterator<char>(modelMeshFile)), std::istreambuf_iterator<char>());

    std::ifstream modelFragFile("shaders/model.frag");
    std::string modelFragSource((std::istreambuf_iterator<char>(modelFragFile)), std::istreambuf_iterator<char>());

    if (!modelMeshSource.empty() && !modelFragSource.empty()) {
        auto modelMeshSpirv = compileShader(modelMeshSource, shaderc_glsl_default_mesh_shader, "model.mesh");
        auto modelFragSpirv = compileShader(modelFragSource, shaderc_glsl_default_fragment_shader, "model.frag");

        if (!modelMeshSpirv.empty() && !modelFragSpirv.empty()) {
            // Create shader modules
            VkShaderModuleCreateInfo meshModuleInfo{};
            meshModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            meshModuleInfo.codeSize = modelMeshSpirv.size() * sizeof(uint32_t);
            meshModuleInfo.pCode = modelMeshSpirv.data();
            vkCreateShaderModule(g_engine.device, &meshModuleInfo, nullptr, &g_engine.modelMeshShader);

            VkShaderModuleCreateInfo fragModuleInfo{};
            fragModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            fragModuleInfo.codeSize = modelFragSpirv.size() * sizeof(uint32_t);
            fragModuleInfo.pCode = modelFragSpirv.data();
            vkCreateShaderModule(g_engine.device, &fragModuleInfo, nullptr, &g_engine.modelFragShader);

            // Create descriptor set layout for storage buffers
            VkDescriptorSetLayoutBinding bindings[3] = {};

            // Binding 0: Vertex buffer (SSBO)
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;

            // Binding 1: Index buffer (SSBO)
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;

            // Binding 2: Draw params (SSBO)
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;

            VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
            descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descriptorLayoutInfo.bindingCount = 3;
            descriptorLayoutInfo.pBindings = bindings;
            vkCreateDescriptorSetLayout(g_engine.device, &descriptorLayoutInfo, nullptr, &g_engine.modelDescriptorSetLayout);

            // Create pipeline layout with push constants
            VkPushConstantRange modelPushConstant{};
            modelPushConstant.stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
            modelPushConstant.offset = 0;
            modelPushConstant.size = sizeof(glm::mat4) * 2 + sizeof(uint32_t) * 2; // viewProj + model + 2 offsets

            VkPipelineLayoutCreateInfo modelLayoutInfo{};
            modelLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            modelLayoutInfo.setLayoutCount = 1;
            modelLayoutInfo.pSetLayouts = &g_engine.modelDescriptorSetLayout;
            modelLayoutInfo.pushConstantRangeCount = 1;
            modelLayoutInfo.pPushConstantRanges = &modelPushConstant;
            vkCreatePipelineLayout(g_engine.device, &modelLayoutInfo, nullptr, &g_engine.modelPipelineLayout);

            // Create model pipeline (similar to cube pipeline but with descriptor sets)
            VkPipelineShaderStageCreateInfo modelShaderStages[2] = {};
            modelShaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            modelShaderStages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
            modelShaderStages[0].module = g_engine.modelMeshShader;
            modelShaderStages[0].pName = "main";

            modelShaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            modelShaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            modelShaderStages[1].module = g_engine.modelFragShader;
            modelShaderStages[1].pName = "main";

            VkGraphicsPipelineCreateInfo modelPipelineInfo{};
            modelPipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            modelPipelineInfo.pNext = &pipelineRenderingInfo;
            modelPipelineInfo.stageCount = 2;
            modelPipelineInfo.pStages = modelShaderStages;
            modelPipelineInfo.pViewportState = &viewportState;
            modelPipelineInfo.pRasterizationState = &rasterizer;
            modelPipelineInfo.pMultisampleState = &multisampling;
            modelPipelineInfo.pColorBlendState = &colorBlending;
            modelPipelineInfo.pDepthStencilState = &depthStencil;
            modelPipelineInfo.pDynamicState = &dynamicState;
            modelPipelineInfo.layout = g_engine.modelPipelineLayout;

            if (vkCreateGraphicsPipelines(g_engine.device, nullptr, 1, &modelPipelineInfo, nullptr, &g_engine.modelPipeline) == VK_SUCCESS) {
                Logger::get().info("✓ Model rendering pipeline created");

                // Create descriptor pools for model rendering (one per frame-in-flight)
                // Support up to 1000 descriptor sets with 3 storage buffers each per pool
                VkDescriptorPoolSize poolSize{};
                poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                poolSize.descriptorCount = 3000; // 1000 sets * 3 bindings

                VkDescriptorPoolCreateInfo poolInfo{};
                poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                poolInfo.poolSizeCount = 1;
                poolInfo.pPoolSizes = &poolSize;
                poolInfo.maxSets = 1000;

                for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                    if (vkCreateDescriptorPool(g_engine.device, &poolInfo, nullptr, &g_engine.modelDescriptorPools[i]) != VK_SUCCESS) {
                        Logger::get().error("Failed to create model descriptor pool {}", i);
                    }
                }
                Logger::get().info("✓ Model descriptor pools created ({} pools)", MAX_FRAMES_IN_FLIGHT);
            } else {
                Logger::get().error("Failed to create model pipeline");
            }
        } else {
            Logger::get().warning("Model shaders not compiled - model rendering disabled");
        }
    } else {
        Logger::get().warning("Model shader files not found - model rendering disabled");
    }

    // Initialize UI system now that all Vulkan resources are ready
    if (boulder_ui_init() != 0) {
        Logger::get().error("Failed to initialize UI system (non-fatal)");
        // Don't fail window creation if UI init fails
    }

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

int boulder_get_full_transform(EntityID entity,
                               float* px, float* py, float* pz,
                               float* rx, float* ry, float* rz,
                               float* sx, float* sy, float* sz) {
    if (!g_engine.ecs) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    const Transform* t = e.get<Transform>();
    if (!t) {
        return -1;
    }

    if (px) *px = t->position.x;
    if (py) *py = t->position.y;
    if (pz) *pz = t->position.z;

    if (rx) *rx = t->rotation.x;
    if (ry) *ry = t->rotation.y;
    if (rz) *rz = t->rotation.z;

    if (sx) *sx = t->scale.x;
    if (sy) *sy = t->scale.y;
    if (sz) *sz = t->scale.z;

    return 0;
}

int boulder_set_full_transform(EntityID entity,
                              float px, float py, float pz,
                              float rx, float ry, float rz,
                              float sx, float sy, float sz) {
    if (!g_engine.ecs) {
        return -1;
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    Transform* t = e.get_mut<Transform>();
    if (!t) {
        return -1;
    }

    t->position = glm::vec3(px, py, pz);
    t->rotation = glm::vec3(rx, ry, rz);
    t->scale = glm::vec3(sx, sy, sz);

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
        Logger::get().error("Invalid parameters for loading model");
        return -1;
    }

    if (!g_engine.device) {
        Logger::get().error("Cannot load model: Vulkan device not initialized");
        return -1;
    }

    Logger::get().info("Loading model: {}", path);

    const aiScene* scene = g_engine.importer->ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        Logger::get().error("Failed to load model: {}", g_engine.importer->GetErrorString());
        return -1;
    }

    // Process all meshes in the scene
    Model model;
    model.path = std::string(path);
    model.scene = scene;
    processNode(scene->mRootNode, scene, model.meshes);

    Logger::get().info("✓ Model loaded: {} meshes extracted", model.meshes.size());

    // Debug: Print mesh statistics
    for (size_t i = 0; i < model.meshes.size(); i++) {
        const auto& mesh = model.meshes[i];
        Logger::get().info("  Mesh {}: {} vertices, {} indices",
                          i, mesh.vertices.size(), mesh.indexCount);

        // Debug: Print first few indices to verify they're in range
        if (mesh.indices.size() >= 10) {
            Logger::get().info("    First 10 indices: {} {} {} {} {} {} {} {} {} {}",
                              mesh.indices[0], mesh.indices[1], mesh.indices[2],
                              mesh.indices[3], mesh.indices[4], mesh.indices[5],
                              mesh.indices[6], mesh.indices[7], mesh.indices[8],
                              mesh.indices[9]);
        } else if (!mesh.indices.empty()) {
            Logger::get().info("    All {} indices loaded", mesh.indices.size());
        }
    }

    flecs::entity e = g_engine.ecs->entity(entity);
    e.set<Model>(std::move(model));

    // Ensure buffer writes are visible to GPU before rendering
    // Even with HOST_COHERENT memory, we need an execution dependency
    vkDeviceWaitIdle(g_engine.device);

    Logger::get().info("Model buffers synchronized with GPU");

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

    // Depth stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

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
    renderingInfo.depthAttachmentFormat = g_engine.depthFormat;

    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDepthStencilState = &depthStencil;
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
        Logger::get().info("SWAPCHAIN NEEDS RECREATION. Recreating...");
        return -2;
    }


    // Wait for the fence for this frame
    vkWaitForFences(g_engine.device, 1, &g_engine.inFlightFences[g_engine.currentFrameIndex], VK_TRUE, UINT64_MAX);

    // Acquire next image (before resetting fence, in case acquisition fails)
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
        Logger::get().error("Failed to acquire swapchain image: {}", (int)result);
        return -1;
    }

    // CRITICAL: Check if this swapchain image is still being used BEFORE resetting our fence
    // If the image is assigned to our own fence from a previous cycle, we must wait for it to signal first
    if (g_engine.imagesInFlight[*imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(g_engine.device, 1, &g_engine.imagesInFlight[*imageIndex], VK_TRUE, UINT64_MAX);
    }

    // Now safe to reset our fence (after waiting for any previous use)
    vkResetFences(g_engine.device, 1, &g_engine.inFlightFences[g_engine.currentFrameIndex]);

    // Mark this image as now being used by this frame's fence (BEFORE we start using it)
    g_engine.imagesInFlight[*imageIndex] = g_engine.inFlightFences[g_engine.currentFrameIndex];

    // Reset descriptor pool for THIS FRAME's model rendering
    if (g_engine.modelDescriptorPools[g_engine.currentFrameIndex]) {
        vkResetDescriptorPool(g_engine.device, g_engine.modelDescriptorPools[g_engine.currentFrameIndex], 0);
    }

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

    // Transition image layout from PRESENT_SRC (or UNDEFINED on first frame, which is compatible)
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Transition depth image to depth attachment optimal
    VkImageMemoryBarrier depthBarrier{};
    depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = g_engine.depthImage;
    depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthBarrier.subresourceRange.baseMipLevel = 0;
    depthBarrier.subresourceRange.levelCount = 1;
    depthBarrier.subresourceRange.baseArrayLayer = 0;
    depthBarrier.subresourceRange.layerCount = 1;
    depthBarrier.srcAccessMask = 0;
    depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

    // Begin rendering
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = g_engine.swapchainImageViews[*imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = g_engine.clearColor;

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = g_engine.depthImageView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = g_engine.swapchainExtent;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

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
    // Signal renderFinished semaphore for this frame-in-flight
    VkSemaphore signalSemaphores[] = {g_engine.renderFinishedSemaphores[g_engine.currentFrameIndex]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    auto res = vkQueueSubmit(g_engine.graphicsQueue, 1, &submitInfo, g_engine.inFlightFences[g_engine.currentFrameIndex]);

    if (res != VK_SUCCESS) {
        Logger::get().error("Failed to submit draw command buffer: {}", (int)res);
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

// Network implementation

// Forward declaration
struct BoulderNetworkSession;

// Global GameNetworkingSockets initialization tracking
static bool g_gnsInitialized = false;
static int g_gnsRefCount = 0;
static std::mutex g_gnsInitMutex;
static uint32_t g_steamAppId = 0;
static bool g_steamAPIInitialized = false;

// Global map to track sessions (needed because GNS doesn't support userdata in all callbacks)
static std::unordered_map<HSteamListenSocket, BoulderNetworkSession*> g_serverSessions;
static std::unordered_map<HSteamNetConnection, BoulderNetworkSession*> g_connectionSessions;
static std::mutex g_sessionMapMutex;

struct BoulderNetworkSession {
    ISteamNetworkingSockets* interface = nullptr;
    HSteamListenSocket listenSocket = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup = k_HSteamNetPollGroup_Invalid;
    std::unordered_map<HSteamNetConnection, ConnectionHandle> connectionMap;
    std::unordered_map<ConnectionHandle, HSteamNetConnection> reverseMap;
    ConnectionHandle nextConnectionId = 1;
    std::queue<NetworkEvent> eventQueue;
    std::mutex eventMutex;
    bool isServer = false;

    static void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char* pszMsg) {
        if (eType == k_ESteamNetworkingSocketsDebugOutputType_Msg ||
            eType == k_ESteamNetworkingSocketsDebugOutputType_Warning ||
            eType == k_ESteamNetworkingSocketsDebugOutputType_Error) {
            Logger::get().info("[GNS] {}", pszMsg);
        }
    }

    ConnectionHandle getOrCreateConnectionHandle(HSteamNetConnection conn) {
        auto it = connectionMap.find(conn);
        if (it != connectionMap.end()) {
            return it->second;
        }
        ConnectionHandle handle = nextConnectionId++;
        connectionMap[conn] = handle;
        reverseMap[handle] = conn;
        return handle;
    }

    void removeConnection(HSteamNetConnection conn) {
        auto it = connectionMap.find(conn);
        if (it != connectionMap.end()) {
            reverseMap.erase(it->second);
            connectionMap.erase(it);
        }
    }

    void processCallbacks() {
        if (!interface) return;

        // Process incoming messages
        ISteamNetworkingMessage* messages[256];
        int numMessages = interface->ReceiveMessagesOnPollGroup(pollGroup, messages, 256);

        for (int i = 0; i < numMessages; i++) {
            auto msg = messages[i];
            ConnectionHandle handle = getOrCreateConnectionHandle(msg->m_conn);

            NetworkEvent event;
            event.type = 3; // Message
            event.connection = handle;
            event.dataSize = msg->m_cbSize;
            event.data = new uint8_t[msg->m_cbSize];
            memcpy(event.data, msg->m_pData, msg->m_cbSize);

            std::lock_guard<std::mutex> lock(eventMutex);
            eventQueue.push(event);

            msg->Release();
        }
    }

    static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo, BoulderNetworkSession* session) {
        if (!session) return;

        switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connected: {
                ConnectionHandle handle = session->getOrCreateConnectionHandle(pInfo->m_hConn);
                if (session->pollGroup != k_HSteamNetPollGroup_Invalid) {
                    session->interface->SetConnectionPollGroup(pInfo->m_hConn, session->pollGroup);
                }

                NetworkEvent event;
                event.type = 1; // Connected
                event.connection = handle;
                event.data = nullptr;
                event.dataSize = 0;

                std::lock_guard<std::mutex> lock(session->eventMutex);
                session->eventQueue.push(event);

                Logger::get().info("Connection established: {}", handle);
                break;
            }

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                auto it = session->connectionMap.find(pInfo->m_hConn);
                if (it != session->connectionMap.end()) {
                    ConnectionHandle handle = it->second;

                    NetworkEvent event;
                    event.type = 2; // Disconnected
                    event.connection = handle;
                    event.data = nullptr;
                    event.dataSize = 0;

                    std::lock_guard<std::mutex> lock(session->eventMutex);
                    session->eventQueue.push(event);

                    Logger::get().info("Connection closed: {}", handle);
                    session->removeConnection(pInfo->m_hConn);
                }

                session->interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                break;
            }

            case k_ESteamNetworkingConnectionState_Connecting: {
                if (session->isServer) {
                    // Register incoming connection in global map
                    {
                        std::lock_guard<std::mutex> lock(g_sessionMapMutex);
                        g_connectionSessions[pInfo->m_hConn] = session;
                    }

                    if (session->interface->AcceptConnection(pInfo->m_hConn) != k_EResultOK) {
                        session->interface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                        Logger::get().error("Failed to accept incoming connection");
                    }
                }
                break;
            }

            default:
                break;
        }
    }
};

static void GlobalConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo) {
    BoulderNetworkSession* session = nullptr;

    // Find session with mutex locked, then unlock before calling OnConnectionStatusChanged
    {
        std::lock_guard<std::mutex> lock(g_sessionMapMutex);

        // Try to find session from connection map first
        auto connIt = g_connectionSessions.find(pInfo->m_hConn);
        if (connIt != g_connectionSessions.end()) {
            session = connIt->second;
        }

        // If not found, try to find from server listen socket (for incoming connections)
        if (!session && pInfo->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
            auto serverIt = g_serverSessions.find(pInfo->m_info.m_hListenSocket);
            if (serverIt != g_serverSessions.end()) {
                session = serverIt->second;
            }
        }
    } // Mutex unlocked here!

    // Call OnConnectionStatusChanged without holding the mutex to avoid deadlock
    if (session) {
        BoulderNetworkSession::OnConnectionStatusChanged(pInfo, session);
    }
}

NetworkSession boulder_create_network_session() {
    // Initialize GNS globally with reference counting
    {
        std::lock_guard<std::mutex> lock(g_gnsInitMutex);
        if (!g_gnsInitialized) {
            SteamDatagramErrMsg errMsg;

            // If Steam AppID is set, initialize Steam API first
            if (g_steamAppId != 0 && !g_steamAPIInitialized) {
                Logger::get().info("Initializing Steam API with AppID {}", g_steamAppId);

                if (!SteamAPI_Init()) {
                    Logger::get().error("Failed to initialize Steam API!");
                    Logger::get().error("Make sure:");
                    Logger::get().error("  1. Steam is running");
                    Logger::get().error("  2. steam_appid.txt exists with AppID {}", g_steamAppId);
                    Logger::get().error("  3. You're logged into Steam");
                    Logger::get().warning("Continuing without Steam authentication (P2P will not work)");
                } else {
                    Logger::get().info("✓ Steam API initialized successfully!");
                    g_steamAPIInitialized = true;
                }
            }

            // Initialize GameNetworkingSockets
            if (g_steamAppId != 0) {
                Logger::get().info("Initializing GameNetworkingSockets with Steam integration");
            }

            if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
                Logger::get().error("Failed to initialize GameNetworkingSockets: {}", errMsg);
                return nullptr;
            }
            g_gnsInitialized = true;

            // If using Steam, wait a moment for authentication
            if (g_steamAppId != 0 && g_steamAPIInitialized) {
                Logger::get().info("Waiting for Steam authentication...");
                // Give Steam time to authenticate
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Set debug output
            SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg,
                                                            BoulderNetworkSession::DebugOutput);

            // Set global connection status changed callback
            SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(GlobalConnectionStatusChanged);
        }
        g_gnsRefCount++;
    }

    BoulderNetworkSession* session = new BoulderNetworkSession();
    session->interface = SteamNetworkingSockets();

    if (!session->interface) {
        Logger::get().error("Failed to get SteamNetworkingSockets interface");
        std::lock_guard<std::mutex> lock(g_gnsInitMutex);
        g_gnsRefCount--;
        delete session;
        return nullptr;
    }

    session->pollGroup = session->interface->CreatePollGroup();
    if (session->pollGroup == k_HSteamNetPollGroup_Invalid) {
        Logger::get().error("Failed to create poll group");
        std::lock_guard<std::mutex> lock(g_gnsInitMutex);
        g_gnsRefCount--;
        delete session;
        return nullptr;
    }

    Logger::get().info("Network session created");
    return session;
}

void boulder_destroy_network_session(NetworkSession session) {
    if (!session) return;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    if (s->listenSocket != k_HSteamListenSocket_Invalid) {
        s->interface->CloseListenSocket(s->listenSocket);
    }

    if (s->pollGroup != k_HSteamNetPollGroup_Invalid) {
        s->interface->DestroyPollGroup(s->pollGroup);
    }

    // Close all connections
    for (const auto& [conn, handle] : s->connectionMap) {
        s->interface->CloseConnection(conn, 0, "Session destroyed", false);
    }

    delete s;

    // Decrement reference count and kill GNS if no more sessions
    {
        std::lock_guard<std::mutex> lock(g_gnsInitMutex);
        g_gnsRefCount--;
        if (g_gnsRefCount == 0 && g_gnsInitialized) {
            GameNetworkingSockets_Kill();
            g_gnsInitialized = false;

            // Shutdown Steam API if it was initialized
            if (g_steamAPIInitialized) {
                SteamAPI_Shutdown();
                g_steamAPIInitialized = false;
                Logger::get().info("Steam API shutdown");
            }
        }
    }

    Logger::get().info("Network session destroyed");
}

void boulder_network_update(NetworkSession session) {
    if (!session) return;

    // Run Steam API callbacks if initialized
    if (g_steamAPIInitialized) {
        SteamAPI_RunCallbacks();
    }

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);
    s->interface->RunCallbacks();
    s->processCallbacks();
}

int boulder_start_server(NetworkSession session, uint16_t port) {
    if (!session) return -1;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;

    s->listenSocket = s->interface->CreateListenSocketIP(addr, 0, nullptr);
    if (s->listenSocket == k_HSteamListenSocket_Invalid) {
        Logger::get().error("Failed to create listen socket on port {}", port);
        return -1;
    }

    // Register server in global map
    {
        std::lock_guard<std::mutex> lock(g_sessionMapMutex);
        g_serverSessions[s->listenSocket] = s;
    }

    s->isServer = true;
    Logger::get().info("Server started on port {}", port);
    return 0;
}

void boulder_stop_server(NetworkSession session) {
    if (!session) return;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    if (s->listenSocket != k_HSteamListenSocket_Invalid) {
        // Remove from global map
        {
            std::lock_guard<std::mutex> lock(g_sessionMapMutex);
            g_serverSessions.erase(s->listenSocket);
        }

        s->interface->CloseListenSocket(s->listenSocket);
        s->listenSocket = k_HSteamListenSocket_Invalid;
        s->isServer = false;
        Logger::get().info("Server stopped");
    }
}

ConnectionHandle boulder_connect(NetworkSession session, const char* address, uint16_t port) {
    if (!session || !address) return 0;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    SteamNetworkingIPAddr addr;
    if (!addr.ParseString(address)) {
        Logger::get().error("Failed to parse address: {}", address);
        return 0;
    }
    addr.m_port = port;

    HSteamNetConnection conn = s->interface->ConnectByIPAddress(addr, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid) {
        Logger::get().error("Failed to connect to {}:{}", address, port);
        return 0;
    }

    // Register connection in global map
    {
        std::lock_guard<std::mutex> lock(g_sessionMapMutex);
        g_connectionSessions[conn] = s;
    }

    ConnectionHandle handle = s->getOrCreateConnectionHandle(conn);
    Logger::get().info("Connecting to {}:{} (handle: {})", address, port, handle);
    return handle;
}

void boulder_disconnect(NetworkSession session, ConnectionHandle conn) {
    if (!session) return;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);
    auto it = s->reverseMap.find(conn);
    if (it != s->reverseMap.end()) {
        // Remove from global map
        {
            std::lock_guard<std::mutex> lock(g_sessionMapMutex);
            g_connectionSessions.erase(it->second);
        }

        s->interface->CloseConnection(it->second, 0, "Disconnected by user", false);
        s->removeConnection(it->second);
        Logger::get().info("Disconnected connection {}", conn);
    }
}

int boulder_connection_state(NetworkSession session, ConnectionHandle conn) {
    if (!session) return -1;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);
    auto it = s->reverseMap.find(conn);
    if (it == s->reverseMap.end()) {
        return -1; // Invalid connection
    }

    SteamNetConnectionInfo_t info;
    if (s->interface->GetConnectionInfo(it->second, &info)) {
        return static_cast<int>(info.m_eState);
    }

    return -1;
}

// Relay and P2P functions
void boulder_network_init_with_steam_app(uint32_t appId) {
    std::lock_guard<std::mutex> lock(g_gnsInitMutex);
    if (!g_gnsInitialized) {
        g_steamAppId = appId;

        // Create steam_appid.txt file for Steam to recognize the AppID
        FILE* f = fopen("steam_appid.txt", "w");
        if (f) {
            fprintf(f, "%u\n", appId);
            fclose(f);
            Logger::get().info("Created steam_appid.txt with AppID {}", appId);
        }

        Logger::get().info("Steam AppID set to {} (will be used on next session creation)", appId);
    }
}

void boulder_network_set_relay_server(const char* address, uint16_t port) {
    if (!address) return;

    // This sets the SDR (Steam Datagram Relay) configuration
    // Note: This is a simplified version. Full implementation would need proper SDR config
    SteamNetworkingIPAddr relayAddr;
    if (relayAddr.ParseString(address)) {
        relayAddr.m_port = port;
        // Configure relay through GNS utils
        Logger::get().info("Relay server set to {}:{}", address, port);
    }
}

void boulder_network_enable_fake_ip() {
    // Enable FakeIP allocation for easier P2P testing without Steam
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(
        k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
    Logger::get().info("FakeIP enabled for testing");
}

int boulder_start_server_p2p(NetworkSession session, int virtualPort) {
    if (!session) return -1;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    // Create P2P listen socket on virtual port
    s->listenSocket = s->interface->CreateListenSocketP2P(virtualPort, 0, nullptr);
    if (s->listenSocket == k_HSteamListenSocket_Invalid) {
        Logger::get().error("Failed to create P2P listen socket on virtual port {}", virtualPort);
        return -1;
    }

    // Register server in global map
    {
        std::lock_guard<std::mutex> lock(g_sessionMapMutex);
        g_serverSessions[s->listenSocket] = s;
    }

    s->isServer = true;
    Logger::get().info("P2P server started on virtual port {}", virtualPort);
    return 0;
}

ConnectionHandle boulder_connect_p2p(NetworkSession session, SteamID steamID, int virtualPort) {
    if (!session) return 0;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    // Create identity from Steam ID
    SteamNetworkingIdentity identity;
    identity.SetSteamID64(steamID);

    // Connect via P2P
    HSteamNetConnection conn = s->interface->ConnectP2P(identity, virtualPort, 0, nullptr);
    if (conn == k_HSteamNetConnection_Invalid) {
        Logger::get().error("Failed to connect P2P to Steam ID {}", steamID);
        return 0;
    }

    // Register connection in global map
    {
        std::lock_guard<std::mutex> lock(g_sessionMapMutex);
        g_connectionSessions[conn] = s;
    }

    ConnectionHandle handle = s->getOrCreateConnectionHandle(conn);
    Logger::get().info("Connecting P2P to Steam ID {} (handle: {})", steamID, handle);
    return handle;
}

void boulder_set_local_identity(NetworkSession session, const char* name) {
    if (!session || !name) return;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    // Set a friendly name for this connection
    // This is useful for debugging and doesn't require Steam authentication
    Logger::get().info("Local identity set to: {}", name);
}

SteamID boulder_get_local_steam_id(NetworkSession session) {
    if (!session) return 0;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    // Get local identity
    SteamNetworkingIdentity identity;
    s->interface->GetIdentity(&identity);

    if (identity.IsInvalid()) {
        Logger::get().warning("Steam identity is invalid - Steam may not be running or not authenticated");
        Logger::get().info("Make sure:");
        Logger::get().info("  1. Steam is running");
        Logger::get().info("  2. steam_appid.txt exists with AppID {}", g_steamAppId);
        Logger::get().info("  3. You're logged into Steam");
        return 0;
    }

    SteamID steamID = identity.GetSteamID64();
    if (steamID != 0) {
        Logger::get().info("Authenticated with Steam ID: {}", steamID);
    }

    return steamID;
}

int boulder_send_message(NetworkSession session, ConnectionHandle conn, const void* data, uint32_t size, int reliable) {
    if (!session || !data || size == 0) return -1;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);
    auto it = s->reverseMap.find(conn);
    if (it == s->reverseMap.end()) {
        return -1; // Invalid connection
    }

    int flags = reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable;
    EResult result = s->interface->SendMessageToConnection(it->second, data, size, flags, nullptr);

    if (result != k_EResultOK) {
        Logger::get().error("Failed to send message: {}", (int)result);
        return -1;
    }

    return 0;
}

int boulder_poll_network_event(NetworkSession session, NetworkEvent* event) {
    if (!session || !event) return 0;

    BoulderNetworkSession* s = static_cast<BoulderNetworkSession*>(session);

    std::lock_guard<std::mutex> lock(s->eventMutex);
    if (s->eventQueue.empty()) {
        event->type = 0; // No event
        return 0;
    }

    *event = s->eventQueue.front();
    s->eventQueue.pop();
    return 1;
}

void boulder_free_network_event_data(void* data) {
    if (data) {
        delete[] static_cast<uint8_t*>(data);
    }
}

// ============================================================================
// UI System Implementation
// ============================================================================

int boulder_ui_init() {
    if (!g_engine.device || !g_engine.physicalDevice) {
        Logger::get().error("Cannot initialize UI: Vulkan not initialized");
        return -1;
    }

    g_engine.uiRenderer = std::make_unique<boulder::UIRenderer>();

    if (!g_engine.uiRenderer->initialize(g_engine.device, g_engine.physicalDevice,
                                         g_engine.swapchainFormat, g_engine.commandPool,
                                         g_engine.graphicsQueue, g_engine.graphicsQueueFamily)) {
        Logger::get().error("Failed to initialize UI renderer");
        g_engine.uiRenderer.reset();
        return -1;
    }

    // Set initial screen size
    g_engine.uiRenderer->updateScreenSize(g_engine.swapchainExtent.width,
                                         g_engine.swapchainExtent.height);

    Logger::get().info("UI system initialized successfully");
    return 0;
}

void boulder_ui_cleanup() {
    if (g_engine.uiRenderer) {
        g_engine.uiRenderer->cleanup();
        g_engine.uiRenderer.reset();
    }
    g_engine.buttonClickStates.clear();
}

UIButtonID boulder_ui_create_button(float x, float y, float width, float height,
                                    float normalR, float normalG, float normalB, float normalA,
                                    float hoverR, float hoverG, float hoverB, float hoverA,
                                    float pressedR, float pressedG, float pressedB, float pressedA) {
    if (!g_engine.uiRenderer) {
        Logger::get().error("UI renderer not initialized");
        return 0;
    }

    glm::vec2 position(x, y);
    glm::vec2 size(width, height);
    glm::vec4 normalColor(normalR, normalG, normalB, normalA);
    glm::vec4 hoverColor(hoverR, hoverG, hoverB, hoverA);
    glm::vec4 pressedColor(pressedR, pressedG, pressedB, pressedA);

    UIButtonID buttonId = g_engine.uiRenderer->createButton(position, size, normalColor,
                                                            hoverColor, pressedColor);

    // Set up click tracking
    g_engine.buttonClickStates[buttonId] = false;

    // Set up callback to track clicks
    g_engine.uiRenderer->setButtonCallback(buttonId, [buttonId]() {
        g_engine.buttonClickStates[buttonId] = true;
    });

    return buttonId;
}

void boulder_ui_destroy_button(UIButtonID buttonId) {
    if (!g_engine.uiRenderer) {
        return;
    }

    g_engine.uiRenderer->destroyButton(buttonId);
    g_engine.buttonClickStates.erase(buttonId);
}

void boulder_ui_set_button_position(UIButtonID buttonId, float x, float y) {
    if (!g_engine.uiRenderer) {
        return;
    }

    g_engine.uiRenderer->setButtonPosition(buttonId, glm::vec2(x, y));
}

void boulder_ui_set_button_size(UIButtonID buttonId, float width, float height) {
    if (!g_engine.uiRenderer) {
        return;
    }

    g_engine.uiRenderer->setButtonSize(buttonId, glm::vec2(width, height));
}

void boulder_ui_set_button_enabled(UIButtonID buttonId, int enabled) {
    if (!g_engine.uiRenderer) {
        return;
    }

    g_engine.uiRenderer->setButtonEnabled(buttonId, enabled != 0);
}

void boulder_ui_handle_mouse_move(float x, float y) {
    if (!g_engine.uiRenderer) {
        return;
    }

    g_engine.uiRenderer->handleMouseMove(x, y);
}

void boulder_ui_handle_mouse_down(float x, float y) {
    if (!g_engine.uiRenderer) {
        return;
    }

    g_engine.uiRenderer->handleMouseDown(x, y);
}

void boulder_ui_handle_mouse_up(float x, float y) {
    if (!g_engine.uiRenderer) {
        return;
    }

    g_engine.uiRenderer->handleMouseUp(x, y);
}

int boulder_ui_button_was_clicked(UIButtonID buttonId) {
    auto it = g_engine.buttonClickStates.find(buttonId);
    if (it != g_engine.buttonClickStates.end()) {
        return it->second ? 1 : 0;
    }
    return 0;
}

void boulder_ui_reset_button_click(UIButtonID buttonId) {
    auto it = g_engine.buttonClickStates.find(buttonId);
    if (it != g_engine.buttonClickStates.end()) {
        it->second = false;
    }
}

void boulder_ui_render(uint32_t imageIndex) {
    if (!g_engine.uiRenderer || !g_engine.activeCommandBuffer) {
        return;
    }

    if (imageIndex >= g_engine.swapchainImages.size()) {
        Logger::get().error("Invalid image index for UI rendering");
        return;
    }

    g_engine.uiRenderer->render(g_engine.activeCommandBuffer, g_engine.swapchainExtent,
                                g_engine.swapchainImages[imageIndex],
                                g_engine.swapchainImageViews[imageIndex]);
}

} // extern "C"