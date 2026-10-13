#pragma once
#define WALKIE_WIFI_SSID "YOUR_WIFI_SSID"
#define WALKIE_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
// Host or IP only, without ws://. This initial LAN build uses plain WebSocket.
#define WALKIE_RELAY_HOST "192.168.1.100"
#define WALKIE_RELAY_PORT 8765
#define WALKIE_RELAY_PATH "/"
#define WALKIE_DEVICE_ID "esp32-a"
#define WALKIE_PEER_ID "laptop-b"
// Match the microphone L/R strap and datasheet, if different from defaults.
#define WALKIE_MIC_SLOT 0
#define WALKIE_MIC_SHIFT 16
#define WALKIE_MIC_GAIN 1
// Setup AP password defaults to "dolphin1". Optional override (8-63 characters).
// #define WALKIE_SETUP_PASSWORD "your-setup-password"
