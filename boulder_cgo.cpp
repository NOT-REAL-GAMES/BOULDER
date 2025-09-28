
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
    SDL_Window* window = nullptr;
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

int boulder_init(const char* appName, Uint32 version) {
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


    g_engine.initialized = true;

    return 0;
}

void boulder_shutdown() {
    if (!g_engine.initialized) {
        return;
    }

    Logger::get().info("Shutting down engine...");

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

int boulder_render() {
    if (!g_engine.initialized || !g_engine.window) {
        return -1;
    }

    // Rendering would go here (Vulkan/OpenGL)
    SDL_GL_SwapWindow(g_engine.window);

    return 0;
}

int boulder_create_window(int width, int height, const char* title) {
    if (!g_engine.initialized) {
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

    return g_engine.window ? 0 : -1;
    
}

void boulder_set_window_size(int width, int height) {
    if (g_engine.window) {
        SDL_SetWindowSize(g_engine.window, width, height);
    }
}

void boulder_get_window_size(int* width, int* height) {
    if (g_engine.window && width && height) {
        SDL_GetWindowSize(g_engine.window, width, height);
    }
}

int boulder_should_close() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            return 1;
        }
    }
    return 0;
}

void boulder_poll_events() {
    SDL_PumpEvents();
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