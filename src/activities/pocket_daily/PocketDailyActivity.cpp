#include "PocketDailyActivity.h"

#include <Bitmap.h>
#include <EpdFontFamily.h>
#include <FsHelpers.h>
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
#include "pocket_daily/ContentImageRenderer.h"
#include "pocket_daily/PocketGlanceStore.h"
#include "pocket_daily/PocketProfileStore.h"
#include "pocket_daily/home/GlanceFormat.h"
#include "pocket_daily/home/HomeDrawing.h"
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

struct JapaneseDailyWord {
  const char* word;
  const char* reading;
  const char* meaning;
  const char* example;
};

// Compact, device-owned starter deck. These are intentionally common words
// with short examples: one 12px NotoSansJP line can carry each entry on X3,
// and no network, account or host process is required to make Study useful.
constexpr JapaneseDailyWord kJapaneseDailyWords[] = {
    {"習慣", "しゅうかん", "habit", "毎日、本を読む習慣をつける。"},
    {"続ける", "つづける", "continue", "日本語の勉強を毎日続ける。"},
    {"気づく", "きづく", "notice", "小さな変化に気づいた。"},
    {"選ぶ", "えらぶ", "choose", "好きな本を一冊選ぶ。"},
    {"確かめる", "たしかめる", "check", "答えをもう一度確かめる。"},
    {"間に合う", "まにあう", "be in time", "電車に間に合った。"},
    {"楽しみ", "たのしみ", "look forward to", "旅行を楽しみにしている。"},
    {"振り返る", "ふりかえる", "reflect", "一日を静かに振り返る。"},
    {"身につける", "みにつける", "acquire", "新しい表現を身につける。"},
    {"試す", "ためす", "try", "別の方法を試してみる。"},
    {"集中", "しゅうちゅう", "focus", "読書に集中する。"},
    {"調べる", "しらべる", "look up", "知らない言葉を調べる。"},
    {"伝える", "つたえる", "convey", "自分の考えを伝える。"},
    {"比べる", "くらべる", "compare", "二つの表現を比べる。"},
    {"慣れる", "なれる", "get used to", "新しい環境に慣れる。"},
    {"工夫", "くふう", "devise", "時間の使い方を工夫する。"},
};

constexpr size_t kJapaneseDailyWordCount = sizeof(kJapaneseDailyWords) / sizeof(kJapaneseDailyWords[0]);

bool clockIsSet(const time_t now) { return now >= static_cast<time_t>(PocketDaily::AppGlance::MIN_EPOCH); }
}  // namespace

void PocketDailyActivity::onEnter() {
  Activity::onEnter();
  appContent.load(nullptr, HalSystem::feedWatchdogIfRegistered);

  // Pocket is a hardware-shaped shell. Normalize it to the chassis portrait
  // coordinate system so edge and front-button affordances remain true even
  // when the preceding reader page used a rotated orientation.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  exitRequested = false;
  exitToReader = false;
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

void PocketDailyActivity::loop() {
  handleButtons();
  if (!exitRequested) return;
  exitRequested = false;
  // Confirm on the reading row resumes the open book. Pocket Daily keeps the
  // radio off, so no defrag restart is needed; Home's own book selection opens
  // the reader the same way.
  if (exitToReader && !APP_STATE.openEpubPath.empty() && WiFi.getMode() == WIFI_MODE_NULL) {
    exitToReader = false;
    activityManager.goToReader(APP_STATE.openEpubPath);
    return;
  }
  finish();
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
  if (exitToNearbySync) {
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(30);
    }
    silentRestartToPocketNearbySync();
    return;  // ESP.restart() does not return; keeps static analysis honest.
  }

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    // Defrag restart (mirrors CalibreConnectActivity::onExit). Confirm on the
    // Daily Brief targets the reader; every other exit lands on Home.
    if (exitToReader && !APP_STATE.openEpubPath.empty()) {
      silentRestartToReader();
    } else {
      silentRestart();
    }
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
    cp(o.project, sizeof(o.project), card.title[0] ? card.title : "POCKET");
    cp(o.activity, sizeof(o.activity), card.question);
    // The local SD lesson is a recall prompt on Home: show only the target
    // glyph there. Opening it reveals the word, reading, meaning and example
    // through renderPocketCard(). The companion's cards keep their text +
    // context overview summary.
    if (strcmp(card.module, "local") != 0 && card.context[0] && strcmp(o.activity, card.context) != 0) {
      const size_t used = strlen(o.activity);
      const size_t extra = (used ? 3 : 0) + strlen(card.context);
      // Keep UTF-8 intact; the detail view still carries the complete context
      // if the compact home row cannot fit it.
      if (used + extra < sizeof(o.activity))
        snprintf(o.activity + used, sizeof(o.activity) - used, "%s%s", used ? " - " : "", card.context);
    }
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
  const time_t now = time(nullptr);
  if (clockIsSet(now)) return static_cast<uint32_t>(now);
  // The companion's compose time is a lower bound on the real date. The POST
  // handler sets an unset clock from it; this covers a boot since then.
  return glanceSnapshot.savedEpoch;
}

