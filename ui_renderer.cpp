#include "ui_renderer.h"
#include "main.h"
#include <cstring>
#include <algorithm>
#include <shaderc/shaderc.hpp>

namespace boulder {

// Embedded shader sources (will be compiled at runtime)
static const char* UI_VERTEX_SHADER = R"(
#version 450

layout(push_constant) uniform PushConstants {
    vec2 screenSize;
    vec2 padding;
} pushConstants;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

void main() {
    // Convert screen-space coordinates to NDC (-1 to 1)
    // In Vulkan NDC: (-1,-1) is top-left, (1,1) is bottom-right
    // Screen space: (0,0) is top-left, (width,height) is bottom-right
    vec2 ndc = (inPosition / pushConstants.screenSize) * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
    fragColor = inColor;
}
)";

static const char* UI_FRAGMENT_SHADER = R"(
#version 450

layout(location = 0) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = fragColor;
}
)";

UIRenderer::UIRenderer() {
}

UIRenderer::~UIRenderer() {
    cleanup();
}

bool UIRenderer::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                           VkFormat swapchainFormat, VkCommandPool commandPool,
                           VkQueue graphicsQueue, uint32_t graphicsQueueFamily) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_commandPool = commandPool;
    m_graphicsQueue = graphicsQueue;

    if (!createShaders()) {
        Logger::get().error("Failed to create UI shaders");
        return false;
    }

    if (!createPipeline(swapchainFormat)) {
        Logger::get().error("Failed to create UI pipeline");
        return false;
    }

    if (!createBuffers()) {
        Logger::get().error("Failed to create UI buffers");
        return false;
    }

    Logger::get().info("UI Renderer initialized successfully");
    return true;
}

void UIRenderer::cleanup() {
    if (m_device) {
        if (m_vertexBuffer) {
            vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
            m_vertexBuffer = nullptr;
        }
        if (m_vertexBufferMemory) {
            vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
            m_vertexBufferMemory = nullptr;
        }
        if (m_indexBuffer) {
            vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
            m_indexBuffer = nullptr;
        }
        if (m_indexBufferMemory) {
            vkFreeMemory(m_device, m_indexBufferMemory, nullptr);
            m_indexBufferMemory = nullptr;
        }
        if (m_pipeline) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            m_pipeline = nullptr;
        }
        if (m_pipelineLayout) {
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            m_pipelineLayout = nullptr;
        }
        if (m_vertShader) {
            vkDestroyShaderModule(m_device, m_vertShader, nullptr);
            m_vertShader = nullptr;
        }
        if (m_fragShader) {
            vkDestroyShaderModule(m_device, m_fragShader, nullptr);
            m_fragShader = nullptr;
        }
    }
}

uint64_t UIRenderer::createButton(const glm::vec2& position, const glm::vec2& size,
                                  const glm::vec4& normalColor, const glm::vec4& hoverColor,
                                  const glm::vec4& pressedColor) {
    UIButton button;
    button.id = m_nextButtonId++;
    button.position = position;
    button.size = size;
    button.normalColor = normalColor;
    button.hoverColor = hoverColor;
    button.pressedColor = pressedColor;
    button.state = ButtonState::Normal;
    button.enabled = true;
    button.onClick = nullptr;

    m_buttons[button.id] = button;
    updateVertexBuffer();

    return button.id;
}

void UIRenderer::destroyButton(uint64_t buttonId) {
    m_buttons.erase(buttonId);
    updateVertexBuffer();
}

void UIRenderer::setButtonCallback(uint64_t buttonId, std::function<void()> callback) {
    auto it = m_buttons.find(buttonId);
    if (it != m_buttons.end()) {
        it->second.onClick = callback;
    }
}

void UIRenderer::setButtonPosition(uint64_t buttonId, const glm::vec2& position) {
    auto it = m_buttons.find(buttonId);
    if (it != m_buttons.end()) {
        it->second.position = position;
        updateVertexBuffer();
    }
}

void UIRenderer::setButtonSize(uint64_t buttonId, const glm::vec2& size) {
    auto it = m_buttons.find(buttonId);
    if (it != m_buttons.end()) {
        it->second.size = size;
        updateVertexBuffer();
    }
}

void UIRenderer::setButtonEnabled(uint64_t buttonId, bool enabled) {
    auto it = m_buttons.find(buttonId);
    if (it != m_buttons.end()) {
        it->second.enabled = enabled;
    }
}

void UIRenderer::handleMouseMove(float x, float y) {
    m_mousePosition = glm::vec2(x, y);
    updateButtonStates();
}

void UIRenderer::handleMouseDown(float x, float y) {
    m_mousePosition = glm::vec2(x, y);

    for (auto& [id, button] : m_buttons) {
        if (button.enabled && isPointInButton(m_mousePosition, button)) {
            button.state = ButtonState::Pressed;
            m_pressedButtonId = id;
            updateVertexBuffer();
            break;
        }
    }
}

