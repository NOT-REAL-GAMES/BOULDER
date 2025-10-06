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

#ifdef __cplusplus
}
#endif

#endif // BOULDER_CGO_H