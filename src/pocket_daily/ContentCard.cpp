#include "ContentCard.h"

#include <cstring>

#include "ContentChecksum.h"

namespace PocketDaily::Content {
namespace {
uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool validText(const char* text, size_t capacity, bool required, bool multiline) {
  const size_t length = strnlen(text, capacity);
  if (length == capacity || (required && !length)) return false;
  for (size_t i = length + 1; i < capacity; ++i) {
    if (text[i]) return false;
  }
  // Strict UTF-8: reject overlong encodings, surrogates, non-Unicode values,
  // truncation and controls; LF is allowed only in body/secondary text.
  for (size_t i = 0; i < length;) {
    uint32_t point = static_cast<uint8_t>(text[i++]);
    unsigned continuation = 0;
    uint32_t minimum = 0;
    if (point >= 0xC2 && point <= 0xDF) {
      point &= 0x1F;
      continuation = 1;
      minimum = 0x80;
    } else if (point >= 0xE0 && point <= 0xEF) {
      point &= 0x0F;
      continuation = 2;
      minimum = 0x800;
    } else if (point >= 0xF0 && point <= 0xF4) {
      point &= 7;
      continuation = 3;
      minimum = 0x10000;
    } else if (point >= 0x80) {
      return false;
    }
    if (continuation > length - i) return false;
    for (unsigned j = 0; j < continuation; ++j) {
      const uint8_t next = static_cast<uint8_t>(text[i++]);
      if ((next & 0xC0) != 0x80) return false;
      point = (point << 6) | (next & 0x3F);
    }
    if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
    if ((point < 0x20 && !(multiline && point == '\n')) || (point >= 0x7F && point <= 0x9F)) return false;
  }
  return true;
}

bool readPart(const ManifestSource& source, size_t offset, void* output, size_t size, uint32_t& crc) {
  auto* bytes = static_cast<uint8_t*>(output);
  if (!source.read(source.context, offset, bytes, size)) return false;
  crc = contentCrcUpdate(crc, bytes, size);
  return true;
}

CardResult decode(const ManifestSource& source, ContentCard& output) {
  if (source.size != CONTENT_CARD_BYTES || !source.read) return CardResult::Shape;
  uint8_t header[16];
  uint32_t crc = 0xFFFFFFFFu;
  if (!readPart(source, 0, header, sizeof(header), crc)) return CardResult::ReadFailed;
  if (memcmp(header, "PDCT", 4) || (header[4] != 1 && header[4] != 2) || header[5] || header[6] != 16 || header[7] ||
      u32(header + 8) != CONTENT_CARD_BYTES || u32(header + 12))
    return CardResult::Shape;
  // Local namespace convention, not sender authentication. module/app is
  // distinct from module/local, which is reserved for the built-in study deck.
  memcpy(output.card.cardId, "app:", 4);
  if (!readPart(source, 16, output.card.cardId + 4, 33, crc) ||
      !readPart(source, 49, output.card.title, sizeof(output.card.title), crc) ||
      !readPart(source, 74, output.card.question, sizeof(output.card.question), crc) ||
      !readPart(source, 235, output.card.context, sizeof(output.card.context), crc) ||
      !readPart(source, 427, output.imagePath, sizeof(output.imagePath), crc))
    return CardResult::ReadFailed;
  if (!validText(output.card.cardId + 4, 33, true, false)) return CardResult::Identifier;
  for (const char* p = output.card.cardId + 4; *p; ++p) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
      return CardResult::Identifier;
  }
  if (!validText(output.card.title, sizeof(output.card.title), true, false) ||
      !validText(output.card.question, sizeof(output.card.question), true, true) ||
      !validText(output.card.context, sizeof(output.card.context), false, true))
    return CardResult::Text;
  if (output.imagePath[0]) {
    if (!validContentFileName(output.imagePath, FileKind::MonoImage)) return CardResult::ImagePath;
  } else {
    for (const char byte : output.imagePath) {
      if (byte) return CardResult::ImagePath;
    }
  }
  uint8_t reserved[17];
  if (!readPart(source, 491, reserved, sizeof(reserved), crc)) return CardResult::ReadFailed;
  if (header[4] == 2) {
    if (reserved[0] < 1 || reserved[0] > 2) return CardResult::Shape;
    output.layout = static_cast<CardLayout>(reserved[0]);
    reserved[0] = 0;
  }
  for (const auto byte : reserved) {
    if (byte) return CardResult::Shape;
  }
  uint8_t trailer[4];
  if (!source.read(source.context, 508, trailer, sizeof(trailer))) return CardResult::ReadFailed;
  if (u32(trailer) != (crc ^ 0xFFFFFFFFu)) return CardResult::Checksum;
  strcpy(output.card.module, "app");
  strcpy(output.card.actionClass, "info");
  return CardResult::Ok;
}
}  // namespace

CardResult decodeContentCard(const ManifestSource& source, ContentCard& output) {
  memset(&output, 0, sizeof(output));
  const auto result = decode(source, output);
  if (result != CardResult::Ok) memset(&output, 0, sizeof(output));
  return result;
}
}  // namespace PocketDaily::Content
