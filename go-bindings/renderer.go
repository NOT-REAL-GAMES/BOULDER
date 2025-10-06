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

// Renderer manages rendering operations
type Renderer struct {
	engine       *Engine
	clearColor   [4]float32
	currentImage uint32
}

// NewRenderer creates a new Renderer instance
func NewRenderer(engine *Engine) *Renderer {
	return &Renderer{
		engine:     engine,
		clearColor: [4]float32{0.1, 0.2, 0.3, 1.0},
	}
}

// SetClearColor sets the clear color for rendering
func (r *Renderer) SetClearColor(red, green, blue, alpha float32) {
	r.clearColor = [4]float32{red, green, blue, alpha}
	C.boulder_set_clear_color(C.float(red), C.float(green), C.float(blue), C.float(alpha))
}

// GetClearColor returns the current clear color
func (r *Renderer) GetClearColor() (red, green, blue, alpha float32) {
	return r.clearColor[0], r.clearColor[1], r.clearColor[2], r.clearColor[3]
}

// BeginFrame starts a new frame and returns the image index
// Returns -2 if swapchain recreation is needed
func (r *Renderer) BeginFrame() (imageIndex uint32, err error) {
	if !r.engine.initialized {
		return 0, errors.New("engine not initialized")
	}

	var idx C.uint32_t
	result := C.boulder_begin_frame(&idx)

	if result == -2 {
		return 0, errors.New("swapchain recreation needed")
	} else if result != 0 {
		return 0, errors.New("failed to begin frame")
	}

	r.currentImage = uint32(idx)
	return r.currentImage, nil
}

// EndFrame ends the current frame and presents it
func (r *Renderer) EndFrame() error {
	if !r.engine.initialized {
		return errors.New("engine not initialized")
	}

	result := C.boulder_end_frame(C.uint32_t(r.currentImage))
	if result != 0 {
		return errors.New("failed to end frame")
	}

	return nil
}

// SetViewport sets the viewport for rendering
func (r *Renderer) SetViewport(x, y, width, height, minDepth, maxDepth float32) {
	if !r.engine.initialized {
		return
	}

	C.boulder_set_viewport(C.float(x), C.float(y), C.float(width), C.float(height), C.float(minDepth), C.float(maxDepth))
}

// SetScissor sets the scissor rectangle
func (r *Renderer) SetScissor(x, y, width, height int) {
	if !r.engine.initialized {
		return
	}

	C.boulder_set_scissor(C.int(x), C.int(y), C.int(width), C.int(height))
}

// GetSwapchainExtent returns the current swapchain dimensions
func (r *Renderer) GetSwapchainExtent() (width, height int) {
	var w, h C.int
	C.boulder_get_swapchain_extent(&w, &h)
	return int(w), int(h)
}

// RecreateSwapchain requests swapchain recreation
func (r *Renderer) RecreateSwapchain() error {
	if !r.engine.initialized {
		return errors.New("engine not initialized")
	}

	if result := C.boulder_recreate_swapchain(); result != 0 {
		return errors.New("failed to recreate swapchain")
	}

	return nil
}

// DrawMesh draws a mesh using mesh shaders
func (r *Renderer) DrawMesh(groupCountX, groupCountY, groupCountZ uint32) {
	if !r.engine.initialized {
		return
	}

	C.boulder_draw_mesh(C.uint32_t(groupCountX), C.uint32_t(groupCountY), C.uint32_t(groupCountZ))
}

// SetPushConstants sets push constants for the bound pipeline
func (r *Renderer) SetPushConstants(data interface{}, offset uint32) error {
	if !r.engine.initialized {
		return errors.New("engine not initialized")
	}

	// Convert data to byte slice
	var ptr unsafe.Pointer
	var size C.uint32_t

	switch v := data.(type) {
	case []byte:
		if len(v) == 0 {
			return errors.New("empty data")
		}
		ptr = unsafe.Pointer(&v[0])
		size = C.uint32_t(len(v))
	case []float32:
		if len(v) == 0 {
			return errors.New("empty data")
		}
		ptr = unsafe.Pointer(&v[0])
		size = C.uint32_t(len(v) * 4)
	case []int32:
		if len(v) == 0 {
			return errors.New("empty data")
		}
		ptr = unsafe.Pointer(&v[0])
		size = C.uint32_t(len(v) * 4)
	default:
		return errors.New("unsupported data type")
	}

	C.boulder_set_push_constants(ptr, size, C.uint32_t(offset))
	return nil
}
