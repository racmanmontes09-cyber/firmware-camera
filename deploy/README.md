# Project L.E.A.F. Camera Relay — Deployment

This documents the minimum set of steps to run the camera relay on the
projectleaf.tech VPS and point the camera at it. The relay is a small Go
service; the camera opens an **outbound** WebSocket to it so the VPS never
needs to reach the camera's private LAN IP.

## Architecture

ESP32-CAM -> WSS /camera/relay (outbound) -> nginx :443 -> relay :8300
Browser   -> HTTPS /camera/stream, /camera/audio -> nginx -> relay -> ESP32-CAM

## Components

| Part                  | Where                              |
|-----------------------|------------------------------------|
| ESP32 RelayClient     | `include/RelayClient.h` (firmware) |
| Go relay service      | `relay/` (this repo)               |
| Nginx proxy           | `deploy/nginx/camera-relay.conf`   |
| systemd unit          | `deploy/leaf-camera-relay.service` |

## 1. Build the relay

Requires Go >= 1.21 on any machine.

```bash
cd relay
CGO_ENABLED=0 go mod tidy
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o leaf-camera-relay .
```

## 2. Deploy to the VPS

```bash
# binary + unit + env template
scp relay/leaf-camera-relay user@YOUR_VPS:/tmp/
scp deploy/leaf-camera-relay.service user@YOUR_VPS:/tmp/
scp deploy/leaf-camera-relay.env.example user@YOUR_VPS:/tmp/

ssh user@YOUR_VPS
  sudo mv /tmp/leaf-camera-relay /usr/local/bin/leaf-camera-relay
  sudo chmod 755 /usr/local/bin/leaf-camera-relay
  sudo mv /tmp/leaf-camera-relay.service /etc/systemd/system/

  # Create env file with real secrets (chmod 600)
  sudo cp /tmp/leaf-camera-relay.env.example /etc/leaf-camera-relay.env
  sudo chmod 600 /etc/leaf-camera-relay.env
  sudoedit /etc/leaf-camera-relay.env
    # RELAY_JWT_SECRET=<same as Laravel LEAF_CAMERA_RELAY_JWT_SECRET>
    # RELAY_DEVICE_TOKENS=<comma-separated camera device tokens>

  # Point the unit at the env file
  sudo sed -i 's|# EnvironmentFile=/etc/leaf-camera-relay.env|EnvironmentFile=/etc/leaf-camera-relay.env|' /etc/systemd/system/leaf-camera-relay.service

  sudo systemctl daemon-reload
  sudo systemctl enable --now leaf-camera-relay
  sudo systemctl status leaf-camera-relay
```

## 3. Nginx config

Copy `deploy/nginx/camera-relay.conf` location blocks into the projectleaf.tech
HTTPS `server {}` block, then reload:

```bash
sudo nginx -t
sudo systemctl reload nginx
```

## 4. Laravel (deployed backend)

Set env vars in the Laravel `.env` on the server:

```
LEAF_CAMERA_RELAY_URL=https://projectleaf.tech
LEAF_CAMERA_RELAY_JWT_SECRET=<same secret as RELAY_JWT_SECRET>
LEAF_CAMERA_RELAY_TOKEN_TTL=300
```

Then clear config cache:

```bash
php artisan config:clear && php artisan config:cache
```

## 5. Flash the camera

Set env vars or `platformio_override.ini` (gitignored) then build:

```bash
export LEAF_WIFI_SSID='...'
export LEAF_WIFI_PASSWORD='...'
export LEAF_CAMERA_DEVICE_ID='esp32-cam-001'
export LEAF_CAMERA_DEVICE_NAME='Greenhouse 1 Camera'
export LEAF_BACKEND_URL='https://projectleaf.tech'
export LEAF_CAMERA_DEVICE_TOKEN='leaf_...'
export LEAF_RELAY_HOST='projectleaf.tech'

pio run -t upload
```

## 6. Test

```bash
# relay health/connected state (from VPS)
curl http://127.0.0.1:8300/health

# from a browser: open the dashboard live view. Stream + audio flow
# through https://projectleaf.tech/camera/stream and /camera/audio

# direct relay test (token minted by Laravel /dashboard/camera/stream-url)
curl -N "http://127.0.0.1:8300/stream?token=<JWT>"
curl -N "http://127.0.0.1:8300/audio?token=<JWT>"
```

## Security notes

- Only the outbound camera WebSocket and nginx can reach the relay
  (bound to 127.0.0.1:8300).
- Camera auth: device token compared against `RELAY_DEVICE_TOKENS`.
- Browser auth: HMAC-SHA256 JWT issued by Laravel, validated by the relay
  with no DB lookup; expires after `LEAF_CAMERA_RELAY_TOKEN_TTL` seconds.
- Never commit `/etc/leaf-camera-relay.env` or `platformio_override.ini`.