void UIRenderer::handleMouseUp(float x, float y) {
    m_mousePosition = glm::vec2(x, y);

    if (m_pressedButtonId != 0) {
        auto it = m_buttons.find(m_pressedButtonId);
        if (it != m_buttons.end()) {
            UIButton& button = it->second;

            // If mouse is still over the button, trigger the click
            if (isPointInButton(m_mousePosition, button) && button.onClick) {
                button.onClick();
            }

            m_pressedButtonId = 0;
        }
    }

    updateButtonStates();
}

void UIRenderer::render(VkCommandBuffer commandBuffer, const VkExtent2D& swapchainExtent,
                       VkImage swapchainImage, VkImageView swapchainImageView) {
    if (m_buttons.empty()) {
        return;
    }

    // Bind the UI pipeline
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Update push constants with screen size
    UIPushConstants pushConstants;
    pushConstants.screenSize = glm::vec2(swapchainExtent.width, swapchainExtent.height);
    vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                      0, sizeof(UIPushConstants), &pushConstants);

    // Bind vertex and index buffers
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT16);

    // Draw all buttons
    uint32_t indexCount = static_cast<uint32_t>(m_buttons.size() * 6);
    vkCmdDrawIndexed(commandBuffer, indexCount, 1, 0, 0, 0);
}

void UIRenderer::updateScreenSize(uint32_t width, uint32_t height) {
    m_screenWidth = width;
    m_screenHeight = height;
}

bool UIRenderer::createShaders() {
    // Compile vertex shader
    std::vector<uint32_t> vertSpirv;
    std::vector<uint32_t> fragSpirv;

    {
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        options.SetTargetSpirv(shaderc_spirv_version_1_5);

        auto vertResult = compiler.CompileGlslToSpv(UI_VERTEX_SHADER, shaderc_shader_kind::shaderc_glsl_vertex_shader, "ui_vertex", options);
        if (vertResult.GetCompilationStatus() != shaderc_compilation_status_success) {
            Logger::get().error("UI vertex shader compilation failed: {}", vertResult.GetErrorMessage());
            return false;
        }
        vertSpirv.assign(vertResult.cbegin(), vertResult.cend());

        auto fragResult = compiler.CompileGlslToSpv(UI_FRAGMENT_SHADER, shaderc_shader_kind::shaderc_glsl_fragment_shader, "ui_fragment", options);
        if (fragResult.GetCompilationStatus() != shaderc_compilation_status_success) {
            Logger::get().error("UI fragment shader compilation failed: {}", fragResult.GetErrorMessage());
            return false;
        }
        fragSpirv.assign(fragResult.cbegin(), fragResult.cend());
    }

    // Create vertex shader module
    VkShaderModuleCreateInfo vertShaderInfo{};
    vertShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertShaderInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);
    vertShaderInfo.pCode = vertSpirv.data();

    if (vkCreateShaderModule(m_device, &vertShaderInfo, nullptr, &m_vertShader) != VK_SUCCESS) {
        Logger::get().error("Failed to create UI vertex shader module");
        return false;
    }

    // Create fragment shader module
    VkShaderModuleCreateInfo fragShaderInfo{};
    fragShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragShaderInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);
    fragShaderInfo.pCode = fragSpirv.data();

    if (vkCreateShaderModule(m_device, &fragShaderInfo, nullptr, &m_fragShader) != VK_SUCCESS) {
        Logger::get().error("Failed to create UI fragment shader module");
        return false;
    }

    Logger::get().info("UI shaders created successfully");
    return true;
}

bool UIRenderer::createPipeline(VkFormat swapchainFormat) {
    // Create push constant range
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(UIPushConstants);

    // Create pipeline layout
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        Logger::get().error("Failed to create UI pipeline layout");
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = m_vertShader;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = m_fragShader;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertStage, fragStage};

    // Vertex input
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(UIVertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributeDescriptions[2];
    // Position
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(UIVertex, position);
    // Color
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(UIVertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 2;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport and scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterization
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color blending (enable alpha blending for UI)
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // Dynamic state
    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Rendering info for dynamic rendering (Vulkan 1.3+)
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapchainFormat;

    // Create graphics pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;  // Using dynamic rendering
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        Logger::get().error("Failed to create UI graphics pipeline");
        return false;
    }

    Logger::get().info("UI pipeline created successfully");
    return true;
}

