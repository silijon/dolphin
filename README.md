# Wi-Fi walkie-talkie POC

Python laptop clients and a native ESP32 Arduino client exchange half-duplex
speech through a separate Python relay. Laptop SPACE toggles push-to-talk; Q
quits. The [ESP32 client](esp32/README.md) targets the Adafruit ESP32-S3 Reverse
TFT Feather with an I2S microphone, amplifier, and hold-to-talk button. Automated
tests do not measure microphone-to-speaker latency or verify physical hardware.

## Architecture

```text
 Linux laptop                                   MacBook
 +-------------------+                          +-------------------+
 | Mic -> PCM capture|                          |PCM capture <- Mic |
 | Speaker <- jitter |                          |jitter -> Speaker  |
 +---------+---------+                          +---------+---------+
           | outbound WebSocket                           |
           |                    outbound WebSocket        |
           v                                              v
       +-----------------------------------------------------+
       | Python relay: registration, floor, audio forwarding |
       +-----------------------------------------------------+
```

Each client keeps RawInputStream and RawOutputStream open at 16 kHz, mono,
PCM16LE, 320 samples per callback. It discards microphone data until the relay
grants the floor and outputs silence when not receiving. Each 20 ms frame is
one 640-byte WebSocket binary message. Controls are small JSON text messages.
Compression is disabled. [PROTOCOL.md](PROTOCOL.md) specifies the complete wire
contract shared by the laptop and ESP32 implementations.

The ESP32 has a setup Wi-Fi network and a browser configuration page at
`http://walkie.setup` for Wi-Fi credentials, relay address, and pairing IDs.
Settings persist across restarts; see the [ESP32 setup guide](esp32/README.md).

The relay owns all floor/routing state on one asyncio thread. Reciprocal IDs
form a pair; each pair can have one transmitter. Per-connection bounded writers
keep slow consumers out of floor arbitration. The client uses one network
reader/writer, a terminal controller, and two PortAudio callbacks. Short,
non-blocking callback lock attempts bridge bounded buffers; callbacks never
log, wait on asyncio, or do network I/O. The network writer polls capture every
5 ms, avoiding an unbounded backlog of thread-to-loop notifications.

The jitter target is 3 frames (60 ms), maximum 8 frames; overflow evicts oldest
speech and underflow outputs silence and re-buffers. Capture holds at most 8
frames and expires stale frames. Constants live in `client/.../jitter.py` and
`audio.py`. PTT release and reconnect clear audio. Release intentionally cuts
off any buffered tail. TCP and host audio buffering can still add latency;
this POC does not claim a hard end-to-end deadline.

```text
relay/                  # independently installable; only websockets at runtime
  pyproject.toml
  src/walkie_relay/      # __main__, server, state, protocol
  tests/
client/                 # independently installable; websockets + sounddevice
  pyproject.toml
  src/walkie_client/     # __main__, client, audio, ptt, protocol, jitter
  tests/
PROTOCOL.md
esp32/                  # native Arduino client, inspection, setup, host tests
```

Neither package imports the other. The root is only documentation/test
configuration, not another runtime package. No database or filesystem state is
needed by the relay, so later containerization requires no architecture change.

## Setup

Use Python **3.11 or newer**. Run the following from the repository root. If
`python3` is older, substitute an installed `python3.11` or newer executable.
Copy/clone this repository onto both laptops. Each package can also be copied
and installed alone.

### Relay (Linux laptop initially)

```sh
python3 -m venv .venv-relay
.venv-relay/bin/python -m pip install -e ./relay
```

The relay has no audio or PortAudio dependency.

### Linux client (Fedora)

Install PortAudio if missing; these are manual setup commands:

```sh
sudo dnf install python3 python3-pip portaudio
python3 -m venv .venv-client
.venv-client/bin/python -m pip install -e ./client
.venv-client/bin/python -m walkie_client --list-devices
```

On Debian/Ubuntu the corresponding runtime library is `libportaudio2`, and
creating a venv may require `python3-venv`. Use the normal desktop user, not
root, so audio-session permissions and defaults are available.

