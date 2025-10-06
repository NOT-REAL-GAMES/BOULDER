#ifndef BOULDER_CGO_H
#define BOULDER_CGO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Engine initialization and lifecycle
int boulder_init(const char* appName, uint version);
void boulder_shutdown();
int boulder_update(float deltaTime);
int boulder_render();

// Window management
int boulder_create_window(int width, int height, const char* title);
void boulder_set_window_size(int width, int height);
void boulder_get_window_size(int* width, int* height);
int boulder_should_close();
void boulder_poll_events();

// Entity management (ECS)
typedef unsigned long long EntityID;
EntityID boulder_create_entity();
void boulder_destroy_entity(EntityID entity);
int boulder_entity_exists(EntityID entity);

// Component operations
int boulder_add_transform(EntityID entity, float x, float y, float z);
int boulder_get_transform(EntityID entity, float* x, float* y, float* z);
int boulder_set_transform(EntityID entity, float x, float y, float z);

// Physics
int boulder_add_physics_body(EntityID entity, float mass);
int boulder_set_velocity(EntityID entity, float vx, float vy, float vz);
int boulder_get_velocity(EntityID entity, float* vx, float* vy, float* vz);
int boulder_apply_force(EntityID entity, float fx, float fy, float fz);

// Model loading
int boulder_load_model(EntityID entity, const char* path);

// Input handling
int boulder_is_key_pressed(int keyCode);
int boulder_is_mouse_button_pressed(int button);
void boulder_get_mouse_position(float* x, float* y);

// Logging
void boulder_log_info(const char* message);
void boulder_log_error(const char* message);

// Shader management
typedef unsigned long long ShaderModuleID;
ShaderModuleID boulder_compile_shader(const char* source, int shaderKind, const char* name);
void boulder_destroy_shader_module(ShaderModuleID shaderId);
ShaderModuleID boulder_reload_shader(ShaderModuleID shaderId, const char* source, int shaderKind, const char* name);

// Pipeline management
typedef unsigned long long PipelineID;
PipelineID boulder_create_graphics_pipeline(ShaderModuleID meshShader, ShaderModuleID fragShader);
void boulder_bind_pipeline(PipelineID pipelineId);
void boulder_destroy_pipeline(PipelineID pipelineId);

// Rendering control
int boulder_begin_frame(uint32_t* imageIndex);
int boulder_end_frame(uint32_t imageIndex);
void boulder_set_clear_color(float r, float g, float b, float a);
void boulder_set_viewport(float x, float y, float width, float height, float minDepth, float maxDepth);
void boulder_set_scissor(int x, int y, int width, int height);

// Draw commands
void boulder_draw_mesh(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);
void boulder_set_push_constants(const void* data, uint32_t size, uint32_t offset);

// Swapchain management
void boulder_get_swapchain_extent(int* width, int* height);
int boulder_recreate_swapchain();

// Network management
typedef void* NetworkSession;
typedef uint64_t ConnectionHandle;
typedef uint64_t SteamID;

// Session lifecycle
NetworkSession boulder_create_network_session();
void boulder_destroy_network_session(NetworkSession session);
void boulder_network_update(NetworkSession session);

// Relay configuration (call before creating sessions)
void boulder_network_init_with_steam_app(uint32_t appId); // Initialize with Steam AppID (e.g., 480 for Spacewar)
void boulder_network_set_relay_server(const char* address, uint16_t port);
void boulder_network_enable_fake_ip(); // For easier testing without real Steam

// Server operations
int boulder_start_server(NetworkSession session, uint16_t port);
int boulder_start_server_p2p(NetworkSession session, int virtualPort); // P2P mode with virtual port
void boulder_stop_server(NetworkSession session);

// Client operations
ConnectionHandle boulder_connect(NetworkSession session, const char* address, uint16_t port);
ConnectionHandle boulder_connect_p2p(NetworkSession session, SteamID steamID, int virtualPort); // Connect by Steam ID
void boulder_disconnect(NetworkSession session, ConnectionHandle conn);
int boulder_connection_state(NetworkSession session, ConnectionHandle conn);

// Identity management
void boulder_set_local_identity(NetworkSession session, const char* name);
SteamID boulder_get_local_steam_id(NetworkSession session);

// Messaging
int boulder_send_message(NetworkSession session, ConnectionHandle conn, const void* data, uint32_t size, int reliable);

// Event polling
typedef struct {
    int type; // 0=none, 1=connected, 2=disconnected, 3=message
    ConnectionHandle connection;
    uint8_t* data;
    uint32_t dataSize;
} NetworkEvent;

int boulder_poll_network_event(NetworkSession session, NetworkEvent* event);
void boulder_free_network_event_data(void* data);

#ifdef __cplusplus
}
#endif

#endif // BOULDER_CGO_H