cmake_minimum_required(VERSION 3.25)
project(Boulder VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL2 QUIET)
find_package(Vulkan QUIET)
find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
    glm STATIC
    GIT_REPOSITORY "https://github.com/g-truc/glm.git"
)

FetchContent_MakeAvailable(glm)

FetchContent_Declare(
    SPIRV-Cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG "main"
)

FetchContent_MakeAvailable(SPIRV-Cross)

# Fetch SPIRV-Headers (required by SPIRV-Tools)
FetchContent_Declare(
    SPIRV-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG main
)

FetchContent_MakeAvailable(SPIRV-Headers)

# Fetch SPIRV-Tools (required by shaderc)
FetchContent_Declare(
    SPIRV-Tools
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
    GIT_TAG main
)

set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SPIRV-Tools)

# Fetch glslang (required by shaderc)
FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG main
)

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glslang)

# Now fetch shaderc
FetchContent_Declare(
    shaderc
    GIT_REPOSITORY https://github.com/google/shaderc.git
    GIT_TAG main
)

set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(shaderc)


FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG "main"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_SHALLOW TRUE
)

# Configure volk with position independent code before making it available
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
FetchContent_MakeAvailable(volk)

# Ensure volk library has PIC enabled
set_target_properties(volk PROPERTIES POSITION_INDEPENDENT_CODE ON)


FetchContent_Declare(
    flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG v4.0.3
)
FetchContent_MakeAvailable(flecs)

# Add Jolt Physics using FetchContent
FetchContent_Declare(
    Jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.3.0
    SOURCE_SUBDIR Build
)

# Configure Jolt options before making it available
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM ON CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS ON CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(Jolt)

# Ensure Jolt uses the correct C++ standard
if(TARGET Jolt)
    set_target_properties(Jolt PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endif()

FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-29-0
)
FetchContent_MakeAvailable(asio)

# Note: gofrontend is a GCC compiler frontend for Go, not a library
# If you need Go language support in your engine, consider:
# 1. Using CGO to create Go bindings for your C++ engine
# 2. Using a Go embedding library if you need scripting
# 3. Creating a separate Go module that interfaces with your engine
#
# For now, commenting out the incorrect gofrontend integration:
# FetchContent_Declare(
#     golang
#     GIT_REPOSITORY https://github.com/golang/gofrontend.git
# )
# FetchContent_MakeAvailable(golang)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp
)
FetchContent_MakeAvailable(assimp)

# Add GameNetworkingSockets for networking
FetchContent_Declare(
    GameNetworkingSockets
    GIT_REPOSITORY https://github.com/ValveSoftware/GameNetworkingSockets.git
    GIT_TAG v1.4.1
)

set(GAMENETWORKINGSOCKETS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GAMENETWORKINGSOCKETS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(GameNetworkingSockets)

# Add Steamworks SDK
set(STEAMWORKS_SDK_ROOT "${CMAKE_SOURCE_DIR}/third-party/steamworks")
set(STEAMWORKS_INCLUDE_DIR "${STEAMWORKS_SDK_ROOT}/public")

# Detect architecture for library path
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(STEAMWORKS_LIB_DIR "${STEAMWORKS_SDK_ROOT}/redistributable_bin/linux64")
else()
    set(STEAMWORKS_LIB_DIR "${STEAMWORKS_SDK_ROOT}/redistributable_bin/linux32")
endif()

# Create imported library for Steam API
add_library(steam_api SHARED IMPORTED)
set_target_properties(steam_api PROPERTIES
    IMPORTED_LOCATION "${STEAMWORKS_LIB_DIR}/libsteam_api.so"
    INTERFACE_INCLUDE_DIRECTORIES "${STEAMWORKS_INCLUDE_DIR}"
)

# Set position independent code for all targets when building shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build shared library for CGO bindings
add_library(boulder_shared SHARED
    boulder_cgo.cpp
    ui_renderer.cpp
)

target_include_directories(boulder_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${asio_SOURCE_DIR}/asio/include
)

# Find zlib as a shared library
find_package(ZLIB REQUIRED)

target_link_libraries(boulder_shared PUBLIC
    spirv-cross-core
    spirv-cross-glsl
    shaderc
    SDL3
    Threads::Threads
    assimp
    flecs::flecs_static
    Jolt
    volk
    glm::glm
    ZLIB::ZLIB
    GameNetworkingSockets
    steam_api
)

target_compile_definitions(boulder_shared PUBLIC
    USING_VULKAN
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    HAS_ASSIMP
    JPH_DOUBLE_PRECISION
)

# Set shared library properties for CGO
set_target_properties(boulder_shared PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
)

# Build main executable
add_executable(Boulder
    main.cpp
)

target_include_directories(Boulder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Boulder PRIVATE
    spirv-cross-core
    spirv-cross-glsl
    shaderc
    SDL3
    Threads::Threads
    assimp
    flecs::flecs_static
    Jolt
    volk
    glm::glm
    GameNetworkingSockets
    steam_api
)

target_compile_definitions(Boulder PRIVATE
    USING_VULKAN
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    HAS_ASSIMP
    JPH_DOUBLE_PRECISION
)

# Set RPATH for runtime library location
set_target_properties(boulder_shared PROPERTIES
    BUILD_RPATH "${STEAMWORKS_LIB_DIR}"
    INSTALL_RPATH "${STEAMWORKS_LIB_DIR}"
)

set_target_properties(Boulder PROPERTIES
    BUILD_RPATH "${STEAMWORKS_LIB_DIR}"
    INSTALL_RPATH "${STEAMWORKS_LIB_DIR}"
)