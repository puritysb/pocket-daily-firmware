#include <zlib.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>

#include "Epub.h"

namespace {
uint16_t le16(const std::vector<uint8_t>& b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }
uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
  return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
         (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
std::string attr(const std::string& tag, const char* name) {
  const std::regex pattern(std::string("\\b") + name + "\\s*=\\s*[\"']([^\"']*)[\"']");
  std::smatch match;
  return std::regex_search(tag, match, pattern) ? match[1].str() : std::string();
}
}  // namespace

Epub::Epub(std::string path) : path(std::move(path)) {}

bool Epub::load() {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  bytes.assign(std::istreambuf_iterator<char>(file), {});
  if (bytes.size() < 22) return false;
  size_t eocd = bytes.size() - 22;
  while (eocd > 0 && le32(bytes, eocd) != 0x06054b50) --eocd;
  if (le32(bytes, eocd) != 0x06054b50) return false;
  const uint16_t count = le16(bytes, eocd + 10);
  size_t at = le32(bytes, eocd + 16);
  for (uint16_t i = 0; i < count; ++i) {
    if (at + 46 > bytes.size() || le32(bytes, at) != 0x02014b50) return false;
    Entry entry;
    entry.method = le16(bytes, at + 10);
    entry.compressedSize = le32(bytes, at + 20);
    entry.size = le32(bytes, at + 24);
    const uint16_t nameLength = le16(bytes, at + 28);
    const uint16_t extra = le16(bytes, at + 30);
    const uint16_t comment = le16(bytes, at + 32);
    entry.localHeader = le32(bytes, at + 42);
    entry.name.assign(reinterpret_cast<const char*>(&bytes[at + 46]), nameLength);
    entries.push_back(entry);
    at += 46 + nameLength + extra + comment;
  }

  std::string container;
  if (!readItem("META-INF/container.xml", container)) return false;
  const std::string opfPath = attr(container.substr(container.find("<rootfile")), "full-path");
  std::string opf;
  if (!readItem(opfPath, opf)) return false;
  const std::string base = opfPath.find('/') == std::string::npos ? "" : opfPath.substr(0, opfPath.rfind('/') + 1);

  std::map<std::string, std::string> manifest;
  const std::regex itemTag("<item\\b[^>]*>");
  for (auto it = std::sregex_iterator(opf.begin(), opf.end(), itemTag); it != std::sregex_iterator(); ++it) {
    manifest[attr(it->str(), "id")] = base + attr(it->str(), "href");
  }
  const std::regex itemrefTag("<itemref\\b[^>]*>");
  uint32_t cumulative = 0;
  for (auto it = std::sregex_iterator(opf.begin(), opf.end(), itemrefTag); it != std::sregex_iterator(); ++it) {
    const auto found = manifest.find(attr(it->str(), "idref"));
    if (found == manifest.end()) return false;
    size_t size = 0;
    if (!getItemSize(found->second, &size)) return false;
    cumulative += static_cast<uint32_t>(size);
    spine.push_back({found->second, cumulative});
  }
  return !spine.empty();
}

std::shared_ptr<Epub> Epub::fromSpine(const std::vector<std::string>& items) {
  auto epub = std::make_shared<Epub>("memory");
  uint32_t cumulative = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    const std::string href = "mem/" + std::to_string(i) + ".xhtml";
    epub->memory[href] = items[i];
    cumulative += static_cast<uint32_t>(items[i].size());
    epub->spine.push_back({href, cumulative});
  }
  return epub;
}

bool Epub::readItem(const std::string& href, std::string& out) const {
  if (const auto found = memory.find(href); found != memory.end()) {
    out = found->second;
    return true;
  }
  for (const auto& entry : entries) {
    if (entry.name != href) continue;
    const size_t local = entry.localHeader;
    if (local + 30 > bytes.size() || le32(bytes, local) != 0x04034b50) return false;
    const size_t data = local + 30 + le16(bytes, local + 26) + le16(bytes, local + 28);
    if (data + entry.compressedSize > bytes.size()) return false;
    out.assign(entry.size, '\0');
    if (entry.method == 0) {
      std::memcpy(out.data(), &bytes[data], entry.size);
      return true;
    }
    if (entry.method != 8) return false;
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
    stream.next_in = const_cast<Bytef*>(&bytes[data]);
    stream.avail_in = entry.compressedSize;
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = entry.size;
    const int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    return status == Z_STREAM_END && stream.total_out == entry.size;
  }
  return false;
}

bool Epub::readItemContentsToStream(const std::string& href, Print& out, const size_t chunkSize) const {
  std::string data;
  if (!readItem(href, data)) return false;
  for (size_t at = 0; at < data.size(); at += chunkSize) {
    const size_t length = std::min(chunkSize, data.size() - at);
    if (out.write(reinterpret_cast<const uint8_t*>(data.data() + at), length) != length) return false;
  }
  return true;
}

bool Epub::getItemSize(const std::string& href, size_t* size) const {
  if (const auto found = memory.find(href); found != memory.end()) {
    *size = found->second.size();
    return true;
  }
  for (const auto& entry : entries) {
    if (entry.name == href) {
      *size = entry.size;
      return true;
    }
  }
  return false;
}

float Epub::calculateProgress(const int spineIndex, const float spineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) return 0.0f;
  const size_t previous = spineIndex >= 1 ? getCumulativeSpineItemSize(spineIndex - 1) : 0;
  const size_t current = getCumulativeSpineItemSize(spineIndex) - previous;
  return (static_cast<float>(previous) + spineRead * static_cast<float>(current)) / static_cast<float>(bookSize);
}
