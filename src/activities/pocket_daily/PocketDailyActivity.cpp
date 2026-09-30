#include "PocketDailyActivity.h"

#include <Bitmap.h>
#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HalGPIO.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "agent/AgentLog.h"
#include "components/UITheme.h"  // GUI (theme) + ThemeMetrics + Rect
#include "fontIds.h"
#include "pocket_daily/ContentActiveStore.h"
#include "pocket_daily/PocketGlanceStore.h"
#include "pocket_daily/PocketProfileStore.h"
#include "pocket_daily/home/DailyWord.h"
#include "pocket_daily/home/HomeDrawing.h"
#include "pocket_daily/home/HomeInputs.h"
#include "pocket_daily/home/HomeRenderer.h"
#include "pocket_daily/learning_pack.h"
#include "util/PowerWakeCue.h"
#include "util/UiCjkFont.h"

namespace {
// Drawing helpers live in pocket_daily/home/HomeDrawing (shared with the host
// preview). Pull them into this translation unit's unqualified scope.
using PocketDaily::HomeDraw::formatWeatherSnapshotDate;

// The device picks an installed SD CJK font for text the built-in fonts lack.
PocketDaily::HomeDraw::FontResolver deviceFonts(const GfxRenderer& renderer) {
  return {const_cast<GfxRenderer*>(&renderer),
          // FontResolver::pick is a fixed `void*` callback type shared with the
          // host preview; a `const void*` parameter would not convert to it.
          // cppcheck-suppress constParameterPointer
          [](void* context, const char* text, int fallback, EpdFontFamily::Style style) {
            return UiCjkFont::fontForText(*static_cast<const GfxRenderer*>(context), text, fallback, style);
          }};
}

// True when the text contains Hangul / CJK / Kana codepoints — i.e. glyphs the
// Latin-only built-in UI fonts can't render (they'd show □). Used to swap to an
// installed SD CJK font for that line.
bool hasCJK(const char* s) {
  if (!s) return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    const unsigned char c = *p;
    if (c < 0x80) {
      p++;
      continue;
    }
    uint32_t cp = 0;
    int n = 0;
    if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 3;
    } else {
      p++;
      continue;
    }
    p++;
    for (int i = 0; i < n && *p; i++, p++) cp = (cp << 6) | (*p & 0x3F);
    if ((cp >= 0xAC00 && cp <= 0xD7A3) ||  // Hangul syllables
        (cp >= 0x1100 && cp <= 0x11FF) ||  // Hangul Jamo
        (cp >= 0x3040 && cp <= 0x30FF) ||  // Hiragana + Katakana
        (cp >= 0x4E00 && cp <= 0x9FFF))    // CJK unified ideographs
      return true;
  }
  return false;
}

}  // namespace

void PocketDailyActivity::onEnter() {
  Activity::onEnter();
  appContent.load(nullptr, HalSystem::feedWatchdogIfRegistered);

  // Pocket is a hardware-shaped shell. Normalize it to the chassis portrait
  // coordinate system so edge and front-button affordances remain true even
  // when the preceding reader page used a rotated orientation.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  exitToNearbySync = false;
  viewMode = ViewMode::Overview;
  glanceReason = GlanceReason::Ambient;
  sleepFramePending = false;

  // Local books, cards and the daily word are the product. Pocket Daily never
  // holds the radio; the companion reaches the reader through Sync.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // The app-provided weather/events glance is read from SD once, before the
  // first paint (no network). A missing or invalid record is the
  // normal "no weather yet" state.
  {
    RenderLock glanceLock(*this);
    if (!PocketDaily::AppGlance::load(glanceSnapshot)) glanceSnapshot.clear();
  }
  localStudyOffset = 0;
  buildLocalStudyCard();

  AgentLog::line("POCKET", "Pocket reader onEnter (glance %s)", glanceSnapshot.savedEpoch ? "saved" : "none");
  // Paint Pocket immediately — local reading and carried cards need nothing.
  requestUpdate();
}

void PocketDailyActivity::loop() { handleButtons(); }

