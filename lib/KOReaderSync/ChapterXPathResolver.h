#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>

class ChapterXPathResolver {
 public:
  /**
   * Resolve the Nth paragraph in a spine item to its real XHTML ancestry path.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]
   *
   * An empty string means parsing failed or the paragraph index was not found.
   */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /**
   * Resolve a zero-based visible-codepoint offset in a spine item to its real
   * XHTML ancestry path plus text-node offset.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text()[1].0
   *
   * An empty string means parsing failed or the offset did not resolve inside
   * paragraph/list-item text.
   */
  static std::string findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                   uint32_t visibleTextOffset);

  /**
   * Resolve intra-spine progress to a real XHTML ancestry path plus text offset.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text().96
   *
   * An empty string means parsing failed or the location could not be resolved.
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

  /**
   * Resolve a position inside the Nth <p> of a spine item (N as in Section's paragraph LUT)
   * to a KOReader XPath with a text offset. `fraction` (0..1) is how far into that
   * paragraph's text the position lies; 0 returns the paragraph element itself.
   * Streams the item twice. An empty string means parsing failed.
   */
  static std::string findXPathForParagraphProgress(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                   uint16_t paragraphIndex, float fraction);

  /**
   * Chapter text offsets: 0-based code point indexes into the character data inside the spine
   * item's <body>, as expat delivers it (HTML named entities as their decoded text; whitespace,
   * and the text of hidden elements, included). ChapterHtmlSlimParser stamps each laid-out page
   * with the offset of its first character (Page::textOffset) in the same count.
   *
   * findXPathForTextOffset returns the XPointer of the character at `textOffset`
   * (/body/DocFragment[N]/body/.../text()[k].offset). A character in a whitespace-only text
   * node moves to the next non-whitespace character, since such nodes are not numbered.
   * Streams the item once. An empty string means parsing failed or the offset lies past the
   * body's text.
   */
  static std::string findXPathForTextOffset(const std::shared_ptr<Epub>& epub, int spineIndex, uint32_t textOffset);

  /**
   * The inverse: the chapter text offset an XPointer into this spine item points at, with
   * the resolution rules of docs/reading-progress-v1.md (text()[k] counts non-whitespace
   * direct runs; an offset past a run clamps to its end; a missing run lands on the element;
   * an element-only path lands on the element's first character). False when the path's
   * element does not exist or the item cannot be parsed. Streams the item once.
   */
  static bool findTextOffsetForXPath(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath,
                                     uint32_t& textOffset);
};
