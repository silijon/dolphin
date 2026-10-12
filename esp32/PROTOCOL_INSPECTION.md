# Python implementation inspection

Inspected `client/src/walkie_client/{client,protocol,audio,jitter,ptt,__main__}.py`
and `relay/src/walkie_relay/{server,state,protocol,__main__}.py`. The client,
audio, and relay inspection preceded firmware work. The implementation agrees
with the existing [wire contract](../PROTOCOL.md).
No Python or protocol change is needed for this port.

## Connection and transport

There is no discovery, broadcast, mDNS, or direct device connection. The laptop
requires `--server ws://HOST:PORT/PATH` (or `wss://...`), `--device`, and `--peer`.
Each device opens one persistent outbound RFC 6455 WebSocket over TCP. The relay
defaults to `127.0.0.1:8765`; use `--host 0.0.0.0` for access from Wi-Fi.
Compression is disabled. WSS needs a TLS endpoint; the supplied relay listens
with plain WS. The first ESP32 build uses this existing plain WS LAN endpoint.

Within five seconds, the first message must be a **text** message:

```json
{"type":"hello","protocol":1,"device_id":"esp32-a","peer_id":"laptop-b"}
```

IDs are distinct, case-sensitive, 1–64 ASCII letters/digits/`-_.`. Duplicate
device IDs are rejected. The peer must select this device in return. Acceptance
is `{"type":"hello_ok","protocol":1}`, followed by
`{"type":"peer_status","online":true}` (or false). Wait for acceptance before
sending anything else.

## Messages and audio

Each JSON control is one WebSocket text message. Each audio frame is one binary
message. TCP reads/packets are not application frame boundaries: WebSocket
framing supplies boundaries, payload lengths, opcodes, and client masking.
There are no application audio headers, sequence numbers, timestamps, frame
IDs, or device IDs inside audio. Controls identify devices during registration
and `rx_start`. WebSocket ordering supplies audio/control ordering.

| Property | Exact value |
| --- | --- |
| Audio | Uncompressed signed two's-complement PCM16LE |
| Rate / channels | 16,000 Hz / mono |
| Frame | 320 samples, 640 bytes, 20 ms |
| Cadence | 50 binary messages/s while granted and transmitting |
| Silence | 640 zero bytes, generated locally when not playing |
| Payload limit | 4096 bytes per incoming WebSocket message |
| WS overhead, normal audio | 8 bytes client→relay (mask included), 4 relay→client; TCP/IP/Wi-Fi overhead additional |

The Python PortAudio streams are native-endian int16; the client explicitly
rejects big-endian hosts. Both streams stay open and their callbacks process
320 samples at a time. No WAV header, codec, base64, stereo, or resampling.

## Push-to-talk and floor

The laptop's SPACE toggles PTT; the hardware port uses press/hold/release.
The relay owns the half-duplex floor, independently for every reciprocal pair.

| Direction | Text JSON `type` | Fields / behavior |
| --- | --- | --- |
| Client→relay | `ptt_start` | Request floor; no extra fields |
| Client→relay | `ptt_stop` | Cancel/release; no extra fields |
| Relay→sender | `floor_granted` | Only now enable microphone forwarding |
| Relay→sender | `floor_denied` | `reason`: `busy` or `peer_offline` |
| Relay→sender | `floor_released` | `reason`: `ptt_stop`, `peer_disconnected`, `not_owner` |
| Relay→receiver | `rx_start` | `device_id`: owner; reset and activate playback buffering |
| Relay→receiver | `rx_stop` | `reason`: `ptt_stop` or `peer_disconnected`; reset playback |
| Relay→client | `peer_status` | Boolean `online` |
| Relay→client | `error` | String `reason`; Python client reconnects |

On release the client stops forwarding and clears captured frames immediately,
then sends `ptt_stop`. It ignores grants received while stopping and waits for
`floor_released` before a new request. Sending before grant yields
`not_floor_owner`. Simultaneous requests are resolved atomically: one grant,
one `busy` denial. A non-owner stop acknowledges `not_owner` without interrupting
the other speaker. There is no PTT bit in audio, end marker, or silence stream.

## Buffering and timing

Python playback starts at three frames (60 ms), holds at most eight (160 ms),
and drops the oldest on overflow. Underflow outputs silence and waits for
three frames again. Frames outside `rx_start`/`rx_stop` are ignored. `rx_stop`
clears the buffered tail intentionally. Capture holds eight frames, drops
oldest on overflow, and discards anything older than 160 ms. Generation numbers
inside the capture queue prevent reuse across TX transitions; they are not sent.
The writer polls every 5 ms and prioritizes controls over audio.

The relay holds 16 outgoing messages per client. At capacity it evicts queued
audio first; if only controls remain, it closes the client. On floor release it
removes unsent receiver audio before queuing `rx_stop`. One writer preserves
control/audio ordering; a send taking 0.5 s closes the slow consumer. TCP and
audio-driver buffers add latency beyond these application queue limits.

## Failures and reconnect

Both endpoints use 20 s ping intervals / 20 s pong timeouts and 1 s close
timeouts. Opening and registration each have a 5 s deadline. The Python client
limits sends to 0.5 s. Retry delays are 0.5, 1, 2, 4, then 8 s maximum; a session
lasting at least 10 s resets backoff. Reconnect clears all floor and audio state
and requires a fresh local PTT action. The relay releases the floor on detected
disconnect and notifies the surviving participant before peer-offline status.

Invalid registration returns `invalid_hello` or `duplicate_device` then closes
with 1008. Post-registration errors are `invalid_json`, `invalid_control`,
`unknown_control`, `invalid_audio_size`, and `not_floor_owner`: the relay drops
the offending message but keeps the connection; the laptop elects to reconnect.
Oversized messages close with 1009; slow writers with 1011. The laptop also
reconnects on malformed/unexpected relay messages, socket exceptions, or timeout.

The transport library in the ESP32 port can have different local socket/retry
timings without altering this wire protocol; those limits are documented in
the firmware README. Actual listening/latency and microphone alignment require
the physical hardware test.
