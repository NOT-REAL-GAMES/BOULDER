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

	boulder.InitWithSteamApp(4098700)

	engine := boulder.NewEngine("YOUR GAME NAME HERE", vkMakeVersion(0, 0, 1))

	// Create and initialize the engine

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

	// UI system is automatically initialized when window is created
	boulder.LogInfo("UI system initialized")

	// Create UI buttons
	quitButton := boulder.CreateUIButton(
		50, 50, 150, 40,
		boulder.UIColor{R: 0.8, G: 0.2, B: 0.2, A: 1.0}, // Red
		boulder.UIColor{R: 1.0, G: 0.3, B: 0.3, A: 1.0}, // Lighter red on hover
		boulder.UIColor{R: 0.6, G: 0.1, B: 0.1, A: 1.0}, // Darker red when pressed
	)
	if quitButton == nil {
		boulder.LogError("Failed to create quit button")
	} else {
		defer quitButton.Destroy()
		boulder.LogInfo("Quit button created at (50, 50)")
	}

	startServerButton := boulder.CreateUIButton(
		50, 100, 150, 40,
		boulder.UIColorBlue,
		boulder.UIColor{R: 0.3, G: 0.5, B: 1.0, A: 1.0},
		boulder.UIColor{R: 0.0, G: 0.2, B: 0.6, A: 1.0},
	)
	if startServerButton == nil {
		boulder.LogError("Failed to create start server button")
	} else {
		defer startServerButton.Destroy()
		boulder.LogInfo("Start Server button created at (50, 100)")
	}

	disconnectButton := boulder.CreateUIButton(
		50, 150, 150, 40,
		boulder.UIColor{R: 0.8, G: 0.5, B: 0.0, A: 1.0}, // Orange
		boulder.UIColor{R: 1.0, G: 0.6, B: 0.2, A: 1.0}, // Lighter orange on hover
		boulder.UIColor{R: 0.6, G: 0.3, B: 0.0, A: 1.0}, // Darker orange when pressed
	)
	if disconnectButton == nil {
		boulder.LogError("Failed to create disconnect button")
	} else {
		defer disconnectButton.Destroy()
		boulder.LogInfo("Disconnect button created at (50, 150)")
	}

	jumpButton := boulder.CreateUIButton(
		50, 200, 150, 40,
		boulder.UIColorGreen,
		boulder.UIColor{R: 0.3, G: 1.0, B: 0.3, A: 1.0},
		boulder.UIColor{R: 0.0, G: 0.6, B: 0.0, A: 1.0},
	)
	if jumpButton == nil {
		boulder.LogError("Failed to create jump button")
	} else {
		defer jumpButton.Destroy()
		boulder.LogInfo("Jump button created at (50, 200)")
	}

	// Status indicator (disabled button showing connection state)
	statusIndicator := boulder.CreateUIButton(
		50, 250, 150, 40,
		boulder.UIColor{R: 0.3, G: 1.0, B: 0.3, A: 1.0},
		boulder.UIColorDarkGray,
		boulder.UIColorDarkGray,
	)
	if statusIndicator != nil {
		defer statusIndicator.Destroy()
		statusIndicator.SetEnabled(false)
	}

	// Create player entity
	player, err := world.NewEntity()
	if err != nil {
		log.Fatalf("Failed to create player entity: %v", err)
	}

	mohrtana, err := world.NewEntity()
	if err != nil {
		log.Fatalf("Failed to create player entity: %v", err)
	}

	// Add transform component to player
	if err := player.AddTransform(boulder.Vector3{X: 0, Y: 0, Z: 0}); err != nil {
		log.Fatalf("Failed to add transform to player: %v", err)
	}

	if err := mohrtana.AddTransform(boulder.Vector3{X: 0, Y: -10, Z: -10}); err != nil {
		log.Fatalf("Failed to add transform to Mohrtana: %v", err)
	}

	// Add physics body to player
	if err := player.AddPhysicsBody(10.0); err != nil {
		log.Fatalf("Failed to add physics body to player: %v", err)
	}

	// Load the mohrtana model
	if err := mohrtana.LoadModel("assets/mohrtana.gltf"); err != nil {
		boulder.LogError(fmt.Sprintf("Failed to load model: %v", err))
	} else {
		boulder.LogInfo("✓ Loaded mohrtana.gltf model")
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
	connected := false
	shouldQuit := false

	// Initialize with dummy network sessions (Null Object pattern)
	server := boulder.NewDummyNetworkSession()
	client := boulder.NewDummyNetworkSession()
	var activeConnection boulder.ConnectionHandle = 0

	for !window.ShouldClose() && !shouldQuit {

		// Calculate delta time
		currentTime := time.Now()
		deltaTime := float32(currentTime.Sub(lastTime).Seconds())
		lastTime = currentTime

		if pos, rot, scale, ok := mohrtana.GetFullTransform(); ok == nil {
			rot.Y = rot.Y + 0.01*deltaTime
			mohrtana.SetFullTransform(pos, rot, scale)

		}

		// Always safe to call Update/PollEvent on dummy sessions (they do nothing)
		server.Update()
		client.Update()

		// Poll server events
		for {
			event := server.PollEvent()
			if event == nil {
				break
			}
			if _, ok := event.(boulder.ConnectedEvent); ok {
				boulder.LogInfo("[SERVER] P2P client connected!")
			}
		}

		// Poll client events
		for {
			event := client.PollEvent()
			if event == nil {
				break
			}
			if _, ok := event.(boulder.ConnectedEvent); ok {
				boulder.LogInfo("[CLIENT] P2P connection established!")
				connected = true
				// Update status indicator color when connected
				if statusIndicator != nil {
					statusIndicator.SetEnabled(!connected) // Still disabled, just visual
				}
			}
		}

		// Poll for events
		window.PollEvents()

		// Handle UI input - get mouse position and send to UI system
		mouseX, mouseY := input.GetMousePosition()
		boulder.UIHandleMouseMove(mouseX, mouseY)

		// Handle mouse button events
		if input.IsMouseButtonPressed(boulder.MouseButtonLeft) {
			boulder.UIHandleMouseDown(mouseX, mouseY)
		} else {
			boulder.UIHandleMouseUp(mouseX, mouseY)
		}

		// Check UI button clicks
		if quitButton != nil && quitButton.WasClicked() {
			boulder.LogInfo("Quit button clicked! Exiting...")
			quitButton.ResetClick()
			shouldQuit = true
		}

		if startServerButton != nil && startServerButton.WasClicked() {
			boulder.LogInfo("Start Server button clicked!")
			startServerButton.ResetClick()

			// Clean up any existing sessions
			if server.IsValid() {
				server.Destroy()
			}
			if client.IsValid() {
				if activeConnection != 0 {
					client.Disconnect(activeConnection)
				}
				client.Destroy()
			}

			// Create real network sessions
			var err error
			server, err = boulder.NewNetworkSession(engine)
			if err != nil {
				boulder.LogError(fmt.Sprintf("Failed to create server: %v", err))
				server = boulder.NewDummyNetworkSession()
			} else {
				server.SetLocalIdentity("P2P Server")
				if err := server.StartServerP2P(1000); err != nil {
					boulder.LogError(fmt.Sprintf("Failed to start P2P server: %v (Steam running?)", err))
					server.Destroy()
					server = boulder.NewDummyNetworkSession()
				} else {
					boulder.LogInfo("✓ P2P Server started!")
					serverSteamID := server.GetLocalSteamID()

					// Create client session
					client, err = boulder.NewNetworkSession(engine)
					if err != nil {
						boulder.LogError(fmt.Sprintf("Failed to create client: %v", err))
						client = boulder.NewDummyNetworkSession()
					} else {
						client.SetLocalIdentity("P2P Client")
						clientSteamID := client.GetLocalSteamID()
						boulder.LogInfo(fmt.Sprintf("✓ P2P Client created (Steam ID: %d)", clientSteamID))

						// Connect P2P
						activeConnection, err = client.ConnectP2P(serverSteamID, 1000)
						if err != nil {
							boulder.LogError(fmt.Sprintf("Failed to connect P2P: %v", err))
							client.Destroy()
							client = boulder.NewDummyNetworkSession()
							activeConnection = 0
						} else {
							boulder.LogInfo("✓ Connecting to P2P server...")
						}
					}
				}
			}
		}

		if disconnectButton != nil && disconnectButton.WasClicked() {
			boulder.LogInfo("Disconnect button clicked!")
			disconnectButton.ResetClick()

			// Disconnect and clean up sessions
			if client.IsValid() {
				if activeConnection != 0 {
					client.Disconnect(activeConnection)
					activeConnection = 0
				}
				client.Destroy()
			}
			if server.IsValid() {
				server.StopServer()
				server.Destroy()
			}

			// Replace with dummy sessions
			server = boulder.NewDummyNetworkSession()
			client = boulder.NewDummyNetworkSession()
			connected = false

			boulder.LogInfo("✓ Disconnected from P2P")
		}

		if jumpButton != nil && jumpButton.WasClicked() {
			boulder.LogInfo("Jump button clicked!")
			jumpButton.ResetClick()
			player.SetVelocity(boulder.Vector3{X: 0, Y: 10, Z: 0})
		}

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

		/*if err := engine.Render(); err != nil {
			boulder.LogError(fmt.Sprintf("Render error: %v", err))
		}*/

		img := boulder.BeginFrame()
		boulder.RenderModels()
		boulder.EndFrame(img)

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
