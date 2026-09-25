#include "PocketUIHost.h"

#include <GfxRenderer.h>
#include <SdCardFont.h>

#include <memory>
#include <mutex>
#include <new>

#include "HostHome.h"
#include "pocket_daily/ContentCard.h"
#include "pocket_daily/ContentImageRenderer.h"
#include "pocket_daily/ContentPageRenderer.h"
#include "pocket_daily/ContentTextLayout.h"

using namespace PocketDaily::Content;
namespace {
constexpr size_t maxFontBytes = 64 * 1024 * 1024;
constexpr size_t maxImageBytes = 32784;
constexpr const char* fontPath = "/preview.cpfont";
// CrossPoint MiniBidi owns static line scratch. Keep its algorithm unchanged
// and serialize host rasterization even across independent C ABI contexts.
std::mutex renderMutex;
struct Bytes {
  const uint8_t* data;
  size_t size;
  ManifestSource source() {
    return {this, size, [](void* context, size_t offset, uint8_t* output, size_t count) {
              const auto& bytes = *static_cast<Bytes*>(context);
              if (offset > bytes.size || count > bytes.size - offset) return false;
              if (count) std::memcpy(output, bytes.data + offset, count);
              return true;
            }};
  }
};
// Validate bounded caller-owned chrome strings before firmware UTF-8 layout.
// Card text is validated by decodeContentCard instead.
bool chromeText(const char* text, size_t capacity) {
  const size_t length = strnlen(text, capacity);
  if (length == capacity) return false;
  for (size_t i = 0; i < length;) {
    uint32_t point = static_cast<uint8_t>(text[i++]);
    unsigned remaining = 0;
    uint32_t minimum = 0;
    if (point >= 0xC2 && point <= 0xDF) {
      point &= 0x1F;
      remaining = 1;
      minimum = 0x80;
    } else if (point >= 0xE0 && point <= 0xEF) {
      point &= 0x0F;
      remaining = 2;
      minimum = 0x800;
    } else if (point >= 0xF0 && point <= 0xF4) {
      point &= 7;
      remaining = 3;
      minimum = 0x10000;
    } else if (point >= 0x80) {
      return false;
    }
    if (remaining > length - i) return false;
    for (unsigned j = 0; j < remaining; ++j) {
      const uint8_t next = static_cast<uint8_t>(text[i++]);
      if ((next & 0xC0) != 0x80) return false;
      point = (point << 6) | (next & 0x3F);
    }
    if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF) || point < 0x20 ||
        (point >= 0x7F && point <= 0x9F))
      return false;
  }
  return true;
}
}  // namespace

// Host heap only: owns bounded font bytes and one framebuffer. Neither these
// allocations nor the C ABI are compiled into firmware. Renderer is destroyed
// before its font, and the font before its backing bytes.
struct pdui_context {
  std::vector<uint8_t> fontBytes;
  PocketUIHost::Asset fontAsset;
  SdCardFont font;
  HalDisplay panel;
  GfxRenderer renderer;
  PocketUIHost::UserCards cards;
  bool frameValid = false;
  pdui_context(uint32_t width, uint32_t height, const uint8_t* bytes, size_t size)
      : fontBytes(bytes, bytes + size), fontAsset{fontPath, fontBytes}, panel(width, height), renderer(panel) {}
};

uint32_t pdui_abi_version() noexcept { return 1; }
int32_t pdui_create(uint32_t width, uint32_t height, uint32_t orientation, const uint8_t* font, size_t size,
                    pdui_context** output) noexcept {
  if (!output) return PDUI_INVALID_ARGUMENT;
  *output = nullptr;
  if (!width || width > 2048 || width % 8 || !height || height > 2048 || orientation > 3 || !font || !size ||
      size > maxFontBytes)
    return PDUI_INVALID_ARGUMENT;
  try {
    auto context = std::make_unique<pdui_context>(width, height, font, size);
    const PocketUIHost::AssetScope scope({&context->fontAsset, 1});
    if (!context->font.load(fontPath, SdCardFont::LoadMode::BoundedUI)) return PDUI_INVALID_FONT;
    context->renderer.begin();
    context->renderer.setOrientation(static_cast<GfxRenderer::Orientation>(orientation));
    context->renderer.insertFont(1, EpdFontFamily(context->font.getEpdFont(0), context->font.getEpdFont(1)));
    context->renderer.registerSdCardFont(1, &context->font);
    PocketUIHost::registerUiFonts(context->renderer);
    *output = context.release();
    return PDUI_OK;
  } catch (const std::bad_alloc&) {
    return PDUI_OUT_OF_MEMORY;
  } catch (...) {
    return PDUI_INTERNAL_ERROR;
  }
}
void pdui_destroy(pdui_context* context) noexcept { delete context; }

