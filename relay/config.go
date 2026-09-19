package main

import (
	"os"
	"strings"
)

type Config struct {
	ListenAddr   string
	JWTSecret    string
	DeviceTokens []string
	LogLevel     string
	StreamUser   string
	StreamPass   string
}

type RelayServer struct {
	config   Config
	sessions *SessionManager
}

func LoadConfig() Config {
	cfg := Config{
		ListenAddr: ":8300",
		LogLevel:   "info",
	}
	if v := os.Getenv("RELAY_LISTEN"); v != "" {
		cfg.ListenAddr = v
	}
	if v := os.Getenv("RELAY_JWT_SECRET"); v != "" {
		cfg.JWTSecret = v
	}
	if v := os.Getenv("RELAY_DEVICE_TOKENS"); v != "" {
		for _, t := range strings.Split(v, ",") {
			t = strings.TrimSpace(t)
			if t != "" {
				cfg.DeviceTokens = append(cfg.DeviceTokens, t)
			}
		}
	}
	if v := os.Getenv("RELAY_LOG_LEVEL"); v != "" {
		cfg.LogLevel = v
	}
	if v := os.Getenv("RELAY_STREAM_USER"); v != "" {
		cfg.StreamUser = v
	}
	if v := os.Getenv("RELAY_STREAM_PASS"); v != "" {
		cfg.StreamPass = v
	}
	return cfg
}