void PocketDailyActivity::resumeReading() {
  // Leaving the book returns here (ActivityManager::leaveReader).
  if (!APP_STATE.openEpubPath.empty())
    activityManager.goToReaderFrom(ReaderReturn::PocketDaily, APP_STATE.openEpubPath);
}

void PocketDailyActivity::onExit() {
  Activity::onExit();
  appContent.reset();

  // Pocket Daily's reader/font lifetime fragments the X3's internal heap even
  // after every reloadable owner is released. Starting NimBLE in the same
  // process can therefore fail its contiguous-allocation preflight and appear
  // to bounce straight back to Pocket. Reboot directly into the Nearby
  // activity: the retained e-ink popup hides the short reset, while the clean
  // heap makes BLE startup deterministic.
  // onEnter turned the radio off and nothing here turns it on, so the other
  // exits are plain activity switches.
  if (exitToNearbySync) {
    silentRestartToPocketNearbySync();
    return;  // ESP.restart() does not return; keeps static analysis honest.
  }
}

bool PocketDailyActivity::findPocketCard(const char* cardId, PocketDaily::Card& out) const {
  if (!cardId || !cardId[0]) return false;
  if (const auto* appCards = appContent.cards()) {
    for (uint8_t i = 0; i < appCards->count; ++i) {
      if (strcmp(appCards->cards[i].card.cardId, cardId) == 0) {
        out = appCards->cards[i].card;
        return true;
      }
    }
  }
  if (localStudyCard.cardId[0] && strcmp(localStudyCard.cardId, cardId) == 0) {
    out = localStudyCard;
    return true;
  }
  return false;
}

int PocketDailyActivity::collectOverview(OverviewRow* out, int cap) const {
  auto cp = [](char* d, size_t n, const char* s) {
    strncpy(d, s, n - 1);
    d[n - 1] = '\0';
  };
  int n = 0;
  auto appendPocket = [&](const PocketDaily::Card& card) {
    if (n >= cap || !card.cardId[0]) return;
    OverviewRow& o = out[n++];
    memset(&o, 0, sizeof(o));
    cp(o.sid, sizeof(o.sid), card.cardId);
    // Shared with the Sync screen presenter (pocket_daily/home/HomeRenderer).
    cp(o.project, sizeof(o.project), PocketDaily::Home::cardRowTitle(card));
    PocketDaily::Home::cardRowActivity(card, o.activity, sizeof(o.activity));
    o.pocket = true;
  };

  // The local book, when one exists. This data lives entirely on SD.
  auto appendReading = [&]() {
    if (APP_STATE.openEpubPath.empty() || n >= cap) return;
    OverviewRow& o = out[n++];
    memset(&o, 0, sizeof(o));
    cp(o.sid, sizeof(o.sid), "local:reading");
    cp(o.project, sizeof(o.project), tr(STR_POCKET_CONTINUE_READING));
    const char* title = nullptr;
    const char* author = nullptr;
    for (const auto& book : RECENT_BOOKS.getBooks()) {
      if (book.path == APP_STATE.openEpubPath) {
        title = book.title.c_str();
        author = book.author.c_str();
        break;
      }
    }
    if (!title || !title[0]) {
      const char* path = APP_STATE.openEpubPath.c_str();
      const char* slash = strrchr(path, '/');
      title = slash ? slash + 1 : path;
    }
    snprintf(o.activity, sizeof(o.activity), "%s%s%s", title, author && author[0] ? " - " : "",
             author && author[0] ? author : "");
    o.reading = true;
  };

  // Study: active app cards, otherwise the firmware-authored daily word
  // (homeRowSources decides which, from the profile).
  const auto* appCards = appContent.cards();
  auto appendAppCards = [&]() {
    for (uint8_t i = 0; appCards && i < appCards->count && n < cap; ++i) {
      const int before = n;
      appendPocket(appCards->cards[i].card);
      if (n > before) {
        out[before].mine = true;
        out[before].hasImage = appCards->cards[i].imagePath[0] != '\0';
      }
    }
  };
  auto appendDailyWord = [&]() {
    const int before = n;
    appendPocket(localStudyCard);
    if (n > before) out[before].word = true;
  };

  // Pocket Daily profile (docs/pocket-profile-v1.md). Retired provider and
  // monitoring items still parse but never produce a row.
  PocketDaily::Home::RowAvailability available;
  available.book = !APP_STATE.openEpubPath.empty();
  available.appCards = appCards && appCards->count;
  PocketDaily::Home::RowSource sources[PocketDaily::DailyProfile::HOME_ITEM_CAP];
  const int count = PocketDaily::Home::homeRowSources(PocketDaily::DailyProfile::current(), available, sources);
  for (int k = 0; k < count && n < cap; ++k) {
    switch (sources[k]) {
      case PocketDaily::Home::RowSource::Reading:
        appendReading();
        break;
      case PocketDaily::Home::RowSource::AppCards:
        appendAppCards();
        break;
      case PocketDaily::Home::RowSource::DailyWord:
        appendDailyWord();
        break;
    }
  }
  return n;
}

