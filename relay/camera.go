package main

import (
	"encoding/binary"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	ReadBufferSize:  32 * 1024,
	WriteBufferSize: 64 * 1024,
	CheckOrigin:     func(r *http.Request) bool { return true },
}

type authMessage struct {
	Token    string `json:"token"`
	DeviceID string `json:"device_id"`
}

func (s *RelayServer) HandleCameraWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("[CAM] WebSocket upgrade failed: %v", err)
		return
	}
	defer conn.Close()
	conn.SetReadLimit(2 * 1024 * 1024)
	conn.SetReadDeadline(time.Now().Add(45 * time.Second))
	conn.SetPongHandler(func(string) error {
		conn.SetReadDeadline(time.Now().Add(45 * time.Second))
		return nil
	})

	authenticated := false
	var session *CameraSession
	var deviceID string

	defer func() {
		if session != nil {
			session.DetachConn(conn)
			session.ClearFrame()
			log.Printf("[CAM] Camera disconnected: %s", deviceID)
		}
	}()

	pingStop := make(chan struct{})
	defer close(pingStop)
	go func() {
		ticker := time.NewTicker(15 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-pingStop:
				return
			case <-ticker.C:
			}
			// WriteControl is safe to call concurrently with other writers,
			// unlike WriteMessage (which must hold connMu via SendCmd).
			if err := conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(5*time.Second)); err != nil {
				return
			}
		}
	}()

	for {
		msgType, message, err := conn.ReadMessage()
		if err != nil {
			if err != io.EOF {
				log.Printf("[CAM] Read error: %v", err)
			}
			return
		}

		if !authenticated {
			var auth authMessage
			if err := json.Unmarshal(message, &auth); err != nil {
				log.Printf("[CAM] First message is not auth JSON: %v", err)
				conn.WriteMessage(websocket.CloseMessage,
					websocket.FormatCloseMessage(websocket.ClosePolicyViolation, "expected auth"))
				return
			}
			if !ValidateDeviceToken(auth.Token, s.config.DeviceTokens) {
				log.Printf("[CAM] Invalid device token from %s", r.RemoteAddr)
				conn.WriteMessage(websocket.CloseMessage,
					websocket.FormatCloseMessage(websocket.ClosePolicyViolation, "invalid token"))
				return
			}
			deviceID = auth.DeviceID
			if deviceID == "" {
				deviceID = "unknown"
			}
			session = s.sessions.GetOrCreate(deviceID)
			session.AttachConn(conn)
			authenticated = true
			log.Printf("[CAM] Camera authenticated: %s from %s", deviceID, r.RemoteAddr)
			// Only start streaming when somebody is actually watching. This is
			// what stopped the ESP32 from pushing full-rate frames 24/7 with
			// zero viewers — the churn that made live view "take too long".
			// The next start/stop is driven by viewer connect/disconnect in
			// stream.go / audio.go.
			session.mu.RLock()
			hasViewers := session.viewerCount > 0
			session.mu.RUnlock()
			if hasViewers {
				session.SendCmd(`{"cmd":"start_stream"}`)
			}
			continue
		}

		if msgType == websocket.BinaryMessage && len(message) > 5 {
			msgTypeByte := message[0]
			length := uint32(message[1])<<24 | uint32(message[2])<<16 |
				uint32(message[3])<<8 | uint32(message[4])
			if int(length)+5 > len(message) {
				continue
			}
			payload := message[5 : 5+length]
			switch msgTypeByte {
			case 0x01:
				session.SetFrame(payload)
			case 0x02:
				samples := make([]int16, len(payload)/2)
				for i := range samples {
					// ESP32 int16_t PCM is little-endian on the wire.
					samples[i] = int16(binary.LittleEndian.Uint16(payload[i*2:]))
				}
				session.WriteAudio(samples)
			}
		}
	}
}

func (s *RelayServer) HandleHealth(w http.ResponseWriter, r *http.Request) {
	deviceID := r.URL.Query().Get("device_id")
	w.Header().Set("Content-Type", "application/json")
	if deviceID != "" {
		connected := s.sessions.IsConnected(deviceID)
		json.NewEncoder(w).Encode(map[string]interface{}{
			"status":   "ok",
			"camera":   deviceID,
			"connected": connected,
		})
		return
	}
	json.NewEncoder(w).Encode(map[string]interface{}{
		"status":  "ok",
		"cameras": s.sessions.Count(),
	})
}
