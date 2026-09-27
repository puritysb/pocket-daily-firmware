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
};