int32_t pdui_render_content(pdui_context* context, const uint8_t* cardBytes, size_t cardSize, const uint8_t* imageBytes,
                            size_t imageSize, const pdui_content_options* options) noexcept {
  if (!context) return PDUI_INVALID_ARGUMENT;
  context->frameValid = false;
  if (!options || (cardBytes == nullptr) != (cardSize == 0) || (imageBytes == nullptr) != (imageSize == 0) ||
      imageSize > maxImageBytes || !chromeText(options->empty_title, sizeof(options->empty_title)) ||
      !chromeText(options->empty_message, sizeof(options->empty_message)))
    return PDUI_INVALID_ARGUMENT;
  for (const auto& label : options->labels)
    if (!chromeText(label, sizeof(label))) return PDUI_INVALID_ARGUMENT;
  try {
    const std::lock_guard<std::mutex> renderLock(renderMutex);
    const PocketUIHost::AssetScope scope({&context->fontAsset, 1});
    ContentCard card{};  // Host stack, not the device render-task stack.
    Bytes input{cardBytes, cardSize}, image{imageBytes, imageSize};
    if (cardBytes && decodeContentCard(input.source(), card) != CardResult::Ok) return PDUI_INVALID_CARD;
    if ((card.imagePath[0] != 0) != (imageSize != 0)) return PDUI_INVALID_IMAGE;
    ImageInfo imageInfo;
    if (imageSize && validateContentImage(image.source(), imageInfo) != ImageResult::Ok) return PDUI_INVALID_IMAGE;
    struct Text {
      const char* value;
      uint8_t styles;
    };
    const Text texts[]{{card.card.title, 2},        {card.card.question, 1},
                       {card.card.context, 1},      {options->empty_title, 3},
                       {options->empty_message, 3}, {options->labels[0], 3},
                       {options->labels[1], 3},     {options->labels[2], 3},
                       {options->labels[3], 3},     {"...", 3}};
    for (const auto& text : texts) {
      struct Check {
        GfxRenderer& renderer;
        uint8_t styles;
      } check{context->renderer, text.styles};
      if (!checkLayoutText(text.value, &check, [](void* value, const char* chunk) {
            auto& c = *static_cast<Check*>(value);
            return c.renderer.ensureSdCardFontReady(1, chunk, c.styles) == 0 &&
                   c.renderer.prewarmSdCardFont(1, chunk, c.styles) == 0;
          }))
        return PDUI_INVALID_FONT;
    }
    if (context->font.boundedReadFailed()) return PDUI_INVALID_FONT;
    const ContentPageOptions page{1,
                                  options->side_padding,
                                  options->top_padding,
                                  options->spacing,
                                  options->empty_title,
                                  options->empty_message,
                                  {options->labels[0], options->labels[1], options->labels[2], options->labels[3]}};
    struct ImageContext {
      GfxRenderer& renderer;
      Bytes& image;
      bool failed = false;
    } imageContext{context->renderer, image};
    const ContentPageImagePainter painter{
        &imageContext, [](void* value, const char*, int x, int y, int width, int height) {
          auto& c = *static_cast<ImageContext*>(value);
          c.failed = renderContentImage(c.renderer, c.image.source(), x, y, width, height) != ImageResult::Ok;
          return !c.failed;
        }};
    if (!renderContentPage(context->renderer, cardBytes ? &card : nullptr, page, painter))
      return imageContext.failed ? PDUI_INVALID_IMAGE : PDUI_RENDER_FAILED;
    if (context->font.boundedReadFailed() || !context->panel.guardsIntact()) return PDUI_RENDER_FAILED;
    context->frameValid = true;
    return PDUI_OK;
  } catch (const std::bad_alloc&) {
    return PDUI_OUT_OF_MEMORY;
  } catch (...) {
    return PDUI_INTERNAL_ERROR;
  }
}

