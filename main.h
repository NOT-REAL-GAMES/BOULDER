#pragma once

#include <errno.h>

//#define VK_USE_64_BIT_PTR_DEFINES 0

#include <algorithm>
#include <future>
#include <source_location>
#include <format>
#include <fstream>

#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>
#include <map>

#include <optional>
#include <chrono>

#include <random>

#include "volk.h"

#include <vulkan/vulkan.hpp>

#include <cstring>
#include <stdexcept>

#include <glm/glm.hpp>

#include <string_view>
#include <unordered_map>
#include <cctype>
#include <charconv>
#include <functional>
#include <array>

#include <cstdio>
#include <memory>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#include "Engine.h"

template<typename T>
concept StringLike = requires(T t) {
    { std::string_view(t) } -> std::convertible_to<std::string_view>;
};

class Logger {
public:
    enum class Level {
        Debug,
        Info,
        Warning,
        Error,
        Critical
    };

    struct Config {
        bool enableConsole = true;
        bool enableFileOutput = false;
        std::string logFilePath = "tremor.log";
        Level minLevel = Level::Info;
        bool useColors = true;
        bool showTimestamps = true;
        bool showSourceLocation = false;
    };

    // Singleton access
    static Logger& get() {
        static Logger instance;
        return instance;
    }

    // Create a new logger instance (alternative to singleton)
    static std::shared_ptr<Logger> create(const Config& config) {
        return std::make_shared<Logger>(config);
    }
    
    // Overload for default config
    static std::shared_ptr<Logger> create() {
        return std::make_shared<Logger>(Config{});
    }

    // Constructor with configuration
    explicit Logger(const Config& config)
        : m_config(config) {
        if (m_config.enableFileOutput) {
            m_logFile.open(m_config.logFilePath, std::ios::out | std::ios::app);
            if (!m_logFile) {
                std::cerr << "Failed to open log file: " << m_config.logFilePath << std::endl;
            }
        }
    }
    
    // Default constructor for singleton
    Logger() : Logger(Config{}) {}

    // Destructor
    ~Logger() {
        if (m_logFile.is_open()) {
            m_logFile.close();
        }
    }

    // Format and log a message with the given level
    template<typename... Args>
    void log(Level level, std::format_string<Args...> fmt, Args&&... args) {
        if (level < m_config.minLevel) {
            return;
        }

        try {
            std::string message = std::format(fmt, std::forward<Args>(args)...);
            logMessage(level, message);
        }
        catch (const std::format_error& e) {
            logMessage(Level::Error, std::format("Format error: {}", e.what()));
        }
    }

