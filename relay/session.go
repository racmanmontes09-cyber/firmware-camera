package main

import (
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const audioRingSize = 16384

type CameraSession struct {
	DeviceID   string
	LastFrame  []byte
	FrameTime  time.Time
	AudioBuf   []int16
	AudioWrite int
	AudioCount int
	mu         sync.RWMutex

	viewerCount int

	// connMu serializes ALL outbound writes on the camera WebSocket
	// (start/stop commands from viewer handlers + auth handler). Gorilla's
	// WriteMessage is not safe for concurrent writers, so every non-control
	// write must hold this mutex. It also guards the single live conn pointer.
	connMu sync.Mutex
	conn   *websocket.Conn
}

func NewCameraSession(deviceID string) *CameraSession {
	return &CameraSession{
		DeviceID: deviceID,
		AudioBuf: make([]int16, audioRingSize),
	}
}

func (s *CameraSession) SetFrame(data []byte) {
	// Drop-on-backlog ingest: overwrite, never append. The relay deliberately
	// holds ONE frame per session, so a camera that is producing faster than a
	// given viewer consumes never grows a queue behind it — the next GetFrame
	// simply sees the newest JPEG and old ones are discarded. Zero latency
	// inflation under load, at the cost of no catch-up playback (by design).
	s.mu.Lock()
	s.LastFrame = data
	s.FrameTime = time.Now()
	s.mu.Unlock()
}

// GetFrameWithTime returns a copy of the newest JPEG frame together with the
// instant the camera produced it, atomically. Viewers use the timestamp to
// push only genuinely new frames — duplicating the cached frame stutters the
// image and wastes the uplink, while its absence lets viewers stream at the
// camera's own cadence with no added latency.
func (s *CameraSession) GetFrameWithTime() ([]byte, time.Time, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.LastFrame == nil {
		return nil, time.Time{}, false
	}
	out := make([]byte, len(s.LastFrame))
	copy(out, s.LastFrame)
	return out, s.FrameTime, true
}

func (s *CameraSession) FrameAge() time.Duration {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.FrameTime.IsZero() {
		return time.Hour
	}
	return time.Since(s.FrameTime)
}

// AttachConn binds the live camera WebSocket so handler goroutines can send
// control commands to it. There is exactly one live connection per session:
// if a previous uplink is still attached (the ESP reconnects before the old
// socket has been reaped), it is force-closed so frames never interleave.
func (s *CameraSession) AttachConn(c *websocket.Conn) {
	s.connMu.Lock()
	if s.conn != nil && s.conn != c {
		old := s.conn
		s.conn = nil
		old.Close()
	}
	s.conn = c
	s.connMu.Unlock()
}

// DetachConn clears the attached connection, but only if it is still the one
// this handler owns — a reaped/replaced uplink must not clobber the live one.
func (s *CameraSession) DetachConn(c *websocket.Conn) {
	s.connMu.Lock()
	if s.conn == c {
		s.conn = nil
	}
	s.connMu.Unlock()
}

func (s *CameraSession) IsAttached() bool {
	s.connMu.Lock()
	defer s.connMu.Unlock()
	return s.conn != nil
}

// SendCmd writes a JSON control command (start_stream / stop_stream) to the
// camera. Returns false if the camera is not currently attached, which is
// fine: on the next camera connect the auth handler re-evaluates viewerCount.
func (s *CameraSession) SendCmd(cmd string) bool {
	s.connMu.Lock()
	defer s.connMu.Unlock()
	if s.conn == nil {
		return false
	}
	s.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	if err := s.conn.WriteMessage(websocket.TextMessage, []byte(cmd)); err != nil {
		return false
	}
	return true
}

// ClearFrame drops the cached frame so browsers stop being served a stale
// image after the camera disconnects.
func (s *CameraSession) ClearFrame() {
	s.mu.Lock()
	s.LastFrame = nil
	s.mu.Unlock()
}

func (s *CameraSession) WriteAudio(samples []int16) {
	s.mu.Lock()
	for _, sample := range samples {
		s.AudioBuf[s.AudioWrite] = sample
		s.AudioWrite = (s.AudioWrite + 1) % audioRingSize
		if s.AudioCount < audioRingSize {
			s.AudioCount++
		}
	}
	s.mu.Unlock()
}

func (s *CameraSession) ReadAudio(readIdx int, buf []int16) (int, int) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.AudioCount == 0 {
		return 0, readIdx
	}
	writeIdx := s.AudioWrite
	count := 0
	for count < len(buf) && readIdx != writeIdx {
		buf[count] = s.AudioBuf[readIdx]
		readIdx = (readIdx + 1) % audioRingSize
		count++
	}
	if readIdx == writeIdx && s.AudioCount >= audioRingSize {
		readIdx = writeIdx
	}
	return count, readIdx
}

type SessionManager struct {
	sessions map[string]*CameraSession
	mu       sync.RWMutex
}

func NewSessionManager() *SessionManager {
	return &SessionManager{
		sessions: make(map[string]*CameraSession),
	}
}

func (m *SessionManager) GetOrCreate(deviceID string) *CameraSession {
	m.mu.Lock()
	defer m.mu.Unlock()
	if s, ok := m.sessions[deviceID]; ok {
		return s
	}
	s := NewCameraSession(deviceID)
	m.sessions[deviceID] = s
	return s
}

func (m *SessionManager) Remove(deviceID string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.sessions, deviceID)
}

func (m *SessionManager) IsConnected(deviceID string) bool {
	m.mu.RLock()
	s, ok := m.sessions[deviceID]
	m.mu.RUnlock()
	if !ok {
		return false
	}
	return s.IsAttached()
}

func (m *SessionManager) Count() int {
	m.mu.RLock()
	defer m.mu.RUnlock()
	return len(m.sessions)
}