int32_t pdui_set_cards(pdui_context* context, const pdui_card_input* cards, uint32_t count) noexcept {
  if (!context || count > 3 || (count && !cards)) return PDUI_INVALID_ARGUMENT;
  try {
    PocketUIHost::UserCards parsed;
    for (uint32_t i = 0; i < count; ++i) {
      const auto& in = cards[i];
      if (!in.card || !in.card_size || (in.image == nullptr) != (in.image_size == 0) || in.image_size > maxImageBytes)
        return PDUI_INVALID_ARGUMENT;
      ContentCard card{};
      Bytes input{in.card, in.card_size}, image{in.image, in.image_size};
      if (decodeContentCard(input.source(), card) != CardResult::Ok) return PDUI_INVALID_CARD;
      if ((card.imagePath[0] != 0) != (in.image_size != 0)) return PDUI_INVALID_IMAGE;
      ImageInfo imageInfo;
      if (in.image_size && validateContentImage(image.source(), imageInfo) != ImageResult::Ok) return PDUI_INVALID_IMAGE;
      PocketUIHost::UserCard user;
      user.card = card.card;
      // Same Home summary as the reader: text, then context when it fits.
      user.summary = card.card.question;
      if (card.card.context[0]) user.summary += std::string(" - ") + card.card.context;
      if (in.image_size) user.image.assign(in.image, in.image + in.image_size);
      parsed.push_back(std::move(user));
    }
    context->cards = std::move(parsed);
    return PDUI_OK;
  } catch (const std::bad_alloc&) {
    return PDUI_OUT_OF_MEMORY;
  } catch (...) {
    return PDUI_INTERNAL_ERROR;
  }
}

namespace {
bool toProfile(const pdui_profile& in, PocketDaily::DailyProfile::Profile& out) {
  using namespace PocketDaily::DailyProfile;
  if (in.home_count > HOME_ITEM_CAP || in.sleep_count > SLEEP_SECTION_CAP || in.daily_word > 1 || in.next_event > 1 ||
      in.weather > 2 || in.sleep_mode > 1)
    return false;
  Profile p;
  p.homeCount = in.home_count;
  for (uint8_t i = 0; i < in.home_count; ++i) p.homeItems[i] = static_cast<HomeItem>(in.home_items[i]);
  p.dailyWord = in.daily_word != 0;
  p.weather = static_cast<WeatherPanel>(in.weather);
  p.nextEvent = in.next_event != 0;
  p.sleepMode = static_cast<SleepMode>(in.sleep_mode);
  p.sleepCount = in.sleep_count;
  for (uint8_t i = 0; i < in.sleep_count; ++i) p.sleepSections[i] = static_cast<SleepSection>(in.sleep_sections[i]);
  if (!valid(p)) return false;
  out = p;
  return true;
}

bool isX3Panel(const HalDisplay& panel) { return std::max(panel.getDisplayWidth(), panel.getDisplayHeight()) == 792; }
}  // namespace

