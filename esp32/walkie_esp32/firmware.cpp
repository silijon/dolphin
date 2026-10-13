#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s_std.h>
#include <cJSON.h>
#include <esp_arduino_version.h>
#include <esp_system.h>
#include "config.h"
#include "audio_core.h"
#include "firmware.h"
#include "provisioning.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Select the ESP32-S3 Reverse TFT Feather with ESP32 Arduino core 3.x"
#endif
static_assert(WALKIE_MIC_SLOT == 0 || WALKIE_MIC_SLOT == 1, "MIC_SLOT must be 0 or 1");
static_assert(WALKIE_MIC_SHIFT >= 0 && WALKIE_MIC_SHIFT <= 31, "Invalid microphone shift");
static_assert(WALKIE_MIC_GAIN > 0 && WALKIE_MIC_GAIN <= 64, "MIC_GAIN must be 1..64");

namespace {
using namespace walkie;
AudioState audio;
Settings settings;  // Loaded once before tasks start; immutable during operation.
portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
i2s_chan_handle_t micChannel = nullptr, ampChannel = nullptr;
WebSocketsClient socketClient;  // Used exclusively by networkTask, including callbacks.
bool fatalSession = false, transportConnected = false, registered = false;
uint32_t connectedAt = 0, requestAt = 0;
uint32_t txCount = 0, rxCount = 0;
char message[4097];
size_t messageLength = 0;
enum class MessageKind { None, Text, Binary };
MessageKind fragmented = MessageKind::None;

[[noreturn]] void halt(const char *reason) {
  digitalWrite(LED_PIN, LOW);
  // USB can reconnect after setup has failed; keep the cause visible then too.
  for (;;) {
    Serial.printf("FATAL: %s\n", reason);
    delay(2000);
  }
}
void check(esp_err_t result, const char *operation) {
  if (result != ESP_OK) {
    char reason[160];
    snprintf(reason, sizeof(reason), "%s: %s", operation, esp_err_to_name(result));
    halt(reason);
  }
}
void resetSession() {
  registered = transportConnected = false;
  fragmented = MessageKind::None;
  messageLength = 0;
  requestAt = 0;
  portENTER_CRITICAL(&audioMux);
  audio.disconnected();
  portEXIT_CRITICAL(&audioMux);
}
void fail(const char *reason) {
  Serial.printf("SESSION ERROR: %s\n", reason);
  fatalSession = true;  // Disconnect after returning from the library callback.
  portENTER_CRITICAL(&audioMux);
  audio.disconnected();
  portEXIT_CRITICAL(&audioMux);
}
bool sendText(const char *text) {
  uint32_t started = millis();
  bool sent = socketClient.sendTXT(text);
  if (!sent || uint32_t(millis() - started) > 500) {
    fail("control send failed or stalled");
    return false;
  }
  return true;
}

void handleControl(size_t length) {
  message[length] = '\0';
  // Require exactly one JSON value, including optional trailing whitespace.
  const char *end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(message, length + 1, &end, true);
  if (!root || end != message + length || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    fail("invalid relay JSON");
    return;
  }
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
  if (!cJSON_IsString(type)) {
    cJSON_Delete(root);
    fail("missing relay control type");
    return;
  }
  const char *kind = type->valuestring;
  Serial.printf("%s%s%s\n", kind, cJSON_IsString(reason) ? ": " : "",
                cJSON_IsString(reason) ? reason->valuestring : "");
  if (!registered) {
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    if (strcmp(kind, "hello_ok") || !cJSON_IsNumber(version) || version->valuedouble != 1) {
      cJSON_Delete(root);
      fail("registration rejected");
      return;
    }
    registered = true;
    portENTER_CRITICAL(&audioMux);
    audio.registered();
    portEXIT_CRITICAL(&audioMux);
    Serial.println("CONNECTED; release then press/hold PTT to speak");
  } else if (!strcmp(kind, "peer_status")) {
    const cJSON *online = cJSON_GetObjectItemCaseSensitive(root, "online");
    if (!cJSON_IsBool(online)) {
      fail("invalid peer status");
    } else {
      portENTER_CRITICAL(&audioMux);
      audio.peer(cJSON_IsTrue(online));
      portEXIT_CRITICAL(&audioMux);
      Serial.println(cJSON_IsTrue(online) ? "PEER ONLINE" : "PEER OFFLINE");
    }
  } else if (!strcmp(kind, "floor_granted")) {
    portENTER_CRITICAL(&audioMux);
    audio.granted();
    portEXIT_CRITICAL(&audioMux);
  } else if (!strcmp(kind, "floor_denied")) {
    portENTER_CRITICAL(&audioMux);
    audio.denied();
    portEXIT_CRITICAL(&audioMux);
  } else if (!strcmp(kind, "floor_released")) {
    portENTER_CRITICAL(&audioMux);
    audio.released();
    portEXIT_CRITICAL(&audioMux);
  } else if (!strcmp(kind, "rx_start")) {
    portENTER_CRITICAL(&audioMux);
    audio.resetRx(true);
    portEXIT_CRITICAL(&audioMux);
  } else if (!strcmp(kind, "rx_stop")) {
    portENTER_CRITICAL(&audioMux);
    audio.resetRx();
    portEXIT_CRITICAL(&audioMux);
  } else {
    fail(!strcmp(kind, "error") ? "relay reported an error (see reason above)" :
                                  "unexpected relay control");
  }
  cJSON_Delete(root);
}
void handleMessage(MessageKind kind, const uint8_t *payload, size_t length) {
  if (fatalSession) return;
  if (kind == MessageKind::Binary) {
    if (!registered || length != FRAME_BYTES) { fail("wrong audio frame size/state"); return; }
    Frame frame;
    memcpy(frame.pcm, payload, FRAME_BYTES);
    frame.captured = millis();
    portENTER_CRITICAL(&audioMux);
    audio.received(frame);
    portEXIT_CRITICAL(&audioMux);
    ++rxCount;
  } else {
    if (length > 4096) { fail("oversized relay control"); return; }
    if (payload != reinterpret_cast<uint8_t *>(message)) memcpy(message, payload, length);
    handleControl(length);
  }
}
void appendFragment(const uint8_t *payload, size_t length) {
  if (length > 4096 - messageLength) { fail("oversized fragmented message"); return; }
  memcpy(message + messageLength, payload, length);
  messageLength += length;
}
void websocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      resetSession();
      transportConnected = true;
      connectedAt = millis();
      portENTER_CRITICAL(&audioMux);
      audio.connected();
      portEXIT_CRITICAL(&audioMux);
      char hello[240];
      snprintf(hello, sizeof(hello),
               "{\"type\":\"hello\",\"protocol\":1,\"device_id\":\"%s\",\"peer_id\":\"%s\"}",
               settings.device, settings.peer);
      sendText(hello);
      break;
    }
    case WStype_DISCONNECTED:
      resetSession();
      Serial.println("DISCONNECTED; retrying (release PTT before next session)");
      break;
    case WStype_ERROR: fail("WebSocket transport error"); break;
    case WStype_TEXT:
    case WStype_BIN:
      if (fragmented != MessageKind::None) { fail("overlapping messages"); break; }
      handleMessage(type == WStype_TEXT ? MessageKind::Text : MessageKind::Binary, payload, length);
      break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
      if (fragmented != MessageKind::None) { fail("overlapping fragments"); break; }
      fragmented = type == WStype_FRAGMENT_TEXT_START ? MessageKind::Text : MessageKind::Binary;
      messageLength = 0;
      appendFragment(payload, length);
      break;
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      if (fragmented == MessageKind::None) { fail("unexpected continuation"); break; }
      appendFragment(payload, length);
      if (type == WStype_FRAGMENT_FIN && !fatalSession) {
        MessageKind kind = fragmented;
        fragmented = MessageKind::None;
        handleMessage(kind, reinterpret_cast<uint8_t *>(message), messageLength);
        messageLength = 0;
      }
      break;
    case WStype_PING:
    case WStype_PONG: break;  // Library responds to pings and checks its heartbeat.
  }
}

