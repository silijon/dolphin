# Walkie-talkie protocol, version 1

This document is the language-independent source of truth. Each device makes one
persistent outbound WebSocket connection to a relay using `ws://` or `wss://`.
There is no connection between devices. Disable WebSocket per-message compression.
WebSocket text messages carry JSON objects; binary messages carry PCM audio.
One WebSocket message is one control object or one audio frame. WebSocket message
ordering is sufficient; there are no application sequence numbers or timestamps.

## Handshake and identity

Within five seconds of connecting, send:

```json
{"type":"hello","protocol":1,"device_id":"device-a","peer_id":"device-b"}
```

Version must be the integer `1`. IDs are 1–64 ASCII letters, digits, hyphens,
underscores or periods. Device and peer IDs must differ. IDs are case-sensitive.
Only one connection can register a given device ID; a second is rejected.
A pair is online only when both devices are registered and select each other.
Nonreciprocal registrations remain connected but report the peer offline.
Multiple disjoint pairs may use the same relay independently.

Successful registration returns, in order:

```json
{"type":"hello_ok","protocol":1}
{"type":"peer_status","online":false}
```

The status may instead be true. An already connected reciprocal peer receives
`peer_status` with `online:true` when the second device registers.
Do not send other messages before `hello_ok`.

## Control messages

Every object has a string `type`. Unknown extra fields are ignored. No request
IDs are needed: each device has at most one outstanding floor request and waits
for denial or release before making another. All controls are small; the POC
limits every incoming WebSocket message to 4096 bytes, including audio.

| Direction | Type | Additional fields / meaning |
| --- | --- | --- |
| Device → relay | `hello` | `protocol`, `device_id`, `peer_id`, as above |
| Relay → device | `hello_ok` | `protocol:1`; registration accepted |
| Relay → device | `peer_status` | `online`: boolean, reciprocal peer availability |
| Device → relay | `ptt_start` | Request the pair's floor |
| Device → relay | `ptt_stop` | Release or cancel the device's request |
| Relay → device | `floor_granted` | Device may now send audio |
| Relay → device | `floor_denied` | `reason`: `busy` or `peer_offline` |
| Relay → device | `floor_released` | `reason`: `ptt_stop`, `peer_disconnected`, or `not_owner` |
| Relay → peer | `rx_start` | `device_id`: floor owner; clear and start buffering |
| Relay → peer | `rx_stop` | `reason`: `ptt_stop` or `peer_disconnected`; clear playback |
| Relay → device | `error` | `reason`: machine-readable error below |

Example floor request: `{"type":"ptt_start"}`. Example denial:
`{"type":"floor_denied","reason":"busy"}`.

## Binary audio

Exactly **640 bytes per binary WebSocket message**:

- 16,000 samples/second, mono.
- Signed two's-complement 16-bit PCM, least-significant byte first.
- 320 samples/frame = 20 milliseconds/frame.
- No header, codec, resampling, encoding wrapper, or padding.
- Silence is 640 zero bytes. Normal transmission is 50 frames/second,
  32,000 bytes/second (256 kbit/s) before transport overhead.

Never transmit audio before the grant. The relay only forwards correctly sized
frames from the current floor owner to its reciprocal peer. Invalid frames are
not forwarded. The relay does not decode samples. Silence need not be sent
when PTT is off; each receiver generates local silence.

## Floor state machine

Each unordered reciprocal pair is either **idle**, **A owns floor**, or
**B owns floor**. Floor transitions are atomic at the relay.

1. On `ptt_start`, first check the reciprocal peer is online; otherwise deny
   `peer_offline`. If either device owns the floor, deny `busy` (including a
   repeated request by its current owner).
2. If idle and online, assign the floor, send `floor_granted` to the requester
   and `rx_start` to its peer. Start forwarding the owner's audio.
3. A competing request gets `floor_denied:busy`; it must not enable capture.
4. On the owner's `ptt_stop`, release the floor, send `rx_stop` to the receiver
   and `floor_released:ptt_stop` to the owner. Discard unsent receiver audio.
5. A non-owner's `ptt_stop` does not disturb the other owner. Acknowledge with
   `floor_released:not_owner`.

On local PTT release, immediately disable capture forwarding and discard queued
capture frames, then send `ptt_stop`. If release occurs before `floor_granted`,
ignore the late grant and wait for `floor_released`. Do not re-request during
this wait. TCP bytes already sent cannot be recalled.

## Peer loss and reconnect

When either participant disconnects, release any floor for that pair. Notify the
remaining participant with `rx_stop:peer_disconnected` if receiving, or
`floor_released:peer_disconnected` if transmitting, followed by
`peer_status:online=false`. Do this as soon as the disconnect is detected;
an abrupt network black hole needs keepalive detection first (20-second ping
interval and 20-second pong timeout in this POC).

A reconnect is a new session: clear TX/RX state and all buffered audio, register
again, and require a new local PTT action. Never resume the old floor or speech.
The laptop retries after 0.5, 1, 2, 4, then at most 8 seconds, resetting the
backoff after a connection has lasted at least 10 seconds.

## Playback and congestion

`rx_start` clears old speech and starts buffering. Laptop playback starts at
3 frames (60 ms). Its queue holds at most 8 frames (160 ms); overflow drops the
oldest frame. Underflow outputs silence and returns to buffering. `rx_stop`
stops playback and clears queued speech; the final buffered syllable may be cut
short by design. Audio already handed to the host cannot be retracted.

Capture is bounded to 8 frames and discards frames older than 160 ms. Relay
outgoing queues hold 16 messages; stale audio is evicted before control messages.
A queue saturated by controls or a write blocked for 0.5 seconds causes a
connection close. Controls and audio use one ordered writer per connection.
These are tunable implementation limits, not extra wire fields. TCP/OS audio
buffers and network head-of-line blocking mean end-to-end latency is not
strictly bounded by the application queue sizes alone.

## Error handling

Invalid registration returns `error` and closes with code 1008:
`invalid_hello` or `duplicate_device`. Missing handshake times out and closes.
After registration, malformed JSON, invalid control shape, unknown controls,
wrong-size audio, or unowned audio return `error` with respectively
`invalid_json`, `invalid_control`, `unknown_control`, `invalid_audio_size`, or
`not_floor_owner`. The offending message is discarded; the relay stays alive
and the session can continue. Oversized WebSocket messages close with 1009.
Slow/saturated consumers close with 1011. Clients should report errors clearly;
the laptop resets and reconnects on a relay `error` or transport failure.

This protocol has no authentication or application encryption. `ws://` is
plaintext. Production use requires WSS/TLS and real device authentication.
