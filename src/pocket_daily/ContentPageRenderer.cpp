#include "ContentPageRenderer.h"

#include <GfxRenderer.h>

#include "ContentCard.h"
#include "ContentPageLayout.h"
#include "ContentTextLayout.h"

namespace PocketDaily::Content {
namespace {
unsigned textBlock(const GfxRenderer& renderer, int font, const char* text, int x, int y, int width, unsigned lines,
                   int advance, EpdFontFamily::Style style) {
  struct Target {
    const GfxRenderer& renderer;
    int font, x, y, advance;
    EpdFontFamily::Style style;
  } target{renderer, font, x, y, advance, style};
  const TextPainter painter{&target,
                            [](void* context, const char* line) {
                              const auto& t = *static_cast<Target*>(context);
                              return t.renderer.getTextWidth(t.font, line, t.style);
                            },
                            [](void* context, const char* line, unsigned index) {
                              const auto& t = *static_cast<Target*>(context);
                              t.renderer.drawText(t.font, t.x, t.y + index * t.advance, line, true, t.style);
                            }};
  return drawWrappedText(text, width, lines, painter).lines;
}

int bodyBlock(const GfxRenderer& renderer, const ContentCard& card, const ContentPageOptions& options,
              const ContentPageLayout& layout, int y, int width) {
  y += textBlock(renderer, options.fontId, card.card.question, layout.x, y, width, layout.remainingLines(y, 6),
                 layout.advance, EpdFontFamily::REGULAR) *
       layout.advance;
  y += textBlock(renderer, options.fontId, card.card.context, layout.x, y, width, layout.remainingLines(y, 4),
                 layout.advance, EpdFontFamily::REGULAR) *
       layout.advance;
  return y;
}
}  // namespace

bool renderContentPage(const GfxRenderer& renderer, const ContentCard* card, const ContentPageOptions& options,
                       const ContentPageImagePainter& images) {
  for (const auto* label : options.labels)
    if (!label) return false;
  if (!card && (!options.emptyTitle || !options.emptyMessage)) return false;
  if (card && card->layout != CardLayout::TextFirst && card->layout != CardLayout::ImageFirst &&
      card->layout != CardLayout::SideBySide)
    return false;
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const auto layout = contentPageLayout(renderer.getScreenWidth(), renderer.getScreenHeight(), top, right, bottom, left,
                                        options.sidePadding, options.topPadding, options.spacing,
                                        renderer.getLineHeight(options.fontId));
  if (!layout.valid) return false;
  const int x = layout.x, width = layout.width, advance = layout.advance, footer = layout.footer;
  int y = layout.y;
  renderer.clearScreen();
  y += textBlock(renderer, options.fontId, card ? card->card.title : options.emptyTitle, x, y, width, 2, advance,
                 EpdFontFamily::BOLD) *
       advance;
  y += layout.gap;
  if (card) {
    if (card->imagePath[0] && card->layout == CardLayout::SideBySide) {
      const int textWidth = (width - layout.gap) / 2;
      const int imageWidth = width - layout.gap - textWidth;
      const int height = layout.imageHeight(y);
      if (textWidth <= 0 || imageWidth <= 0 || height <= 0 || !images.draw ||
          !images.draw(images.context, card->imagePath, x + textWidth + layout.gap, y, imageWidth, height))
        return false;
      bodyBlock(renderer, *card, options, layout, y, textWidth);
    } else {
      if (card->imagePath[0] && card->layout == CardLayout::ImageFirst) {
        const int height = layout.imageHeight(y) / 3;
        if (height <= 0 || !images.draw || !images.draw(images.context, card->imagePath, x, y, width, height))
          return false;
        y += height + layout.gap;
      }
      y = bodyBlock(renderer, *card, options, layout, y, width);
      const int height = layout.imageHeight(y);
      if (card->imagePath[0] && card->layout == CardLayout::TextFirst && height > 0 &&
          (!images.draw || !images.draw(images.context, card->imagePath, x, y, width, height)))
        return false;
    }
  } else {
    textBlock(renderer, options.fontId, options.emptyMessage, x, y, width, layout.remainingLines(y, 2), advance,
              EpdFontFamily::REGULAR);
  }
  for (unsigned i = 0; i < 4; ++i)
    textBlock(renderer, options.fontId, options.labels[i], x + i * (width / 4), footer, width / 4, 1, advance,
              EpdFontFamily::REGULAR);
  return true;
}
}  // namespace PocketDaily::Content