uint32_t PocketDailyActivity::studyEpoch() const {
  return PocketDaily::Home::dailyWordEpoch(time(nullptr), glanceSnapshot.savedEpoch);
}

void PocketDailyActivity::buildLocalStudyCard() {
  // The daily word turns over at the companion's local midnight when a glance
  // carried its UTC offset, else at UTC midnight (shared with the Sync screen
  // presenter: pocket_daily/home/DailyWord).
  const uint32_t day = PocketDaily::Home::dailyWordDay(studyEpoch(), glanceSnapshot.utcOffsetMinutes);
  PocketDaily::Card card{};
  size_t index = 0;
  bool fromPack = false;
  PocketDaily::LearningPack::Record lesson{};
  if (localStudyPackRecordCount == 0) {
    PocketDaily::LearningPack::Metadata metadata{};
    if (PocketDaily::LearningPack::ensureAvailable(&metadata)) {
      localStudyPackVersion = metadata.contentVersion;
      localStudyPackRecordCount = metadata.recordCount;
    }
  }
  if (localStudyPackRecordCount) {
    index = (static_cast<size_t>(day) + localStudyOffset) % localStudyPackRecordCount;
    if (PocketDaily::LearningPack::readRecord(static_cast<uint32_t>(index), lesson)) {
      fromPack = true;
      PocketDaily::Home::packDailyWord(lesson, localStudyPackVersion, card);
    }
  }
  if (!fromPack) index = PocketDaily::Home::builtInDailyWord(day, localStudyOffset, card);
  PocketDaily::Home::finishDailyWord(card);

  {
    RenderLock studyLock(*this);
    localStudyCard = card;
  }
  AgentLog::line("POCKET", "offline JP lesson ready: source=%s day=%lu index=%u", fromPack ? "sd-pack" : "firmware",
                 (unsigned long)day, (unsigned)index);
}

