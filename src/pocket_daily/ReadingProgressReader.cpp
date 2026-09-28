#include "ReadingProgressReader.h"

#include <Arduino.h>
#include <ChapterXPathResolver.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "PocketGlance.h"
#include "ReadingProgressStore.h"

namespace PocketDaily::ReadingProgress {
namespace {
// Streaming a chapter needs its parser (~8 KB) and, without the reader's inflated
// copy of the chapter, the 32 KB inflate window of ZipFile::readFileToStream.
constexpr size_t XPOINTER_BLOCK_CACHED = 12 * 1024;
constexpr size_t XPOINTER_BLOCK_ZIPPED = 40 * 1024;

struct RecordWork {
  Record previous;
  Record next;
  Scratch scratch;
};

bool chapterHtmlCached(const Epub& epub, const int spine) {
  char path[112];
  const int n = snprintf(path, sizeof(path), "%s/html/%d.html", epub.getCachePath().c_str(), spine);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && Storage.exists(path);
}

size_t fileSize(const std::string& path) {
  HalFile file = Storage.open(path.c_str(), O_RDONLY);
  return file ? file.size() : 0;
}

bool copyText(const std::string& text, char* out, const size_t capacity) {
  if (text.empty() || text.size() >= capacity) return false;
  memcpy(out, text.c_str(), text.size() + 1);
  return true;
}

// `exact` reports an XPointer at the page's first character (not an estimate).
std::string xpointerFor(const std::shared_ptr<Epub>& epub, const ExitPosition& position, bool& exact) {
  exact = false;
  if (position.page == 0) {
    // The top of the chapter; the body pointer is exact there.
    exact = true;
    return "/body/DocFragment[" + std::to_string(position.spine + 1) + "]/body";
  }
  std::string xpath;
  if (position.hasTextOffset) {
    // The character the page starts with (one stream of the chapter).
    xpath = ChapterXPathResolver::findXPathForTextOffset(epub, position.spine, position.textOffset);
    exact = !xpath.empty();
  }
  if (xpath.empty() && position.hasParagraph) {
    xpath = ChapterXPathResolver::findXPathForParagraphProgress(epub, position.spine, position.paragraph,
                                                                position.withinParagraph);
  }
  if (xpath.empty() && position.pageCount > 0) {
    xpath = ChapterXPathResolver::findXPathForProgress(
        epub, position.spine, static_cast<float>(position.page) / static_cast<float>(position.pageCount));
  }
  return xpath;
}
}  // namespace

ExitPosition capturePosition(Section* section, const int spine, int page, const int pageCount) {
  ExitPosition position;
  if (page < 0 || page == UINT16_MAX) page = 0;
  if (pageCount > 0 && page >= pageCount) page = pageCount - 1;
  position.spine = spine;
  position.page = page;
  position.pageCount = std::max(0, pageCount);
  if (section && page > 0) {
    if (const auto offset = section->getPageTextOffset(page)) {
      position.hasTextOffset = true;
      position.textOffset = *offset;
      return position;
    }
  }
  uint16_t paragraph = 0;
  uint16_t first = 0;
  uint16_t last = 0;
  // Fallback estimate. The LUT holds the paragraph open at the end of each page, so the entry of the
  // previous page is the paragraph this page starts in. Pages [first, last] all end
  // inside it; assuming its text spreads evenly and its first and last pages are
  // half filled, this page starts (page - first - 0.5) / (last - first + 1) into it.
  if (section && page > 0 && section->getParagraphRunForPage(static_cast<uint16_t>(page - 1), paragraph, first, last) &&
      paragraph > 0) {
    position.hasParagraph = true;
    position.paragraph = paragraph;
    const float fraction =
        (static_cast<float>(page - first) - 0.5f) / static_cast<float>(static_cast<int>(last) - first + 1);
    position.withinParagraph = std::max(0.0f, std::min(0.999f, fraction));
  }
  return position;
}

void recordPosition(const std::shared_ptr<Epub>& epub, const ExitPosition& position) {
  if (!epub || epub->getSpineItemsCount() <= 0) return;
  [[maybe_unused]] const unsigned long started = millis();  // log only
  auto work = makeUniqueNoThrow<RecordWork>();
  if (!work) {
    LOG_ERR("RPS", "No memory to record the reading position");
    return;
  }
  const char* cachePath = epub->getCachePath().c_str();
  const bool hadRecord = loadRecord(cachePath, work->previous, work->scratch);
  const bool atEnd = position.spine >= epub->getSpineItemsCount();
  // A record whose XPointer was estimated is refreshed once an exact one can be computed.
  const bool previousExact = (work->previous.flags & RECORD_FLAG_EXACT_XPOINTER) != 0;
  const bool canBeExact = position.page == 0 || position.hasTextOffset;
  if (hadRecord && work->previous.spine == position.spine && work->previous.page == position.page &&
      work->previous.pageCount == position.pageCount && (work->previous.xpointer[0] || atEnd) &&
      (previousExact || !canBeExact || atEnd)) {
    return;  // Unchanged: no SD write, no chapter stream.
  }

  Record& next = work->next;
  next.spine = static_cast<uint16_t>(position.spine);
  next.page = static_cast<uint16_t>(position.page);
  next.pageCount = static_cast<uint16_t>(position.pageCount);
  const float intra =
      position.pageCount > 0 ? static_cast<float>(position.page) / static_cast<float>(position.pageCount) : 0.0f;
  next.percentage = atEnd ? 1.0f : std::max(0.0f, std::min(1.0f, epub->calculateProgress(position.spine, intra)));
  if (!std::isfinite(next.percentage)) next.percentage = 0.0f;

  // The document digest is cached with the book size it was computed for.
  next.fileSize = static_cast<uint32_t>(fileSize(epub->getPath()));
  if (hadRecord && work->previous.document[0] && work->previous.fileSize == next.fileSize) {
    memcpy(next.document, work->previous.document, sizeof(next.document));
  } else if (!copyText(KOReaderDocumentId::calculate(epub->getPath()), next.document, sizeof(next.document))) {
    next.document[0] = '\0';
  }

  if (!atEnd) {
    const size_t needed = chapterHtmlCached(*epub, position.spine) ? XPOINTER_BLOCK_CACHED : XPOINTER_BLOCK_ZIPPED;
    const size_t block = ESP.getMaxAllocHeap();
    if (block >= needed) {
      bool exact = false;
      const std::string xpath = xpointerFor(epub, position, exact);
      if (!xpath.empty() && xpath.size() <= MAX_XPOINTER_BYTES && validXPointer(xpath.c_str(), xpath.size())) {
        memcpy(next.xpointer, xpath.c_str(), xpath.size() + 1);
        if (exact) next.flags |= RECORD_FLAG_EXACT_XPOINTER;
      }
    } else {
      LOG_INF("RPS", "Reading position kept without XPointer: largest block %u B < %u B", static_cast<unsigned>(block),
              static_cast<unsigned>(needed));
    }
  }

  const time_t now = time(nullptr);
  next.updated = now >= static_cast<time_t>(AppGlance::MIN_EPOCH) ? static_cast<uint32_t>(now) : 0;
  next.seq = nextSequence();
  if (!saveRecord(cachePath, next, work->scratch)) {
    LOG_ERR("RPS", "Could not store the reading position for %s", epub->getPath().c_str());
    return;
  }
  // Hardware evidence: exit cost and heap after the chapter stream.
  LOG_INF("RPS", "Recorded spine=%d page=%d/%d %.4f seq=%lu %s in %lu ms (heap %u, block %u)", position.spine,
          position.page, position.pageCount, static_cast<double>(next.percentage), static_cast<unsigned long>(next.seq),
          next.xpointer[0] ? next.xpointer : "(no xpointer)", static_cast<unsigned long>(millis() - started),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

bool hasPendingOffer(const Epub& epub) { return hasOffer(epub.getCachePath().c_str()); }

bool takeFurtherOffer(const Epub& epub, const float currentPercentage, std::string& question, std::string& xpointer,
                      float& percentage) {
  struct OfferWork {
    Offer offer;
    Scratch scratch;
    char question[MAX_DEVICE_BYTES + 96];
  };
  auto work = makeUniqueNoThrow<OfferWork>();
  if (!work) return false;  // Keep the offer; the next open asks again.
  const char* cachePath = epub.getCachePath().c_str();
  if (!loadOffer(cachePath, work->offer, work->scratch)) {
    removeOffer(cachePath);  // Unreadable or damaged: never ask about it.
    return false;
  }
  const Offer& offer = work->offer;
  if (!isFurther(offer.percentage, currentPercentage)) {
    LOG_DBG("RPS", "Discarding offer %.4f from %s: current %.4f", static_cast<double>(offer.percentage), offer.device,
            static_cast<double>(currentPercentage));
    removeOffer(cachePath);
    return false;
  }
  const int percent = std::max(0, std::min(100, static_cast<int>(std::lround(offer.percentage * 100.0f))));
  snprintf(work->question, sizeof(work->question), tr(STR_READING_OFFER_FORMAT), offer.device, percent);
  question = work->question;
  xpointer = offer.xpointer;
  percentage = offer.percentage;
  return true;
}

void discardOffer(const Epub& epub) { removeOffer(epub.getCachePath().c_str()); }
}  // namespace PocketDaily::ReadingProgress
