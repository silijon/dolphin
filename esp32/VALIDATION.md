# Validation performed

## Browser provisioning feature

- Built the complete client with ESP32 Arduino **3.3.12**, WebSockets **2.7.2**,
  the Reverse TFT Feather target, and hardware USB CDC enabled.
- Verified compilation with dummy local defaults and with **no `config.local.h`**
  (first-boot provisioning requires no compiled-in Wi-Fi credentials).
- Native settings validation tests and existing native audio/state tests pass
  with `g++ -std=c++17 -Wall -Wextra -Werror`.
- No new library dependencies or changes to the Python relay/protocol.
- The fixed AP password (`dolphin1`), DNS/captive-popup behavior, station/AP channel
  changes, NVS persistence, and connection-test/save flow need the hardware
  checks in the [setup guide](README.md#configure-from-a-phone-or-laptop).
  No physical board was flashed for this feature.

## Original client and startup diagnostics

- Board target: `esp32:esp32:adafruit_feather_esp32s3_reversetft`.
- ESP32 Arduino core: **3.3.12**; WebSockets library: **2.7.2**.
- Startup diagnostics also compiled with `USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default`
  (Hardware CDC and JTAG, CDC enabled). The diagnostic build linked at 1,083,343
  bytes of flash and 61,604 bytes of static RAM with the dummy configuration.
- Compiled and linked the sketch with a dummy, non-placeholder local
  configuration, so the complete Wi-Fi, WebSocket, JSON, I2S, and task paths
  are included. No real credentials were needed or stored. No board was flashed.
- Production C++ audio/state code passed the host tests using
  `g++ -std=c++17 -Wall -Wextra -Werror`. Address/undefined sanitizer execution
  was unavailable because the host's AddressSanitizer runtime was missing.
- Existing Python suite: **30 passed**, including actual WebSocket relay
  forwarding, floor arbitration, disconnect/rejoin, and client reconnect tests.
  Python runtime sources and the wire protocol are unchanged.
- `git diff --check` passed.

These checks establish compilation, state/conversion behavior, and continued
Python behavior. They do **not** establish an ESP32-to-laptop listening result.
Microphone slot/alignment, speaker output, task timing on hardware, and acoustic
latency still need the [breadboard test](README.md#interoperability-listening-test).
