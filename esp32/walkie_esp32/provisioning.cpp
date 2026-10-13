#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <cJSON.h>
#include <esp_system.h>
#include "config.h"
#include "provisioning.h"

namespace {
using namespace walkie;
static_assert(sizeof(WALKIE_SETUP_PASSWORD) >= 9 && sizeof(WALKIE_SETUP_PASSWORD) <= 64,
              "Setup AP password must contain 8-63 characters");
const IPAddress AP_ADDRESS(192, 168, 4, 1);
const IPAddress AP_MASK(255, 255, 255, 0);
// Bind only to the setup interface, including during the temporary AP+STA test.
WebServer web(AP_ADDRESS, 80);
DNSServer dns;
Settings candidate;
bool finished = false;
String csrf;
portMUX_TYPE reportMux = portMUX_INITIALIZER_UNLOCKED;
enum class State { Idle, Running, WifiFailed, RelayFailed, Ready };
struct Report { State state = State::Idle; char detail[192] = {}; };
Report report;

Report readReport() {
  portENTER_CRITICAL(&reportMux);
  Report result = report;
  portEXIT_CRITICAL(&reportMux);
  return result;
}
void publish(State state, const char *detail) {
  Report result;
  result.state = state;
  snprintf(result.detail, sizeof(result.detail), "%s", detail);
  portENTER_CRITICAL(&reportMux);
  report = result;
  portEXIT_CRITICAL(&reportMux);
}
bool load(Settings &settings) {
  Preferences prefs;
  if (!prefs.begin("walkie", true)) return false;
  bool valid = prefs.getBytesLength("settings") == sizeof(settings) &&
               prefs.getBytes("settings", &settings, sizeof(settings)) == sizeof(settings);
  prefs.end();
  return valid && !validateSettings(settings);
}
bool save(const Settings &settings) {
  Preferences prefs;
  if (!prefs.begin("walkie", false)) return false;
  bool ok = prefs.putBytes("settings", &settings, sizeof(settings)) == sizeof(settings);
  prefs.end();
  return ok;
}
String escape(const char *raw) {
  String out;
  for (const char *p = raw; *p; ++p) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += *p;
    }
  }
  return out;
}
String relayUrl(const Settings &settings) {
  return String("ws://") + settings.host + ":" + String(settings.port) + settings.path;
}
String pageStart(const char *title, bool refresh = false) {
  String html;
  html.reserve(5500);
  html = "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  if (refresh) html += "<meta http-equiv='refresh' content='2;url=/status'>";
  html += "<title>Walkie setup</title><style>"
          "body{font:17px system-ui,sans-serif;background:#eef2f6;color:#182534;margin:0;padding:24px}"
          "main{max-width:520px;margin:auto;background:white;padding:24px;border-radius:16px}"
          "h1{margin-top:0}label{display:block;margin-top:18px}input{box-sizing:border-box;width:100%;"
          "padding:12px;margin-top:6px;border:1px solid #9aa9b6;border-radius:6px;font:inherit}"
          "input[type=checkbox]{width:auto}button{font:inherit;padding:12px 18px;border:0;"
          "border-radius:6px;background:#164e83;color:white;margin-top:20px;cursor:pointer}"
          "p,small{line-height:1.5}small{display:block;color:#465a6c}details{margin-top:22px}"
          "a{color:#164e83}.notice{background:#edf4fb;padding:12px;border-radius:6px}"
          "</style></head><body><main><h1>";
  html += title;
  html += "</h1>";
  return html;
}
void sendPage(const String &html, int code = 200) {
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("X-Content-Type-Options", "nosniff");
  web.send(code, "text/html; charset=utf-8", html + "</main></body></html>");
}
String token() { return "<input type='hidden' name='token' value='" + csrf + "'>"; }
void input(String &html, const char *label, const char *name, const char *value, unsigned maximum) {
  html += "<label>";
  html += label;
  html += "<input required name='";
  html += name;
  html += "' maxlength='" + String(maximum) + "' value='" + escape(value) + "'></label>";
}
void showForm(const char *error = nullptr) {
  if (readReport().state == State::Running) {
    web.sendHeader("Location", "/status");
    web.send(303);
    return;
  }
  String html = pageStart("Walkie setup");
  html += "<p>Choose the Wi-Fi network your walkie should join and enter its relay address.</p>";
  if (error) html += "<p class='notice'>" + escape(error) + "</p>";
  html += "<form action='/test' method='post'>" + token();
  input(html, "Wi-Fi network name (SSID)", "ssid", candidate.ssid, 32);
  html += "<label>Wi-Fi password<input id='wifi-password' type='password' name='password' maxlength='64' "
          "autocomplete='new-password'></label>"
          "<button type='button' aria-controls='wifi-password' aria-pressed='false' "
          "onclick=\"const field=document.getElementById('wifi-password');"
          "const visible=field.type==='password';field.type=visible?'text':'password';"
          "this.textContent=visible?'Hide password':'Show password';"
          "this.setAttribute('aria-pressed',String(visible));\">Show password</button>"
          "<small>Leave blank to keep the existing password "
          "for the same network. Enter a password when changing networks.</small>"
          "<label><input type='checkbox' name='open' value='1'> This network has no password</label>";
  String url = *candidate.host ? relayUrl(candidate) : String("ws://192.168.1.42:8765/");
  input(html, "Relay address", "relay", url.c_str(), 520);
  html += "<small>Example: ws://192.168.1.42:8765/</small><details><summary>Advanced: device pairing</summary>";
  input(html, "This walkie's device ID", "device", candidate.device, 64);
  input(html, "Peer device ID", "peer", candidate.peer, 64);
  html += "<small>The other device must select this walkie's ID as its peer.</small></details>"
          "<button type='submit'>Test connection</button></form>"
          "<p><small>Settings are saved only when you choose Save and start. "
          "To return here later, hold PTT while powering on for five seconds.</small></p>";
  sendPage(html, error ? 400 : 200);
}
bool authorized() {
  if (!web.hasArg("token") || web.arg("token") != csrf) {
    web.send(403, "text/plain", "Reload the setup page and try again.");
    return false;
  }
  return true;
}
template <size_t N> bool field(const char *name, char (&dest)[N]) {
  String value = web.arg(name);
  // Reject embedded NUL rather than silently truncating network credentials.
  if (value.length() != strlen(value.c_str())) return false;
  return copySetting(dest, value.c_str());
}

