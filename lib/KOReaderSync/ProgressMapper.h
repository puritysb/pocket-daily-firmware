#pragma once
#include <Epub.h>
#include <GfxRenderer.h>

#include <cstdint>
#include <memory>
#include <string>

/**
 * CrossPoint position representation.
 */
struct CrossPointPosition {
  int spineIndex;                  // Current spine item (chapter) index
  int pageNumber;                  // Current page within the spine item
  int totalPages;                  // Total pages in the current spine item
  uint16_t paragraphIndex = 0;     // 1-based synthetic paragraph index from XPath p[N]
  bool hasParagraphIndex = false;  // True when paragraphIndex was resolved from XPath
  uint16_t liIndex = 0;            // Running <li> count at the matched XPath element
  bool hasLiIndex = false;         // True when target element is <li> and liIndex was resolved
  char xpathAnchorId[64] = {};     // First <a id> captured inside the matched XPath element
  // Chapter text offset of the XPath's character (ChapterXPathResolver::findTextOffsetForXPath).
  // With the section laid out, Section::findPageForTextOffset turns it into the exact page;
  // pageNumber above is only the estimate for when it is not.
  uint32_t textOffset = 0;
  bool hasTextOffset = false;
};

/**
 * Progress position representation.
 */
struct SavedProgressPosition {
  std::string xpath;  // XPath-like progress string
  float percentage;   // Progress percentage (0.0 to 1.0)
};

/**
 * Where an XPath lands inside one spine item, counted in the item's visible text
 * (codepoints outside markup and outside head/style/script/title, entities decoded).
 */
struct XPathSpineTarget {
  bool found = false;            // The XPath's element (and text node, if any) exists in the item
  size_t visibleChar = 0;        // 0-based visible codepoint index the XPath points at
  size_t totalVisibleChars = 0;  // Visible codepoints in the whole item
  uint16_t paragraphIndex = 0;   // <p> count at the matched element (0 = none)
  uint16_t liIndex = 0;          // <li> count at the matched element when it is an <li>
  char anchorId[64] = {};        // First <a id> inside the matched element
};

/**
 * Maps between CrossPoint and SavedProgress position formats, such as those used by KOReader.
 *
 * CrossPoint tracks position as (spineIndex, pageNumber).
 * SavedProgress uses XPath-like strings + percentage.
 *
 * Since CrossPoint discards HTML structure during parsing, we generate
 * synthetic XPath strings based on spine index, using percentage as the
 * primary sync mechanism.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to SavedProgress format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return SavedProgress position
   */
  static SavedProgressPosition toSavedProgress(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * Convert SavedProgress position to CrossPoint format.
   *
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param savedPos SavedProgress position
   * @param renderer GfxRenderer for page count estimation
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const SavedProgressPosition& savedPos,
                                         GfxRenderer& renderer, int currentSpineIndex = -1,
                                         int totalPagesInCurrentSpine = 0, int fallbackTotalPages = 0);

  /**
   * Stream one spine item and locate the XPath's element/text offset in its visible text.
   * Text steps count only direct text nodes that contain non-whitespace, and the offset
   * counts codepoints, matching KOReader/crengine serialization.
   */
  static XPathSpineTarget locateInSpine(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath);

 private:
  /**
   * Generate a fallback XPath by streaming the spine item's XHTML and resolving
   * a paragraph/text position from intra-spine progress.
   * Produces a full ancestry path such as
   * /body/DocFragment[3]/body/p[42]/text().17.
   */
  static std::string generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);
};