    // Convenience methods for different log levels
    template<typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) {
        log(Level::Debug, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) {
        log(Level::Info, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void warning(std::format_string<Args...> fmt, Args&&... args) {
        log(Level::Warning, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) {
        log(Level::Error, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void critical(std::format_string<Args...> fmt, Args&&... args) {
        log(Level::Critical, fmt, std::forward<Args>(args)...);
    }

    // Set the minimum log level
    void setLevel(Level level) {
        m_config.minLevel = level;
    }

    // Get current config
    const Config& getConfig() const {
        return m_config;
    }

private:
    Config m_config;
    std::ofstream m_logFile;
    std::mutex m_mutex;

    // Convert level to string
    std::string_view levelToString(Level level) const {
        switch (level) {
        case Level::Debug:    return "DEBUG";
        case Level::Info:     return "INFO";
        case Level::Warning:  return "WARNING";
        case Level::Error:    return "ERROR";
        case Level::Critical: return "CRITICAL";
        default:              return "UNKNOWN";
        }
    }

    // Get ANSI color code for level
    std::string_view levelToColor(Level level) const {
        if (!m_config.useColors) {
            return "";
        }

        switch (level) {
        case Level::Debug:    return "\033[37m"; // White
        case Level::Info:     return "\033[32m"; // Green
        case Level::Warning:  return "\033[33m"; // Yellow
        case Level::Error:    return "\033[31m"; // Red
        case Level::Critical: return "\033[35m"; // Magenta
        default:              return "\033[0m";  // Reset
        }
    }

    // Reset ANSI color
    std::string_view resetColor() const {
        return m_config.useColors ? "\033[0m" : "";
    }

    // Format timestamp
    std::string formatTimestamp() const {
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &now_time_t);
#else
        localtime_r(&now_time_t, &tm_buf);
#endif

        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, now_ms.count());
    }

    // Log a message with the given level
    void logMessage(Level level, std::string_view message,
        const std::source_location& location = std::source_location::current()) {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string fullMessage;

        // Add timestamp if enabled
        if (m_config.showTimestamps) {
            fullMessage += std::format("[{}] ", formatTimestamp());
        }

        // Add log level
        fullMessage += std::format("{}{}{} ",
            levelToColor(level), levelToString(level), resetColor());

        // Add source location if enabled
        if (m_config.showSourceLocation) {
            fullMessage += std::format("{}:{}:{}: ",
                std::filesystem::path(location.file_name()).filename().string(),
                location.line(), location.column());
        }

        // Add the message
        fullMessage += message;

        // Log to console if enabled
        if (m_config.enableConsole) {
            std::cout << fullMessage << std::endl;
        }

        // Log to file if enabled
        if (m_config.enableFileOutput && m_logFile.is_open()) {
            m_logFile << fullMessage << std::endl;
            m_logFile.flush();
        }
    }
};

    class MemoryManager {
    public:
        static MemoryManager& instance() {
            static MemoryManager s_instance;
            return s_instance;
        }

        // Memory allocation stats
        struct Stats {
            std::atomic<size_t> totalAllocated{ 0 };
            std::atomic<size_t> totalFreed{ 0 };
            std::atomic<size_t> peakUsage{ 0 };
            std::atomic<size_t> currentUsage{ 0 };
            std::atomic<size_t> allocCount{ 0 };
            std::atomic<size_t> freeCount{ 0 };

            // No need to synchronize this as it's only accessed with the mutex held
            std::unordered_map<size_t, size_t> allocationSizeHistogram;
        };

        // Explicit memory allocations (use sparingly)
        void* allocate(size_t size, const char* tag = nullptr) {
            if (size == 0) return nullptr;

            // Allocate memory with header for tracking
            constexpr size_t headerSize = sizeof(AllocationHeader);
            size_t totalSize = size + headerSize;

            void* rawMemory = std::malloc(totalSize);
            if (!rawMemory) {
                std::cerr << "MemoryManager: Failed to allocate " << size << " bytes" << std::endl;
                return nullptr;
            }

            // Setup header
            AllocationHeader* header = static_cast<AllocationHeader*>(rawMemory);
            header->size = size;
            header->magic = ALLOCATION_MAGIC;

            if (tag) {
                std::string_view src(tag);
                auto copyLen = std::min(src.length(), sizeof(header->tag) - 1);
                std::copy_n(src.begin(), copyLen, header->tag);
                header->tag[copyLen] = '\0';
            }
            else {
                header->tag[0] = '\0';
            }

            // Update statistics
            stats.totalAllocated += size;
            stats.currentUsage += size;
            stats.allocCount++;

            size_t current = stats.currentUsage.load();
            size_t peak = stats.peakUsage.load();
            while (current > peak) {
                if (stats.peakUsage.compare_exchange_weak(peak, current)) {
                    break;
                }
                current = stats.currentUsage.load();
                peak = stats.peakUsage.load();
            }

            // Track allocation size distribution for debugging
            {
                std::lock_guard<std::mutex> lock(allocationMutex);
                stats.allocationSizeHistogram[size]++;

                // Track detailed allocation info if debugging is enabled
                if (trackAllocations) {
                    AllocationInfo info;
                    info.size = size;
                    info.tag = tag ? tag : "";

                    // Capture stack trace if available
#ifdef TREMOR_CAPTURE_STACK_TRACES
                    info.stackTraceSize = captureStackTrace(info.stackTrace, 20);
#else
                    info.stackTraceSize = 0;
#endif

                    allocations[static_cast<uint8_t*>(rawMemory) + headerSize] = info;
                }
            }

            // Return pointer past the header
            return static_cast<uint8_t*>(rawMemory) + headerSize;
        }

        void* reallocate(void* ptr, size_t newSize, const char* tag = nullptr) {
            if (!ptr) {
                return allocate(newSize, tag);
            }

            if (newSize == 0) {
                free(ptr);
                return nullptr;
            }

            // Get the header
            AllocationHeader* header = getAllocationHeader(ptr);
            if (!header || header->magic != ALLOCATION_MAGIC) {
                std::cerr << "MemoryManager: Invalid pointer passed to reallocate" << std::endl;
                return nullptr;
            }

            // Update statistics for the old allocation
            size_t oldSize = header->size;
            stats.totalFreed += oldSize;
            stats.currentUsage -= oldSize;
            stats.freeCount++;

            {
                std::lock_guard<std::mutex> lock(allocationMutex);
                // Decrement the old size in the histogram
                auto it = stats.allocationSizeHistogram.find(oldSize);
                if (it != stats.allocationSizeHistogram.end()) {
                    if (it->second > 1) {
                        it->second--;
                    }
                    else {
                        stats.allocationSizeHistogram.erase(it);
                    }
                }

                // Remove from detailed tracking
                if (trackAllocations) {
                    allocations.erase(ptr);
                }
            }

            // Perform reallocation
            constexpr size_t headerSize = sizeof(AllocationHeader);
            size_t totalSize = newSize + headerSize;

            void* rawMemory = std::realloc(header, totalSize);
            if (!rawMemory) {
                std::cerr << "MemoryManager: Failed to reallocate " << newSize << " bytes" << std::endl;
                return nullptr;
            }

            // Update header
            header = static_cast<AllocationHeader*>(rawMemory);
            header->size = newSize;

            if (tag) {
                std::string_view src(tag);
                auto copyLen = std::min(src.length(), sizeof(header->tag) - 1);
                std::copy_n(src.begin(), copyLen, header->tag);
                header->tag[sizeof(header->tag) - 1] = '\0';
            }

            // Update statistics for the new allocation
            stats.totalAllocated += newSize;
            stats.currentUsage += newSize;
            stats.allocCount++;

            size_t current = stats.currentUsage.load();
            size_t peak = stats.peakUsage.load();
            while (current > peak) {
                if (stats.peakUsage.compare_exchange_weak(peak, current)) {
                    break;
                }
                current = stats.currentUsage.load();
                peak = stats.peakUsage.load();
            }

            {
                std::lock_guard<std::mutex> lock(allocationMutex);
                // Increment the new size in the histogram
                stats.allocationSizeHistogram[newSize]++;

                // Track the new allocation
                if (trackAllocations) {
                    AllocationInfo info;
                    info.size = newSize;
                    info.tag = tag ? tag : "";
                    info.stackTraceSize = 0;
                    allocations[static_cast<uint8_t*>(rawMemory) + headerSize] = info;
                }
            }

            // Return pointer past the header
            return static_cast<uint8_t*>(rawMemory) + headerSize;
        }

        void free(void* ptr) {
            if (!ptr) return;

            // Get the header
            AllocationHeader* header = getAllocationHeader(ptr);
            if (!header || header->magic != ALLOCATION_MAGIC) {
                std::cerr << "MemoryManager: Invalid pointer passed to free" << std::endl;
                return;
            }

            // Update statistics
            size_t size = header->size;
            stats.totalFreed += size;
            stats.currentUsage -= size;
            stats.freeCount++;

            {
                std::lock_guard<std::mutex> lock(allocationMutex);
                // Update the size histogram
                auto it = stats.allocationSizeHistogram.find(size);
                if (it != stats.allocationSizeHistogram.end()) {
                    if (it->second > 1) {
                        it->second--;
                    }
                    else {
                        stats.allocationSizeHistogram.erase(it);
                    }
                }

                // Remove from detailed tracking
                if (trackAllocations) {
                    allocations.erase(ptr);
                }
            }

            // Invalidate the header to catch double-frees
            header->magic = 0;

            // Free the actual memory
            std::free(header);
        }

        // Create objects with memory tracking
        template<typename T, typename... Args>
        T* createObject(Args&&... args) {
            void* memory = allocate(sizeof(T), typeid(T).name());
            if (!memory) return nullptr;

            try {
                return new(memory) T(std::forward<Args>(args)...);
            }
            catch (...) {
                free(memory);
                throw;
            }
        }

        // Destroy tracked objects
        template<typename T>
        void destroyObject(T* obj) {
            if (obj) {
                obj->~T();
                free(obj);
            }
        }

        // Get memory stats
        const Stats& getStats() const { return stats; }

        // Reset stats (for level changes, etc)
        void resetStats() {
            stats.totalAllocated = 0;
            stats.totalFreed = 0;
            stats.peakUsage = 0;
            stats.currentUsage = 0;
            stats.allocCount = 0;
            stats.freeCount = 0;

            std::lock_guard<std::mutex> lock(allocationMutex);
            stats.allocationSizeHistogram.clear();
        }

        // Enable/disable detailed allocation tracking
        void setTrackAllocations(bool enable) {
            std::lock_guard<std::mutex> lock(allocationMutex);
            trackAllocations = enable;
            if (!enable) {
                allocations.clear();
            }
        }

        // Dump memory leaks - call before shutdown
        void dumpLeaks(std::ostream& out = std::cerr) {
            std::lock_guard<std::mutex> lock(allocationMutex);

            if (!trackAllocations || allocations.empty()) {
                out << "No memory leaks detected or tracking disabled." << std::endl;
                return;
            }

            out << "Memory leaks detected: " << allocations.size() << " allocations not freed" << std::endl;
            out << "Current memory usage: " << stats.currentUsage << " bytes" << std::endl;

            size_t totalLeaked = 0;
            for (const auto& [ptr, info] : allocations) {
                totalLeaked += info.size;
                out << "  Leak: " << info.size << " bytes";
                if (!info.tag.empty()) {
                    out << " [" << info.tag << "]";
                }
                out << std::endl;

#ifdef TREMOR_CAPTURE_STACK_TRACES
                if (info.stackTraceSize > 0) {
                    out << "    Allocation stack trace:" << std::endl;
                    printStackTrace(out, info.stackTrace, info.stackTraceSize);
                }
#endif
            }

            out << "Total leaked memory: " << totalLeaked << " bytes" << std::endl;
        }

    private:
        static constexpr uint32_t ALLOCATION_MAGIC = 0xDEADBEEF;

        // Header stored before each allocation
        struct AllocationHeader {
            size_t size;          // Size of the allocation (excluding header)
            uint32_t magic;       // Magic number to detect invalid frees
            char tag[32];         // Optional tag for debugging
        };

        // Get the header from a user pointer
        AllocationHeader* getAllocationHeader(void* ptr) {
            if (!ptr) return nullptr;
            return reinterpret_cast<AllocationHeader*>(
                static_cast<uint8_t*>(ptr) - sizeof(AllocationHeader)
                );
        }

        Stats stats;
        std::mutex allocationMutex;
        bool trackAllocations = true;  // Enable by default

        // Track allocations for leak detection
        struct AllocationInfo {
            size_t size;
            std::string tag;
            void* stackTrace[20];
            int stackTraceSize;
        };
        std::unordered_map<void*, AllocationInfo> allocations;
    };



    template<typename T>
    class ScopedAlloc {
    public:
        explicit ScopedAlloc(size_t count = 1) {
            data = static_cast<T*>(MemoryManager::instance().allocate(sizeof(T) * count, "ScopedAlloc"));
            elementCount = count;

            // Default initialize the memory if it's not trivially constructible
            if constexpr (!std::is_trivially_constructible_v<T>) {
                for (size_t i = 0; i < count; ++i) {
                    new(&data[i]) T();
                }
            }
        }

        ~ScopedAlloc() {
            if (data) {
                // Call destructors if needed
                if constexpr (!std::is_trivially_destructible_v<T>) {
                    for (size_t i = 0; i < elementCount; ++i) {
                        data[i].~T();
                    }
                }

                MemoryManager::instance().free(data);
            }
        }

        // No copying
        ScopedAlloc(const ScopedAlloc&) = delete;
        ScopedAlloc& operator=(const ScopedAlloc&) = delete;

        // Allow moving
        ScopedAlloc(ScopedAlloc&& other) noexcept : data(other.data), elementCount(other.elementCount) {
            other.data = nullptr;
            other.elementCount = 0;
        }

        ScopedAlloc& operator=(ScopedAlloc&& other) noexcept {
            if (this != &other) {
                // Clean up existing data
                if (data) {
                    // Call destructors if needed
                    if constexpr (!std::is_trivially_destructible_v<T>) {
                        for (size_t i = 0; i < elementCount; ++i) {
                            data[i].~T();
                        }
                    }

                    MemoryManager::instance().free(data);
                }

                // Take ownership of other's data
                data = other.data;
                elementCount = other.elementCount;
                other.data = nullptr;
                other.elementCount = 0;
            }
            return *this;
        }

        // Access operators
        T* get() const { return data; }
        T& operator[](size_t index) {
            if (index >= elementCount) {
                throw std::out_of_range("ScopedAlloc index out of range");
            }
            return data[index];
        }
        const T& operator[](size_t index) const {
            if (index >= elementCount) {
                throw std::out_of_range("ScopedAlloc index out of range");
            }
            return data[index];
        }

        // Size info
        size_t size() const { return elementCount; }

        // Iterator support
        T* begin() { return data; }
        T* end() { return data + elementCount; }
        const T* begin() const { return data; }
        const T* end() const { return data + elementCount; }

        // Pointer arithmetic operators
        T* operator+(size_t offset) const {
            if (offset >= elementCount) {
                throw std::out_of_range("ScopedAlloc offset out of range");
            }
            return data + offset;
        }

    private:
        T* data = nullptr;
        size_t elementCount = 0;
    };

    class VulkanDevice {
    public:
        // Structure for tracking physical device capabilities
        struct VulkanDeviceCapabilities {
            bool dedicatedAllocation = false;
            bool fullScreenExclusive = false;
            bool rayQuery = false;
            bool meshShaders = false;
            bool bresenhamLineRasterization = false;
            bool nonSolidFill = false;
            bool multiDrawIndirect = false;
            bool sparseBinding = false;  // For megatextures
            bool bufferDeviceAddress = false;  // For ray tracing
            bool dynamicRendering = false;  // Modern rendering without render passes
        };

        // Structure for tracking preferences in device selection
        struct DevicePreferences {
            bool preferDiscreteGPU = true;
            bool requireMeshShaders = false;
            bool requireRayQuery = true;
            bool requireSparseBinding = true;  // For megatextures
            int preferredDeviceIndex = -1;     // -1 means auto-select
        };

        // Constructor
        VulkanDevice(VkInstance instance, VkSurfaceKHR surface,
            const DevicePreferences& preferences);

        // Destructor - automatically cleans up Vulkan resources
        ~VulkanDevice() {
            if (m_device != VK_NULL_HANDLE) {
                vkDestroyDevice(m_device, nullptr);
                m_device = VK_NULL_HANDLE;
            }
        }

        // Delete copy operations to prevent double-free
        VulkanDevice(const VulkanDevice&) = delete;
        VulkanDevice& operator=(const VulkanDevice&) = delete;

        // Move operations
        VulkanDevice(VulkanDevice&& other) noexcept;
        VulkanDevice& operator=(VulkanDevice&& other) noexcept;

        // Access the Vulkan handles
        VkDevice device() const { return m_device; }
        VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
        VkQueue graphicsQueue() const { return m_graphicsQueue; }
        uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }

        // Get device capabilities and properties
        const VulkanDeviceCapabilities& capabilities() const { return m_capabilities; }
        const VkPhysicalDeviceProperties& properties() const { return m_deviceProperties; }
        const VkPhysicalDeviceMemoryProperties& memoryProperties() const { return m_memoryProperties; }

        // Format information
        VkFormat colorFormat() const { return m_colorFormat; }
        VkFormat depthFormat() const { return m_depthFormat; }

        // Utility functions
        std::optional<uint32_t> findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

        // Setup functions for specialized features
        void setupBresenhamLineRasterization(VkPipelineRasterizationStateCreateInfo& rasterInfo) const;
        void setupFloatingOriginUniforms(VkDescriptorSetLayoutCreateInfo& layoutInfo) const;

    private:
        // Vulkan handles
        VkInstance m_instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        uint32_t m_graphicsQueueFamily = 0;

        // Device properties
        VkPhysicalDeviceProperties m_deviceProperties{};
        VkPhysicalDeviceFeatures2 m_deviceFeatures2{};
        VkPhysicalDeviceMemoryProperties m_memoryProperties{};

        // Formats
        VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
        VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;

        // Capabilities
        VulkanDeviceCapabilities m_capabilities{};

        // The surface we're rendering to
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;

        // Initialization helpers
        void selectPhysicalDevice(const DevicePreferences& preferences);
        void createLogicalDevice(const DevicePreferences& preferences);
        void determineFormats();
        void logDeviceInfo() const;

        // Template for structure initialization with sType
        template<typename T>
        static T createStructure() {
            T result{};
            result.sType = getStructureType<T>();
            return result;
        }

        // Helper to get correct sType for Vulkan structures
        template<typename T>
        static VkStructureType getStructureType();
    };