void initializeI2S() {
  // Separate physical controllers, not TX/RX sharing one clock configuration.
  i2s_chan_config_t mic = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  mic.dma_desc_num = 4;
  mic.dma_frame_num = 160;  // 4 * 10 ms = 40 ms hardware capacity.
  check(i2s_new_channel(&mic, nullptr, &micChannel), "allocate mic I2S0");
  i2s_std_config_t input = {};
  input.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE);
  input.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  input.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  input.gpio_cfg.bclk = gpio_num_t(MIC_BCLK);
  input.gpio_cfg.ws = gpio_num_t(MIC_LRCLK);
  input.gpio_cfg.dout = I2S_GPIO_UNUSED;
  input.gpio_cfg.din = gpio_num_t(MIC_DATA);
  check(i2s_channel_init_std_mode(micChannel, &input), "configure mic");

  i2s_chan_config_t amp = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  amp.dma_desc_num = 4;
  amp.dma_frame_num = 160;
  amp.auto_clear = true;  // Silence rather than repeating old DMA speech on starvation.
  check(i2s_new_channel(&amp, &ampChannel, nullptr), "allocate amp I2S1");
  i2s_std_config_t output = {};
  output.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE);
  output.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  output.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  output.gpio_cfg.bclk = gpio_num_t(AMP_BCLK);
  output.gpio_cfg.ws = gpio_num_t(AMP_LRCLK);
  output.gpio_cfg.dout = gpio_num_t(AMP_DATA);
  output.gpio_cfg.din = I2S_GPIO_UNUSED;
  check(i2s_channel_init_std_mode(ampChannel, &output), "configure amp");
  check(i2s_channel_enable(micChannel), "enable mic");
  check(i2s_channel_enable(ampChannel), "enable amp");
}