void PocketDailyActivity::handleButtons() {
  using Btn = MappedInputManager::Button;

  // Pocket Home uses the four physical front positions directly.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) backPressMs = millis();

  // Cards are local: a press answers the daily word or closes the card.
  if (viewMode == ViewMode::Card) {
    PocketDaily::Card pocket{};
    if (!findPocketCard(cardSid, pocket)) {
      // The card changed underneath (for example the daily word turned over).
      cardSid[0] = '\0';
      viewMode = ViewMode::Overview;
      requestUpdate();
      return;
    }
    const int raw = mappedInput.getPressedFrontButton();
    if (raw == HalGPIO::BTN_BACK) {
      closePocketCard(pocket);
    } else {
      int pos = -1;
      if (raw == HalGPIO::BTN_CONFIRM)
        pos = 0;
      else if (raw == HalGPIO::BTN_LEFT)
        pos = 1;
      else if (raw == HalGPIO::BTN_RIGHT)
        pos = 2;
      if (pos >= 0 && pos < pocket.choiceCount) applyPocketChoice(pocket, pos);
    }
    return;
  }

  // ── OVERVIEW: one Reading/Study carousel + explicit Library/Sync ──
  OverviewRow* const rows = inputRows;
  const int n = collectOverview(rows, kOverviewCap);
  const int selected = overviewCursor >= 0 && overviewCursor < n ? overviewCursor : 0;

  // Reading and Study are one content carousel. The side keys move through
  // it; Confirm always opens exactly what the panel currently shows.
  if (mappedInput.wasReleased(Btn::Up) && n > 1) {
    overviewCursor = (selected - 1 + n) % n;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Btn::Down) && n > 1) {
    overviewCursor = (selected + 1) % n;
    requestUpdate();
    return;
  }
  if (gpio.wasReleased(HalGPIO::BTN_CONFIRM) && n > 0) {
    if (rows[selected].reading) {
      resumeReading();
    } else if (rows[selected].pocket) {
      strncpy(cardSid, rows[selected].sid, sizeof(cardSid) - 1);
      cardSid[sizeof(cardSid) - 1] = '\0';
      viewMode = ViewMode::Card;
      requestUpdate();
    }
    return;
  }
  // Left opens the Articles library: the companion's articles on SD, listed
  // and tidied on the reader (docs/articles-v1.md). Home stays stock CrossPoint.
  if (gpio.wasReleased(HalGPIO::BTN_LEFT)) {
    activityManager.goToArticles();
    return;
  }
  // Sync opens Pocket Nearby Sync, where the companion sends cards, the
  // profile and the weather/events glance.
  if (gpio.wasReleased(HalGPIO::BTN_RIGHT) || (gpio.wasReleased(HalGPIO::BTN_CONFIRM) && n == 0)) {
    exitToNearbySync = true;
    activityManager.goToPocketNearbySync();
    return;
  }
  // With neither a book nor a card row the ambient Daily Brief is the face;
  // Confirm there resumes the open book.
  if (ambientGlanceShown && gpio.wasReleased(HalGPIO::BTN_CONFIRM) && !APP_STATE.openEpubPath.empty()) {
    resumeReading();
    return;
  }

  // Back (labelled Home) exits to CrossPoint Home; the guard ignores a stale
  // release from a prior activity.
  if (gpio.wasReleased(HalGPIO::BTN_BACK) && backPressMs != 0) finish();
}

bool PocketDailyActivity::closePocketCard(const PocketDaily::Card& card) {
  if (millis() - lastDecisionMs < kDecisionCooldownMs || !card.cardId[0]) return false;
  // Back/Later on a card simply closes it; nothing is queued anywhere.
  lastDecisionMs = millis();
  cardSid[0] = '\0';
  viewMode = ViewMode::Overview;
  backPressMs = 0;
  requestUpdate();
  return true;
}

bool PocketDailyActivity::applyPocketChoice(const PocketDaily::Card& card, int selectedCursor) {
  if (millis() - lastDecisionMs < kDecisionCooldownMs || selectedCursor < 0 || selectedCursor >= card.choiceCount)
    return false;
  const auto& choice = card.choices[selectedCursor];
  if (!card.cardId[0] || !choice.id[0]) return false;
  // Only the local daily word carries choices. Again keeps today's word; Next
  // and Known move through the offline deck immediately.
  if (strcmp(card.module, "local") == 0 && (strcmp(choice.id, "next") == 0 || strcmp(choice.id, "known") == 0)) {
    ++localStudyOffset;
    buildLocalStudyCard();
  }
  lastDecisionMs = millis();
  cardSid[0] = '\0';
  viewMode = ViewMode::Overview;
  backPressMs = 0;
  requestUpdate();
  return true;
}

int PocketDailyActivity::fontForText(int uiFontId, const char* text) const {
  return UiCjkFont::fontForText(renderer, text, uiFontId, EpdFontFamily::REGULAR,
                                UiCjkFont::CoveragePolicy::RequireFull);
}

