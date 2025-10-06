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
	boulder.LogInfo("Starting simple network test")

	// Create engine
	engine := boulder.NewEngine("Network Test", vkMakeVersion(0, 0, 1))
	if err := engine.Init(); err != nil {
		log.Fatalf("Failed to initialize engine: %v", err)
	}
	defer engine.Shutdown()

	// Create server session
	server, err := boulder.NewNetworkSession(engine)
	if err != nil {
		log.Fatalf("Failed to create server session: %v", err)
	}
	defer server.Destroy()

	// Start server
	if err := server.StartServer(27015); err != nil {
		log.Fatalf("Failed to start server: %v", err)
	}
	defer server.StopServer()

	boulder.LogInfo("Server listening on port 27015")

	// Create client session
	client, err := boulder.NewNetworkSession(engine)
	if err != nil {
		log.Fatalf("Failed to create client session: %v", err)
	}
	defer client.Destroy()

	// Connect to server
	conn, err := client.Connect("127.0.0.1", 27015)
	if err != nil {
		log.Fatalf("Failed to connect: %v", err)
	}
	defer client.Disconnect(conn)

	boulder.LogInfo(fmt.Sprintf("Client connecting... handle: %d", conn))

	// Poll for events for 5 seconds
	connected := false
	for i := 0; i < 50 && !connected; i++ {
		// Update both server and client
		server.Update()
		client.Update()

		// Check server events
		for {
			event := server.PollEvent()
			if event == nil {
				break
			}
			switch e := event.(type) {
			case boulder.ConnectedEvent:
				boulder.LogInfo(fmt.Sprintf("[SERVER] Client connected: %d", e.Connection))
			case boulder.DisconnectedEvent:
				boulder.LogInfo(fmt.Sprintf("[SERVER] Client disconnected: %d", e.Connection))
			case boulder.MessageEvent:
				boulder.LogInfo(fmt.Sprintf("[SERVER] Message from %d: %s", e.Connection, string(e.Data)))
			}
		}

		// Check client events
		for {
			event := client.PollEvent()
			if event == nil {
				break
			}
			switch e := event.(type) {
			case boulder.ConnectedEvent:
				boulder.LogInfo(fmt.Sprintf("[CLIENT] Connected to server: %d", e.Connection))
				connected = true
			case boulder.DisconnectedEvent:
				boulder.LogInfo(fmt.Sprintf("[CLIENT] Disconnected: %d", e.Connection))
			case boulder.MessageEvent:
				boulder.LogInfo(fmt.Sprintf("[CLIENT] Message: %s", string(e.Data)))
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	if !connected {
		boulder.LogError("Failed to establish connection")
		return
	}

	// Send a test message
	msg := []byte("Hello from client!")
	if err := client.SendReliable(conn, msg); err != nil {
		boulder.LogError(fmt.Sprintf("Failed to send: %v", err))
	} else {
		boulder.LogInfo("Sent message from client")
	}

	// Process messages for a bit
	for i := 0; i < 20; i++ {
		server.Update()
		client.Update()

		for {
			event := server.PollEvent()
			if event == nil {
				break
			}
			if e, ok := event.(boulder.MessageEvent); ok {
				boulder.LogInfo(fmt.Sprintf("[SERVER] Got message: %s", string(e.Data)))
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	boulder.LogInfo("Test complete")
}
