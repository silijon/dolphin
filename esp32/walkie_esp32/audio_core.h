#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// No Arduino dependency: these exact state/buffer/conversion routines are also
// tested on the host. Caller serializes access with a short ESP32 critical section.
namespace walkie {
constexpr unsigned RATE = 16000;
constexpr unsigned SAMPLES = 320;
constexpr unsigned FRAME_BYTES = SAMPLES * 2;
constexpr unsigned DEPTH = 8;
constexpr unsigned PREFILL = 3;
constexpr uint32_t MAX_AGE_MS = 160;

struct Frame {
  uint8_t pcm[FRAME_BYTES];
  uint32_t captured = 0;
  uint32_t generation = 0;
};

class Frames {
 public:
  Frame slots[DEPTH];
  unsigned head = 0, count = 0;
  uint32_t drops = 0;
  void clear() { head = count = 0; }
  void push(const Frame &frame) {
    if (count == DEPTH) {
      head = (head + 1) % DEPTH;
      --count;
      ++drops;
    }
    slots[(head + count++) % DEPTH] = frame;
  }
  bool pop(Frame &frame) {
    if (!count) return false;
    frame = slots[head];
    head = (head + 1) % DEPTH;
    --count;
    return true;
  }
};

enum class Phase { Disconnected, Registering, Idle, Requested, Transmitting, Stopping };
enum class Command { None, Start, Stop };

class AudioState {
 public:
  Phase phase = Phase::Disconnected;
  Frames tx, rx;
  bool pressed = false, armed = false, peerOnline = false;
  bool rxActive = false, playing = false;
  uint32_t generation = 0, underruns = 0;
  bool startPending = false, stopPending = false;

  bool transmitting() const { return phase == Phase::Transmitting && pressed; }
  void clearTx() { ++generation; tx.clear(); }
  void resetRx(bool active = false) { rx.clear(); rxActive = active; playing = false; }
  void disconnected() {
    phase = Phase::Disconnected;
    peerOnline = armed = startPending = stopPending = false;
    clearTx();
    resetRx();
    // Preserve physical pressed state: a held button cannot restart on reconnect.
  }
  void connected() { disconnected(); phase = Phase::Registering; }
  void registered() { phase = Phase::Idle; }
  void button(bool down) {
    if (!down) {
      armed = true;
      if (phase == Phase::Requested || phase == Phase::Transmitting) {
        phase = Phase::Stopping;
        startPending = false;
        stopPending = true;
        clearTx();
      }
    } else if (!pressed) {
      rx.clear();
      playing = false;
      if (armed && phase == Phase::Idle) {
        phase = Phase::Requested;
        startPending = true;
      }
      armed = false;
    }
    pressed = down;
  }
  Command command() {
    if (stopPending) { stopPending = false; return Command::Stop; }
    if (startPending) { startPending = false; return Command::Start; }
    return Command::None;
  }
  void granted() {
    if (phase == Phase::Requested && pressed) {
      phase = Phase::Transmitting;
      clearTx();
    }
  }
  void denied() {
    if (phase != Phase::Stopping) phase = Phase::Idle;
    clearTx();
  }
  void released() {
    phase = Phase::Idle;
    startPending = stopPending = false;
    clearTx();
  }
  void peer(bool online) {
    peerOnline = online;
    if (!online) {
      clearTx();
      resetRx();
      startPending = false;
      if (phase != Phase::Stopping) phase = Phase::Idle;
    }
  }
  void captured(const Frame &frame) {
    if (transmitting() && frame.generation == generation) tx.push(frame);
  }
  bool takeTx(Frame &frame, uint32_t now) {
    while (tx.pop(frame)) {
      if (transmitting() && frame.generation == generation &&
          uint32_t(now - frame.captured) <= MAX_AGE_MS) return true;
      ++tx.drops;
    }
    return false;
  }
  void received(const Frame &frame) {
    if (rxActive && !pressed) rx.push(frame);
  }
  bool playback(Frame &frame, uint32_t now) {
    memset(frame.pcm, 0, FRAME_BYTES);
    if (!rxActive || pressed) return false;
    // Unlike an ever-growing queue, age also bounds speech after a task stall.
    while (rx.count && uint32_t(now - rx.slots[rx.head].captured) > MAX_AGE_MS) {
      Frame discarded;
      rx.pop(discarded);
      ++rx.drops;
      playing = false;
    }
    if (!playing && rx.count < PREFILL) return false;
    playing = true;
    if (rx.pop(frame)) return true;
    ++underruns;
    playing = false;
    return false;
  }
};

inline int16_t micSample(int32_t word, unsigned shift, int gain) {
  // Explicit arithmetic shift, independent of implementation-defined signed >>.
  int64_t value = word;
  const int64_t divisor = int64_t(1) << shift;
  value = (value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor)) * gain;
  if (value > 32767) value = 32767;
  if (value < -32768) value = -32768;
  return int16_t(value);
}
inline void micToWire(const int32_t *stereo, uint8_t *pcm, unsigned slot,
                      unsigned shift, int gain) {
  for (unsigned i = 0; i < SAMPLES; ++i) {
    uint16_t sample = uint16_t(micSample(stereo[2 * i + slot], shift, gain));
    pcm[2 * i] = uint8_t(sample);
    pcm[2 * i + 1] = uint8_t(sample >> 8);
  }
}
inline void wireToSpeaker(const uint8_t *pcm, int16_t *stereo) {
  // Duplicate mono to both 16-bit slots, accommodating either amplifier channel.
  for (unsigned i = 0; i < SAMPLES; ++i) {
    unsigned bits = unsigned(pcm[2 * i]) | unsigned(pcm[2 * i + 1]) << 8;
    int16_t sample = int16_t(bits < 32768 ? int(bits) : int(bits) - 65536);
    stereo[2 * i] = stereo[2 * i + 1] = sample;
  }
}
}  // namespace walkie
