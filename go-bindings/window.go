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

// Window manages the application window
type Window struct {
	engine *Engine
	width  int
	height int
	title  string
}

// NewWindow creates a new window manager
func NewWindow(engine *Engine) *Window {
	return &Window{
		engine: engine,
	}
}

// Create creates a new window with the specified dimensions and title
func (w *Window) Create(width, height int, title string) error {
	if !w.engine.initialized {
		return errors.New("engine not initialized")
	}

	cTitle := C.CString(title)
	defer C.free(unsafe.Pointer(cTitle))

	if ret := C.boulder_create_window(C.int(width), C.int(height), cTitle); ret != 0 {
		return errors.New("failed to create window")
	}

	w.width = width
	w.height = height
	w.title = title

	return nil
}

// SetSize sets the window size
func (w *Window) SetSize(width, height int) {
	if !w.engine.initialized {
		return
	}

	C.boulder_set_window_size(C.int(width), C.int(height))
	w.width = width
	w.height = height
}

// GetSize gets the current window size
func (w *Window) GetSize() (width, height int) {
	if !w.engine.initialized {
		return 0, 0
	}

	var cWidth, cHeight C.int
	C.boulder_get_window_size(&cWidth, &cHeight)
	w.width = int(cWidth)
	w.height = int(cHeight)
	return w.width, w.height
}

// ShouldClose returns true if the window should close
func (w *Window) ShouldClose() bool {
	if !w.engine.initialized {
		return true
	}

	return C.boulder_should_close() != 0
}

// PollEvents polls for window and input events
func (w *Window) PollEvents() {
	if !w.engine.initialized {
		return
	}

	C.boulder_poll_events()
}

// GetTitle returns the window title
func (w *Window) GetTitle() string {
	return w.title
}
