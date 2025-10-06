package main

import (
	"fmt"
	"log"
	"time"

	boulder "github.com/NOT-REAL-GAMES/BOULDER/go-bindings"
)

func runServer(engine *boulder.Engine) {
	// Create network session
	session, err := boulder.NewNetworkSession(engine)
	if err != nil {
		log.Fatalf("Failed to create network session: %v", err)
	}
	defer session.Destroy()

	// Start server on port 27015
	if err := session.StartServer(27015); err != nil {
		log.Fatalf("Failed to start server: %v", err)
	}
	defer session.StopServer()

	boulder.LogInfo("Server started on port 27015")

	// Server event loop
	for i := 0; i < 100; i++ {
		session.Update()

		// Poll for events
		for {
			event := session.PollEvent()
			if event == nil {
				break
			}

			switch e := event.(type) {
			case boulder.ConnectedEvent:
				boulder.LogInfo(fmt.Sprintf("Client connected: %d", e.Connection))

			case boulder.DisconnectedEvent:
				boulder.LogInfo(fmt.Sprintf("Client disconnected: %d", e.Connection))

			case boulder.MessageEvent:
				boulder.LogInfo(fmt.Sprintf("Received message from %d: %s", e.Connection, string(e.Data)))
				// Echo back
				response := fmt.Sprintf("Server received: %s", string(e.Data))
				session.SendReliable(e.Connection, []byte(response))
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	boulder.LogInfo("Server shutting down")
}

func runClient(engine *boulder.Engine) {
	// Give server time to start
	time.Sleep(500 * time.Millisecond)

	// Create network session
	session, err := boulder.NewNetworkSession(engine)
	if err != nil {
		log.Fatalf("Failed to create network session: %v", err)
	}
	defer session.Destroy()

	// Connect to server
	conn, err := session.Connect("127.0.0.1", 27015)
	if err != nil {
		log.Fatalf("Failed to connect to server: %v", err)
	}
	defer session.Disconnect(conn)

	boulder.LogInfo(fmt.Sprintf("Connecting to server... Connection handle: %d", conn))

	// Wait for connection
	connected := false
	for i := 0; i < 50 && !connected; i++ {
		session.Update()

		for {
			event := session.PollEvent()
			if event == nil {
				break
			}

			switch e := event.(type) {
			case boulder.ConnectedEvent:
				boulder.LogInfo(fmt.Sprintf("Connected to server! Connection: %d", e.Connection))
				connected = true

			case boulder.DisconnectedEvent:
				boulder.LogInfo(fmt.Sprintf("Disconnected from server: %d", e.Connection))
				return

			case boulder.MessageEvent:
				boulder.LogInfo(fmt.Sprintf("Received from server: %s", string(e.Data)))
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	if !connected {
		boulder.LogError("Failed to connect to server")
		return
	}

	// Send some messages
	messages := []string{"Hello Server!", "How are you?", "Goodbye!"}
	for _, msg := range messages {
		boulder.LogInfo(fmt.Sprintf("Sending: %s", msg))
		if err := session.SendReliable(conn, []byte(msg)); err != nil {
			boulder.LogError(fmt.Sprintf("Failed to send message: %v", err))
		}

		// Process responses
		for i := 0; i < 10; i++ {
			session.Update()

			for {
				event := session.PollEvent()
				if event == nil {
					break
				}

				switch e := event.(type) {
				case boulder.MessageEvent:
					boulder.LogInfo(fmt.Sprintf("Received: %s", string(e.Data)))
				}
			}

			time.Sleep(100 * time.Millisecond)
		}
	}

	boulder.LogInfo("Client done")
}

func network_demo() {
	boulder.LogInfo("Starting network test")

	// Create engine
	engine := boulder.NewEngine("Network Test", vkMakeVersion(0, 0, 1))
	if err := engine.Init(); err != nil {
		log.Fatalf("Failed to initialize engine: %v", err)
	}
	defer engine.Shutdown()

	// Run server in background
	go runServer(engine)

	// Run client
	runClient(engine)

	// Give server time to finish
	time.Sleep(2 * time.Second)

	boulder.LogInfo("Network test complete")
}
