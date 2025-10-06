package boulder

// #cgo CFLAGS: -I..
// #cgo LDFLAGS: -L../build -lboulder_shared -Wl,-rpath,${SRCDIR}/../build
// #include <stdlib.h>
// #include "../boulder_cgo.h"
import "C"
import (
	"errors"
	"os"
	"unsafe"
)

// ShaderKind represents the type of shader
type ShaderKind int

const (
	// Shader kinds from shaderc
	ShaderKindVertex         ShaderKind = 0
	ShaderKindFragment       ShaderKind = 1
	ShaderKindCompute        ShaderKind = 2
	ShaderKindGeometry       ShaderKind = 3
	ShaderKindTessControl    ShaderKind = 4
	ShaderKindTessEvaluation ShaderKind = 5
	ShaderKindMesh           ShaderKind = 0x0000000B // shaderc_glsl_default_mesh_shader
	ShaderKindTask           ShaderKind = 0x0000000C // shaderc_glsl_default_task_shader
	ShaderKindRayGen         ShaderKind = 8
	ShaderKindAnyHit         ShaderKind = 9
	ShaderKindClosestHit     ShaderKind = 10
	ShaderKindMiss           ShaderKind = 11
	ShaderKindIntersection   ShaderKind = 12
	ShaderKindCallable       ShaderKind = 13
)

// ShaderModuleID uniquely identifies a compiled shader module
type ShaderModuleID uint64

// Shader represents a compiled shader module
type Shader struct {
	ID     ShaderModuleID
	Kind   ShaderKind
	Name   string
	engine *Engine
}

// CompileShader compiles shader source code and creates a shader module
func (e *Engine) CompileShader(source string, kind ShaderKind, name string) (*Shader, error) {
	if !e.initialized {
		return nil, errors.New("engine not initialized")
	}

	cSource := C.CString(source)
	defer C.free(unsafe.Pointer(cSource))

	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	id := C.boulder_compile_shader(cSource, C.int(kind), cName)
	if id == 0 {
		return nil, errors.New("failed to compile shader: " + name)
	}

	return &Shader{
		ID:     ShaderModuleID(id),
		Kind:   kind,
		Name:   name,
		engine: e,
	}, nil
}

// CompileShaderFromFile loads and compiles a shader from a file
func (e *Engine) CompileShaderFromFile(path string, kind ShaderKind) (*Shader, error) {
	if !e.initialized {
		return nil, errors.New("engine not initialized")
	}

	// Read file
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	source := string(data)
	return e.CompileShader(source, kind, path)
}

// Destroy destroys the shader module and frees resources
func (s *Shader) Destroy() {
	if s.engine == nil || !s.engine.initialized {
		return
	}

	C.boulder_destroy_shader_module(C.ShaderModuleID(s.ID))
	s.ID = 0
}

// Reload recompiles the shader with new source code
func (s *Shader) Reload(source string) error {
	if s.engine == nil || !s.engine.initialized {
		return errors.New("engine not initialized")
	}

	cSource := C.CString(source)
	defer C.free(unsafe.Pointer(cSource))

	cName := C.CString(s.Name)
	defer C.free(unsafe.Pointer(cName))

	newID := C.boulder_reload_shader(C.ShaderModuleID(s.ID), cSource, C.int(s.Kind), cName)
	if newID == 0 {
		return errors.New("failed to reload shader: " + s.Name)
	}

	s.ID = ShaderModuleID(newID)
	return nil
}

// ReloadFromFile reloads the shader from a file
func (s *Shader) ReloadFromFile(path string) error {
	if s.engine == nil || !s.engine.initialized {
		return errors.New("engine not initialized")
	}

	// Read file
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}

	source := string(data)
	s.Name = path
	return s.Reload(source)
}
