package boulder

// #cgo CFLAGS: -I..
// #cgo LDFLAGS: -L../build -lboulder_shared -Wl,-rpath,${SRCDIR}/../build
// #include <stdlib.h>
// #include "../boulder_cgo.h"
import "C"
import (
	"errors"
	"unsafe"
)

// ConnectionHandle uniquely identifies a network connection
type ConnectionHandle uint64

// NetworkEventType represents the type of network event
type NetworkEventType int

const (
	NetworkEventNone         NetworkEventType = 0
	NetworkEventConnected    NetworkEventType = 1
	NetworkEventDisconnected NetworkEventType = 2
	NetworkEventMessage      NetworkEventType = 3
)

// NetworkEvent represents a network event
type NetworkEvent interface {
	Type() NetworkEventType
}

// ConnectedEvent is fired when a connection is established
type ConnectedEvent struct {
	Connection ConnectionHandle
}

func (e ConnectedEvent) Type() NetworkEventType { return NetworkEventConnected }

// DisconnectedEvent is fired when a connection is closed
type DisconnectedEvent struct {
	Connection ConnectionHandle
}

func (e DisconnectedEvent) Type() NetworkEventType { return NetworkEventDisconnected }

// MessageEvent is fired when a message is received
type MessageEvent struct {
	Connection ConnectionHandle
	Data       []byte
}

func (e MessageEvent) Type() NetworkEventType { return NetworkEventMessage }

// ConnectionState represents the state of a connection
type ConnectionState int

const (
	ConnectionStateNone                  ConnectionState = 0
	ConnectionStateConnecting            ConnectionState = 1
	ConnectionStateFindingRoute          ConnectionState = 2
	ConnectionStateConnected             ConnectionState = 3
	ConnectionStateClosedByPeer          ConnectionState = 4
	ConnectionStateProblemDetectedLocally ConnectionState = 5
)

// SendFlags for network messages
const (
	SendUnreliable = 0
	SendReliable   = 1
)

// NetworkSession manages network connections (client or server)
type NetworkSession struct {
	handle C.NetworkSession
	engine *Engine
}

// NewNetworkSession creates a new network session
func NewNetworkSession(engine *Engine) (*NetworkSession, error) {
	if !engine.initialized {
		return nil, errors.New("engine not initialized")
	}

	handle := C.boulder_create_network_session()
	if handle == nil {
		return nil, errors.New("failed to create network session")
	}

	return &NetworkSession{
		handle: handle,
		engine: engine,
	}, nil
}

// Destroy cleans up the network session
func (ns *NetworkSession) Destroy() {
	if ns.handle != nil {
		C.boulder_destroy_network_session(ns.handle)
		ns.handle = nil
	}
}

// Update processes network callbacks (call this every frame)
func (ns *NetworkSession) Update() {
	if ns.handle != nil {
		C.boulder_network_update(ns.handle)
	}
}

// StartServer starts listening for connections on the specified port
func (ns *NetworkSession) StartServer(port uint16) error {
	if ns.handle == nil {
		return errors.New("session not initialized")
	}

	result := C.boulder_start_server(ns.handle, C.uint16_t(port))
	if result != 0 {
		return errors.New("failed to start server")
	}

	return nil
}

// StopServer stops the server
func (ns *NetworkSession) StopServer() {
	if ns.handle != nil {
		C.boulder_stop_server(ns.handle)
	}
}

// Connect initiates a connection to a remote address
func (ns *NetworkSession) Connect(address string, port uint16) (ConnectionHandle, error) {
	if ns.handle == nil {
		return 0, errors.New("session not initialized")
	}

	cAddr := C.CString(address)
	defer C.free(unsafe.Pointer(cAddr))

	handle := C.boulder_connect(ns.handle, cAddr, C.uint16_t(port))
	if handle == 0 {
		return 0, errors.New("failed to connect")
	}

	return ConnectionHandle(handle), nil
}

// Disconnect closes a connection
func (ns *NetworkSession) Disconnect(conn ConnectionHandle) {
	if ns.handle != nil {
		C.boulder_disconnect(ns.handle, C.ConnectionHandle(conn))
	}
}

// ConnectionState returns the current state of a connection
func (ns *NetworkSession) ConnectionState(conn ConnectionHandle) ConnectionState {
	if ns.handle == nil {
		return ConnectionStateNone
	}

	state := C.boulder_connection_state(ns.handle, C.ConnectionHandle(conn))
	return ConnectionState(state)
}

// SendMessage sends data to a connection
func (ns *NetworkSession) SendMessage(conn ConnectionHandle, data []byte, reliable bool) error {
	if ns.handle == nil {
		return errors.New("session not initialized")
	}

	if len(data) == 0 {
		return errors.New("empty data")
	}

	flags := SendUnreliable
	if reliable {
		flags = SendReliable
	}

	result := C.boulder_send_message(
		ns.handle,
		C.ConnectionHandle(conn),
		unsafe.Pointer(&data[0]),
		C.uint32_t(len(data)),
		C.int(flags),
	)

	if result != 0 {
		return errors.New("failed to send message")
	}

	return nil
}

// SendReliable sends data reliably (guaranteed delivery, ordered)
func (ns *NetworkSession) SendReliable(conn ConnectionHandle, data []byte) error {
	return ns.SendMessage(conn, data, true)
}

// SendUnreliable sends data unreliably (faster, no guarantees)
func (ns *NetworkSession) SendUnreliable(conn ConnectionHandle, data []byte) error {
	return ns.SendMessage(conn, data, false)
}

// PollEvent retrieves the next network event (non-blocking)
func (ns *NetworkSession) PollEvent() NetworkEvent {
	if ns.handle == nil {
		return nil
	}

	var event C.NetworkEvent
	result := C.boulder_poll_network_event(ns.handle, &event)

	if result == 0 || event._type == 0 {
		return nil
	}

	switch NetworkEventType(event._type) {
	case NetworkEventConnected:
		return ConnectedEvent{
			Connection: ConnectionHandle(event.connection),
		}

	case NetworkEventDisconnected:
		return DisconnectedEvent{
			Connection: ConnectionHandle(event.connection),
		}

	case NetworkEventMessage:
		// Copy the data
		data := C.GoBytes(unsafe.Pointer(event.data), C.int(event.dataSize))
		// Free the C-allocated data
		C.boulder_free_network_event_data(unsafe.Pointer(event.data))

		return MessageEvent{
			Connection: ConnectionHandle(event.connection),
			Data:       data,
		}

	default:
		return nil
	}
}

// PollEvents retrieves all pending network events
func (ns *NetworkSession) PollEvents() []NetworkEvent {
	events := make([]NetworkEvent, 0, 16)

	for {
		event := ns.PollEvent()
		if event == nil {
			break
		}
		events = append(events, event)
	}

	return events
}
