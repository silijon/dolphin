# ESP32 Arduino client

Initial native client for the **Adafruit ESP32-S3 Reverse TFT Feather**. The
existing Python relay and laptop client are unchanged. Read the
[implementation inspection](PROTOCOL_INSPECTION.md) and existing
[protocol specification](../PROTOCOL.md) for the exact wire contract.

## Arduino IDE setup

1. Install/select **esp32 by Espressif Systems**, version **3.x** (compiled here
   with **3.3.12**), and select **Adafruit Feather ESP32-S3 Reverse TFT**.
2. Install **WebSockets by Markus Sattler**, version **2.7.2**, from Library
   Manager. This is the only additional library. WiFi, FreeRTOS, the standard
   I2S driver, and cJSON come with the ESP32 core.
3. Open [walkie_esp32/walkie_esp32.ino](walkie_esp32/walkie_esp32.ino).
4. Copy `config.local.example.h` to `config.local.h` in the sketch folder and
   set SSID/password, relay LAN IP/hostname, port, path, and reciprocal IDs.
   This file is ignored by git. Defaults in `config.h` are placeholders.
5. Set **USB Mode → Hardware CDC and JTAG**, **USB CDC On Boot → Enabled**, and
   **Upload Mode → UART0 / Hardware CDC**. Select the board's port, compile, and
   upload. Open Serial Monitor at **115200 baud**. Firmware waits at most three
   seconds for a serial host, then boots independently.

`Hard resetting via RTS pin...` is the uploader's final reset message, not a
firmware log. After upload/reset, reselect the board's current port if necessary
and reopen Serial Monitor. Startup prints `BOOT walkie v1`, followed by I2S,
Wi-Fi, and task checkpoints. A fatal startup error repeats every two seconds
so it remains visible if the monitor attaches late. The TFT is not initialized,
and the external LED remains off until a peer connects and a PTT grant arrives.

Use a reachable host/IP, **without** `ws://`, in `WALKIE_RELAY_HOST`. This first
build uses plain `ws://` on the LAN, matching the supplied relay. It doesn't
configure WSS, automatic discovery, or a TFT interface. Credentials and IDs are
compile-time settings. The board needs a 2.4 GHz network reachable from the relay.

The microphone/amp part numbers and microphone L/R strap were not supplied.
Defaults assume a standard Philips I2S MEMS microphone with signed, MSB-aligned
24-bit data in a 32-bit slot, strapped for left, and an amplifier accepting
16-bit Philips I2S stereo. Check their datasheets if these defaults don't match.
`WALKIE_MIC_SLOT` selects 0=left or 1=right; both slots are captured with 64 BCLKs
per sample. `WALKIE_MIC_SHIFT=16` extracts the upper signed 16 bits;
`WALKIE_MIC_GAIN` applies integer gain with saturation, default 1. Adjust these
hardware conversion settings only; the wire stays PCM16LE/16 kHz/mono.

## Exact wiring

| Function | BCLK | LRCLK | Data | Other |
| --- | --- | --- | --- | --- |
| Microphone, I2S0 RX | GPIO 5 | GPIO 6 | DOUT → GPIO 9 | VIN → 3.3V; GND → GND |
| Amplifier, I2S1 TX | GPIO 10 | GPIO 11 | GPIO 12 → DIN | VIN → 3.3V; GND → GND; speaker ± → amplifier ± |
| PTT | | | GPIO 13 | Button connects to ground; `INPUT_PULLUP`; LOW=pressed |
| External button LED | | | GPIO 18 (A0) | Positive → GPIO18; negative → GND |

