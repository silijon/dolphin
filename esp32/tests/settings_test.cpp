#include <assert.h>
#include <stdio.h>
#include "../walkie_esp32/settings.h"
using namespace walkie;

static Settings valid() {
  Settings s;
  copySetting(s.ssid, "Kitchen Wi-Fi");
  copySetting(s.password, "space & quote ' password");
  copySetting(s.device, "esp32-a");
  copySetting(s.peer, "laptop-b");
  assert(!parseRelay("ws://192.168.1.42:8765/", s));
  assert(!validateSettings(s));
  return s;
}
int main() {
  Settings s = valid();
  assert(s.port == 8765 && !strcmp(s.host, "192.168.1.42") && !strcmp(s.path, "/"));
  assert(!parseRelay("ws://relay.example/path?pair=a&other=b", s));
  assert(s.port == 80 && !strcmp(s.path, "/path?pair=a&other=b"));
  assert(!parseRelay("ws://relay:65535", s) && s.port == 65535 && !strcmp(s.path, "/"));
  const char *bad[] = {
    "", "192.168.1.42:8765", "wss://relay/", "http://relay/", "ws:///",
    "ws://user:password@relay/", "ws://relay:", "ws://relay:0/",
    "ws://relay:65536/", "ws://relay:-1/", "ws://relay:12a/",
    "ws://relay:999999999999999999999999999/", "ws://relay:80:90/",
    "ws://relay/white space", "ws://relay/#fragment", "ws://relay\\path/",
    "ws://[::1]:8765/", "ws://relay?query", "ws://relay/\r\nInjected: header",
  };
  for (const char *url : bad) {
    Settings previous = s;
    assert(parseRelay(url, s));
    assert(!strcmp(s.host, previous.host) && !strcmp(s.path, previous.path) && s.port == previous.port);
  }
  char url[600];
  memset(url, 'a', sizeof(url));
  memcpy(url, "ws://", 5);
  url[sizeof(url) - 1] = '\0';
  assert(parseRelay(url, s));
  memset(url, 'a', sizeof(url));
  memcpy(url, "ws://relay/", 11);
  url[sizeof(url) - 1] = '\0';
  assert(parseRelay(url, s));

  s = valid();
  s.password[0] = '\0';  // Explicit open network is allowed.
  assert(!validateSettings(s));
  copySetting(s.password, "short");
  assert(validateSettings(s));
  memset(s.password, 'f', 64);
  s.password[64] = '\0';
  assert(!validateSettings(s));  // Raw 256-bit PSK.
  s.password[0] = 'g';
  assert(validateSettings(s));
  s = valid();
  copySetting(s.peer, s.device);
  assert(validateSettings(s));
  copySetting(s.peer, "laptop with spaces");
  assert(validateSettings(s));
  s = valid();
  s.ssid[0] = '\0';
  assert(validateSettings(s));
  s = valid();
  memset(s.host, 'x', sizeof(s.host));  // Corrupt saved blob must not over-read.
  assert(validateSettings(s));
  s = valid();
  s.version = 999;
  assert(validateSettings(s));
  char bounded[4] = "old";
  assert(!copySetting(bounded, "long") && !strcmp(bounded, "old"));
  assert(copySetting(bounded, "new") && !strcmp(bounded, "new"));
  puts("ESP32 settings validation tests passed");
}
