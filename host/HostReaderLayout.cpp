#include "HostReaderLayout.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstring>

#include "pocket_daily/ContentTextLayout.h"
#include "util/CjkScript.h"
#include "util/PowerWakeCue.h"

namespace PocketUIHost {
namespace {
using namespace PocketDaily;
struct DrawContext {
  const Preview::LayoutInputs& inputs;
  Home::Env imageEnv;
  bool failed = false;
  const uint8_t* cover = nullptr;
  size_t coverSize = 0;
};
}  // namespace

bool renderReaderLayout(GfxRenderer& renderer, const PocketDaily::Preview::LayoutInputs& in,
                        const PocketDaily::DailyProfile::Profile& profile, const UserCards& cards, const bool brief,
                        const bool sleepCover, const bool actual, const bool wakeIndicator, const uint8_t* cover,
                        const size_t coverSize) {
  using namespace PocketDaily;
  DrawContext context{in, hostEnv(renderer, cards), false, cover, coverSize};
  Home::Env env;
  env.context = &context;
  env.metrics = in.metrics;
  env.text = {&context, [](void* value, const char* text, int fallback, EpdFontFamily::Style) {
                auto& d = *static_cast<DrawContext*>(value);
                if (!CjkScript::needsCjkFont(text)) return fallback;
                // Device preflights both styles and refuses missing coverage. Never render
                // an approximate replacement glyph and call it a successful exact frame.
                auto& r = *static_cast<GfxRenderer*>(d.imageEnv.text.context);
                if (!Content::checkLayoutText(text, &r, [](void* data, const char* chunk) {
                      return static_cast<GfxRenderer*>(data)->ensureSdCardFontReady(1, chunk, 3) == 0;
                    }))
                  d.failed = true;
                return 1;
              }};
  env.labels = env.text;
  env.drawHeader = [](void* value, const GfxRenderer& r, int x, int y, int w, int h, const char* title,
                      const char* subtitle) {
    const auto& d = *static_cast<const DrawContext*>(value);
    HeaderPainter::paintHeader(r, d.inputs.header, x, y, w, h, title, subtitle);
  };
  env.drawCover = [](void* value, GfxRenderer& r, int x, int y, int w, int h) {
    if (w <= 12 || h <= 18) return false;
    auto& d = *static_cast<DrawContext*>(value);
    if (d.coverSize) {
      HalFile file({d.cover, d.coverSize});
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 ||
          bitmap.getWidth() > 2048 || bitmap.getHeight() > 2048) {
        d.failed = true;
        return false;
      }
      const float scale = std::min(float(w - 4) / bitmap.getWidth(), float(h - 4) / bitmap.getHeight());
      const int dw = std::max(1, int(bitmap.getWidth() * scale)), dh = std::max(1, int(bitmap.getHeight() * scale));
      const int ax = x + (w - dw) / 2, ay = y + (h - dh) / 2;
      r.drawBitmap(bitmap, ax, ay, dw, dh, 0, 0, true);
      r.drawRect(ax, ay, dw, dh, 2, true);
      return true;
    }
    r.drawRect(x, y, w, h, 2, true);
    HomeDraw::drawCoverPlaceholder(r, x, y, w, h);
    return false;
  };
  env.drawCardImage = [](void* value, GfxRenderer& r, const char* id, int x, int y, int w, int h) {
    auto& d = *static_cast<DrawContext*>(value);
    const int drawn = d.imageEnv.drawCardImage(d.imageEnv.context, r, id, x, y, w, h);
    if (drawn <= 0) d.failed = true;
    return drawn;
  };
  const Home::Reading reading{in.book.valid, in.book.title, in.book.author, in.book.percent};
  if (brief) {
    Home::BriefView view;
    view.isSleep = true;
    if (actual && wakeIndicator) view.contentTopInset = PowerWakeCue::portraitContentTop(in.isX3);
    view.sleepCover = sleepCover;
    view.glance = &in.glance.glance;
    view.reading = reading;
    Card hiddenWord{};
    strcpy(hiddenWord.cardId, "local:word");
    strcpy(hiddenWord.module, "local");
    // Match ScreenFrame's undrawn placeholder when dailyWord is disabled.
    const Card* first = cards.empty() ? nullptr : &cards.front().card;
    view.pocketCard = first ? first : profile.dailyWord ? &in.word : &hiddenWord;
    view.pinnedCard = first;
    view.pinnedHasImage = first && !cards.front().image.empty();
    view.snapshotStale = in.glanceStale;
    view.status = in.labels[actual ? 20 : 19];
    view.profile = profile;
    view.strings = Preview::layoutStrings(in);
    Home::renderBrief(renderer, view, env);
  } else {
    Home::Row rows[Home::ROW_CAP]{};
    Home::RowSource sources[DailyProfile::HOME_ITEM_CAP];
    const int count = Home::homeRowSources(profile, {in.book.valid, !cards.empty()}, sources);
    int n = 0;
    const Card* firstCard = nullptr;
    for (int i = 0; i < count && n < Home::ROW_CAP; ++i) {
      switch (sources[i]) {
        case Home::RowSource::Reading:
          rows[n++] = {true, false, false, false, false, "local:reading", "", ""};
          break;
        case Home::RowSource::AppCards:
          for (const auto& card : cards) {
            if (n >= Home::ROW_CAP) break;
            if (!card.card.cardId[0]) continue;
            if (n == 0) firstCard = &card.card;
            rows[n++] = {false, true, true, false, !card.image.empty(), card.card.cardId, "", ""};
          }
          break;
        case Home::RowSource::DailyWord:
          if (n == 0) firstCard = &in.word;
          rows[n++] = {false, true, false, true, false, in.word.cardId, "", ""};
          break;
      }
    }
    char project[40]{};
    char activity[HomeDraw::kWrapLineBytes]{};
    if (firstCard) {
      snprintf(project, sizeof(project), "%s", Home::cardRowTitle(*firstCard));
      Home::cardRowActivity(*firstCard, activity, sizeof(activity));
      rows[0].project = project;
      rows[0].activity = activity;
    }
    Home::HomeView view;
    view.isX3 = in.isX3;
    view.rows = rows;
    view.count = n;
    view.selected = 0;
    view.reading = reading;
    view.glance = &in.glance.glance;
    view.snapshotStale = in.glanceStale;
    view.profile = profile;
    view.strings = Preview::layoutStrings(in);
    Home::renderHome(renderer, view, env);
  }
  if (brief && actual && wakeIndicator) PowerWakeCue::draw(renderer, in.isX3, in.labels[21]);
  return !context.failed;
}
}  // namespace PocketUIHost