### macOS client

With Python 3.11+ installed:

```sh
python3 -m venv .venv-client
.venv-client/bin/python -m pip install -e ./client
.venv-client/bin/python -m walkie_client --list-devices
```

The sounddevice macOS wheel normally includes PortAudio
([installation reference](https://github.com/spatialaudio/python-sounddevice/blob/master/doc/installation.rst)). If it cannot load the
library, install it with `brew install portaudio` (if using Homebrew) and retry.
When the client first opens input, allow microphone access for the application
hosting the terminal. Check **System Settings → Privacy & Security → Microphone**
if denied (Terminal, iTerm, or your IDE may be listed); restart that terminal
application after changing access. No Accessibility/global-keyboard permission
is needed.

## Audio device selection

```sh
.venv-client/bin/python -m walkie_client --list-devices
```

Output includes device indexes, names, host APIs, input/output channel counts,
and default input/output indexes. Omitting device options uses system defaults.
To choose indexes from your own listing, append, for example:

```sh
--input-device 2 --output-device 4
```

Indexes are local to each machine and may change after plugging devices in.
Use a device with at least one channel in the required direction. Streams must
accept **16000 Hz / mono / int16 / 320 samples**; errors identify the failed
direction and selection. The program never changes rate or resamples audio.
Try wired headphones first; Bluetooth devices can add significant buffering.

## Running the POC

Use an interactive terminal on each laptop. Replace `<LINUX_IP>` with the Linux
laptop's LAN IPv4 address (for example, inspect `ip -4 addr` on Linux). The Mac
must be able to reach that address; guest Wi-Fi/client isolation can prevent it.

**Linux, terminal 1 — relay:**

```sh
.venv-relay/bin/python -m walkie_relay --host 0.0.0.0 --port 8765
```

**Linux, terminal 2 — device A:**

```sh
.venv-client/bin/python -m walkie_client \
  --server ws://127.0.0.1:8765 --device device-a --peer device-b
```

**MacBook — device B:**

```sh
.venv-client/bin/python -m walkie_client \
  --server 'ws://<LINUX_IP>:8765' --device device-b --peer device-a
```

1. Confirm both clients report `CONNECTED`, then `PEER ONLINE`.
2. Press SPACE on Linux; wait for `TRANSMITTING` (floor granted).
3. Speak into the Linux microphone and hear it on the Mac (`RECEIVING`).
4. While Linux is transmitting, press SPACE on the Mac: it must report `BUSY`.
5. Press SPACE on Linux again to stop.
6. Press SPACE on the Mac; speak into its microphone and hear it on Linux.
7. While Mac owns the floor, attempt PTT on Linux and verify `BUSY` again.
8. Press SPACE on Mac to stop. After a denied request, a fresh SPACE requests
   the floor; a denied request does not leave PTT toggled on.
9. Quit a transmitting client and check that its peer reports stopped/offline.
   Restart it and check online status. Restart the relay and verify clients
   reconnect automatically with PTT off.
10. Q or Ctrl-C exits a client; Ctrl-C exits the relay. Final counters are logged.

The Fedora/Linux firewall may need to permit inbound **TCP 8765** on the Linux
relay's LAN interface for the Mac to connect. No rules are changed automatically.
If using firewalld, identify the appropriate zone with
`sudo firewall-cmd --get-active-zones`, then explicitly allow the port in that
zone for the test (for example,
`sudo firewall-cmd --zone=home --add-port=8765/tcp` when `home` is the actual LAN
zone). Remove the temporary rule afterward with the matching `--remove-port`.
Do not open a router Internet port for this plaintext POC. See the
[firewalld port instructions](https://firewalld.org/documentation/howto/open-a-port-or-service.html).

## Future remote-network test

The same client already accepts `wss://` URLs with normal certificate validation.
Later put the relay on a reachable host behind TLS termination and add real
device authentication before public deployment:

```text
Household A: client --outbound--> public relay <--outbound-- client: Household B
```

Only the relay needs inbound reachability. No inbound port is needed on either
client network, and there is no NAT traversal, STUN/TURN, or peer-to-peer path.
The initial Linux laptop needs an inbound port only because it also hosts the
relay. TLS termination and authentication are not implemented here.

## Troubleshooting

- **Mac has no microphone audio:** allow microphone access for the terminal app;
  check input selection and system input level. Reopen the terminal after changing
  permissions. The client opens the microphone even while PTT is off.
- **Wrong device / no sound:** list devices and select input/output separately;
  check mute, volume, and the sound settings. System defaults may change with USB
  or Bluetooth devices. Only one side may transmit at a time.
- **PortAudio unavailable:** install the platform library above. On Fedora, use
  a desktop login with working system audio. On source-only installs, CFFI may
  also need a compiler, Python development headers, and libffi development files.
- **Stream cannot open:** the error gives direction, device and required format.
  Choose another device or fix the host audio configuration. No ALSA-specific
  selection or application resampling is built into this cross-platform client.
- **PEER OFFLINE:** IDs must be reciprocal and distinct, and both clients must
  reach the same relay. Duplicate device IDs are explicitly rejected. Check relay
  registration logs; a typo/nonreciprocal peer is offline.
- **Mac cannot connect:** use the Linux LAN address, not Mac's `127.0.0.1`; check
  the IP, firewall, same network/client isolation, and relay bind address.
  `--host 127.0.0.1` (the safe relay default) accepts local connections only;
  use `--host 0.0.0.0` for this LAN test.
- **Dropouts / delay:** try wired headphones and stable Wi-Fi; reduce CPU load.
  `--debug` logs periodic buffering counters; shutdown summaries include TX/RX,
  drops, playback underruns and PortAudio input overflows/output underflows.
  Increasing `TARGET_FRAMES` can smooth jitter at the cost of latency. Keep it
  below `MAX_FRAMES`. Sustained congestion drops audio or disconnects a stalled
  writer, rather than growing application queues indefinitely.
- **Feedback:** use headphones or physically separate the laptops; do not place
  an active microphone beside the other laptop's speaker.
- **Terminal:** focus the client terminal; SPACE is a toggle, not press-and-hold.
  Piped stdin is unsupported. Terminal settings restore on normal exit, Ctrl-C,
  and Python exceptions. As with any terminal app, SIGKILL cannot run cleanup;
  `stty sane` can repair a terminal after a forced kill.

## Tests and debugging

No automated test opens a microphone or speaker. Tests use real loopback
WebSockets, synthetic PCM, buffer unit tests, and a pseudo-terminal. They cover
floor contention and handoff, disconnects of either participant, reconnect,
malformed controls/audio, duplicate/nonreciprocal identity, bounded queues,
early PTT release, and terminal restoration on exception.

```sh
python3 -m venv .venv-test
.venv-test/bin/python -m pip install -e './relay[test]' -e './client[test]'
.venv-test/bin/python -m pytest -q
.venv-test/bin/python -m compileall -q relay/src client/src
```

Each package's tests also run independently after installing its own `[test]`
extra: `python -m pytest relay/tests` or `python -m pytest client/tests`.
Add `--debug` to either runtime CLI for verbose logs. INFO never logs individual
audio packets. Relay `frames_forwarded` counts frames accepted for its outbound
queue, not playback/delivery acknowledgments; queue evictions are separate.

API references: [websockets asyncio server](https://websockets.readthedocs.io/en/stable/reference/asyncio/server.html)
and [sounddevice usage](https://github.com/spatialaudio/python-sounddevice/blob/master/doc/usage.rst).

## Security limitations

This POC:

- Has no authentication.
- Has no encryption when using `ws://`.
- Trusts device IDs; anyone who can reach it can impersonate a device.
- Should not be exposed directly to the public Internet.

The later production system will use WSS/TLS plus real device authentication.
This prototype has no production abuse controls or privacy guarantees. It does
not record audio to disk, but that is not a security boundary.