void PocketDailyActivity::preparePersonalSnapshot() {
  // A glance saved on an earlier local day drops that day's schedule and past
  // forecast days (shared with the Sync screen presenter).
  PocketDaily::Home::rollGlanceToToday(glanceSnapshot, time(nullptr));

  memset(&renderPocketSnapshot, 0, sizeof(renderPocketSnapshot));
  if (const auto* appCards = appContent.cards(); appCards && appCards->count)
    renderPocketSnapshot = appCards->cards[0].card;
  else if (localStudyCard.cardId[0])
    renderPocketSnapshot = localStudyCard;

  // Title, author and percent are shared with the Sync screen presenter
  // (pocket_daily/home/HomeInputs); only the cover path is Pocket's own.
  renderReadingSnapshot.clear();
  const RecentBook* book = PocketDaily::Home::readOpenBook(renderReadingSnapshot);
  if (book && !book->coverBmpPath.empty()) {
    // Pocket's editorial hero needs the full retained cover. The previous
    // code always selected Home's small thumbnail; drawBitmap deliberately
    // does not upscale, so additional layout space could never enlarge it.
    std::string fullPath = book->coverBmpPath;
    const size_t thumb = fullPath.rfind("/thumb_");
    if (thumb != std::string::npos) fullPath.replace(thumb, std::string::npos, "/cover.bmp");
    const std::string thumbPath =
        UITheme::getCoverThumbPath(book->coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);
    const std::string& displayPath = Storage.exists(fullPath.c_str()) ? fullPath : thumbPath;
    snprintf(renderReadingSnapshot.coverBmpPath, sizeof(renderReadingSnapshot.coverBmpPath), "%s", displayPath.c_str());
  }
}

bool PocketDailyActivity::drawReadingCover(int x, int y, int width, int height) const {
  if (width <= 12 || height <= 18) return false;
  bool coverDrawn = false;
  if (renderReadingSnapshot.coverBmpPath[0]) {
    HalFile coverFile;
    if (Storage.openFileForRead("POCKET", renderReadingSnapshot.coverBmpPath, coverFile)) {
      Bitmap bitmap(coverFile);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        // Home thumbnails are intentionally small. Pocket's editorial hero is
        // allowed to scale that retained 1-bit bitmap up to the available
        // frame; capping at 1.0 left a tiny image floating inside a large box.
        const float scale = std::min((float)(width - 4) / bitmap.getWidth(), (float)(height - 4) / bitmap.getHeight());
        const int drawW = std::max(1, (int)(bitmap.getWidth() * scale));
        const int drawH = std::max(1, (int)(bitmap.getHeight() * scale));
        const int artX = x + (width - drawW) / 2;
        const int artY = y + (height - drawH) / 2;
        renderer.drawBitmap(bitmap, artX, artY, drawW, drawH, 0, 0, true);
        renderer.drawRect(artX, artY, drawW, drawH, 2, true);
        coverDrawn = true;
      }
      coverFile.close();
    }
  }

  if (!coverDrawn) {
    renderer.drawRect(x, y, width, height, 2, true);
    PocketDaily::HomeDraw::drawCoverPlaceholder(renderer, x, y, width, height);
  }
  return coverDrawn;
}

void PocketDailyActivity::drawBrandedHeader(const char* title, const char* subtitle) const {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const Rect r{0, m.topPadding, w, m.headerHeight};
  // Product identity is Pocket itself; card titles are content.
  GUI.drawHeader(renderer, r, title, subtitle);
}

PocketDaily::Home::Strings PocketDailyActivity::homeStrings() { return PocketDaily::Home::deviceStrings(); }

PocketDaily::Home::Env PocketDailyActivity::homeEnv() const {
  PocketDaily::Home::Env env;
  env.metrics = PocketDaily::Home::deviceMetrics();
  env.text = {const_cast<PocketDailyActivity*>(this),
              [](void* self, const char* text, int fallback, EpdFontFamily::Style) {
                return static_cast<PocketDailyActivity*>(self)->fontForText(fallback, text);
              }};
  env.labels = deviceFonts(renderer);
  env.context = const_cast<PocketDailyActivity*>(this);
  env.drawHeader = [](void* self, const GfxRenderer& renderer, int x, int y, int width, int height, const char* title,
                      const char* subtitle) {
    (void)self;
    // Product identity is Pocket itself.
    GUI.drawHeader(renderer, Rect{x, y, width, height}, title, subtitle);
  };
  env.drawCover = [](void* self, GfxRenderer&, int x, int y, int width, int height) {
    return static_cast<PocketDailyActivity*>(self)->drawReadingCover(x, y, width, height);
  };
  env.drawCardImage = [](void* self, GfxRenderer&, const char* cardId, int x, int y, int width, int height) {
    return static_cast<PocketDailyActivity*>(self)->drawAppCardImage(cardId, x, y, width, height);
  };
  return env;
}

