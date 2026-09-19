#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <algorithm>
#include <cstring>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

OpdsParser::OpdsParser(AllocationCheck allocationCheck) : allocationCheck(allocationCheck) {
  if (!ensureAllocation(2048)) return;
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    resourceLimited = errorOccured = true;
    return;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return 0;

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 256;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    if (!ensureAllocation(toRead + 1024)) return 0;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      resourceLimited = errorOccured = true;
      destroyXmlParser(parser);
      return 0;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      resourceLimited = resourceLimited || XML_GetErrorCode(parser) == XML_ERROR_NO_MEMORY;
      if (!resourceLimited) {
        LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
                XML_ErrorString(XML_GetErrorCode(parser)));
      }
      destroyXmlParser(parser);
      return 0;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    resourceLimited = resourceLimited || XML_GetErrorCode(parser) == XML_ERROR_NO_MEMORY;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->errorOccured) return;

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        if (strstr(href, "{searchTerms}") != nullptr && !self->assignText(self->searchTemplate, href)) return;
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        if (!self->assignText(self->nextPageUrl, href)) return;
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        if (!self->assignText(self->prevPageUrl, href)) return;
      }

      if (self->inEntry) {
        if (rel && type && strstr(rel, "opds-spec.org/acquisition") != nullptr &&
            strcmp(type, "application/epub+zip") == 0) {
          // Prefer plain EPUB links over derived formats when multiple
          // acquisition links are present for one entry.
          const bool isPlainEpub = strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr;
          const bool alreadyHasPlainEpub = self->currentEntry.type == OpdsEntryType::BOOK &&
                                           (self->currentEntry.href.find(".epub") != std::string::npos ||
                                            self->currentEntry.href.find("/epub/") != std::string::npos);
          if (self->currentEntry.type != OpdsEntryType::BOOK || (isPlainEpub && !alreadyHasPlainEpub)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            if (!self->assignText(self->currentEntry.href, href)) return;
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            if (!self->assignText(self->currentEntry.href, href)) return;
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->errorOccured) return;

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      if (self->entries.size() >= MAX_ENTRIES || !self->reserveEntries(self->entries.size() + 1)) {
        self->failResourceLimit();
        return;
      }
      self->entries.push_back(std::move(self->currentEntry));
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = std::move(self->currentText);
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = std::move(self->currentText);
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = std::move(self->currentText);
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->errorOccured) return;
  if (self->inTitle || self->inAuthorName || self->inId) {
    if (len <= 0 || !self->reserveText(self->currentText, self->currentText.size() + static_cast<size_t>(len))) return;
    self->currentText.append(s, static_cast<size_t>(len));
  }
}

void OpdsParser::failResourceLimit() {
  resourceLimited = errorOccured = true;
  if (parser) XML_StopParser(parser, XML_FALSE);
}

bool OpdsParser::ensureAllocation(size_t bytes) {
  if (errorOccured) return false;
  bool available = !allocationCheck || allocationCheck(bytes);
#ifdef ARDUINO
  // Keep radio/event and UI headroom as well as one contiguous allocation.
  available = available && heap_caps_get_free_size(MALLOC_CAP_8BIT) >= bytes + 4096 &&
              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= bytes + 512;
#endif
  if (!available) failResourceLimit();
  return available;
}

bool OpdsParser::reserveText(std::string& text, size_t length) {
  if (length > MAX_FIELD_BYTES) {
    failResourceLimit();
    return false;
  }
  if (length <= text.capacity()) return true;
  const size_t capacity = std::max(length, text.capacity() * 2);
  if (!ensureAllocation(capacity + 1)) return false;
  text.reserve(capacity);
  return true;
}

bool OpdsParser::assignText(std::string& text, const char* value) {
  const size_t length = strnlen(value, MAX_FIELD_BYTES + 1);
  if (!reserveText(text, length)) return false;
  text.assign(value, length);
  return true;
}

bool OpdsParser::reserveEntries(size_t count) {
  if (count <= entries.capacity()) return true;
  // Small fixed growth steps avoid a doubling peak on the no-PSRAM reader.
  const size_t capacity = std::min(MAX_ENTRIES + 2, ((count + 7) / 8) * 8);
  if (count > capacity || !ensureAllocation(capacity * sizeof(OpdsEntry))) return false;
  entries.reserve(capacity);
  return true;
}

bool OpdsParser::reserveNavigationEntries() { return !errorOccured && reserveEntries(entries.size() + 2); }
