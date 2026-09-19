package main

import (
	"log"
	"net/http"
	"time"
)

func (s *RelayServer) HandleAudio(w http.ResponseWriter, r *http.Request) {
	token := TokenFromRequest(r)
	if token == "" {
		http.Error(w, "missing token", http.StatusUnauthorized)
		return
	}
	payload, err := ValidateBrowserToken(token, s.config.JWTSecret)
	if err != nil {
		http.Error(w, "invalid token: "+err.Error(), http.StatusUnauthorized)
		return
	}
	session := s.sessions.GetOrCreate(payload.DeviceID)

	// An audio-only browser still needs the camera to send its uplink, so it
	// participates in the viewer count that drives start/stop_stream.
	session.mu.Lock()
	session.viewerCount++
	firstViewer := session.viewerCount == 1
	session.mu.Unlock()
	if firstViewer {
		if session.SendCmd(`{"cmd":"start_stream"}`) {
			log.Printf("[AUDIO] Requested start_stream for %s", payload.DeviceID)
		}
	}
	defer func() {
		session.mu.Lock()
		session.viewerCount--
		lastViewer := session.viewerCount == 0
		session.mu.Unlock()
		if lastViewer {
			if session.SendCmd(`{"cmd":"stop_stream"}`) {
				log.Printf("[AUDIO] Requested stop_stream for %s", payload.DeviceID)
			}
		}
	}()

	ctx := r.Context()
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "no-cache, no-store")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}

	log.Printf("[AUDIO] Browser connected for %s", payload.DeviceID)
	defer log.Printf("[AUDIO] Browser disconnected from %s", payload.DeviceID)

	buf := make([]int16, 512)
	readIdx := 0
	silentCount := 0

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		count, newIdx := session.ReadAudio(readIdx, buf)
		readIdx = newIdx
		if count == 0 {
			silentCount++
			if silentCount > 300 {
				time.Sleep(100 * time.Millisecond)
			} else {
				time.Sleep(10 * time.Millisecond)
			}
			continue
		}
		silentCount = 0
		pcmBytes := make([]byte, count*2)
		for i := 0; i < count; i++ {
			pcmBytes[i*2] = byte(buf[i])
			pcmBytes[i*2+1] = byte(buf[i] >> 8)
		}
		if _, err := w.Write(pcmBytes); err != nil {
			return
		}
		flusher.Flush()
	}
}
