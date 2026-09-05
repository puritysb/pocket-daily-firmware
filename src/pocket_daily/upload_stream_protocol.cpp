#include "upload_stream_protocol.h"

#include <cstdio>
#include <cstring>

namespace PocketDaily::UploadStream {
namespace {

bool parseUint32(const char* text, const size_t length, uint32_t& value) {
  if (length == 0 || length > 10) return false;
  uint64_t parsed = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    parsed = parsed * 10U + static_cast<uint64_t>(c - '0');
    if (parsed > 0xFFFFFFFFULL) return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool lineEquals(const char* line, const size_t length, const char* literal) {
  return strlen(literal) == length && memcmp(line, literal, length) == 0;
}

bool keyMatches(const char* line, const size_t length, const char* key, const char*& value, size_t& valueLength) {
  const size_t keyLength = strlen(key);
  if (length < keyLength + 2 || memcmp(line, key, keyLength) != 0 || line[keyLength] != ':' ||
      line[keyLength + 1] != ' ') {
    return false;
  }
  value = line + keyLength + 2;
  valueLength = length - keyLength - 2;
  return true;
}

}  // namespace

HeaderStatus headerStatus(const char* buffer, const size_t length, const size_t capacity) {
  if (!buffer) return HeaderStatus::TooLarge;
  for (size_t i = 1; i < length; ++i) {
    if (buffer[i - 1] == '\n' && buffer[i] == '\n') return HeaderStatus::Complete;
  }
  return length >= capacity ? HeaderStatus::TooLarge : HeaderStatus::Incomplete;
}

bool isStagingPath(const char* path) {
  if (!path || path[0] != '/') return false;
  const size_t length = strlen(path);
  if (length < 2 || length >= MAX_PATH_BYTES || path[length - 1] == '/') return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = path[i];
    if (c == '\r' || c == '\n') return false;
    if (c == '/' && path[i + 1] == '/') return false;
  }
  if (strstr(path, "..") != nullptr) return false;

  const char* name = strrchr(path, '/') + 1;
  constexpr const char* PREFIX = ".pocket-";
  constexpr const char* SUFFIX = ".part";
  const size_t nameLength = strlen(name);
  if (nameLength <= strlen(PREFIX) + strlen(SUFFIX)) return false;
  if (strncmp(name, PREFIX, strlen(PREFIX)) != 0) return false;
  return strcmp(name + nameLength - strlen(SUFFIX), SUFFIX) == 0;
}

bool parseHeader(const char* header, const size_t length, Request& out) {
  if (!header || length < 2 || length > MAX_HEADER_BYTES) return false;
  if (headerStatus(header, length, length + 1) != HeaderStatus::Complete) return false;

  Request request;
  bool sawMagic = false;
  bool sawPath = false;
  bool sawSize = false;
  bool sawResume = false;

  size_t lineStart = 0;
  for (size_t i = 0; i < length; ++i) {
    if (header[i] == '\r') return false;
    if (header[i] != '\n') continue;

    const char* line = header + lineStart;
    const size_t lineLength = i - lineStart;
    lineStart = i + 1;

    if (!sawMagic) {
      if (!lineEquals(line, lineLength, MAGIC)) return false;
      sawMagic = true;
      continue;
    }
    if (lineLength == 0) {
      // Terminator: nothing may follow it in a valid header.
      if (lineStart != length) return false;
      break;
    }

    const char* value = nullptr;
    size_t valueLength = 0;
    if (keyMatches(line, lineLength, "Path", value, valueLength)) {
      if (sawPath || valueLength == 0 || valueLength >= MAX_PATH_BYTES) return false;
      memcpy(request.path, value, valueLength);
      request.path[valueLength] = '\0';
      sawPath = true;
    } else if (keyMatches(line, lineLength, "Size", value, valueLength)) {
      if (sawSize || !parseUint32(value, valueLength, request.size)) return false;
      sawSize = true;
    } else if (keyMatches(line, lineLength, "Resume", value, valueLength)) {
      if (sawResume || !lineEquals(value, valueLength, "1")) return false;
      request.resume = true;
      sawResume = true;
    } else if (memchr(line, ':', lineLength) == nullptr) {
      return false;  // Not a "Key: value" record.
    }
    // Unknown keys are ignored so a future companion can add fields.
  }

  if (!sawMagic || !sawPath || !sawSize || !isStagingPath(request.path)) return false;
  out = request;
  return true;
}

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, size_t length) {
  while (length-- > 0) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return crc;
}

size_t formatOkReply(char* out, const size_t outSize, const uint32_t received, const uint32_t finalizedCrc32) {
  const int written = snprintf(out, outSize, "OK %lu %08lX\n", static_cast<unsigned long>(received),
                               static_cast<unsigned long>(finalizedCrc32));
  return written > 0 && static_cast<size_t>(written) < outSize ? static_cast<size_t>(written) : 0;
}

size_t formatResumeReply(char* out, const size_t outSize, const uint32_t received) {
  const int written = snprintf(out, outSize, "RESUME %lu\n", static_cast<unsigned long>(received));
  return written > 0 && static_cast<size_t>(written) < outSize ? static_cast<size_t>(written) : 0;
}

size_t formatErrorReply(char* out, const size_t outSize, const char* message) {
  const int written = snprintf(out, outSize, "ERROR %s\n", message ? message : "Internal error");
  return written > 0 && static_cast<size_t>(written) < outSize ? static_cast<size_t>(written) : 0;
}

}  // namespace PocketDaily::UploadStream
