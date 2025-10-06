package boulder

// #cgo CFLAGS: -I..
// #cgo LDFLAGS: -L../build -lboulder_shared -Wl,-rpath,${SRCDIR}/../build
// #include <stdlib.h>
// #include "../boulder_cgo.h"
import "C"
import "errors"

// PipelineID uniquely identifies a graphics pipeline
type PipelineID uint64

// Pipeline represents a graphics pipeline
type Pipeline struct {
	ID         PipelineID
	MeshShader *Shader
	FragShader *Shader
	engine     *Engine
}

// PipelineConfig contains configuration for creating a graphics pipeline
type PipelineConfig struct {
	MeshShader *Shader
	FragShader *Shader
}

// CreateGraphicsPipeline creates a new graphics pipeline from shaders
func (e *Engine) CreateGraphicsPipeline(config PipelineConfig) (*Pipeline, error) {
	if !e.initialized {
		return nil, errors.New("engine not initialized")
	}

	if config.MeshShader == nil || config.FragShader == nil {
		return nil, errors.New("both mesh and fragment shaders are required")
	}

	id := C.boulder_create_graphics_pipeline(
		C.ShaderModuleID(config.MeshShader.ID),
		C.ShaderModuleID(config.FragShader.ID),
	)

	if id == 0 {
		return nil, errors.New("failed to create graphics pipeline")
	}

	return &Pipeline{
		ID:         PipelineID(id),
		MeshShader: config.MeshShader,
		FragShader: config.FragShader,
		engine:     e,
	}, nil
}

// Bind binds this pipeline for rendering
func (p *Pipeline) Bind() error {
	if p.engine == nil || !p.engine.initialized {
		return errors.New("engine not initialized")
	}

	C.boulder_bind_pipeline(C.PipelineID(p.ID))
	return nil
}

// Destroy destroys the pipeline and frees resources
func (p *Pipeline) Destroy() {
	if p.engine == nil || !p.engine.initialized {
		return
	}

	C.boulder_destroy_pipeline(C.PipelineID(p.ID))
	p.ID = 0
}

// PipelineBuilder provides a fluent interface for building pipelines
type PipelineBuilder struct {
	engine     *Engine
	meshShader *Shader
	fragShader *Shader
}

// NewPipelineBuilder creates a new pipeline builder
func (e *Engine) NewPipelineBuilder() *PipelineBuilder {
	return &PipelineBuilder{
		engine: e,
	}
}

// WithMeshShader sets the mesh shader
func (pb *PipelineBuilder) WithMeshShader(shader *Shader) *PipelineBuilder {
	pb.meshShader = shader
	return pb
}

// WithFragmentShader sets the fragment shader
func (pb *PipelineBuilder) WithFragmentShader(shader *Shader) *PipelineBuilder {
	pb.fragShader = shader
	return pb
}

// Build creates the pipeline
func (pb *PipelineBuilder) Build() (*Pipeline, error) {
	if pb.meshShader == nil {
		return nil, errors.New("mesh shader not set")
	}
	if pb.fragShader == nil {
		return nil, errors.New("fragment shader not set")
	}

	return pb.engine.CreateGraphicsPipeline(PipelineConfig{
		MeshShader: pb.meshShader,
		FragShader: pb.fragShader,
	})
}