bool UIRenderer::createBuffers() {
    // Create vertex buffer (start with space for 100 buttons)
    const size_t maxButtons = 100;
    const size_t vertexBufferSize = maxButtons * 4 * sizeof(UIVertex);  // 4 vertices per button
    const size_t indexBufferSize = maxButtons * 6 * sizeof(uint16_t);   // 6 indices per button

    // Vertex buffer
    VkBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexBufferInfo.size = vertexBufferSize;
    vertexBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &vertexBufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        Logger::get().error("Failed to create UI vertex buffer");
        return false;
    }

    VkMemoryRequirements vertexMemReqs;
    vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &vertexMemReqs);

    VkMemoryAllocateInfo vertexAllocInfo{};
    vertexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vertexAllocInfo.allocationSize = vertexMemReqs.size;
    vertexAllocInfo.memoryTypeIndex = findMemoryType(vertexMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &vertexAllocInfo, nullptr, &m_vertexBufferMemory) != VK_SUCCESS) {
        Logger::get().error("Failed to allocate UI vertex buffer memory");
        return false;
    }

    vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexBufferMemory, 0);

    // Index buffer
    VkBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    indexBufferInfo.size = indexBufferSize;
    indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    indexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &indexBufferInfo, nullptr, &m_indexBuffer) != VK_SUCCESS) {
        Logger::get().error("Failed to create UI index buffer");
        return false;
    }

    VkMemoryRequirements indexMemReqs;
    vkGetBufferMemoryRequirements(m_device, m_indexBuffer, &indexMemReqs);

    VkMemoryAllocateInfo indexAllocInfo{};
    indexAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    indexAllocInfo.allocationSize = indexMemReqs.size;
    indexAllocInfo.memoryTypeIndex = findMemoryType(indexMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &indexAllocInfo, nullptr, &m_indexBufferMemory) != VK_SUCCESS) {
        Logger::get().error("Failed to allocate UI index buffer memory");
        return false;
    }

    vkBindBufferMemory(m_device, m_indexBuffer, m_indexBufferMemory, 0);

    // Initialize index buffer (indices pattern is the same for all buttons)
    void* indexData;
    vkMapMemory(m_device, m_indexBufferMemory, 0, indexBufferSize, 0, &indexData);
    uint16_t* indices = static_cast<uint16_t*>(indexData);

    for (size_t i = 0; i < maxButtons; ++i) {
        uint16_t baseVertex = static_cast<uint16_t>(i * 4);
        size_t baseIndex = i * 6;

        // Two triangles per quad
        indices[baseIndex + 0] = baseVertex + 0;
        indices[baseIndex + 1] = baseVertex + 1;
        indices[baseIndex + 2] = baseVertex + 2;
        indices[baseIndex + 3] = baseVertex + 2;
        indices[baseIndex + 4] = baseVertex + 3;
        indices[baseIndex + 5] = baseVertex + 0;
    }

    vkUnmapMemory(m_device, m_indexBufferMemory);

    Logger::get().info("UI buffers created successfully");
    return true;
}

void UIRenderer::updateVertexBuffer() {
    if (!m_vertexBuffer || m_buttons.empty()) {
        return;
    }

    // Build vertex data for all buttons
    std::vector<UIVertex> vertices;
    vertices.reserve(m_buttons.size() * 4);

    for (const auto& [id, button] : m_buttons) {
        glm::vec4 color;
        switch (button.state) {
            case ButtonState::Pressed:
                color = button.pressedColor;
                break;
            case ButtonState::Hovered:
                color = button.hoverColor;
                break;
            case ButtonState::Normal:
            default:
                color = button.normalColor;
                break;
        }

        // If disabled, darken the color
        if (!button.enabled) {
            color *= 0.5f;
        }

        // Create quad vertices (top-left, top-right, bottom-right, bottom-left)
        glm::vec2 topLeft = button.position;
        glm::vec2 bottomRight = button.position + button.size;

        vertices.push_back({topLeft, color});
        vertices.push_back({{bottomRight.x, topLeft.y}, color});
        vertices.push_back({bottomRight, color});
        vertices.push_back({{topLeft.x, bottomRight.y}, color});
    }

    // Upload to GPU
    void* data;
    vkMapMemory(m_device, m_vertexBufferMemory, 0, vertices.size() * sizeof(UIVertex), 0, &data);
    memcpy(data, vertices.data(), vertices.size() * sizeof(UIVertex));
    vkUnmapMemory(m_device, m_vertexBufferMemory);
}

uint32_t UIRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    Logger::get().error("Failed to find suitable memory type");
    return 0;
}

bool UIRenderer::isPointInButton(const glm::vec2& point, const UIButton& button) {
    return point.x >= button.position.x &&
           point.x <= button.position.x + button.size.x &&
           point.y >= button.position.y &&
           point.y <= button.position.y + button.size.y;
}

void UIRenderer::updateButtonStates() {
    m_hoveredButtonId = 0;

    for (auto& [id, button] : m_buttons) {
        if (!button.enabled) {
            button.state = ButtonState::Normal;
            continue;
        }

        // Don't change state if button is currently pressed
        if (id == m_pressedButtonId) {
            continue;
        }

        if (isPointInButton(m_mousePosition, button)) {
            button.state = ButtonState::Hovered;
            m_hoveredButtonId = id;
        } else {
            button.state = ButtonState::Normal;
        }
    }

    updateVertexBuffer();
}

} // namespace boulder