int32_t pdui_render_home(pdui_context* context, const pdui_profile* profile, uint32_t samples,
                         uint32_t selected) noexcept {
  if (!context) return PDUI_INVALID_ARGUMENT;
  context->frameValid = false;
  PocketDaily::DailyProfile::Profile parsed;
  if (!profile || samples > PDUI_SAMPLE_ALL || !toProfile(*profile, parsed)) return PDUI_INVALID_ARGUMENT;
  try {
    const std::lock_guard<std::mutex> renderLock(renderMutex);
    const PocketUIHost::AssetScope scope({&context->fontAsset, 1});
    const auto sample = PocketUIHost::buildSample(samples);
    PocketDaily::Home::Row rows[PocketDaily::DailyProfile::HOME_ITEM_CAP];
    PocketDaily::Home::HomeView view;
    view.isX3 = isX3Panel(context->panel);
    view.rows = rows;
    view.count = PocketUIHost::buildRows(parsed, sample, samples, context->cards, rows,
                                         PocketDaily::DailyProfile::HOME_ITEM_CAP);
    view.selected = view.count ? static_cast<int>(std::min<uint32_t>(selected, view.count - 1)) : 0;
    view.reading = sample.reading;
    view.glance = &sample.glance;
    view.profile = parsed;
    PocketDaily::Home::renderHome(context->renderer, view, PocketUIHost::hostEnv(context->renderer, context->cards));
    if (context->font.boundedReadFailed() || !context->panel.guardsIntact()) return PDUI_RENDER_FAILED;
    context->frameValid = true;
    return PDUI_OK;
  } catch (const std::bad_alloc&) {
    return PDUI_OUT_OF_MEMORY;
  } catch (...) {
    return PDUI_INTERNAL_ERROR;
  }
}

int32_t pdui_render_brief(pdui_context* context, const pdui_profile* profile, uint32_t samples) noexcept {
  if (!context) return PDUI_INVALID_ARGUMENT;
  context->frameValid = false;
  PocketDaily::DailyProfile::Profile parsed;
  if (!profile || samples > PDUI_SAMPLE_ALL || !toProfile(*profile, parsed)) return PDUI_INVALID_ARGUMENT;
  try {
    const std::lock_guard<std::mutex> renderLock(renderMutex);
    const PocketUIHost::AssetScope scope({&context->fontAsset, 1});
    const auto sample = PocketUIHost::buildSample(samples);
    PocketDaily::Home::BriefView view;
    view.isSleep = true;
    view.glance = &sample.glance;
    view.reading = sample.reading;
    // As on the reader: the first own card, else the sample, else the daily word.
    const PocketDaily::Card* first = context->cards.empty() ? nullptr : &context->cards.front().card;
    const PocketDaily::Card* sampleCard = sample.card.cardId[0] ? &sample.card : nullptr;
    view.pocketCard = first ? first : sampleCard ? sampleCard : &sample.word;
    view.pinnedCard = first ? first : sampleCard;
    view.pinnedHasImage = first && !context->cards.front().image.empty();
    view.status = "Powered off";
    view.profile = parsed;
    PocketDaily::Home::renderBrief(context->renderer, view, PocketUIHost::hostEnv(context->renderer, context->cards));
    if (context->font.boundedReadFailed() || !context->panel.guardsIntact()) return PDUI_RENDER_FAILED;
    context->frameValid = true;
    return PDUI_OK;
  } catch (const std::bad_alloc&) {
    return PDUI_OUT_OF_MEMORY;
  } catch (...) {
    return PDUI_INTERNAL_ERROR;
  }
}

int32_t pdui_get_frame_info(const pdui_context* context, pdui_frame_info* output) noexcept {
  if (!output) return PDUI_INVALID_ARGUMENT;
  *output = {};
  if (!context) return PDUI_INVALID_ARGUMENT;
  if (!context->frameValid) return PDUI_NO_FRAME;
  *output = {context->panel.getDisplayWidth(),
             context->panel.getDisplayHeight(),
             context->panel.getDisplayWidthBytes(),
             context->panel.getBufferSize(),
             static_cast<uint32_t>(context->renderer.getScreenWidth()),
             static_cast<uint32_t>(context->renderer.getScreenHeight()),
             static_cast<uint32_t>(context->renderer.getOrientation())};
  return PDUI_OK;
}
int32_t pdui_copy_frame(const pdui_context* context, uint8_t* output, size_t capacity) noexcept {
  if (!context || !output) return PDUI_INVALID_ARGUMENT;
  if (!context->frameValid) return PDUI_NO_FRAME;
  if (capacity < context->panel.getBufferSize()) return PDUI_BUFFER_TOO_SMALL;
  std::memcpy(output, context->panel.getFrameBuffer(), context->panel.getBufferSize());
  return PDUI_OK;
}
