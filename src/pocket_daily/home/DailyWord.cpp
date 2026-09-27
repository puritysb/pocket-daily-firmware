#include "DailyWord.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "pocket_daily/PocketGlance.h"

namespace PocketDaily::Home {
namespace {
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

// Header + one record: 1316 B, too large for the loop/render task stacks.
struct PackScratch {
  LearningPack::Header header;
  LearningPack::Record record;
};
}  // namespace

uint32_t dailyWordEpoch(const time_t now, const uint32_t glanceSavedEpoch) {
  if (now >= static_cast<time_t>(PocketDaily::AppGlance::MIN_EPOCH)) return static_cast<uint32_t>(now);
  // The companion's compose time is a lower bound on the real date. The POST
  // handler sets an unset clock from it; this covers a boot since then.
  return glanceSavedEpoch;
}

uint32_t dailyWordDay(const uint32_t epoch, const int16_t utcOffsetMinutes) {
  const int64_t localEpoch = epoch ? static_cast<int64_t>(epoch) + utcOffsetMinutes * 60 : 0;
  return localEpoch > 0 ? static_cast<uint32_t>(localEpoch / 86400) : 0;
}

size_t builtInDailyWord(const uint32_t day, const uint16_t offset, PocketDaily::Card& card) {
  const size_t index = (static_cast<size_t>(day) + offset) % kJapaneseDailyWordCount;
  const auto& word = kJapaneseDailyWords[index];
  snprintf(card.cardId, sizeof(card.cardId), "local:jp:%lu:%u", (unsigned long)day, (unsigned)offset);
  snprintf(card.title, sizeof(card.title), "今日の単語");
  snprintf(card.question, sizeof(card.question), "%s（%s）", word.word, word.reading);
  snprintf(card.context, sizeof(card.context), "%s · %s", word.meaning, word.example);
  return index;
}

void packDailyWord(const LearningPack::Record& lesson, const uint32_t contentVersion, PocketDaily::Card& card) {
  snprintf(card.cardId, sizeof(card.cardId), "local:jp:%lu:%08lx", (unsigned long)contentVersion,
           (unsigned long)lesson.itemId);
  snprintf(card.title, sizeof(card.title), "今日の漢字");
  snprintf(card.question, sizeof(card.question), "%s", lesson.glyph);
  // Use the compact English gloss on the shared card renderer. The pack
  // retains the richer Korean fields for the dedicated study activity,
  // whose combined JP/KR font is distributed beside the SD content.
  snprintf(card.context, sizeof(card.context), "%s（%s） · %s · %s", lesson.primaryWord, lesson.wordReading,
           lesson.meaningEn, lesson.example);
}

void finishDailyWord(PocketDaily::Card& card) {
  snprintf(card.module, sizeof(card.module), "local");
  snprintf(card.actionClass, sizeof(card.actionClass), "day");
  static constexpr const char* ids[] = {"review", "next", "known"};
  static constexpr const char* labels[] = {"Again", "Next", "Known"};
  card.choiceCount = 3;
  for (uint8_t i = 0; i < card.choiceCount; i++) {
    snprintf(card.choices[i].id, sizeof(card.choices[i].id), "%s", ids[i]);
    snprintf(card.choices[i].label, sizeof(card.choices[i].label), "%s", labels[i]);
  }
}

WordSource lightDailyWord(const uint32_t day, PocketDaily::Card& card) {
  memset(&card, 0, sizeof(card));
  // One paint's scratch, freed on return; never retained by the caller.
  auto scratch = makeUniqueNoThrow<PackScratch>();
  if (!scratch) {
    LOG_ERR("WORD", "OOM allocating %u B learning-pack scratch", (unsigned)sizeof(PackScratch));
    return WordSource::OutOfMemory;
  }
  WordSource source = WordSource::BuiltIn;
  if (LearningPack::readDayRecord(day, scratch->header, scratch->record)) {
    packDailyWord(scratch->record, scratch->header.contentVersion, card);
    source = WordSource::Pack;
  } else {
    builtInDailyWord(day, 0, card);
  }
  finishDailyWord(card);
  return source;
}

}  // namespace PocketDaily::Home
