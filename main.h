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

