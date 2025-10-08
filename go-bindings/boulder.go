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

// ============================================================================
// UI System
// ============================================================================

// UIColor represents an RGBA color
type UIColor struct {
	R, G, B, A float32
}

// Common UI colors
var (
	UIColorRed     = UIColor{1.0, 0.0, 0.0, 1.0}
	UIColorGreen   = UIColor{0.0, 1.0, 0.0, 1.0}
	UIColorBlue    = UIColor{0.0, 0.0, 1.0, 1.0}
	UIColorYellow  = UIColor{1.0, 1.0, 0.0, 1.0}
	UIColorWhite   = UIColor{1.0, 1.0, 1.0, 1.0}
	UIColorBlack   = UIColor{0.0, 0.0, 0.0, 1.0}
	UIColorGray    = UIColor{0.5, 0.5, 0.5, 1.0}
	UIColorDarkGray = UIColor{0.3, 0.3, 0.3, 1.0}
)

// UIButton represents a clickable UI button
type UIButton struct {
	id            C.UIButtonID
	clickedThisFrame bool
}

// UIInitialize initializes the UI system
func UIInitialize() error {
	if ret := C.boulder_ui_init(); ret != 0 {
		return errors.New("failed to initialize UI system")
	}
	return nil
}

// UICleanup cleans up the UI system
func UICleanup() {
	C.boulder_ui_cleanup()
}

// CreateUIButton creates a new UI button with the specified properties
func CreateUIButton(x, y, width, height float32, normalColor, hoverColor, pressedColor UIColor) *UIButton {
	buttonID := C.boulder_ui_create_button(
		C.float(x), C.float(y), C.float(width), C.float(height),
		C.float(normalColor.R), C.float(normalColor.G), C.float(normalColor.B), C.float(normalColor.A),
		C.float(hoverColor.R), C.float(hoverColor.G), C.float(hoverColor.B), C.float(hoverColor.A),
		C.float(pressedColor.R), C.float(pressedColor.G), C.float(pressedColor.B), C.float(pressedColor.A),
	)

	if buttonID == 0 {
		return nil
	}

	return &UIButton{id: buttonID}
}

// Destroy destroys the button and frees its resources
func (b *UIButton) Destroy() {
	if b.id != 0 {
		C.boulder_ui_destroy_button(b.id)
		b.id = 0
	}
}

// SetPosition sets the button's position
func (b *UIButton) SetPosition(x, y float32) {
	if b.id != 0 {
		C.boulder_ui_set_button_position(b.id, C.float(x), C.float(y))
	}
}

// SetSize sets the button's size
func (b *UIButton) SetSize(width, height float32) {
	if b.id != 0 {
		C.boulder_ui_set_button_size(b.id, C.float(width), C.float(height))
	}
}

// SetEnabled enables or disables the button
func (b *UIButton) SetEnabled(enabled bool) {
	if b.id != 0 {
		var cEnabled C.int
		if enabled {
			cEnabled = 1
		} else {
			cEnabled = 0
		}
		C.boulder_ui_set_button_enabled(b.id, cEnabled)
	}
}

// WasClicked returns true if the button was clicked since the last reset
func (b *UIButton) WasClicked() bool {
	if b.id == 0 {
		return false
	}
	return C.boulder_ui_button_was_clicked(b.id) != 0
}

// ResetClick resets the button's click state
func (b *UIButton) ResetClick() {
	if b.id != 0 {
		C.boulder_ui_reset_button_click(b.id)
	}
}

// UI input handling functions

// UIHandleMouseMove updates the UI with the current mouse position
func UIHandleMouseMove(x, y float32) {
	C.boulder_ui_handle_mouse_move(C.float(x), C.float(y))
}

// UIHandleMouseDown notifies the UI of a mouse button press
func UIHandleMouseDown(x, y float32) {
	C.boulder_ui_handle_mouse_down(C.float(x), C.float(y))
}

// UIHandleMouseUp notifies the UI of a mouse button release
func UIHandleMouseUp(x, y float32) {
	C.boulder_ui_handle_mouse_up(C.float(x), C.float(y))
}

// UIRender renders the UI overlay for the given swapchain image
func UIRender(imageIndex uint32) {
	C.boulder_ui_render(C.uint32_t(imageIndex))
}
