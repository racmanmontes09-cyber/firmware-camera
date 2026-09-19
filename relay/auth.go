package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"
)

type jwtPayload struct {
	UserID   int    `json:"user_id"`
	DeviceID string `json:"device_id"`
	Iat      int64  `json:"iat"`
	Exp      int64  `json:"exp"`
}

func ValidateBrowserToken(tokenStr string, secret string) (*jwtPayload, error) {
	parts := strings.Split(tokenStr, ".")
	if len(parts) != 3 {
		return nil, fmt.Errorf("invalid token format")
	}
	signingInput := parts[0] + "." + parts[1]
	mac := hmac.New(sha256.New, []byte(secret))
	mac.Write([]byte(signingInput))
	expectedSig := mac.Sum(nil)
	decodedSig, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return nil, fmt.Errorf("invalid signature encoding")
	}
	if !hmac.Equal(decodedSig, expectedSig) {
		return nil, fmt.Errorf("invalid signature")
	}
	payloadBytes, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, fmt.Errorf("invalid payload encoding")
	}
	var payload jwtPayload
	if err := json.Unmarshal(payloadBytes, &payload); err != nil {
		return nil, fmt.Errorf("invalid payload JSON")
	}
	if time.Now().Unix() > payload.Exp {
		return nil, fmt.Errorf("token expired")
	}
	return &payload, nil
}

func ValidateDeviceToken(token string, validTokens []string) bool {
	for _, vt := range validTokens {
		if token == vt {
			return true
		}
	}
	return false
}

func TokenFromRequest(r *http.Request) string {
	if t := r.URL.Query().Get("token"); t != "" {
		return t
	}
	if t := r.Header.Get("Authorization"); strings.HasPrefix(t, "Bearer ") {
		return strings.TrimPrefix(t, "Bearer ")
	}
	return ""
}