The official [Adafruit pinout](https://learn.adafruit.com/esp32-s3-reverse-tft-feather/pinouts)
and installed board variant identify GPIO13 as `LED_BUILTIN` and A0 as GPIO18.
GPIO13 is used strictly as an input; the firmware never drives the onboard LED.

## Behavior and tasks

Hold PTT to request the relay floor. The external LED turns on **after grant**,
only while transmitting; a busy/offline request leaves it off. Release stops
capture forwarding immediately (button polling every 5 ms), clears TX, and
sends `ptt_stop`. Presses require 20 ms stable LOW; release is immediate.
Late grants are ignored while stopping. No automatic retry occurs while a
denied button remains held. Release and press again to retry.

The Arduino loop handles button/LED only. Core 1 runs separate microphone and
speaker tasks at priority 3; core 0 runs the priority-2 network task, the only
owner of the WebSocket object. Short critical sections protect shared state and
the fixed audio rings. No lock spans socket, I2S, logging, or heap operations.

Mic DMA runs continuously and is drained while idle; idle samples are discarded.
Frames are queued only after grant and are tagged with an internal generation
to reject samples crossing release/reconnect. Speaker DMA runs continuously
with local silence when idle, rebuffering, or the button is held. Received mono
samples are duplicated to both amplifier slots. The relay's half-duplex floor
is preserved, and playback is also suppressed locally while PTT is held.

`audio_core.h` isolates PCM conversion, floor transitions, and bounded rings;
`firmware.cpp` contains Arduino/FreeRTOS/I2S/WebSocket adapters.

| Buffer / frame | Size / policy |
| --- | --- |
| Wire audio | 320 samples, 640 bytes per binary WS message, 20 ms, 50/s |
| Transport overhead | 648 WS bytes ESP32→relay; 644 relay→ESP32; excludes TCP/IP/Wi-Fi |
| Microphone task frame | 320 stereo 32-bit I2S frames = 2560 bytes; select one slot → 640 wire bytes |
| Speaker task frame | 320 stereo 16-bit I2S frames = 1280 bytes |
| TX ring | 8 frames / 160 ms maximum; evict oldest, expire after 160 ms |
| RX ring | 3-frame / 60 ms prefill, 8-frame / 160 ms maximum; evict oldest and expire after 160 ms |
| Each I2S DMA ring | 4 descriptors × 160 samples = 40 ms capacity |
| RX message assembly | Fixed 4097 bytes, accepts up to 4096 including fragmented messages |

RX underflow emits silence and waits for three fresh frames again. `rx_stop`,
disconnect, and peer loss clear RX. Up to the speaker's DMA capacity and a
currently selected frame may remain audible after stop/PTT; already submitted
hardware samples and TCP bytes cannot be recalled. DMA auto-clear prevents
old speech repeating on speaker starvation. Application buffers bound queued
speech, but TCP, Wi-Fi, and DMA mean end-to-end latency isn't strictly bounded.

## Failure behavior

Wi-Fi automatically reconnects with an explicit retry every 10 s while offline.
WebSocket retries every 1 s (a local timing difference from Python's exponential
backoff, with no protocol impact). The library handles masking, frame boundaries,
fragmentation, ping/pong, and uncompressed WebSocket negotiation. Heartbeat uses
20 s ping / 20 s pong timeout. Registration and outstanding floor responses
have 5 s deadlines. Malformed/unexpected messages, relay errors, and failed or
slow sends clear audio and reconnect; serial output records the reason.

The unmodified WebSockets library uses a **5 s** TCP timeout setting and has a
**15 KiB** incoming transport-frame allocation cap. Progress can restart its
write timeout, so this is not a strict total-operation deadline. The
application rejects messages over 4096 bytes. Sends taking over 500 ms cause
reconnect **after the call returns**; this is not Python's interruptible 500 ms
send deadline. Separate audio/button tasks keep running during socket stalls;
bounded queues discard old capture. A network black hole can still take the
keepalive interval to detect. No custom socket framework is introduced here.

A new connection resets all floor/audio state. Holding PTT across reconnect
does not resume transmission: release and press again. Every 10 s the network
task prints frame/drop/underrun counters, queue depths, and free heap. The LED
shows actual granted transmission, not just a physical button press.

## Interoperability listening test

Start the existing relay and laptop client using their normal environments:

```sh
.venv-relay/bin/python -m walkie_relay --host 0.0.0.0 --port 8765
.venv-client/bin/python -m walkie_client \
  --server ws://127.0.0.1:8765 --device laptop-b --peer esp32-a
```

Flash the board with `device=esp32-a`, `peer=laptop-b`, and the relay's LAN IP.
Check Serial Monitor for `hello_ok` and `PEER ONLINE`.

1. Hold Feather PTT, check `floor_granted` and external LED ON, speak, and
   listen on the laptop. Release; verify LED OFF and laptop `rx_stop`.
2. Toggle laptop SPACE on, speak, and listen on the Feather. Toggle off;
   verify playback stops and the next burst doesn't replay the prior tail.
3. While laptop owns the floor, press Feather PTT: expect `floor_denied: busy`,
   LED OFF, and no transmission. Release, stop laptop, then try Feather again.
4. Briefly tap/release before the grant arrives: no late speech/LED activation.
5. Disconnect/restart relay and Wi-Fi, including while Feather PTT is held.
   Confirm reset/re-registration and a fresh release/press needed to speak.
6. Close the laptop client during Feather TX; verify floor release and LED OFF.
7. Listen for clipping, missing audio, and lag in both directions. If Feather TX
   is silent, check MIC_SLOT/L/R strap first; if quiet, check alignment then gain.
   RX/TX counters indicate network frames, not proof of audible hardware audio.

## Local verification

Host tests exercise the production C++ state/buffer/conversion code, including
release-before-grant, stale capture generations, held-button reconnect, busy
denial, prefill/underflow, overflow eviction, expiry, rollover, channel selection,
signed little-endian values, stereo duplication, and saturation:

```sh
g++ -std=c++17 -Wall -Wextra -Werror \
  esp32/tests/audio_core_test.cpp -o /tmp/dolphin-audio-test
/tmp/dolphin-audio-test
```

Add `-fsanitize=address,undefined` if the host has the sanitizer runtimes installed.

Optional CLI compile with the installed core and WebSockets library:

```sh
arduino-cli compile --fqbn esp32:esp32:adafruit_feather_esp32s3_reversetft \
  --build-path /tmp/dolphin-esp32-build esp32/walkie_esp32
```

Compilation and host tests do not validate microphone word alignment, amplifier
behavior, Wi-Fi throughput, or acoustic latency. The listening steps above
remain necessary on the breadboard; no physical board was flashed during this work.

API references: [ESP32 Arduino I2S](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/i2s.html),
[WebSockets library](https://github.com/Links2004/arduinoWebSockets/tree/2.7.2).
