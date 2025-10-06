package main

import (
	"encoding/json"
	"fmt"
	"log"
	"time"

	boulder "github.com/NOT-REAL-GAMES/BOULDER/go-bindings"
)

// Game messages
type MessageType uint8

const (
	MsgPlayerJoin MessageType = iota
	MsgPlayerMove
	MsgPlayerLeave
)

type GameMessage struct {
	Type     MessageType `json:"type"`
	PlayerID uint64      `json:"player_id"`
	X        float32     `json:"x,omitempty"`
	Y        float32     `json:"y,omitempty"`
	Z        float32     `json:"z,omitempty"`
}

// Server state
type GameServer struct {
	session *boulder.NetworkSession
	players map[boulder.ConnectionHandle]uint64
	nextID  uint64
}

func NewGameServer(engine *boulder.Engine, port uint16) (*GameServer, error) {
	session, err := boulder.NewNetworkSession(engine)
	if err != nil {
		return nil, err
	}

	if err := session.StartServer(port); err != nil {
		session.Destroy()
		return nil, err
	}

	return &GameServer{
		session: session,
		players: make(map[boulder.ConnectionHandle]uint64),
		nextID:  1,
	}, nil
}

func (gs *GameServer) Update() {
	gs.session.Update()

	for {
		event := gs.session.PollEvent()
		if event == nil {
			break
		}

		switch e := event.(type) {
		case boulder.ConnectedEvent:
			playerID := gs.nextID
			gs.nextID++
			gs.players[e.Connection] = playerID

			// Send welcome message
			msg := GameMessage{
				Type:     MsgPlayerJoin,
				PlayerID: playerID,
			}
			data, _ := json.Marshal(msg)
			gs.session.SendReliable(e.Connection, data)

			boulder.LogInfo(fmt.Sprintf("[SERVER] Player %d connected", playerID))

		case boulder.DisconnectedEvent:
			if playerID, exists := gs.players[e.Connection]; exists {
				boulder.LogInfo(fmt.Sprintf("[SERVER] Player %d disconnected", playerID))
				delete(gs.players, e.Connection)
			}

		case boulder.MessageEvent:
			var msg GameMessage
			if err := json.Unmarshal(e.Data, &msg); err == nil {
				switch msg.Type {
				case MsgPlayerMove:
					boulder.LogInfo(fmt.Sprintf("[SERVER] Player %d moved to (%.1f, %.1f, %.1f)",
						msg.PlayerID, msg.X, msg.Y, msg.Z))
					// Broadcast to all other players
					for conn := range gs.players {
						if conn != e.Connection {
							gs.session.SendReliable(conn, e.Data)
						}
					}
				}
			}
		}
	}
}

func (gs *GameServer) Shutdown() {
	gs.session.StopServer()
	gs.session.Destroy()
}

// Client state
type GameClient struct {
	session  *boulder.NetworkSession
	conn     boulder.ConnectionHandle
	playerID uint64
}

func NewGameClient(engine *boulder.Engine, serverAddr string, port uint16) (*GameClient, error) {
	session, err := boulder.NewNetworkSession(engine)
	if err != nil {
		return nil, err
	}

	conn, err := session.Connect(serverAddr, port)
	if err != nil {
		session.Destroy()
		return nil, err
	}

	return &GameClient{
		session: session,
		conn:    conn,
	}, nil
}

func (gc *GameClient) Update() bool {
	gc.session.Update()

	for {
		event := gc.session.PollEvent()
		if event == nil {
			break
		}

		switch e := event.(type) {
		case boulder.ConnectedEvent:
			boulder.LogInfo("[CLIENT] Connected to server!")
			return true

		case boulder.DisconnectedEvent:
			boulder.LogInfo("[CLIENT] Disconnected from server")
			return false

		case boulder.MessageEvent:
			var msg GameMessage
			if err := json.Unmarshal(e.Data, &msg); err == nil {
				switch msg.Type {
				case MsgPlayerJoin:
					gc.playerID = msg.PlayerID
					boulder.LogInfo(fmt.Sprintf("[CLIENT] Assigned player ID: %d", gc.playerID))

				case MsgPlayerMove:
					boulder.LogInfo(fmt.Sprintf("[CLIENT] Player %d moved to (%.1f, %.1f, %.1f)",
						msg.PlayerID, msg.X, msg.Y, msg.Z))
				}
			}
		}
	}
	return false
}

func (gc *GameClient) SendMove(x, y, z float32) {
	msg := GameMessage{
		Type:     MsgPlayerMove,
		PlayerID: gc.playerID,
		X:        x,
		Y:        y,
		Z:        z,
	}
	data, _ := json.Marshal(msg)
	gc.session.SendReliable(gc.conn, data)
}

func (gc *GameClient) Shutdown() {
	gc.session.Disconnect(gc.conn)
	gc.session.Destroy()
}

func mp_demo() {
	boulder.LogInfo("=== Multiplayer Game Demo ===")

	// Initialize engine
	engine := boulder.NewEngine("Multiplayer Game", vkMakeVersion(1, 0, 0))
	if err := engine.Init(); err != nil {
		log.Fatalf("Failed to initialize engine: %v", err)
	}
	defer engine.Shutdown()

	// Start server in background
	server, err := NewGameServer(engine, 27015)
	if err != nil {
		log.Fatalf("Failed to start server: %v", err)
	}
	defer server.Shutdown()

	boulder.LogInfo("[SERVER] Listening on port 27015")

	// Wait a bit for server to be ready
	time.Sleep(200 * time.Millisecond)

	// Create two clients
	client1, err := NewGameClient(engine, "127.0.0.1", 27015)
	if err != nil {
		log.Fatalf("Failed to create client 1: %v", err)
	}
	defer client1.Shutdown()

	client2, err := NewGameClient(engine, "127.0.0.1", 27015)
	if err != nil {
		log.Fatalf("Failed to create client 2: %v", err)
	}
	defer client2.Shutdown()

	// Wait for connections
	connected1, connected2 := false, false
	for i := 0; i < 50 && (!connected1 || !connected2); i++ {
		server.Update()
		if !connected1 && client1.Update() {
			connected1 = true
		}
		if !connected2 && client2.Update() {
			connected2 = true
		}
		time.Sleep(100 * time.Millisecond)
	}

	if !connected1 || !connected2 {
		log.Fatal("Failed to connect clients")
	}

	boulder.LogInfo("\n=== Both players connected! ===\n")
	time.Sleep(500 * time.Millisecond)

	// Simulate gameplay
	moves := []struct {
		client  *GameClient
		name    string
		x, y, z float32
	}{
		{client1, "Player 1", 10, 0, 5},
		{client2, "Player 2", -5, 0, 8},
		{client1, "Player 1", 15, 2, 10},
		{client2, "Player 2", -3, 1, 12},
	}

	for _, move := range moves {
		boulder.LogInfo(fmt.Sprintf("\n--- %s moving to (%.1f, %.1f, %.1f) ---",
			move.name, move.x, move.y, move.z))
		move.client.SendMove(move.x, move.y, move.z)

		// Process updates
		for i := 0; i < 10; i++ {
			server.Update()
			client1.Update()
			client2.Update()
			time.Sleep(100 * time.Millisecond)
		}
	}

	boulder.LogInfo("\n=== Game demo complete! ===")
}
