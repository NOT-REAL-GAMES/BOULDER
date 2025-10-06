package main

import (
	"fmt"
	"log"
	"time"

	boulder "github.com/NOT-REAL-GAMES/BOULDER/go-bindings"
)

func vkMakeVersion(major, minor, patch uint32) uint32 {
	return uint32(major<<22 | minor<<12 | patch)
}

func main() {
	// Create and initialize the engine
	engine := boulder.NewEngine("YOUR GAME NAME HERE", vkMakeVersion(0, 0, 1))

	boulder.LogInfo("Starting Boulder Engine from Go!")

	if err := engine.Init(); err != nil {
		log.Fatalf("Failed to initialize engine: %v", err)
	}
	defer engine.Shutdown()

	// Create modular subsystems
	window := boulder.NewWindow(engine)
	world := boulder.NewWorld(engine)
	input := boulder.NewInput(engine)

	// Create a window
	if err := window.Create(1280, 720, engine.GetAppName()); err != nil {
		log.Fatalf("Failed to create window: %v", err)
	}

	boulder.LogInfo("Window created successfully")

	// Create player entity
	player, err := world.NewEntity()
	if err != nil {
		log.Fatalf("Failed to create player entity: %v", err)
	}

	// Add transform component to player
	if err := player.AddTransform(boulder.Vector3{X: 0, Y: 0, Z: 0}); err != nil {
		log.Fatalf("Failed to add transform to player: %v", err)
	}

	// Add physics body to player
	if err := player.AddPhysicsBody(10.0); err != nil {
		log.Fatalf("Failed to add physics body to player: %v", err)
	}

	boulder.LogInfo(fmt.Sprintf("Created player entity with ID: %d", player.ID))

	// Create some additional entities
	for i := 0; i < 5; i++ {
		entity, err := world.NewEntity()
		if err != nil {
			boulder.LogError(fmt.Sprintf("Failed to create entity %d: %v", i, err))
			continue
		}

		// Position entities in a line
		position := boulder.Vector3{
			X: float32(i * 2),
			Y: 0,
			Z: 5,
		}

		if err := entity.AddTransform(position); err != nil {
			boulder.LogError(fmt.Sprintf("Failed to add transform to entity %d: %v", i, err))
			continue
		}

		if err := entity.AddPhysicsBody(1.0); err != nil {
			boulder.LogError(fmt.Sprintf("Failed to add physics to entity %d: %v", i, err))
			continue
		}

		// Apply some initial velocity
		velocity := boulder.Vector3{
			X: 0,
			Y: float32(i),
			Z: 0,
		}
		entity.SetVelocity(velocity)
	}

	// Main game loop
	lastTime := time.Now()
	frameCount := 0
	fpsTime := time.Now()

	boulder.LogInfo("Starting main loop")

	for !window.ShouldClose() {
		// Calculate delta time
		currentTime := time.Now()
		deltaTime := float32(currentTime.Sub(lastTime).Seconds())
		lastTime = currentTime

		// Poll for events
		window.PollEvents()

		// Simple input handling
		if input.IsKeyPressed(boulder.KeyRight) {
			pos, _ := player.GetTransform()
			pos.X += 5.0 * deltaTime
			player.SetTransform(pos)
		}
		if input.IsKeyPressed(boulder.KeyLeft) {
			pos, _ := player.GetTransform()
			pos.X -= 5.0 * deltaTime
			player.SetTransform(pos)
		}
		if input.IsKeyPressed(boulder.KeyUp) {
			pos, _ := player.GetTransform()
			pos.Z -= 5.0 * deltaTime
			player.SetTransform(pos)
		}
		if input.IsKeyPressed(boulder.KeyDown) {
			pos, _ := player.GetTransform()
			pos.Z += 5.0 * deltaTime
			player.SetTransform(pos)
		}

		// Jump on spacebar
		if input.IsKeyPressed(boulder.KeySpace) {
			player.SetVelocity(boulder.Vector3{X: 0, Y: 10, Z: 0})
		}

		// Update physics and render
		if err := engine.Update(deltaTime); err != nil {
			boulder.LogError(fmt.Sprintf("Update error: %v", err))
		}

		if err := engine.Render(); err != nil {
			boulder.LogError(fmt.Sprintf("Render error: %v", err))
		}

		// Calculate FPS
		frameCount++
		if time.Since(fpsTime) >= time.Second {
			fps := float64(frameCount) / time.Since(fpsTime).Seconds()
			fmt.Printf("FPS: %.2f\n", fps)
			frameCount = 0
			fpsTime = time.Now()
		}

		// Cap frame rate at ~60 FPS
		//if deltaTime < 0.016 {
		//	time.Sleep(time.Duration((0.016-deltaTime)*1000) * time.Millisecond)
		//}
	}

	boulder.LogInfo("Shutting down...")
}
