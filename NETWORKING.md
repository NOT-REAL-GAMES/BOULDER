# BOULDER Networking

GameNetworkingSockets integration for BOULDER engine, providing reliable UDP networking with NAT traversal support.

## Features

- **Valve's GameNetworkingSockets** (v1.4.1) - Battle-tested networking library used in Steam games
- **Client-Server Architecture** - Dedicated server with multiple client support
- **Reliable & Unreliable Messaging** - Choose between guaranteed delivery or low-latency unreliable
- **Connection Management** - Automatic connection tracking, state changes, and disconnect handling
- **Event-Driven API** - Poll-based event system for connections, disconnections, and messages
- **Thread-Safe** - Safe for concurrent access across goroutines

## Quick Start

### Server

```go
import boulder "github.com/NOT-REAL-GAMES/BOULDER/go-bindings"

// Create network session
session, err := boulder.NewNetworkSession(engine)
if err != nil {
    log.Fatal(err)
}
defer session.Destroy()

// Start listening
if err := session.StartServer(27015); err != nil {
    log.Fatal(err)
}
defer session.StopServer()

// Game loop
for {
    session.Update()  // Process network callbacks

    // Handle events
    for {
        event := session.PollEvent()
        if event == nil {
            break
        }

        switch e := event.(type) {
        case boulder.ConnectedEvent:
            fmt.Printf("Client connected: %d\n", e.Connection)

        case boulder.MessageEvent:
            fmt.Printf("Received: %s\n", string(e.Data))
            // Echo back
            session.SendReliable(e.Connection, e.Data)

        case boulder.DisconnectedEvent:
            fmt.Printf("Client disconnected: %d\n", e.Connection)
        }
    }
}
```

### Client

```go
// Create session and connect
session, err := boulder.NewNetworkSession(engine)
if err != nil {
    log.Fatal(err)
}
defer session.Destroy()

conn, err := session.Connect("127.0.0.1", 27015)
if err != nil {
    log.Fatal(err)
}
defer session.Disconnect(conn)

// Wait for connection
connected := false
for i := 0; i < 50 && !connected; i++ {
    session.Update()

    if event := session.PollEvent(); event != nil {
        if _, ok := event.(boulder.ConnectedEvent); ok {
            connected = true
            fmt.Println("Connected!")
        }
    }
    time.Sleep(100 * time.Millisecond)
}

// Send message
session.SendReliable(conn, []byte("Hello Server!"))
```

## API Reference

### NetworkSession

```go
// Create a new network session
func NewNetworkSession(engine *Engine) (*NetworkSession, error)

// Server operations
func (ns *NetworkSession) StartServer(port uint16) error
func (ns *NetworkSession) StopServer()

// Client operations
func (ns *NetworkSession) Connect(address string, port uint16) (ConnectionHandle, error)
func (ns *NetworkSession) Disconnect(conn ConnectionHandle)
func (ns *NetworkSession) ConnectionState(conn ConnectionHandle) ConnectionState

// Messaging
func (ns *NetworkSession) SendReliable(conn ConnectionHandle, data []byte) error
func (ns *NetworkSession) SendUnreliable(conn ConnectionHandle, data []byte) error

// Event handling
func (ns *NetworkSession) Update()              // Call every frame
func (ns *NetworkSession) PollEvent() NetworkEvent
func (ns *NetworkSession) PollEvents() []NetworkEvent

// Cleanup
func (ns *NetworkSession) Destroy()
```

### Events

```go
type ConnectedEvent struct {
    Connection ConnectionHandle
}

type DisconnectedEvent struct {
    Connection ConnectionHandle
}

type MessageEvent struct {
    Connection ConnectionHandle
    Data       []byte
}
```

### Connection States

```go
const (
    ConnectionStateNone                  ConnectionState = 0
    ConnectionStateConnecting            ConnectionState = 1
    ConnectionStateFindingRoute          ConnectionState = 2
    ConnectionStateConnected             ConnectionState = 3
    ConnectionStateClosedByPeer          ConnectionState = 4
    ConnectionStateProblemDetectedLocally ConnectionState = 5
)
```

## Examples

See `go-bindings/examples/`:
- **network_simple.go** - Basic client-server communication
- **multiplayer_game.go** - Multiplayer game with player movement synchronization
- **network_demo.go** - Advanced example with multiple message types

## Architecture

### C++ Backend
- `boulder_cgo.cpp` - GameNetworkingSockets wrapper implementation
- `boulder_cgo.h` - C API definitions
- Global callback system with session tracking
- Reference-counted GNS initialization

### Go Bindings
- `go-bindings/network.go` - Idiomatic Go API
- CGO bindings to C++ backend
- Type-safe event system with interfaces

### Threading Model
- Network callbacks execute on GNS internal thread
- Events are queued thread-safely
- Call `Update()` on your main thread to dispatch callbacks
- `PollEvent()` is thread-safe and can be called from any goroutine

## Performance

- **Reliable Messages**: TCP-like reliability with better performance
- **Unreliable Messages**: For position updates, non-critical data
- **Latency**: Sub-millisecond on localhost, optimized for real-time games
- **Bandwidth**: Automatic packet coalescing and compression

## Best Practices

1. **Call Update() Every Frame**: Process network callbacks regularly
   ```go
   for !window.ShouldClose() {
       session.Update()  // Process network first
       // ... game logic ...
   }
   ```

2. **Use Reliable for Critical Data**: Player actions, chat messages
   ```go
   session.SendReliable(conn, criticalData)
   ```

3. **Use Unreliable for Frequent Updates**: Position, rotation
   ```go
   session.SendUnreliable(conn, positionUpdate)
   ```

4. **Handle All Events**: Don't skip disconnection events
   ```go
   for {
       event := session.PollEvent()
       if event == nil { break }
       // Handle all event types
   }
   ```

5. **Proper Cleanup**: Always defer session destruction
   ```go
   defer session.Destroy()
   ```

## Troubleshooting

### Connection Timeouts
- Ensure `Update()` is called regularly on both client and server
- Check firewall settings (UDP port 27015)
- Verify server is listening before client connects

### Missed Messages
- Use `SendReliable()` for important messages
- Check that you're polling all events in a loop

### Deadlocks
- Don't hold locks while calling network functions
- The implementation handles internal synchronization

## Future Enhancements

- [ ] NAT punchthrough / relay servers
- [ ] Bandwidth monitoring and throttling
- [ ] Connection quality metrics (ping, packet loss)
- [ ] Encryption support
- [ ] Multiple poll groups for connection prioritization

## Credits

Built on [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) by Valve Corporation.
