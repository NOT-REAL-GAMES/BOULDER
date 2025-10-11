package boulder

// #cgo CFLAGS: -I..
// #cgo LDFLAGS: -L../build -lboulder_shared -Wl,-rpath,${SRCDIR}/../build
// #include <stdlib.h>
// #include "../boulder_cgo.h"
import "C"
import (
	"errors"
	"unsafe"
)

// EntityID represents a unique identifier for an entity in the ECS
type EntityID uint64

// World manages the ECS (Entity Component System)
type World struct {
	engine *Engine
}

// NewWorld creates a new World manager
func NewWorld(engine *Engine) *World {
	return &World{
		engine: engine,
	}
}

// CreateEntity creates a new entity and returns its ID
func (w *World) CreateEntity() (EntityID, error) {
	if !w.engine.initialized {
		return 0, errors.New("engine not initialized")
	}

	id := C.boulder_create_entity()
	if id == 0 {
		return 0, errors.New("failed to create entity")
	}

	return EntityID(id), nil
}

// DestroyEntity destroys an entity
func (w *World) DestroyEntity(entity EntityID) {
	if !w.engine.initialized {
		return
	}

	C.boulder_destroy_entity(C.EntityID(entity))
}

// EntityExists checks if an entity exists
func (w *World) EntityExists(entity EntityID) bool {
	if !w.engine.initialized {
		return false
	}

	return C.boulder_entity_exists(C.EntityID(entity)) != 0
}

// Entity represents a game entity with components
type Entity struct {
	ID    EntityID
	world *World
}

// NewEntity wraps an entity ID with helper methods
func (w *World) NewEntity() (*Entity, error) {
	id, err := w.CreateEntity()
	if err != nil {
		return nil, err
	}

	return &Entity{
		ID:    id,
		world: w,
	}, nil
}

// Destroy destroys this entity
func (e *Entity) Destroy() {
	e.world.DestroyEntity(e.ID)
}

// Exists checks if this entity exists
func (e *Entity) Exists() bool {
	return e.world.EntityExists(e.ID)
}

// Transform component methods

// AddTransform adds a transform component to an entity
func (e *Entity) AddTransform(position Vector3) error {
	if !e.world.engine.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_add_transform(C.EntityID(e.ID),
		C.float(position.X), C.float(position.Y), C.float(position.Z)); ret != 0 {
		return errors.New("failed to add transform")
	}

	return nil
}

// GetTransform gets the transform position of an entity
func (e *Entity) GetTransform() (Vector3, error) {
	if !e.world.engine.initialized {
		return Vector3{}, errors.New("engine not initialized")
	}

	var x, y, z C.float
	if ret := C.boulder_get_transform(C.EntityID(e.ID), &x, &y, &z); ret != 0 {
		return Vector3{}, errors.New("failed to get transform")
	}

	return Vector3{X: float32(x), Y: float32(y), Z: float32(z)}, nil
}

// GetFullTransform gets the complete transform (position, rotation, scale) of an entity
func (e *Entity) GetFullTransform() (position, rotation, scale Vector3, err error) {
	if !e.world.engine.initialized {
		return Vector3{}, Vector3{}, Vector3{}, errors.New("engine not initialized")
	}

	var px, py, pz C.float
	var rx, ry, rz C.float
	var sx, sy, sz C.float

	if ret := C.boulder_get_full_transform(C.EntityID(e.ID),
		&px, &py, &pz,
		&rx, &ry, &rz,
		&sx, &sy, &sz); ret != 0 {
		return Vector3{}, Vector3{}, Vector3{}, errors.New("failed to get full transform")
	}

	position = Vector3{X: float32(px), Y: float32(py), Z: float32(pz)}
	rotation = Vector3{X: float32(rx), Y: float32(ry), Z: float32(rz)}
	scale = Vector3{X: float32(sx), Y: float32(sy), Z: float32(sz)}

	return position, rotation, scale, nil
}

// SetTransform sets the transform position of an entity
func (e *Entity) SetTransform(position Vector3) error {
	if !e.world.engine.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_set_transform(C.EntityID(e.ID),
		C.float(position.X), C.float(position.Y), C.float(position.Z)); ret != 0 {
		return errors.New("failed to set transform")
	}

	return nil
}

// SetFullTransform sets the complete transform (position, rotation, scale) of an entity
// Rotation is in radians
func (e *Entity) SetFullTransform(position, rotation, scale Vector3) error {
	if !e.world.engine.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_set_full_transform(C.EntityID(e.ID),
		C.float(position.X), C.float(position.Y), C.float(position.Z),
		C.float(rotation.X), C.float(rotation.Y), C.float(rotation.Z),
		C.float(scale.X), C.float(scale.Y), C.float(scale.Z)); ret != 0 {
		return errors.New("failed to set full transform")
	}

	return nil
}

// Physics component methods

// AddPhysicsBody adds a physics body component to an entity
func (e *Entity) AddPhysicsBody(mass float32) error {
	if !e.world.engine.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_add_physics_body(C.EntityID(e.ID), C.float(mass)); ret != 0 {
		return errors.New("failed to add physics body")
	}

	return nil
}

// SetVelocity sets the velocity of an entity's physics body
func (e *Entity) SetVelocity(velocity Vector3) error {
	if !e.world.engine.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_set_velocity(C.EntityID(e.ID),
		C.float(velocity.X), C.float(velocity.Y), C.float(velocity.Z)); ret != 0 {
		return errors.New("failed to set velocity")
	}

	return nil
}

// GetVelocity gets the velocity of an entity's physics body
func (e *Entity) GetVelocity() (Vector3, error) {
	if !e.world.engine.initialized {
		return Vector3{}, errors.New("engine not initialized")
	}

	var vx, vy, vz C.float
	if ret := C.boulder_get_velocity(C.EntityID(e.ID), &vx, &vy, &vz); ret != 0 {
		return Vector3{}, errors.New("failed to get velocity")
	}

	return Vector3{X: float32(vx), Y: float32(vy), Z: float32(vz)}, nil
}

// ApplyForce applies a force to an entity's physics body
func (e *Entity) ApplyForce(force Vector3) error {
	if !e.world.engine.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_apply_force(C.EntityID(e.ID),
		C.float(force.X), C.float(force.Y), C.float(force.Z)); ret != 0 {
		return errors.New("failed to apply force")
	}

	return nil
}

// Model component methods

// LoadModel loads a 3D model for an entity
func (e *Entity) LoadModel(path string) error {
	if !e.world.engine.initialized {
		return errors.New("engine not initialized")
	}

	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	if ret := C.boulder_load_model(C.EntityID(e.ID), cPath); ret != 0 {
		return errors.New("failed to load model")
	}

	return nil
}
