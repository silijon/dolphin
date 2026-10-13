#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace walkie {
// A versioned, single NVS blob avoids partially saving related fields.
struct Settings {
  uint32_t version = 1;
  char ssid[33] = {};
  char password[65] = {};
  char host[254] = {};
  char path[256] = {};
  char device[65] = {};
  char peer[65] = {};
  uint16_t port = 80;
};

template <size_t N> bool copySetting(char (&dest)[N], const char *value) {
  size_t length = strlen(value);
  if (length >= N) return false;
  memcpy(dest, value, length + 1);
  return true;
}
template <size_t N> bool terminated(const char (&value)[N]) {
  return memchr(value, '\0', N) != nullptr;
}
inline bool validId(const char *id) {
  size_t length = strlen(id);
  if (!length || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}
inline bool validHost(const char *host) {
  if (!*host || strlen(host) > 253) return false;
  for (const char *p = host; *p; ++p) {
    char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_')) return false;
  }
  return true;
}
inline bool validPath(const char *path) {
  if (*path != '/' || strlen(path) > 255) return false;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(path); *p; ++p)
    if (*p <= 32 || *p >= 127 || *p == '#' || *p == '\\') return false;
  return true;
}
inline const char *validateSettings(const Settings &s) {
  if (s.version != 1 || !terminated(s.ssid) || !terminated(s.password) ||
      !terminated(s.host) || !terminated(s.path) || !terminated(s.device) || !terminated(s.peer))
    return "Invalid saved settings format.";
  if (!*s.ssid) return "Enter a Wi-Fi network name.";
  size_t length = strlen(s.password);
  if (length && length < 8) return "Wi-Fi password must be at least 8 characters, or select an open network.";
  if (length == 64) {
    for (const char *p = s.password; *p; ++p)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
        return "A 64-character Wi-Fi key must contain only hexadecimal digits.";
  }
  if (!validHost(s.host) || !s.port || !validPath(s.path)) return "Invalid relay host, port, or path.";
  if (!validId(s.device) || !validId(s.peer) || !strcmp(s.device, s.peer))
    return "Device and peer IDs must differ and use 1-64 letters, digits, hyphens, underscores, or periods.";
  return nullptr;
}
inline const char *parseRelay(const char *url, Settings &s) {
  if (strncmp(url, "ws://", 5)) return "Use a ws:// relay address. This firmware does not configure TLS/wss://.";
  const char *authority = url + 5;
  const char *slash = strchr(authority, '/');
  const char *end = slash ? slash : authority + strlen(authority);
  if (end == authority || size_t(end - authority) > 259) return "Invalid relay address.";
  const char *colon = static_cast<const char *>(memchr(authority, ':', end - authority));
  size_t hostLength = (colon ? colon : end) - authority;
  if (!hostLength || hostLength >= sizeof(s.host)) return "Invalid relay hostname.";
  char host[254] = {};
  memcpy(host, authority, hostLength);
  if (!validHost(host)) return "Use a hostname or IPv4 address without credentials or spaces.";
  unsigned port = 80;
  if (colon) {
    if (colon + 1 == end) return "Enter a relay port between 1 and 65535.";
    port = 0;
    for (const char *p = colon + 1; p < end; ++p) {
      if (*p < '0' || *p > '9' || port > 6553) return "Enter a relay port between 1 and 65535.";
      port = port * 10 + unsigned(*p - '0');
      if (port > 65535) return "Enter a relay port between 1 and 65535.";
    }
    if (!port) return "Enter a relay port between 1 and 65535.";
  }
  const char *path = slash ? slash : "/";
  if (!validPath(path)) return "Relay path must start with /, contain no spaces or fragment, and be at most 255 bytes.";
  copySetting(s.host, host);
  copySetting(s.path, path);
  s.port = uint16_t(port);
  return nullptr;
}
}  // namespace walkie