void microphoneTask(void *) {
  int32_t samples[SAMPLES * 2];
  Frame frame;
  for (;;) {
    portENTER_CRITICAL(&audioMux);
    frame.generation = audio.generation;
    bool capturing = audio.transmitting();
    portEXIT_CRITICAL(&audioMux);
    // Always drain DMA, even while released, so the next grant starts with fresh samples.
    size_t total = 0;
    while (total < sizeof(samples)) {
      size_t bytes = 0;
      esp_err_t result = i2s_channel_read(micChannel, reinterpret_cast<uint8_t *>(samples) + total,
                                        sizeof(samples) - total, &bytes, 60);
      total += bytes;
      if (result != ESP_OK || !bytes) { total = 0; break; }
    }
    if (!total) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
    if (!capturing) continue;
    micToWire(samples, frame.pcm, WALKIE_MIC_SLOT, WALKIE_MIC_SHIFT, WALKIE_MIC_GAIN);
    frame.captured = millis();
    portENTER_CRITICAL(&audioMux);
    audio.captured(frame);  // Rejects samples crossing a release or session transition.
    portEXIT_CRITICAL(&audioMux);
  }
}
void speakerTask(void *) {
  Frame frame;
  int16_t samples[SAMPLES * 2];
  for (;;) {
    uint32_t now = millis();
    portENTER_CRITICAL(&audioMux);
    audio.playback(frame, now);
    portEXIT_CRITICAL(&audioMux);
    wireToSpeaker(frame.pcm, samples);
    // DMA write provides the 20 ms playout clock. No network work or heap allocation.
    size_t written = 0;
    esp_err_t result = i2s_channel_write(ampChannel, samples, sizeof(samples), &written, 60);
    if (result != ESP_OK || written != sizeof(samples)) {
      portENTER_CRITICAL(&audioMux);
      audio.resetRx(audio.rxActive);
      portEXIT_CRITICAL(&audioMux);
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}
void networkTask(void *) {
  // Empty subprotocol: Python websockets doesn't select the library's default "arduino".
  socketClient.begin(settings.host, settings.port, settings.path, "");
  socketClient.onEvent(websocketEvent);
  socketClient.setReconnectInterval(1000);
  socketClient.enableHeartbeat(20000, 20000, 1);
  Frame frame;
  uint32_t wifiAttempt = millis(), statsAt = millis();
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (transportConnected) { socketClient.disconnect(); resetSession(); }
      if (uint32_t(millis() - wifiAttempt) >= 10000) {
        wifiAttempt = millis();
        WiFi.reconnect();
        Serial.println("Wi-Fi reconnecting");
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    socketClient.loop();
    if (transportConnected && !registered && uint32_t(millis() - connectedAt) >= 5000)
      fail("hello_ok timeout");

    portENTER_CRITICAL(&audioMux);
    Phase phase = audio.phase;
    Command command = registered && !fatalSession ? audio.command() : Command::None;
    portEXIT_CRITICAL(&audioMux);
    if (command != Command::None) {
      requestAt = millis();
      sendText(command == Command::Stop ? "{\"type\":\"ptt_stop\"}" : "{\"type\":\"ptt_start\"}");
    } else if (registered && !fatalSession) {
      uint32_t now = millis();
      portENTER_CRITICAL(&audioMux);
      bool available = audio.takeTx(frame, now);
      portEXIT_CRITICAL(&audioMux);
      if (available) {
        // Last state check before the single ordered network writer. Sent TCP
        // bytes cannot be recalled if the button releases during this send.
        portENTER_CRITICAL(&audioMux);
        bool allowed = audio.transmitting() && audio.generation == frame.generation;
        portEXIT_CRITICAL(&audioMux);
        if (allowed && digitalRead(BUTTON_PIN) == LOW) {
          uint32_t started = millis();
          if (!socketClient.sendBIN(frame.pcm, FRAME_BYTES) || uint32_t(millis() - started) > 500)
            fail("audio send failed or stalled");
          else ++txCount;
        }
      }
    }
    if (registered && (phase == Phase::Requested || phase == Phase::Stopping) &&
        requestAt && uint32_t(millis() - requestAt) >= 5000) fail("floor response timeout");
    if (fatalSession) {
      socketClient.disconnect();
      resetSession();
      fatalSession = false;
    }
    if (uint32_t(millis() - statsAt) >= 10000) {
      statsAt = millis();
      portENTER_CRITICAL(&audioMux);
      uint32_t txDrops = audio.tx.drops, rxDrops = audio.rx.drops, underruns = audio.underruns;
      unsigned txDepth = audio.tx.count, rxDepth = audio.rx.count;
      portEXIT_CRITICAL(&audioMux);
      Serial.printf("TX=%lu RX=%lu TXdrop=%lu RXdrop=%lu underrun=%lu queues=%u/%u heap=%u\n",
                    (unsigned long)txCount, (unsigned long)rxCount, (unsigned long)txDrops,
                    (unsigned long)rxDrops, (unsigned long)underruns, txDepth, rxDepth,
                    ESP.getFreeHeap());
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}  // namespace

void walkieSetup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);  // GPIO13 is also LED_BUILTIN: never drive it.
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  uint32_t bootAt = millis();
  bool setupHeld = digitalRead(BUTTON_PIN) == LOW;
  Serial.begin(115200);
  // Give native USB a bounded opportunity to enumerate and attach a monitor.
  // A battery-powered board still boots when no serial host is attached.
  uint32_t serialStarted = millis();
  while (!Serial && uint32_t(millis() - serialStarted) < 3000) {
    if (digitalRead(BUTTON_PIN) != LOW) setupHeld = false;
    delay(10);
  }
  Serial.printf("\nBOOT walkie v1; reset=%d USB_MODE=%d CDC_ON_BOOT=%d\n",
                int(esp_reset_reason()), ARDUINO_USB_MODE, ARDUINO_USB_CDC_ON_BOOT);
  if (setupHeld) Serial.println("PTT held at boot; keep holding for five seconds to enter setup");
  while (setupHeld && uint32_t(millis() - bootAt) < 5000) {
    if (digitalRead(BUTTON_PIN) != LOW) setupHeld = false;
    delay(10);
  }
  if (digitalRead(BUTTON_PIN) != LOW) setupHeld = false;
  if (!configureWalkie(settings, setupHeld)) halt("could not initialize setup network or settings storage");
  if (const char *error = validateSettings(settings)) halt(error);
  Serial.println("Initializing microphone I2S0 and amplifier I2S1...");
  initializeI2S();
  Serial.println("I2S ready; starting Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(settings.ssid, settings.password);
  Serial.println("Starting audio and network tasks...");
  if (xTaskCreatePinnedToCore(microphoneTask, "microphone", 6144, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(speakerTask, "speaker", 6144, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(networkTask, "network", 12288, nullptr, 2, nullptr, 0) != pdPASS)
    halt("could not create tasks");
  Serial.printf("Walkie v1: %s -> %s, ws://%s:%u%s, PCM16LE 16000Hz, 640B/20ms\n",
                settings.device, settings.peer, settings.host,
                unsigned(settings.port), settings.path);
}

void walkieButtonLoop() {
  static bool lastRaw = false;
  static uint32_t changedAt = 0;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (down != lastRaw) { lastRaw = down; changedAt = now; }
  portENTER_CRITICAL(&audioMux);
  // Release immediately; accept a press only after 20 ms stable LOW.
  // This loop runs independently of socket stalls, so LED/TX disable stays responsive.
  if (!down || uint32_t(now - changedAt) >= 20) audio.button(down);
  bool transmitting = audio.transmitting();
  portEXIT_CRITICAL(&audioMux);
  digitalWrite(LED_PIN, transmitting && down ? HIGH : LOW);
  delay(5);
}
