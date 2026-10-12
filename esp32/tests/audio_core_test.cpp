#include <assert.h>
#include <stdio.h>
#include "../walkie_esp32/audio_core.h"
using namespace walkie;

static void connectReleased(AudioState &state) {
  state.connected();
  state.registered();
  state.button(false);
  state.peer(true);
}

static void floorAndReconnect() {
  AudioState state;
  connectReleased(state);
  state.button(true);
  assert(state.command() == Command::Start);
  Frame captured;
  captured.generation = state.generation;
  state.captured(captured);
  assert(!state.tx.count);  // Never forward before grant.
  state.button(false);  // Release before grant reaches device.
  state.granted();
  assert(!state.transmitting());
  assert(state.command() == Command::Stop);
  state.button(true);  // Ignore a new press until release acknowledgement.
  assert(state.command() == Command::None);
  state.released();
  assert(!state.transmitting());
  state.button(false);
  state.button(true);
  assert(state.command() == Command::Start);
  state.granted();
  assert(state.transmitting());
  captured.generation = state.generation;
  captured.captured = 10;
  state.captured(captured);
  assert(state.tx.count == 1);
  state.button(false);
  assert(!state.tx.count);
  state.captured(captured);  // In-flight I2S read completes after release.
  assert(!state.tx.count);
  state.released();
  state.button(true);
  state.command();
  state.granted();
  state.captured(captured);  // Old generation cannot cross into a new burst.
  assert(!state.tx.count);
  state.connected();  // Abrupt reconnect with button still physically held.
  state.registered();
  state.button(true);
  assert(state.command() == Command::None);
  state.granted();
  assert(!state.transmitting());
  state.button(false);
  state.button(true);
  assert(state.command() == Command::Start);
  state.denied();
  assert(!state.transmitting());
  state.button(true);  // No auto retry while held after denial.
  assert(state.command() == Command::None);
  state.button(false);
  state.button(true);
  state.peer(false);  // Peer disappears before the network writer sends start.
  assert(state.command() == Command::None && !state.transmitting());
}

static void buffering() {
  AudioState state;
  connectReleased(state);
  Frame frame, out;
  memset(frame.pcm, 0x55, FRAME_BYTES);
  frame.captured = 100;
  state.received(frame);
  assert(state.rx.count == 0);  // No rx_start yet.
  state.resetRx(true);
  state.received(frame);
  state.received(frame);
  assert(!state.playback(out, 100) && out.pcm[0] == 0);
  state.received(frame);
  assert(state.playback(out, 100) && out.pcm[0] == 0x55);
  assert(state.playback(out, 100));
  assert(state.playback(out, 100));
  assert(!state.playback(out, 100) && state.underruns == 1);
  state.received(frame);
  assert(!state.playback(out, 100));  // Underflow requires prefill again.
  state.resetRx(true);
  for (unsigned i = 0; i < 20; ++i) {
    frame.pcm[0] = i;
    state.received(frame);
  }
  assert(state.rx.count == DEPTH && state.rx.drops == 12);
  assert(state.playback(out, 100) && out.pcm[0] == 12);  // Oldest evicted.
  assert(!state.playback(out, 261) && !state.rx.count);  // Stale speech expires.
  state.received(frame);
  state.button(true);
  state.received(frame);
  assert(!state.playback(out, 100) && !state.rx.count);  // Half duplex while held.
  state.resetRx();
  assert(!state.playback(out, 100));

  state.button(false);
  state.released();
  state.button(true);
  state.command();
  state.granted();
  frame.generation = state.generation;
  for (unsigned i = 0; i < 20; ++i) {
    frame.captured = i * 20;
    state.captured(frame);
  }
  assert(state.tx.count == DEPTH && state.tx.drops == 12);
  assert(state.takeTx(out, 380) && out.captured == 240);
  assert(!state.takeTx(out, 1000) && !state.tx.count);
  frame.captured = UINT32_MAX - 50;
  state.captured(frame);
  assert(state.takeTx(out, 20));  // Clock rollover, only 71 ms old.
  state.captured(frame);
  assert(!state.takeTx(out, 200));
}

static void conversion() {
  int32_t hardware[SAMPLES * 2] = {};
  int16_t speaker[SAMPLES * 2];
  uint8_t pcm[FRAME_BYTES];
  hardware[0] = INT32_MIN;
  hardware[2] = INT32_MAX;
  hardware[4] = -65536;
  hardware[6] = 0x12340000;
  hardware[1] = 0x56780000;
  micToWire(hardware, pcm, 0, 16, 1);
  assert(pcm[0] == 0 && pcm[1] == 0x80);
  assert(pcm[2] == 0xff && pcm[3] == 0x7f);
  assert(pcm[4] == 0xff && pcm[5] == 0xff);
  assert(pcm[6] == 0x34 && pcm[7] == 0x12);
  wireToSpeaker(pcm, speaker);
  assert(speaker[0] == -32768 && speaker[1] == -32768);
  assert(speaker[2] == 32767 && speaker[3] == 32767);
  assert(speaker[4] == -1 && speaker[5] == -1);
  assert(speaker[6] == 0x1234 && speaker[7] == 0x1234);
  micToWire(hardware, pcm, 1, 16, 1);
  assert(pcm[0] == 0x78 && pcm[1] == 0x56);
  assert(micSample(INT32_MAX, 16, 4) == 32767);
  assert(micSample(INT32_MIN, 16, 4) == -32768);
  assert(micSample(-1, 16, 1) == -1);
}

int main() {
  floorAndReconnect();
  buffering();
  conversion();
  puts("ESP32 audio/state tests passed");
}
