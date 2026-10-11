#pragma once

#include <cstring>

#include "ReadingSyncProtocol.h"

namespace Pocket::NearbySync::WifiSetup {
enum class Route : uint32_t { None, Bluetooth, Join };
enum class Result : uint32_t { None, Pending, Saved, Failed, SaveFailed };
// One bounded RTC ticket, never SD/NVS staging. Only an intentional software
// restart may use credentials. Successful/failed association erases them.
struct Ticket {
  uint32_t magic = 0;
  Route route = Route::None;
  Result result = Result::None;
  char requestId[9] = {};
  char ssid[33] = {};
  char password[65] = {};
  uint32_t checksum = 0;
};
static_assert(sizeof(Ticket) <= 128);

inline void erase(void* data, size_t size) {
  auto* bytes = static_cast<volatile unsigned char*>(data);
  while (size--) *bytes++ = 0;
}
inline void seal(Ticket& ticket) {
  ticket.magic = 0x57494631;
  ticket.checksum = crcFinish(crcUpdate(CRC_START, &ticket, offsetof(Ticket, checksum)));
}
inline bool valid(const Ticket& ticket) {
  return ticket.magic == 0x57494631 &&
         ticket.checksum == crcFinish(crcUpdate(CRC_START, &ticket, offsetof(Ticket, checksum)));
}
inline bool decodeHex(const char* text, size_t length, char* out, size_t capacity) {
  if (!length || length % 2 || length / 2 >= capacity) return false;
  for (size_t i = 0; i < length; i += 2) {
    unsigned value = 0;
    for (size_t j = 0; j < 2; ++j) {
      const char c = text[i + j];
      if (c >= '0' && c <= '9')
        value = value * 16 + c - '0';
      else if (c >= 'A' && c <= 'F')
        value = value * 16 + c - 'A' + 10;
      else
        return false;
    }
    if (value < 32 || value == 127) return false;
    out[i / 2] = static_cast<char>(value);
  }
  out[length / 2] = '\0';
  return true;
}
inline bool stage(Ticket& ticket, const ParsedCommand& command) {
  erase(&ticket, sizeof(ticket));
  if (command.verb != Verb::WIFI_JOIN || !command.chunk) return false;
  const auto* separator = static_cast<const char*>(memchr(command.chunk, ' ', command.chunkLength));
  if (!separator) return false;
  const size_t ssidLength = separator - command.chunk;
  const size_t passwordLength = command.chunkLength - ssidLength - 1;
  if (!decodeHex(command.chunk, ssidLength, ticket.ssid, sizeof(ticket.ssid))) return false;
  if (!(passwordLength == 1 && separator[1] == '-')) {
    if (!decodeHex(separator + 1, passwordLength, ticket.password, sizeof(ticket.password))) return false;
    const size_t length = passwordLength / 2;
    if (length < 8) return false;
    for (size_t i = 0; i < length; ++i) {
      const unsigned char c = ticket.password[i];
      if (c > 126 || (length == 64 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))))
        return false;
    }
  }
  memcpy(ticket.requestId, command.requestId, sizeof(ticket.requestId));
  ticket.route = Route::Join;
  ticket.result = Result::Pending;
  seal(ticket);
  return true;
}
inline Route boot(Ticket& ticket, bool softwareRestart) {
  if (!softwareRestart || !valid(ticket)) {
    erase(&ticket, sizeof(ticket));
    return Route::None;
  }
  const auto route = ticket.route;
  ticket.route = Route::None;
  seal(ticket);
  return route;
}
inline void finish(Ticket& ticket, Result result) {
  erase(ticket.ssid, sizeof(ticket.ssid));
  erase(ticket.password, sizeof(ticket.password));
  ticket.result = result;
  ticket.route = Route::None;
  seal(ticket);
}
}  // namespace Pocket::NearbySync::WifiSetup
