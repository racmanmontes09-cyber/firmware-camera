package main

import (
	"fmt"
	"net/http"
)

// HandleCredentials returns the MediaMTX playback credentials for the
// browser player. It uses the same browser JWT gate as /stream, so only
// authenticated dashboard viewers can read the (now WebRTC) camera feed.
func (s *RelayServer) HandleCredentials(w http.ResponseWriter, r *http.Request) {
	token := TokenFromRequest(r)
	if token == "" {
		http.Error(w, "missing token", http.StatusUnauthorized)
		return
	}
	if _, err := ValidateBrowserToken(token, s.config.JWTSecret); err != nil {
		http.Error(w, "invalid token: "+err.Error(), http.StatusUnauthorized)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	fmt.Fprintf(w, `{"user":%q,"pass":%q}`, s.config.StreamUser, s.config.StreamPass)
}