# Boulder Engine - Go Bindings

This directory contains CGO bindings for the Boulder game engine, allowing you to use the engine from Go programs.

## Building

### Step 1: Build the C++ shared library

First, build the Boulder engine shared library from the root directory:

```bash
cd /home/tremor/Desktop/BOULDER
mkdir -p build
cd build
cmake ..
make boulder_shared
```

This will create `libboulder_shared.so` (on Linux) in the build directory.

### Step 2: Build and run Go programs

```bash
cd /home/tremor/Desktop/BOULDER/go-bindings

# Run the example
go run examples/simple_game.go

# Or build it
go build -o simple_game examples/simple_game.go
./simple_game
```

## Usage

Here's a minimal example:

```go
package main

import (
    "log"
	boulder "github.com/NOT-REAL-GAMES/BOULDER/go-bindings"
)

func main() {
    engine := boulder.NewEngine()

    if err := engine.Init(); err != nil {
        log.Fatal(err)
    }
    defer engine.Shutdown()

    if err := engine.CreateWindow(800, 600, "My Game"); err != nil {
        log.Fatal(err)
    }

    // Create an entity
    entity, _ := engine.CreateEntity()
    engine.AddTransform(entity, boulder.Vector3{X: 0, Y: 0, Z: 0})
    engine.AddPhysicsBody(entity, 1.0)

    // Game loop
    for !engine.ShouldClose() {
        engine.PollEvents()
        engine.Update(0.016) // 60 FPS
        engine.Render()
    }
}
```

## API Reference

### Engine Management
- `NewEngine()` - Create a new engine instance
- `Init()` - Initialize the engine
- `Shutdown()` - Clean up and shutdown
- `Update(deltaTime)` - Update physics and systems
- `Render()` - Render the frame

### Window Management
- `CreateWindow(width, height, title)` - Create a window
- `SetWindowSize(width, height)` - Resize the window
- `GetWindowSize()` - Get current window dimensions
- `ShouldClose()` - Check if window should close
- `PollEvents()` - Process window events

### Entity Component System
- `CreateEntity()` - Create a new entity
- `DestroyEntity(entity)` - Remove an entity
- `EntityExists(entity)` - Check if entity exists

### Components
- `AddTransform(entity, position)` - Add position component
- `GetTransform(entity)` - Get position
- `SetTransform(entity, position)` - Update position
- `AddPhysicsBody(entity, mass)` - Add physics
- `SetVelocity(entity, velocity)` - Set velocity
- `GetVelocity(entity)` - Get current velocity
- `ApplyForce(entity, force)` - Apply physics force
- `LoadModel(entity, path)` - Load 3D model

### Input
- `IsKeyPressed(keyCode)` - Check key state
- `IsMouseButtonPressed(button)` - Check mouse button
- `GetMousePosition()` - Get mouse coordinates

### Logging
- `LogInfo(message)` - Log info message
- `LogError(message)` - Log error message

## Notes

- The shared library must be in the library path or the build directory
- SDL key codes are used for input handling
- All positions and vectors use float32
- Entity IDs are uint64 values