void probeTask(void *) {
  // Candidate is frozen until the final report is published. No HTTP handler can
  // edit/save it while Running. This task owns all station/probe socket activity.
  Settings settings = candidate;
  WiFi.disconnect(false, false);
  WiFi.begin(settings.ssid, settings.password);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && uint32_t(millis() - started) < 15000)
    vTaskDelay(pdMS_TO_TICKS(50));
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    publish(State::WifiFailed, "Could not join Wi-Fi. Check the network name/password and 2.4 GHz availability. Nothing was saved.");
    vTaskDelete(nullptr);
    return;
  }
  publish(State::Running, "Wi-Fi connected. Checking relay registration...");
  bool accepted = false, failed = false;
  char detail[192] = "Wi-Fi connected, but the relay could not be reached. Check its address, port, and firewall.";
  {
    WebSocketsClient probe;
    probe.begin(settings.host, settings.port, settings.path, "");
    probe.setReconnectInterval(1000);
    probe.onEvent([&](WStype_t type, uint8_t *payload, size_t length) {
      if (accepted || failed) return;
      if (type == WStype_CONNECTED) {
        char hello[240];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"protocol\":1,\"device_id\":\"%s\",\"peer_id\":\"%s\"}",
                 settings.device, settings.peer);
        if (!probe.sendTXT(hello)) failed = true;
      } else if (type == WStype_TEXT && length <= 4096) {
        cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(payload), length);
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "type");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "protocol");
        if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "hello_ok") &&
            cJSON_IsNumber(version) && version->valuedouble == 1) {
          accepted = true;
          snprintf(detail, sizeof(detail), "Wi-Fi connected and relay registration accepted. Ready to save.");
        } else {
          const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
          if (cJSON_IsString(kind) && !strcmp(kind->valuestring, "error") && cJSON_IsString(reason))
            snprintf(detail, sizeof(detail), "Wi-Fi connected. Relay rejected registration: %.100s", reason->valuestring);
          else snprintf(detail, sizeof(detail), "Wi-Fi connected. The endpoint did not accept walkie protocol version 1.");
          failed = true;
        }
        cJSON_Delete(root);
      } else if (type == WStype_BIN || type == WStype_ERROR) failed = true;
    });
    started = millis();
    while (!accepted && !failed && uint32_t(millis() - started) < 10000) {
      probe.loop();  // Can block; the portal continues on the Arduino setup task.
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    probe.disconnect();  // End temporary registration before operational connection.
  }
  // Publish only after the probe socket is gone, so Save/new Test cannot race it.
  publish(accepted ? State::Ready : State::RelayFailed, detail);
  vTaskDelete(nullptr);
}
void test() {
  if (!authorized()) return;
  if (readReport().state == State::Running) { web.send(409, "text/plain", "A connection test is already running."); return; }
  Settings next;
  if (!field("ssid", next.ssid) || !field("device", next.device) || !field("peer", next.peer)) {
    showForm("A field is missing or too long.");
    return;
  }
  if (!field("password", next.password)) { showForm("Wi-Fi password is too long."); return; }
  if (web.arg("open") == "1") next.password[0] = '\0';
  else if (!*next.password && !strcmp(next.ssid, candidate.ssid)) copySetting(next.password, candidate.password);
  else if (!*next.password) { showForm("Enter the Wi-Fi password, or select that the network has no password."); return; }
  String relay = web.arg("relay");
  if (relay.length() > 520 || relay.length() != strlen(relay.c_str())) { showForm("Invalid relay address length."); return; }
  const char *error = parseRelay(relay.c_str(), next);
  if (!error) error = validateSettings(next);
  if (error) { showForm(error); return; }
  candidate = next;
  // Invalidate forms from older tests/tabs, especially their Save actions.
  char nextToken[33];
  snprintf(nextToken, sizeof(nextToken), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
  csrf = nextToken;
  publish(State::Running, "Connecting to Wi-Fi. This may take about 30 seconds; stay on the setup network.");
  if (xTaskCreatePinnedToCore(probeTask, "setup-test", 12288, nullptr, 2, nullptr, 0) != pdPASS)
    publish(State::WifiFailed, "Could not start a connection test. Reset the walkie and try again. Nothing was saved.");
  web.sendHeader("Location", "/status");
  web.send(303);
}
void status() {
  Report result = readReport();
  String html = pageStart("Connection test", result.state == State::Running);
  html += "<p class='notice'>" + escape(result.detail) + "</p><p>Network: " + escape(candidate.ssid) +
          "<br>Relay: " + escape(relayUrl(candidate).c_str()) + "</p>";
  if (result.state == State::Ready || result.state == State::RelayFailed) {
    if (result.state == State::RelayFailed)
      html += "<p>You can save anyway if the relay is switched off, or return to edit its address.</p>";
    html += "<form action='/save' method='post'>" + token() +
            "<button type='submit'>Save and start</button></form>";
  }
  if (result.state != State::Running) html += "<p><a href='/'>Edit settings or test again</a></p>";
  else html += "<p>This page refreshes automatically. If your phone disconnects while Wi-Fi changes channel, "
               "reconnect to Walkie-Setup and reopen http://walkie.setup.</p>";
  sendPage(html);
}
void finish() {
  if (!authorized()) return;
  State state = readReport().state;
  if (state != State::Ready && state != State::RelayFailed) {
    web.send(409, "text/plain", "Test the Wi-Fi connection before saving.");
    return;
  }
  if (validateSettings(candidate) || !save(candidate)) {
    sendPage(pageStart("Could not save") + "<p>Storage failed. Previous settings are unchanged.</p><a href='/'>Try again</a>", 500);
    return;
  }
  sendPage(pageStart("Settings saved") + "<p>Your walkie is starting. The setup network will close; "
           "reconnect your phone to its normal Wi-Fi.</p><p>Release PTT before speaking.</p>"
           "<h2>To change settings later</h2>"
           "<ol><li>Hold the external push-to-talk (PTT) button.</li>"
           "<li>While holding PTT, press and release RESET, or power the walkie off and back on.</li>"
           "<li>Keep holding PTT for five seconds after it powers on, then release it.</li>"
           "<li>Connect to the Walkie-Setup Wi-Fi network. Its password is <strong>" +
           escape(WALKIE_SETUP_PASSWORD) + "</strong>.</li>"
           "<li>Open <a href='http://walkie.setup'>http://walkie.setup</a>. "
           "If needed, use <a href='http://192.168.4.1'>http://192.168.4.1</a>.</li></ol>"
           "<p>Use the external PTT button, not the board's BOOT/0 button.</p>");
  finished = true;
}
void defaults(Settings &settings) {
  settings = Settings{};
  if (strcmp(WALKIE_WIFI_SSID, "YOUR_WIFI_SSID")) {
    copySetting(settings.ssid, WALKIE_WIFI_SSID);
    copySetting(settings.password, WALKIE_WIFI_PASSWORD);
  }
  copySetting(settings.host, WALKIE_RELAY_HOST);
  copySetting(settings.path, WALKIE_RELAY_PATH);
  copySetting(settings.device, WALKIE_DEVICE_ID);
  copySetting(settings.peer, WALKIE_PEER_ID);
  settings.port = WALKIE_RELAY_PORT;
}
}  // namespace