void PocketDailyActivity::renderOverview(const OverviewRow* rows, int n) {
  preparePersonalSnapshot();
  // Drawing is shared with the host preview (pocket_daily/home/HomeRenderer).
  PocketDaily::Home::Row homeRows[kOverviewCap];
  for (int i = 0; i < n && i < kOverviewCap; ++i)
    homeRows[i] = {rows[i].reading,  rows[i].pocket, rows[i].mine,    rows[i].word,
                   rows[i].hasImage, rows[i].sid,    rows[i].project, rows[i].activity};
  PocketDaily::Home::HomeView view;
  view.isX3 = gpio.deviceIsX3();
  view.rows = homeRows;
  view.count = std::min(n, kOverviewCap);
  view.selected = overviewCursor;
  view.reading = {renderReadingSnapshot.valid, renderReadingSnapshot.title, renderReadingSnapshot.author,
                  renderReadingSnapshot.percent};
  view.glance = &glanceSnapshot.glance;
  view.snapshotStale = PocketDaily::HomeDraw::snapshotIsStale(glanceSnapshot.savedEpoch, time(nullptr));
  view.profile = PocketDaily::DailyProfile::current();
  view.strings = homeStrings();
  PocketDaily::Home::renderHome(renderer, view, homeEnv());
  renderer.displayBuffer();
}

int PocketDailyActivity::drawAppCardImage(const char* cardId, int x, int y, int width, int height) const {
  return PocketDaily::Home::drawContentCardImage(renderer, appContent, cardId, x, y, width, height);
}

