package main

import (
	"fmt"
	"log"
	"strings"
	"time"

	boulder "github.com/NOT-REAL-GAMES/BOULDER/go-bindings"
)

// P2P Demo - Shows how to use Steam Datagram Relay for P2P connections
// This uses virtual ports instead of real IP addresses for easier NAT traversal

func vkMakeVersion(major, minor, patch uint32) uint32 {
	return uint32(major<<22 | minor<<12 | patch)
}

func testP2PWithSpacewar(engine *boulder.Engine) {
	boulder.LogInfo("\n=== P2P with Spacewar AppID ===")
	boulder.LogInfo("NOTE: This requires Steam to be running!")
	boulder.LogInfo("Testing P2P connection (AppID 480 already initialized)\n")

	// Create server session
	server, err := boulder.NewNetworkSession(engine)
	if err != nil {
		boulder.LogError(fmt.Sprintf("Failed to create server: %v", err))
		return
	}
	defer server.Destroy()

	server.SetLocalIdentity("P2P Server")
	if err := server.StartServerP2P(1000); err != nil {
		boulder.LogError(fmt.Sprintf("Failed to start P2P server: %v (Steam running?)", err))
		return
	}
	defer server.StopServer()

	serverSteamID := server.GetLocalSteamID()
	if serverSteamID == 0 {
		boulder.LogError("❌ No Steam ID obtained")
		boulder.LogInfo("\nP2P requires full Steamworks SDK integration:")
		boulder.LogInfo("  • Currently using GameNetworkingSockets in standalone mode")
		boulder.LogInfo("  • For real P2P, integrate Steamworks SDK (SteamAPI_Init)")
		boulder.LogInfo("  • Alternative: Use IP-based networking (works everywhere!)")
		boulder.LogInfo("\nWorkaround for testing P2P without Steam:")
		boulder.LogInfo("  1. Use relay servers (configure with SetRelayServer)")
		boulder.LogInfo("  2. Deploy dedicated relay infrastructure")
		boulder.LogInfo("  3. Use IP-based mode for LAN/VPN scenarios")
		return
	}

	boulder.LogInfo(fmt.Sprintf("✓ P2P Server started (Steam ID: %d)", serverSteamID))
	time.Sleep(200 * time.Millisecond)

	// Create client
	client, err := boulder.NewNetworkSession(engine)
	if err != nil {
		boulder.LogError(fmt.Sprintf("Failed to create client: %v", err))
		return
	}
	defer client.Destroy()

	client.SetLocalIdentity("P2P Client")
	clientSteamID := client.GetLocalSteamID()
	boulder.LogInfo(fmt.Sprintf("✓ P2P Client created (Steam ID: %d)", clientSteamID))

	// Connect P2P
	conn, err := client.ConnectP2P(serverSteamID, 1000)
	if err != nil {
		boulder.LogError(fmt.Sprintf("Failed to connect P2P: %v", err))
		return
	}
	defer client.Disconnect(conn)

	boulder.LogInfo("Connecting via P2P...")

	// Wait for connection
	connected := false
	for i := 0; i < 50 && !connected; i++ {
		server.Update()
		client.Update()

		for {
			event := server.PollEvent()
			if event == nil {
				break
			}
			if _, ok := event.(boulder.ConnectedEvent); ok {
				boulder.LogInfo("[SERVER] P2P client connected!")
			}
		}

		for {
			event := client.PollEvent()
			if event == nil {
				break
			}
			if _, ok := event.(boulder.ConnectedEvent); ok {
				boulder.LogInfo("[CLIENT] P2P connection established!")
				connected = true
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	if !connected {
		boulder.LogError("P2P connection failed (timeout)")
		return
	}

	// Test P2P messaging
	boulder.LogInfo("\n[CLIENT] Sending via P2P...")
	if err := client.SendReliable(conn, []byte("Hello via P2P!")); err != nil {
		boulder.LogError(fmt.Sprintf("Send failed: %v", err))
		return
	}

	for i := 0; i < 10; i++ {
		server.Update()
		client.Update()

		for {
			event := server.PollEvent()
			if event == nil {
				break
			}
			if e, ok := event.(boulder.MessageEvent); ok {
				boulder.LogInfo(fmt.Sprintf("[SERVER] Received via P2P: %s", string(e.Data)))
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	boulder.LogInfo("\n✓ P2P networking successful!")
	boulder.LogInfo("  • No port forwarding needed")
	boulder.LogInfo("  • Works through NAT")
	boulder.LogInfo("  • Uses Steam Datagram Relay")
}

func p2p_demo() {
	boulder.LogInfo("=== BOULDER Networking Demo ===\n")

	boulder.LogInfo("This demo shows:")
	boulder.LogInfo("  ✓ IP-based networking (fully working)")
	boulder.LogInfo("  ✓ P2P API demonstration (requires Steamworks SDK)")
	boulder.LogInfo("")

	// IMPORTANT: Initialize with Spacewar AppID BEFORE creating any sessions
	// This enables P2P/Steam features for ALL sessions
	boulder.LogInfo("Initializing with Spacewar (AppID 480)...")
	boulder.InitWithSteamApp(480)

	// Initialize engine
	engine := boulder.NewEngine("Networking Demo", vkMakeVersion(1, 0, 0))
	if err := engine.Init(); err != nil {
		log.Fatalf("Failed to initialize engine: %v", err)
	}
	defer engine.Shutdown()

	// Demonstrate both modes
	boulder.LogInfo("\nBOULDER supports two networking modes:\n")

	boulder.LogInfo("1. IP-BASED NETWORKING (Direct Connection)")
	boulder.LogInfo("   - Uses real IP addresses and ports")
	boulder.LogInfo("   - Works on LAN without configuration")
	boulder.LogInfo("   - Requires port forwarding for internet play")
	boulder.LogInfo("   - Example: session.StartServer(27015)")
	boulder.LogInfo("            session.Connect(\"192.168.1.100\", 27015)\n")

	boulder.LogInfo("2. P2P NETWORKING (Steam Datagram Relay)")
	boulder.LogInfo("   - Uses Steam IDs instead of IP addresses")
	boulder.LogInfo("   - Automatic NAT traversal")
	boulder.LogInfo("   - No port forwarding needed")
	boulder.LogInfo("   - Requires Steam authentication")
	boulder.LogInfo("   - Example: session.StartServerP2P(virtualPort)")
	boulder.LogInfo("            session.ConnectP2P(steamID, virtualPort)\n")

	// Show working IP-based networking
	boulder.LogInfo("=== IP-Based Connection Demo ===\n")

	// Create server
	server, err := boulder.NewNetworkSession(engine)
	if err != nil {
		log.Fatalf("Failed to create server: %v", err)
	}
	defer server.Destroy()

	server.SetLocalIdentity("Demo Server")
	if err := server.StartServer(27015); err != nil {
		log.Fatalf("Failed to start server: %v", err)
	}
	defer server.StopServer()

	boulder.LogInfo("Server started on port 27015")
	time.Sleep(200 * time.Millisecond)

	// Create client
	client, err := boulder.NewNetworkSession(engine)
	if err != nil {
		log.Fatalf("Failed to create client: %v", err)
	}
	defer client.Destroy()

	client.SetLocalIdentity("Demo Client")
	conn, err := client.Connect("127.0.0.1", 27015)
	if err != nil {
		log.Fatalf("Failed to connect: %v", err)
	}
	defer client.Disconnect(conn)

	boulder.LogInfo("Client connecting...")

	// Wait for connection
	connected := false
	for i := 0; i < 50 && !connected; i++ {
		server.Update()
		client.Update()

		for {
			event := server.PollEvent()
			if event == nil {
				break
			}
			if e, ok := event.(boulder.ConnectedEvent); ok {
				boulder.LogInfo(fmt.Sprintf("[SERVER] Client connected: %d", e.Connection))
			}
		}

		for {
			event := client.PollEvent()
			if event == nil {
				break
			}
			if _, ok := event.(boulder.ConnectedEvent); ok {
				boulder.LogInfo("[CLIENT] Connected!")
				connected = true
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	if !connected {
		boulder.LogError("Failed to connect")
		return
	}

	// Test messaging
	boulder.LogInfo("\n[CLIENT] Sending test message...")
	if err := client.SendReliable(conn, []byte("Hello from IP-based networking!")); err != nil {
		boulder.LogError(fmt.Sprintf("Failed to send: %v", err))
		return
	}

	// Process messages
	for i := 0; i < 10; i++ {
		server.Update()
		client.Update()

		for {
			event := server.PollEvent()
			if event == nil {
				break
			}
			if e, ok := event.(boulder.MessageEvent); ok {
				boulder.LogInfo(fmt.Sprintf("[SERVER] Received: %s", string(e.Data)))
			}
		}

		time.Sleep(100 * time.Millisecond)
	}

	boulder.LogInfo("\n✓ IP-based networking working!\n")

	// Try P2P with Spacewar if Steam is running
	boulder.LogInfo("\n" + strings.Repeat("=", 60))
	testP2PWithSpacewar(engine)
	boulder.LogInfo(strings.Repeat("=", 60))

	boulder.LogInfo("\n=== Demo Complete ===")
}

func main() {
	p2p_demo()
}
