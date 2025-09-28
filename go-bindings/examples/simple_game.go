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

	// Create a window
	if err := engine.CreateWindow(1280, 720, engine.GetAppName()); err != nil {
		log.Fatalf("Failed to create window: %v", err)
	}

	boulder.LogInfo("Window created successfully")

	// Create some entities
	player, err := engine.CreateEntity()
	if err != nil {
		log.Fatalf("Failed to create player entity: %v", err)
	}

	// Add transform component to player
	if err := engine.AddTransform(player, boulder.Vector3{X: 0, Y: 0, Z: 0}); err != nil {
		log.Fatalf("Failed to add transform to player: %v", err)
	}

	// Add physics body to player
	if err := engine.AddPhysicsBody(player, 10.0); err != nil {
		log.Fatalf("Failed to add physics body to player: %v", err)
	}

	boulder.LogInfo(fmt.Sprintf("Created player entity with ID: %d", player))

	// Create some additional entities
	for i := 0; i < 5; i++ {
		entity, err := engine.CreateEntity()
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

		if err := engine.AddTransform(entity, position); err != nil {
			boulder.LogError(fmt.Sprintf("Failed to add transform to entity %d: %v", i, err))
			continue
		}

		if err := engine.AddPhysicsBody(entity, 1.0); err != nil {
			boulder.LogError(fmt.Sprintf("Failed to add physics to entity %d: %v", i, err))
			continue
		}

		// Apply some initial velocity
		velocity := boulder.Vector3{
			X: 0,
			Y: float32(i),
			Z: 0,
		}
		engine.SetVelocity(entity, velocity)
	}

	// Main game loop
	lastTime := time.Now()
	frameCount := 0
	fpsTime := time.Now()

	boulder.LogInfo("Starting main loop")

	for !engine.ShouldClose() {
		// Calculate delta time
		currentTime := time.Now()
		deltaTime := float32(currentTime.Sub(lastTime).Seconds())
		lastTime = currentTime

		// Poll for events
		engine.PollEvents()

		// Simple input handling
		if engine.IsKeyPressed(79) { // Right arrow (SDL scancode)
			pos, _ := engine.GetTransform(player)
			pos.X += 5.0 * deltaTime
			engine.SetTransform(player, pos)
		}
		if engine.IsKeyPressed(80) { // Left arrow
			pos, _ := engine.GetTransform(player)
			pos.X -= 5.0 * deltaTime
			engine.SetTransform(player, pos)
		}
		if engine.IsKeyPressed(82) { // Up arrow
			pos, _ := engine.GetTransform(player)
			pos.Z -= 5.0 * deltaTime
			engine.SetTransform(player, pos)
		}
		if engine.IsKeyPressed(81) { // Down arrow
			pos, _ := engine.GetTransform(player)
			pos.Z += 5.0 * deltaTime
			engine.SetTransform(player, pos)
		}

		// Jump on spacebar
		if engine.IsKeyPressed(44) { // Space (SDL scancode)
			engine.SetVelocity(player, boulder.Vector3{X: 0, Y: 10, Z: 0})
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
