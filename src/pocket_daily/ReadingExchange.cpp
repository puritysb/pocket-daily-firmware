#include "ReadingExchange.h"

#include <HalStorage.h>
#include <HalSystem.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "Epub.h"
#include "FsHelpers.h"
#include "RecentBooksStore.h"

namespace PocketDaily::ReadingProgress {
namespace {
bool copyDigest(const std::string& digest, char (&out)[DIGEST_HEX + 1]) {
  if (digest.size() != DIGEST_HEX || !validDigest(digest.c_str())) return false;
  memcpy(out, digest.c_str(), DIGEST_HEX + 1);
  return true;
}
}  // namespace

bool loadExchangeBook(const RecentBook& recent, ExchangeWork& work, ExchangeBook& book) {
  if (!FsHelpers::hasEpubExtension(recent.path)) return false;
  size_t size = 0;
  {
    HalFile file = Storage.open(recent.path.c_str(), O_RDONLY);
    if (!file || file.isDirectory()) return false;
    size = file.size();
  }
  book.cachePath = Epub(recent.path, "/.crosspoint").getCachePath();
  book.hasRecord = loadRecord(book.cachePath.c_str(), work.record, work.scratch);
  // progress.bin: spine u16, page u16, page count u16, book percent u8 (EpubReaderUtils::saveProgress).
  uint8_t saved[7] = {};
  int savedBytes = 0;
  {
    HalFile progress = Storage.open((book.cachePath + "/progress.bin").c_str(), O_RDONLY);
    if (progress) savedBytes = progress.read(saved, sizeof(saved));
  }
  const bool hasSaved = savedBytes >= 4;
  const uint16_t spine = static_cast<uint16_t>(saved[0] | (saved[1] << 8));
  const uint16_t page = static_cast<uint16_t>(saved[2] | (saved[3] << 8));
  book.current = book.hasRecord && (!hasSaved || (work.record.spine == spine && work.record.page == page));
  if (book.current) {
    book.percentage = work.record.percentage;
  } else if (savedBytes >= 7 && saved[6] <= 100) {
    book.percentage = static_cast<float>(saved[6]) / 100.0f;
  }
  if (!(book.hasRecord && work.record.document[0] && work.record.fileSize == size &&
        copyDigest(work.record.document, book.document)) &&
      !copyDigest(KOReaderDocumentId::calculate(recent.path), book.document)) {
    return false;
  }
  if (!copyDigest(KOReaderDocumentId::calculateFromFilename(recent.path), book.filenameDocument)) {
    book.filenameDocument[0] = '\0';
  }
  return true;
}

ListStream::ListStream(const char* id, const bool includePaths, ExchangeWork& work)
    : includePaths(includePaths), work(work) {
  snprintf(deviceId, sizeof(deviceId), "%s", id ? id : "");
}

size_t ListStream::next(char* out, const size_t capacity) {
  switch (phase) {
    case Phase::HEAD:
      phase = Phase::BOOKS;
      return composer.head(deviceId, out, capacity);
    case Phase::BOOKS: {
      const auto& books = RECENT_BOOKS.getBooks();
      while (index < books.size() && !composer.full()) {
        const RecentBook& recent = books[index++];
        HalSystem::feedWatchdogIfRegistered();
        ExchangeBook book;
        if (!loadExchangeBook(recent, work, book)) continue;
        ListEntry entry;
        entry.path = includePaths ? recent.path.c_str() : nullptr;
        entry.document = book.document;
        entry.filenameDocument = book.filenameDocument;
        entry.xpointer = book.current ? work.record.xpointer : "";
        entry.percentage = book.percentage;
        entry.updated = book.current ? work.record.updated : 0;
        entry.seq = book.current ? work.record.seq : 0;
        const size_t length = composer.entry(entry, out, capacity);
        if (length) return length;
      }
      phase = Phase::DONE;
      return composer.tail(out, capacity);
    }
    case Phase::DONE:
      return 0;
  }
  return 0;
}

OfferStoreResult storeOffer(const OfferRequest& request, ExchangeWork& work) {
  for (const auto& recent : RECENT_BOOKS.getBooks()) {
    HalSystem::feedWatchdogIfRegistered();
    ExchangeBook book;
    if (!loadExchangeBook(recent, work, book) ||
        (strcmp(book.document, request.document) != 0 && strcmp(book.filenameDocument, request.document) != 0)) {
      continue;
    }
    if (request.offer.readerSeq != 0 && (!book.current || request.offer.readerSeq != work.record.seq)) {
      return OfferStoreResult::STALE_POSITION;
    }
    // Only a pending offer: the reader asks before moving when the book opens.
    if (!saveOffer(book.cachePath.c_str(), request.offer, work.scratch)) return OfferStoreResult::FAILED;
    LOG_DBG("RPS", "Reading offer %.4f from %s stored for %s", static_cast<double>(request.offer.percentage),
            request.offer.device, recent.path.c_str());
    return OfferStoreResult::STORED;
  }
  return OfferStoreResult::UNKNOWN_DOCUMENT;
}

}  // namespace PocketDaily::ReadingProgress
