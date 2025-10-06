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

// Vector3 represents a 3D vector
type Vector3 struct {
	X, Y, Z float32
}

// Engine represents the Boulder game engine core
type Engine struct {
	appName     string
	version     uint32
	initialized bool
}

// NewEngine creates a new Engine instance
func NewEngine(appName string, ver uint32) *Engine {
	return &Engine{
		appName: appName,
		version: ver,
	}
}

// Init initializes the Boulder engine
func (e *Engine) Init() error {
	if e.initialized {
		return errors.New("engine already initialized")
	}

	cAppName := C.CString(e.appName)
	defer C.free(unsafe.Pointer(cAppName))

	if ret := C.boulder_init(cAppName, C.uint(e.version)); ret != 0 {
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

// IsInitialized returns whether the engine is initialized
func (e *Engine) IsInitialized() bool {
	return e.initialized
}

// GetAppName returns the application name
func (e *Engine) GetAppName() string {
	return e.appName
}

// GetVersion returns the application version
func (e *Engine) GetVersion() uint32 {
	return e.version
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

// Render renders the current frame (legacy function, prefer using Renderer)
func (e *Engine) Render() error {
	if !e.initialized {
		return errors.New("engine not initialized")
	}

	if ret := C.boulder_render(); ret != 0 {
		return errors.New("failed to render frame")
	}

	return nil
}

// Logging functions

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