bool configureWalkie(walkie::Settings &settings, bool forceSetup) {
  bool saved = load(settings);
  if (saved && !forceSetup) return true;
  if (saved) candidate = settings;
  else defaults(candidate);
  WiFi.persistent(false);  // Credentials are committed explicitly as one Preferences blob.
  WiFi.mode(WIFI_AP_STA);  // Radio entropy is available before generating form tokens.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  const char *password = WALKIE_SETUP_PASSWORD;
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06lX", (unsigned long)(ESP.getEfuseMac() >> 24) & 0xffffffUL);
  String name = String("Walkie-Setup-") + suffix;
  char nonce[33];
  snprintf(nonce, sizeof(nonce), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
  csrf = nonce;
  finished = false;
  publish(State::Idle, "Enter settings, then test the connection.");
  // Advertise the local DNS server explicitly through DHCP, not only the gateway.
  if (!WiFi.softAPConfig(AP_ADDRESS, AP_ADDRESS, AP_MASK, IPAddress(192, 168, 4, 2), AP_ADDRESS) ||
      !WiFi.softAP(name.c_str(), password))
    return false;
  if (!dns.start(53, "*", AP_ADDRESS)) { WiFi.softAPdisconnect(true); return false; }
  web.on("/", HTTP_GET, []() { showForm(); });
  web.on("/test", HTTP_POST, test);
  web.on("/status", HTTP_GET, status);
  web.on("/save", HTTP_POST, finish);
  web.onNotFound([]() {
    web.sendHeader("Location", "http://walkie.setup/");
    web.send(302, "text/plain", "Open http://walkie.setup while connected to this setup network.");
  });
  web.begin();
  uint32_t announced = millis() - 10000;
  while (!finished) {
    web.handleClient();
    dns.processNextRequest();
    // Repeat because Serial Monitor may attach after the AP has already started.
    if (uint32_t(millis() - announced) >= 10000) {
      announced = millis();
      Serial.printf("SETUP AP: %s\nSETUP AP password: %s\nOpen http://walkie.setup or http://192.168.4.1\n",
                    name.c_str(), password);
    }
    delay(5);
  }
  delay(750);  // Let the HTTP success response reach the phone before closing the AP.
  web.stop();
  dns.stop();
  WiFi.disconnect(false, false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  settings = candidate;
  Serial.println("Settings saved; setup AP stopped");
  return true;
}