void PocketDailyActivity::buildLocalStudyCard() {
  // The daily word turns over at the companion's local midnight when a glance
  // carried its UTC offset, else at UTC midnight.
  const uint32_t epoch = studyEpoch();
  const int64_t localEpoch = epoch ? static_cast<int64_t>(epoch) + glanceSnapshot.utcOffsetMinutes * 60 : 0;
  const uint32_t day = localEpoch > 0 ? static_cast<uint32_t>(localEpoch / 86400) : 0;
  PocketDaily::Card card{};
  snprintf(card.module, sizeof(card.module), "local");
  snprintf(card.actionClass, sizeof(card.actionClass), "day");
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
      snprintf(card.cardId, sizeof(card.cardId), "local:jp:%lu:%08lx", (unsigned long)localStudyPackVersion,
               (unsigned long)lesson.itemId);
      snprintf(card.title, sizeof(card.title), "今日の漢字");
      snprintf(card.question, sizeof(card.question), "%s", lesson.glyph);
      // Use the compact English gloss on the shared card renderer. The pack
      // retains the richer Korean fields for the dedicated study activity,
      // whose combined JP/KR font is distributed beside the SD content.
      snprintf(card.context, sizeof(card.context), "%s（%s） · %s · %s", lesson.primaryWord, lesson.wordReading,
               lesson.meaningEn, lesson.example);
    }
  }
  if (!fromPack) {
    index = (static_cast<size_t>(day) + localStudyOffset) % kJapaneseDailyWordCount;
    const auto& word = kJapaneseDailyWords[index];
    snprintf(card.cardId, sizeof(card.cardId), "local:jp:%lu:%u", (unsigned long)day, (unsigned)localStudyOffset);
    snprintf(card.title, sizeof(card.title), "今日の単語");
    snprintf(card.question, sizeof(card.question), "%s（%s）", word.word, word.reading);
    snprintf(card.context, sizeof(card.context), "%s · %s", word.meaning, word.example);
  }
  const char* ids[] = {"review", "next", "known"};
  const char* labels[] = {"Again", "Next", "Known"};
  card.choiceCount = 3;
  for (uint8_t i = 0; i < card.choiceCount; i++) {
    snprintf(card.choices[i].id, sizeof(card.choices[i].id), "%s", ids[i]);
    snprintf(card.choices[i].label, sizeof(card.choices[i].label), "%s", labels[i]);
  }

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
      exitToReader = true;
      exitRequested = true;
    } else if (rows[selected].pocket) {
      strncpy(cardSid, rows[selected].sid, sizeof(cardSid) - 1);
      cardSid[sizeof(cardSid) - 1] = '\0';
      viewMode = ViewMode::Card;
      requestUpdate();
    }
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
    exitToReader = true;
    exitRequested = true;
    return;
  }

  // Back exits Pocket Daily (guard a stale release from a prior activity).
  if (gpio.wasReleased(HalGPIO::BTN_BACK) && backPressMs != 0) exitRequested = true;
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
  // forecast days (docs/pocket-glance-v1.md). Needs a set clock; the app's UTC
  // offset gives the local date without a timezone database.
  const time_t now = time(nullptr);
  if (glanceSnapshot.savedEpoch && clockIsSet(now)) {
    char savedIso[11];
    char todayIso[11];
    if (PocketDaily::GlanceFormat::formatLocalIsoDate(savedIso, sizeof(savedIso), glanceSnapshot.savedEpoch,
                                                      glanceSnapshot.utcOffsetMinutes) &&
        PocketDaily::GlanceFormat::formatLocalIsoDate(todayIso, sizeof(todayIso), now, glanceSnapshot.utcOffsetMinutes))
      PocketDaily::GlanceFormat::rollToLocalDay(glanceSnapshot.glance, savedIso, todayIso);
  }

  memset(&renderPocketSnapshot, 0, sizeof(renderPocketSnapshot));
  if (const auto* appCards = appContent.cards(); appCards && appCards->count)
    renderPocketSnapshot = appCards->cards[0].card;
  else if (localStudyCard.cardId[0])
    renderPocketSnapshot = localStudyCard;

  renderReadingSnapshot.clear();
  if (APP_STATE.openEpubPath.empty()) return;

  renderReadingSnapshot.valid = true;
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (book.path != APP_STATE.openEpubPath) continue;
    snprintf(renderReadingSnapshot.title, sizeof(renderReadingSnapshot.title), "%s", book.title.c_str());
    snprintf(renderReadingSnapshot.author, sizeof(renderReadingSnapshot.author), "%s", book.author.c_str());
    if (!book.coverBmpPath.empty()) {
      // Pocket's editorial hero needs the full retained cover. The previous
      // code always selected Home's small thumbnail; drawBitmap deliberately
      // does not upscale, so additional layout space could never enlarge it.
      std::string fullPath = book.coverBmpPath;
      const size_t thumb = fullPath.rfind("/thumb_");
      if (thumb != std::string::npos) fullPath.replace(thumb, std::string::npos, "/cover.bmp");
      const std::string thumbPath =
          UITheme::getCoverThumbPath(book.coverBmpPath, UITheme::getInstance().getMetrics().homeCoverHeight);
      const std::string& displayPath = Storage.exists(fullPath.c_str()) ? fullPath : thumbPath;
      snprintf(renderReadingSnapshot.coverBmpPath, sizeof(renderReadingSnapshot.coverBmpPath), "%s",
               displayPath.c_str());
    }
    break;
  }
  if (!renderReadingSnapshot.title[0]) {
    const char* path = APP_STATE.openEpubPath.c_str();
    const char* slash = strrchr(path, '/');
    snprintf(renderReadingSnapshot.title, sizeof(renderReadingSnapshot.title), "%s", slash ? slash + 1 : path);
  }

  // The seventh progress byte is a backwards-compatible whole-book percent.
  // Read it once per paint through HalStorage; older six-byte files simply keep
  // percent=-1 and still resume at their exact local spine/page position.
  if (!FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) return;
  char progressPath[64];
  snprintf(progressPath, sizeof(progressPath), "/.crosspoint/epub_%u/progress.bin",
           (unsigned)std::hash<std::string>{}(APP_STATE.openEpubPath));
  HalFile progressFile;
  if (!Storage.openFileForRead("POCKET", progressPath, progressFile)) return;
  uint8_t progressData[7];
  if (progressFile.read(progressData, sizeof(progressData)) == (int)sizeof(progressData) && progressData[6] <= 100)
    renderReadingSnapshot.percent = (int8_t)progressData[6];
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
        renderer.drawBitmap(bitmap, x + (width - drawW) / 2, y + (height - drawH) / 2, drawW, drawH, 0, 0, true);
        coverDrawn = true;
      }
      coverFile.close();
    }
  }

  renderer.drawRect(x, y, width, height, 2, true);
  if (!coverDrawn) {
    // Private/no-art fallback: still reads as a book, but never invents a
    // remote cover or leaks title text when sleep-cover privacy is disabled.
    renderer.drawLine(x + 8, y, x + 8, y + height, true);
    const int horizon = y + height * 61 / 100;
    renderer.drawLine(x + 10, horizon, x + width - 2, horizon, true);
    const int mountainX[] = {x + 10, x + width / 3, x + width / 2, x + width * 3 / 4, x + width - 2};
    const int mountainY[] = {horizon, y + height * 41 / 100, horizon - 4, y + height * 31 / 100, horizon};
    for (int i = 0; i < 4; i++) renderer.drawLine(mountainX[i], mountainY[i], mountainX[i + 1], mountainY[i + 1], true);
    for (int i = 0; i < 4; i++) {
      const int tx = x + 18 + i * std::max(8, (width - 32) / 4);
      renderer.drawLine(tx, y + height - 8, tx + 5, horizon - 2, 2, true);
      renderer.drawLine(tx + 10, y + height - 8, tx + 5, horizon - 2, 2, true);
    }
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

PocketDaily::Home::Strings PocketDailyActivity::homeStrings() {
  PocketDaily::Home::Strings s;
  s.pocketDaily = tr(STR_POCKET_DAILY);
  s.continueReading = tr(STR_POCKET_CONTINUE_READING);
  s.noOpenBook = tr(STR_NO_OPEN_BOOK);
  s.startReading = tr(STR_START_READING);
  s.study = tr(STR_POCKET_STUDY);
  s.myCards = tr(STR_POCKET_MY_CARDS);
  s.word = tr(STR_POCKET_DAILY_WORD);
  s.empty = tr(STR_POCKET_EMPTY);
  s.weather = tr(STR_POCKET_WEATHER);
  s.noWeather = tr(STR_POCKET_NO_WEATHER);
  s.weatherHint = tr(STR_POCKET_WEATHER_HINT);
  s.nextEvent = tr(STR_POCKET_NEXT_EVENT);
  s.today = tr(STR_POCKET_TODAY);
  s.library = tr(STR_POCKET_LIBRARY);
  s.select = tr(STR_SELECT);
  s.sync = tr(STR_POCKET_SYNC);
  return s;
}

PocketDaily::Home::Env PocketDailyActivity::homeEnv() const {
  const auto& m = UITheme::getInstance().getMetrics();
  PocketDaily::Home::Env env;
  env.metrics = {m.topPadding, m.headerHeight, m.verticalSpacing, m.contentSidePadding, m.sideButtonHintsWidth};
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
  const auto* appCards = appContent.cards();
  if (!appCards || !appContent.revision()[0] || width <= 0 || height <= 0) return 0;
  for (uint8_t i = 0; i < appCards->count; ++i) {
    const auto& card = appCards->cards[i];
    if (!card.imagePath[0] || strcmp(card.card.cardId, cardId) != 0) continue;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s/%s", PocketDaily::Content::CONTENT_ROOT, appContent.revision(), card.imagePath);
    HalFile file = Storage.open(path, O_RDONLY);
    if (!file) {
      LOG_ERR("CONTENT", "Card image unavailable");
      return 0;
    }
    const PocketDaily::Content::ManifestSource source{
        &file, file.size(), [](void* context, size_t offset, uint8_t* bytes, size_t count) {
          HalSystem::feedWatchdogIfRegistered();
          auto& file = *static_cast<HalFile*>(context);
          return file.seek(offset) && file.read(bytes, count) == static_cast<int>(count);
        }};
    const int drawn = PocketDaily::Content::contentImageHeight(renderer, source, x, y, width, height);
    return drawn > 0 && GUI.drawContentImage(renderer, source, x, y, width, height) ? drawn : 0;
  }
  return 0;
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
    PowerWakeCue::draw(renderer);
    // The panel holds this frame for hours or days: always clear the ghosting
    // with a full waveform (it is painted once, at power-off).
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  } else {
    // Confirm resumes the open book — label it only when there is one.
    const auto labels = mappedInput.mapLabels(tr(STR_POCKET_LIBRARY),
                                              APP_STATE.openEpubPath.empty() ? "" : tr(STR_POCKET_READ), "", "");
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
