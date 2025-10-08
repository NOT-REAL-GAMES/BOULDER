#pragma once

#include <vector>
#include <unordered_map>
#include <functional>
#include <string>
#include "volk.h"
#include <glm/glm.hpp>

namespace boulder {

// UI Button state
enum class ButtonState {
    Normal,
    Hovered,
    Pressed
};

// UI Button structure
struct UIButton {
    uint64_t id;
    glm::vec2 position;      // Screen-space position (pixels)
    glm::vec2 size;          // Size in pixels
    glm::vec4 normalColor;   // RGBA color when normal
    glm::vec4 hoverColor;    // RGBA color when hovered
    glm::vec4 pressedColor;  // RGBA color when pressed
    ButtonState state;
    bool enabled;
    std::function<void()> onClick;
};

// Vertex format for UI quads
struct UIVertex {
    glm::vec2 position;  // Screen-space position
    glm::vec4 color;     // Vertex color
};

// Push constants for UI rendering
struct UIPushConstants {
    glm::vec2 screenSize;  // Screen dimensions for coordinate conversion
    float padding[2];      // Align to 16 bytes
};

class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    // Initialize the UI renderer with Vulkan resources
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                   VkFormat swapchainFormat, VkCommandPool commandPool,
                   VkQueue graphicsQueue, uint32_t graphicsQueueFamily);

    // Cleanup Vulkan resources
    void cleanup();

    // Create and manage buttons
    uint64_t createButton(const glm::vec2& position, const glm::vec2& size,
                         const glm::vec4& normalColor, const glm::vec4& hoverColor,
                         const glm::vec4& pressedColor);

    void destroyButton(uint64_t buttonId);
    void setButtonCallback(uint64_t buttonId, std::function<void()> callback);
    void setButtonPosition(uint64_t buttonId, const glm::vec2& position);
    void setButtonSize(uint64_t buttonId, const glm::vec2& size);
    void setButtonEnabled(uint64_t buttonId, bool enabled);

    // Input handling
    void handleMouseMove(float x, float y);
    void handleMouseDown(float x, float y);
    void handleMouseUp(float x, float y);

    // Render the UI overlay
    void render(VkCommandBuffer commandBuffer, const VkExtent2D& swapchainExtent,
               VkImage swapchainImage, VkImageView swapchainImageView);

    // Update screen size (call when window resizes)
    void updateScreenSize(uint32_t width, uint32_t height);

private:
    // Vulkan resources
    VkDevice m_device = nullptr;
    VkPhysicalDevice m_physicalDevice = nullptr;
    VkPipeline m_pipeline = nullptr;
    VkPipelineLayout m_pipelineLayout = nullptr;
    VkShaderModule m_vertShader = nullptr;
    VkShaderModule m_fragShader = nullptr;
    VkBuffer m_vertexBuffer = nullptr;
    VkDeviceMemory m_vertexBufferMemory = nullptr;
    VkBuffer m_indexBuffer = nullptr;
    VkDeviceMemory m_indexBufferMemory = nullptr;
    VkCommandPool m_commandPool = nullptr;
    VkQueue m_graphicsQueue = nullptr;

    // UI state
    std::unordered_map<uint64_t, UIButton> m_buttons;
    uint64_t m_nextButtonId = 1;
    glm::vec2 m_mousePosition = {0.0f, 0.0f};
    uint64_t m_hoveredButtonId = 0;
    uint64_t m_pressedButtonId = 0;
    uint32_t m_screenWidth = 800;
    uint32_t m_screenHeight = 600;

    // Helper functions
    bool createShaders();
    bool createPipeline(VkFormat swapchainFormat);
    bool createBuffers();
    void updateVertexBuffer();
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    bool isPointInButton(const glm::vec2& point, const UIButton& button);
    void updateButtonStates();
};

} // namespace boulder
