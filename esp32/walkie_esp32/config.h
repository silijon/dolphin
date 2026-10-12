#pragma once

// Copy config.local.example.h to config.local.h to keep credentials out of git.
#if __has_include("config.local.h")
#include "config.local.h"
#else
#define WALKIE_WIFI_SSID "YOUR_WIFI_SSID"
#define WALKIE_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define WALKIE_RELAY_HOST "192.168.1.100"
#define WALKIE_RELAY_PORT 8765
#define WALKIE_RELAY_PATH "/"
#define WALKIE_DEVICE_ID "esp32-a"
#define WALKIE_PEER_ID "laptop-b"
#endif

#define MIC_BCLK  5
#define MIC_LRCLK 6
#define MIC_DATA  9
#define AMP_BCLK  10
#define AMP_LRCLK 11
#define AMP_DATA  12
#define BUTTON_PIN 13
#define LED_PIN 18

// Common Philips I2S microphones: signed 24-bit, MSB-aligned in 32-bit slots.
// We read both slots (64 BCLKs per sample), then select left (0) or right (1).
#ifndef WALKIE_MIC_SLOT
#define WALKIE_MIC_SLOT 0
#endif
#ifndef WALKIE_MIC_SHIFT
#define WALKIE_MIC_SHIFT 16
#endif
#ifndef WALKIE_MIC_GAIN
#define WALKIE_MIC_GAIN 1
#endif
