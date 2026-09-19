package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	config := LoadConfig()
	if config.JWTSecret == "" {
		log.Fatal("RELAY_JWT_SECRET is required")
	}
	if len(config.DeviceTokens) == 0 {
		log.Fatal("RELAY_DEVICE_TOKENS is required (comma-separated)")
	}

	server := &RelayServer{
		config:   config,
		sessions: NewSessionManager(),
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/health", server.HandleHealth)
	mux.HandleFunc("/stream", server.HandleStream)
	mux.HandleFunc("/audio", server.HandleAudio)
	mux.HandleFunc("/credentials", server.HandleCredentials)
	mux.HandleFunc("/", server.HandleCameraWS)

	srv := &http.Server{
		Addr:              config.ListenAddr,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
		// Read/Write/Idle stay 0: WebSocket, MJPEG and PCM streams are
		// long-lived and must not be cut off by server timeouts.
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	go func() {
		log.Printf("[RELAY] Listening on %s", config.ListenAddr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			log.Fatalf("[RELAY] Server error: %v", err)
		}
	}()

	<-ctx.Done()
	log.Println("[RELAY] Shutting down...")
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		log.Printf("[RELAY] Shutdown error: %v", err)
	}
	log.Println("[RELAY] Stopped")
}
