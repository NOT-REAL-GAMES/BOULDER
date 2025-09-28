package boulder

// #cgo CFLAGS: -I..
// #cgo LDFLAGS: -L../build -lboulder_shared -Wl,-rpath,../build
// #include <stdlib.h>
// #include "../boulder_cgo.h"
import "C"
import (
	"errors"
	"unsafe"
)

// EntityID represents a unique identifier for an entity in the ECS
type EntityID uint64

// Vector3 represents a 3D vector
type Vector3 struct {
	X, Y, Z float32
}

// Engine represents the Boulder game engine
type Engine struct {
	initialized bool
}

// NewEngine creates a new Engine instance
func NewEngine() *Engine {
	return &Engine{}
}

// Init initializes the Boulder engine
func (e *Engine) Init() error {
	if e.initialized {
		return errors.New("engine already initialized")
	}

	if ret := C.boulder_init(); ret != 0 {
		return errors.New("failed to initialize engine")
	}

	e.initialized = true
	return nil
}

// Shutdown shuts down the engine and releases resources
func (e *Engine) Shutdown() {
	if !e.initialized {
		return
	}

	C.boulder_shutdown()
	e.initialized = false
}

// Update updates the engine with the given delta time
func (e *Engine) Update(deltaTime float32) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_update(C.float(deltaTime)); ret != 0 {
		return errors.New("failed to update engine")
	}

	return nil
}

// Render renders the current frame
func (e *Engine) Render() error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_render(); ret != 0 {
		return errors.New("failed to render frame")
	}

	return nil
}

// CreateWindow creates a new window
func (e *Engine) CreateWindow(width, height int, title string) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	cTitle := C.CString(title)
	defer C.free(unsafe.Pointer(cTitle))

	if ret := C.boulder_create_window(C.int(width), C.int(height), cTitle); ret != 0 {
		return errors.New("failed to create window")
	}

	return nil
}

// SetWindowSize sets the window size
func (e *Engine) SetWindowSize(width, height int) {
	if !e.initialized {
		return
	}

	C.boulder_set_window_size(C.int(width), C.int(height))
}

// GetWindowSize gets the current window size
func (e *Engine) GetWindowSize() (int, int) {
	if !e.initialized {
		return 0, 0
	}

	var width, height C.int
	C.boulder_get_window_size(&width, &height)
	return int(width), int(height)
}

// ShouldClose returns true if the window should close
func (e *Engine) ShouldClose() bool {
	if !e.initialized {
		return true
	}

	return C.boulder_should_close() != 0
}

// PollEvents polls for window and input events
func (e *Engine) PollEvents() {
	if !e.initialized {
		return
	}

	C.boulder_poll_events()
}

// CreateEntity creates a new entity and returns its ID
func (e *Engine) CreateEntity() (EntityID, error) {
	if !e.initialized {
		return 0, errors.New("engine not initialized")
	}

	id := C.boulder_create_entity()
	if id == 0 {
		return 0, errors.New("failed to create entity")
	}

	return EntityID(id), nil
}

// DestroyEntity destroys an entity
func (e *Engine) DestroyEntity(entity EntityID) {
	if !e.initialized {
		return
	}

	C.boulder_destroy_entity(C.EntityID(entity))
}

// EntityExists checks if an entity exists
func (e *Engine) EntityExists(entity EntityID) bool {
	if !e.initialized {
		return false
	}

	return C.boulder_entity_exists(C.EntityID(entity)) != 0
}

// AddTransform adds a transform component to an entity
func (e *Engine) AddTransform(entity EntityID, position Vector3) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_add_transform(C.EntityID(entity),
		C.float(position.X), C.float(position.Y), C.float(position.Z)); ret != 0 {
		return errors.New("failed to add transform")
	}

	return nil
}

// GetTransform gets the transform position of an entity
func (e *Engine) GetTransform(entity EntityID) (Vector3, error) {
	if !e.initialized {
		return Vector3{}, errors.New("engine not initialized")
	}

	var x, y, z C.float
	if ret := C.boulder_get_transform(C.EntityID(entity), &x, &y, &z); ret != 0 {
		return Vector3{}, errors.New("failed to get transform")
	}

	return Vector3{X: float32(x), Y: float32(y), Z: float32(z)}, nil
}

// SetTransform sets the transform position of an entity
func (e *Engine) SetTransform(entity EntityID, position Vector3) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_set_transform(C.EntityID(entity),
		C.float(position.X), C.float(position.Y), C.float(position.Z)); ret != 0 {
		return errors.New("failed to set transform")
	}

	return nil
}

// AddPhysicsBody adds a physics body component to an entity
func (e *Engine) AddPhysicsBody(entity EntityID, mass float32) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_add_physics_body(C.EntityID(entity), C.float(mass)); ret != 0 {
		return errors.New("failed to add physics body")
	}

	return nil
}

// SetVelocity sets the velocity of an entity's physics body
func (e *Engine) SetVelocity(entity EntityID, velocity Vector3) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_set_velocity(C.EntityID(entity),
		C.float(velocity.X), C.float(velocity.Y), C.float(velocity.Z)); ret != 0 {
		return errors.New("failed to set velocity")
	}

	return nil
}

// GetVelocity gets the velocity of an entity's physics body
func (e *Engine) GetVelocity(entity EntityID) (Vector3, error) {
	if !e.initialized {
		return Vector3{}, errors.New("engine not initialized")
	}

	var vx, vy, vz C.float
	if ret := C.boulder_get_velocity(C.EntityID(entity), &vx, &vy, &vz); ret != 0 {
		return Vector3{}, errors.New("failed to get velocity")
	}

	return Vector3{X: float32(vx), Y: float32(vy), Z: float32(vz)}, nil
}

// ApplyForce applies a force to an entity's physics body
func (e *Engine) ApplyForce(entity EntityID, force Vector3) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_apply_force(C.EntityID(entity),
		C.float(force.X), C.float(force.Y), C.float(force.Z)); ret != 0 {
		return errors.New("failed to apply force")
	}

	return nil
}

// LoadModel loads a 3D model for an entity
func (e *Engine) LoadModel(entity EntityID, path string) error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	if ret := C.boulder_load_model(C.EntityID(entity), cPath); ret != 0 {
		return errors.New("failed to load model")
	}

	return nil
}

// IsKeyPressed checks if a key is pressed
func (e *Engine) IsKeyPressed(keyCode int) bool {
	if !e.initialized {
		return false
	}

	return C.boulder_is_key_pressed(C.int(keyCode)) != 0
}

// IsMouseButtonPressed checks if a mouse button is pressed
func (e *Engine) IsMouseButtonPressed(button int) bool {
	if !e.initialized {
		return false
	}

	return C.boulder_is_mouse_button_pressed(C.int(button)) != 0
}

// GetMousePosition gets the current mouse position
func (e *Engine) GetMousePosition() (float32, float32) {
	if !e.initialized {
		return 0, 0
	}

	var x, y C.float
	C.boulder_get_mouse_position(&x, &y)
	return float32(x), float32(y)
}

// LogInfo logs an info message
func LogInfo(message string) {
	cMsg := C.CString(message)
	defer C.free(unsafe.Pointer(cMsg))
	C.boulder_log_info(cMsg)
}

// LogError logs an error message
func LogError(message string) {
	cMsg := C.CString(message)
	defer C.free(unsafe.Pointer(cMsg))
	C.boulder_log_error(cMsg)
}