void PocketDailyActivity::renderPocketCard(const PocketDaily::Card& card) {
  const auto& m = UITheme::getInstance().getMetrics();
  const int w = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int pad = m.contentSidePadding;
  const int lineS = renderer.getLineHeight(SMALL_FONT_ID);

  renderer.clearScreen();
  // The item title is content; the shell remains visibly Pocket even though
  // the companion authored the payload.
  drawBrandedHeader(card.title[0] ? card.title : tr(STR_POCKET_TITLE), tr(STR_POCKET_SUBTITLE));
  int y = m.topPadding + m.headerHeight + m.verticalSpacing + 8;

  const int qFont = fontForText(UI_12_FONT_ID, card.question);
  const int qAdvance = renderer.getLineHeight(qFont) + 5;
  const int optionAdvance = renderer.getLineHeight(UI_10_FONT_ID) + 7;
  const int hintTop = pageH - m.buttonHintsHeight - 6;
  const int optionsTop = hintTop - card.choiceCount * optionAdvance - 6;
  auto qLines = renderer.wrappedText(qFont, card.question, w - pad * 2, 6);
  for (const auto& line : qLines) {
    if (y + qAdvance > optionsTop - lineS - 10) break;
    renderer.drawText(qFont, pad, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += qAdvance;
  }
  if (card.context[0] && y + lineS < optionsTop - 4) {
    y += 5;
    const int contextFont = fontForText(SMALL_FONT_ID, card.context);
    auto lines = renderer.wrappedText(contextFont, card.context, w - pad * 2, 4);
    for (const auto& line : lines) {
      if (y + renderer.getLineHeight(contextFont) > optionsTop - 4) break;
      renderer.drawText(contextFont, pad, y, line.c_str(), true);
      y += renderer.getLineHeight(contextFont) + 2;
    }
  }

  // Keep the image below text and above the fixed action/hint area. The
  // theme clamps to the oriented framebuffer; no second framebuffer is used.
  if (y >= 0 && y < optionsTop && optionsTop <= pageH && pad >= 0 && pad < w / 2) {
    const int imageGap = std::clamp(m.verticalSpacing, 0, optionsTop - y);
    drawAppCardImage(card.cardId, pad, y + imageGap, w - pad * 2, optionsTop - y - imageGap);
  }

  y = optionsTop;
  char hints[3][24] = {{0}, {0}, {0}};
  for (uint8_t i = 0; i < card.choiceCount; i++) {
    const char* label = card.choices[i].label;
    const int font = fontForText(UI_10_FONT_ID, label);
    char row[64];
    snprintf(row, sizeof(row), "[%u] %s", (unsigned)(i + 2), label);
    renderer.drawText(font, pad, y, renderer.truncatedText(font, row, w - pad * 2).c_str(), true);
    if (hasCJK(label) || strlen(label) >= sizeof(hints[i]))
      snprintf(hints[i], sizeof(hints[i]), "%u", (unsigned)(i + 2));
    else
      snprintf(hints[i], sizeof(hints[i]), "%s", renderer.truncatedText(UI_10_FONT_ID, label, 96).c_str());
    y += renderer.getLineHeight(font) + 7;
  }
  GUI.drawButtonHints(renderer, card.choiceCount == 0 ? tr(STR_POCKET_DONE) : tr(STR_POCKET_LATER), hints[0], hints[1],
                      hints[2]);
  renderer.displayBuffer();
}

void PocketDailyActivity::renderGlance(GlanceReason reason) {
  const bool isSleep = reason != GlanceReason::Ambient;

  // One bounded snapshot feeds the whole retained frame. This includes the
  // first carried Pocket item so a useful daily study prompt survives offline
  // and remains visible while the panel is asleep.
  preparePersonalSnapshot();
  const PocketDaily::Glance& g = glanceSnapshot.glance;
  const char* syncedHm = glanceSnapshot.syncedHm;
  const bool stale = PocketDaily::HomeDraw::snapshotIsStale(glanceSnapshot.savedEpoch, time(nullptr));

  // A powered-off frame may remain unchanged for days. Date and sync time add
  // little value there and eventually become misleading, so the ambient face
  // alone carries the snapshot's date and the app's compose time.
  char glanceHeaderMeta[40] = {0};
  if (reason == GlanceReason::Ambient) {
    char snapshotDate[8] = {0};
    int metaChars = 0;
    if (formatWeatherSnapshotDate(snapshotDate, sizeof(snapshotDate), g.weather))
      metaChars = snprintf(glanceHeaderMeta, sizeof(glanceHeaderMeta), "%s", snapshotDate);
    if (syncedHm[0] && metaChars < (int)sizeof(glanceHeaderMeta))
      snprintf(glanceHeaderMeta + metaChars, sizeof(glanceHeaderMeta) - metaChars, "%s%s %s",
               metaChars ? " \xC2\xB7 " : "", stale ? "SAVED" : "SYNC", syncedHm);
  }
  // ── Bottom status: absolute times only — a retained frame must stay true
  // without a repaint, so never a relative age here. ──
  char status[64];
  if (reason == GlanceReason::PoweredOff) {
    // The physical wake tab communicates the important state. Do not repeat
    // a stale date or the last Sync time beside the front-button area.
    snprintf(status, sizeof(status), "%s", tr(STR_POCKET_POWERED_OFF));
  } else if (syncedHm[0]) {
    snprintf(status, sizeof(status), "%s \xC2\xB7 %s", tr(STR_POCKET_OFFLINE), syncedHm);
  } else {
    snprintf(status, sizeof(status), "%s", tr(STR_POCKET_OFFLINE));
  }
  // Drawing is shared with the host preview (pocket_daily/home/HomeRenderer).
  PocketDaily::Home::BriefView view;
  view.isSleep = isSleep;
  if (isSleep && SETTINGS.sleepWakeIndicator)
    view.contentTopInset = PowerWakeCue::portraitContentTop(gpio.deviceIsX3());
  view.headerMeta = glanceHeaderMeta[0] ? glanceHeaderMeta : nullptr;
  view.glance = &g;
  view.reading = {renderReadingSnapshot.valid, renderReadingSnapshot.title, renderReadingSnapshot.author,
                  renderReadingSnapshot.percent};
  view.sleepCover = SETTINGS.pocketDailySleepCover;
  view.pocketCard = &renderPocketSnapshot;
  // The companion's first card stays in appContent for the whole paint.
  if (const auto* appCards = appContent.cards(); appCards && appCards->count) {
    view.pinnedCard = &appCards->cards[0].card;
    view.pinnedHasImage = appCards->cards[0].imagePath[0] != '\0';
  }
  view.snapshotStale = stale;
  view.status = status;
  view.profile = PocketDaily::DailyProfile::current();
  view.strings = homeStrings();
  PocketDaily::Home::renderBrief(renderer, view, homeEnv());
  if (isSleep) {
    if (SETTINGS.sleepWakeIndicator) PowerWakeCue::draw(renderer, gpio.deviceIsX3(), tr(STR_POCKET_WAKE_READ));
    // The panel holds this frame for hours or days: always clear the ghosting
    // with a full waveform (it is painted once, at power-off).
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  } else {
    // Confirm resumes the open book — label it only when there is one. Left
    // and Right are physical positions here (handleButtons reads raw gpio), so
    // they bypass the front-button remap the mapped labels follow.
    auto labels =
        mappedInput.mapLabels(tr(STR_POCKET_HOME), APP_STATE.openEpubPath.empty() ? "" : tr(STR_POCKET_READ), "", "");
    labels.btn3 = tr(STR_ARTICLES);
    labels.btn4 = tr(STR_POCKET_SYNC);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
  }
}

bool PocketDailyActivity::paintSleepFrame() {
  // Profile sleep mode "reader": hand the power-off frame to the reader's own
  // Sleep Screen setting (SleepActivity) instead of the Daily Brief.
  if (PocketDaily::DailyProfile::current().sleepMode == PocketDaily::DailyProfile::SleepMode::Reader) return false;
  // Pocket Daily owns its retained e-ink frame. Delegating to SleepActivity
  // redraws the last book cover over the dashboard just before the panel powers
  // down, which looks like Pocket Daily disappeared. Paint the powered-off
  // Daily Brief instead; it promises no next sync time and stays truthful if
  // the device remains off for days.
  glanceReason = GlanceReason::PoweredOff;
  sleepFramePending = true;
  requestUpdateAndWait();
  AgentLog::line("POCKET", "retaining powered-off Daily Brief frame");
  return true;
}

void PocketDailyActivity::render(RenderLock&&) {
  // Assume a non-ambient face until the Ambient branch below proves otherwise;
  // every other path (sleep frame, Card, Overview) must not leave Confirm bound
  // to "resume reading".
  ambientGlanceShown = false;

  // Frozen sleep frame: a local Daily Brief painted once immediately before
  // power-off. It deliberately uses only persisted device state (book
  // position, carried card, the saved weather and schedule), so the retained
  // panel remains useful and truthful without a network.
  if (sleepFramePending) {
    renderGlance(glanceReason);
    return;
  }

  // Cards are local and remain valid offline. The render-owned card buffer
  // keeps the ~700 B copy off the render stack.
  if (viewMode == ViewMode::Card && findPocketCard(cardSid, renderPocketSnapshot)) {
    renderPocketCard(renderPocketSnapshot);
    return;
  }

  // ── Face: content-first shell. The Face renders whatever is known (or an
  // honest empty state). ──
  OverviewRow* const rows = renderRows;
  const int n = collectOverview(rows, kOverviewCap);

  // With neither a book nor a card row, the personal glance remains useful as
  // an offline face (weather/today). It never outranks items that can be
  // opened.
  if (n == 0 && (glanceSnapshot.glance.valid || !APP_STATE.openEpubPath.empty())) {
    renderGlance(GlanceReason::Ambient);
    ambientGlanceShown = true;
    return;
  }
  renderOverview(rows, n);
}
