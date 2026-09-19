package main

import (
	"fmt"
	"log"
	"net/http"
	"time"
)

const streamBoundary = "leafcamera1234567890"

func (s *RelayServer) HandleStream(w http.ResponseWriter, r *http.Request) {
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

	session.mu.Lock()
	session.viewerCount++
	firstViewer := session.viewerCount == 1
	session.mu.Unlock()
	if firstViewer {
		if session.SendCmd(`{"cmd":"start_stream"}`) {
			log.Printf("[STREAM] Requested start_stream for %s", payload.DeviceID)
		}
	}
	defer func() {
		session.mu.Lock()
		session.viewerCount--
		lastViewer := session.viewerCount == 0
		session.mu.Unlock()
		if lastViewer {
			if session.SendCmd(`{"cmd":"stop_stream"}`) {
				log.Printf("[STREAM] Requested stop_stream for %s", payload.DeviceID)
			}
		}
	}()

	ctx := r.Context()
	w.Header().Set("Content-Type", "multipart/x-mixed-replace;boundary="+streamBoundary)
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Pragma", "no-cache")
	w.Header().Set("X-Framerate", "30")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}

	log.Printf("[STREAM] Browser connected for %s", payload.DeviceID)
	defer log.Printf("[STREAM] Browser disconnected from %s", payload.DeviceID)

	var lastSent time.Time
	// Drop-on-backlog delivery policy:
	//  1. The relay keeps ONLY the single freshest frame per session
	//     (CameraSession.SetFrame overwrites, never appends — no queue).
	//  2. This loop re-reads that latest frame and skips writing anything when
	//     the camera hasn't produced a new one (stale re-broadcast = wasted
	//     bandwidth + a frozen-looking picture = perceived latency).
	//  3. No buffering exists anywhere in the chain: if a viewer's socket is
	//     slow, w.Write/TCP backpressure stalls only that goroutine while
	//     newer frames keep replacing the shared buffer — the fast consumer
	//     still gets the freshest frame every iteration, never a backlog.
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		if session.FrameAge() > 5*time.Second {
			time.Sleep(100 * time.Millisecond)
			continue
		}
		frame, ft, ok := session.GetFrameWithTime()
		if !ok {
			time.Sleep(50 * time.Millisecond)
			continue
		}
		// Push only genuinely new frames. Resending the cached frame when the
		// camera has nothing new both wastes the uplink and re-displays a
		// stale picture; skipping it lets the browser run at the camera's own
		// cadence (~30 FPS) with no added latency.
		if ft.Equal(lastSent) {
			time.Sleep(10 * time.Millisecond)
			continue
		}
		lastSent = ft
		header := fmt.Sprintf("\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
			streamBoundary, len(frame))
		if _, err := fmt.Fprint(w, header); err != nil {
			return
		}
		if _, err := w.Write(frame); err != nil {
			return
		}
		flusher.Flush()
	}
}
