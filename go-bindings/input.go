package boulder

// #cgo CFLAGS: -I..
// #cgo LDFLAGS: -L../build -lboulder_shared -Wl,-rpath,${SRCDIR}/../build
// #include <stdlib.h>
// #include "../boulder_cgo.h"
import "C"

// Input manages input handling (keyboard, mouse)
type Input struct {
	engine *Engine
}

// NewInput creates a new Input manager
func NewInput(engine *Engine) *Input {
	return &Input{
		engine: engine,
	}
}

// IsKeyPressed checks if a key is pressed
func (i *Input) IsKeyPressed(keyCode int) bool {
	if !i.engine.initialized {
		return false
	}

	return C.boulder_is_key_pressed(C.int(keyCode)) != 0
}

// IsMouseButtonPressed checks if a mouse button is pressed
func (i *Input) IsMouseButtonPressed(button int) bool {
	if !i.engine.initialized {
		return false
	}

	return C.boulder_is_mouse_button_pressed(C.int(button)) != 0
}

// GetMousePosition gets the current mouse position
func (i *Input) GetMousePosition() (x, y float32) {
	if !i.engine.initialized {
		return 0, 0
	}

	var cX, cY C.float
	C.boulder_get_mouse_position(&cX, &cY)
	return float32(cX), float32(cY)
}

// Common key codes (SDL key codes)
const (
	KeyUnknown = 0

	KeyA = 4
	KeyB = 5
	KeyC = 6
	KeyD = 7
	KeyE = 8
	KeyF = 9
	KeyG = 10
	KeyH = 11
	KeyI = 12
	KeyJ = 13
	KeyK = 14
	KeyL = 15
	KeyM = 16
	KeyN = 17
	KeyO = 18
	KeyP = 19
	KeyQ = 20
	KeyR = 21
	KeyS = 22
	KeyT = 23
	KeyU = 24
	KeyV = 25
	KeyW = 26
	KeyX = 27
	KeyY = 28
	KeyZ = 29

	Key1 = 30
	Key2 = 31
	Key3 = 32
	Key4 = 33
	Key5 = 34
	Key6 = 35
	Key7 = 36
	Key8 = 37
	Key9 = 38
	Key0 = 39

	KeyReturn    = 40
	KeyEscape    = 41
	KeyBackspace = 42
	KeyTab       = 43
	KeySpace     = 44

	KeyRight = 79
	KeyLeft  = 80
	KeyDown  = 81
	KeyUp    = 82

	KeyLCtrl  = 224
	KeyLShift = 225
	KeyLAlt   = 226
	KeyRCtrl  = 228
	KeyRShift = 229
	KeyRAlt   = 230
)

// Mouse button codes
const (
	MouseButtonLeft   = 1
	MouseButtonMiddle = 2
	MouseButtonRight  = 3
	MouseButtonX1     = 4
	MouseButtonX2     = 5
)
