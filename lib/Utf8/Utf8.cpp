#include "Utf8.h"

#include <cstring>

#include "Utf8ComposeTable.h"

namespace {
// Look up canonical composition, including algorithmic Hangul LV / LVT pairs.
uint32_t utf8ComposePair(const uint32_t base, const uint32_t mark) {
  if (base >= 0x1100 && base <= 0x1112 && mark >= 0x1161 && mark <= 0x1175) {
    return 0xAC00 + (base - 0x1100) * 588 + (mark - 0x1161) * 28;
  }
  if (base >= 0xAC00 && base <= 0xD7A3 && (base - 0xAC00) % 28 == 0 && mark >= 0x11A8 && mark <= 0x11C2) {
    return base + mark - 0x11A7;
  }
  if (!utf8IsCombiningMark(mark) || base > 0xFFFF || mark > 0xFFFF) return 0;
  int lo = 0;
  int hi = kUtf8ComposeTableSize - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const Utf8ComposeEntry& e = kUtf8ComposeTable[mid];
    if (e.base < base || (e.base == base && e.mark < mark)) {
      lo = mid + 1;
    } else if (e.base > base || (e.base == base && e.mark > mark)) {
      hi = mid - 1;
    } else {
      return e.composed;
    }
  }
  return 0;
}
}  // namespace

uint32_t utf8DecomposedBase(const uint32_t cp) {
  if (cp < 0x00C0) return 0;  // no precomposed Latin letter below this
  for (const auto& e : kUtf8ComposeTable) {
    if (e.composed == cp) return e.base;
  }
  return 0;
}

std::string utf8ComposeNfc(const std::string& in) {
  // Fast path: NFC composition can only change text that contains a combining
  // diacritical mark U+0300-036F (UTF-8 lead byte 0xCC or 0xCD) or conjoining
  // Hangul jamo (lead byte 0xE1 for U+1000-1FFF). Plain ASCII and
  // already-precomposed (NFC) text -- the vast majority of words -- have none, so
  // return them untouched without walking codepoints or allocating. A 0xCD or
  // 0xE1 that is actually a non-composing codepoint (e.g. Georgian, Cherokee)
  // just falls through to the full pass below.
  bool maybeHasMarks = false;
  for (const unsigned char c : in) {
    if (c == 0xCC || c == 0xCD || c == 0xE1) {
      maybeHasMarks = true;
      break;
    }
  }
  if (!maybeHasMarks) return in;

  std::string out;
  out.reserve(in.size());
  const unsigned char* p = reinterpret_cast<const unsigned char*>(in.c_str());
  uint32_t base = 0;
  bool haveBase = false;
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    const uint32_t composed = haveBase ? utf8ComposePair(base, cp) : 0;
    if (composed) {
      base = composed;  // keep accumulating marks or trailing jamo
      continue;
    }
    if (utf8IsCombiningMark(cp)) {
      // No composition: flush the pending base, then emit the mark unchanged.
      if (haveBase) {
        utf8AppendCodepoint(base, out);
        haveBase = false;
      }
      utf8AppendCodepoint(cp, out);
    } else {
      if (haveBase) {
        utf8AppendCodepoint(base, out);
      }
      base = cp;
      haveBase = true;
    }
  }
  if (haveBase) utf8AppendCodepoint(base, out);
  return out;
}

void utf8ComposeNfcInPlace(char* buffer) {
  const auto* read = reinterpret_cast<const unsigned char*>(buffer);
  char* write = buffer;
  char* baseStart = buffer;
  uint32_t base = 0;
  while (*read) {
    const auto* start = read;
    const uint32_t cp = utf8NextCodepoint(&read);
    const uint32_t composed = utf8ComposePair(base, cp);
    if (composed) {
      // All supported compositions are BMP codepoints and fit within the
      // consumed pair's bytes, so rewriting the base cannot overtake read.
      write = baseStart;
      if (composed < 0x800) {
        *write++ = static_cast<char>(0xC0 | (composed >> 6));
      } else {
        *write++ = static_cast<char>(0xE0 | (composed >> 12));
        *write++ = static_cast<char>(0x80 | ((composed >> 6) & 0x3F));
      }
      *write++ = static_cast<char>(0x80 | (composed & 0x3F));
      base = composed;
    } else {
      baseStart = write;
      const size_t length = read - start;
      // Preserve uncomposed bytes, including malformed UTF-8: replacement
      // characters could expand the buffer. Earlier compositions may overlap.
      memmove(write, start, length);
      write += length;
      base = utf8IsCombiningMark(cp) ? 0 : cp;
    }
  }
  *write = '\0';
}

int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80) return 1;          // 0xxxxxxx
  if ((c >> 5) == 0x6) return 2;   // 110xxxxx
  if ((c >> 4) == 0xE) return 3;   // 1110xxxx
  if ((c >> 3) == 0x1E) return 4;  // 11110xxx
  return 1;                        // fallback for invalid
}

uint32_t utf8NextCodepoint(const unsigned char** string) {
  if (**string == 0) {
    return 0;
  }

  const unsigned char lead = **string;
  const int bytes = utf8CodepointLen(lead);
  const uint8_t* chr = *string;

  // Invalid lead byte (stray continuation byte 0x80-0xBF, or 0xFE/0xFF)
  if (bytes == 1 && lead >= 0x80) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  if (bytes == 1) {
    (*string)++;
    return chr[0];
  }

  // Validate continuation bytes before consuming them
  for (int i = 1; i < bytes; i++) {
    if ((chr[i] & 0xC0) != 0x80) {
      // Missing or invalid continuation byte — skip all bytes consumed so far
      *string += i;
      return REPLACEMENT_GLYPH;
    }
  }

  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1);  // mask header bits

  for (int i = 1; i < bytes; i++) {
    cp = (cp << 6) | (chr[i] & 0x3F);
  }

  // Reject overlong encodings, surrogates, and out-of-range values
  const bool overlong = (bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000);
  const bool surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
  if (overlong || surrogate || cp > 0x10FFFF) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  *string += bytes;

  return cp;
}

void utf8AppendCodepoint(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

int utf8SafeTruncateBuffer(const char* buf, int len) {
  if (len <= 0) return 0;

  // Walk back past continuation bytes (10xxxxxx) to find the lead byte
  int leadPos = len - 1;
  while (leadPos > 0 && (static_cast<uint8_t>(buf[leadPos]) & 0xC0) == 0x80) {
    leadPos--;
  }

  // Determine expected length of the sequence starting at leadPos
  int expectedLen = utf8CodepointLen(static_cast<unsigned char>(buf[leadPos]));
  int actualLen = len - leadPos;

  if (actualLen < expectedLen && leadPos > 0) {
    // Incomplete UTF-8 sequence at the end — exclude it
    return leadPos;
  }
  return len;
}

size_t utf8RemoveLastChar(std::string& str) {
  if (str.empty()) return 0;
  size_t pos = str.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  str.resize(pos);
  return pos;
}

// Truncate string by removing N UTF-8 characters from the end
void utf8TruncateChars(std::string& str, const size_t numChars) {
  for (size_t i = 0; i < numChars && !str.empty(); ++i) {
    utf8RemoveLastChar(str);
  }
}

namespace {
struct Utf8Range {
  uint32_t first;
  uint32_t last;
};

bool inRanges(const uint32_t cp, const Utf8Range* ranges, const size_t count) {
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (cp < ranges[mid].first) {
      hi = mid;
    } else if (cp > ranges[mid].last) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

// Unicode 16.0 DerivedCoreProperties.txt, Default_Ignorable_Code_Point. Sorted.
constexpr Utf8Range kDefaultIgnorable[] = {
    {0x00AD, 0x00AD}, {0x034F, 0x034F}, {0x061C, 0x061C},   {0x115F, 0x1160},   {0x17B4, 0x17B5},   {0x180B, 0x180F},
    {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x206F},   {0x3164, 0x3164},   {0xFE00, 0xFE0F},   {0xFEFF, 0xFEFF},
    {0xFFA0, 0xFFA0}, {0xFFF0, 0xFFF8}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0000, 0xE0FFF},
};

// Unicode 16.0 emoji-data.txt, Extended_Pictographic (merged ranges). Sorted.
constexpr Utf8Range kExtendedPictographic[] = {
    {0x00A9, 0x00A9},   {0x00AE, 0x00AE},   {0x203C, 0x203C},   {0x2049, 0x2049},   {0x2122, 0x2122},
    {0x2139, 0x2139},   {0x2194, 0x2199},   {0x21A9, 0x21AA},   {0x231A, 0x231B},   {0x2328, 0x2328},
    {0x2388, 0x2388},   {0x23CF, 0x23CF},   {0x23E9, 0x23F3},   {0x23F8, 0x23FA},   {0x24C2, 0x24C2},
    {0x25AA, 0x25AB},   {0x25B6, 0x25B6},   {0x25C0, 0x25C0},   {0x25FB, 0x25FE},   {0x2600, 0x2605},
    {0x2607, 0x2612},   {0x2614, 0x2685},   {0x2690, 0x2705},   {0x2708, 0x2712},   {0x2714, 0x2714},
    {0x2716, 0x2716},   {0x271D, 0x271D},   {0x2721, 0x2721},   {0x2728, 0x2728},   {0x2733, 0x2734},
    {0x2744, 0x2744},   {0x2747, 0x2747},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2763, 0x2767},   {0x2795, 0x2797},   {0x27A1, 0x27A1},   {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},   {0x2934, 0x2935},   {0x2B05, 0x2B07},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},   {0x3030, 0x3030},   {0x303D, 0x303D},   {0x3297, 0x3297},   {0x3299, 0x3299},
    {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F}, {0x1F16C, 0x1F171}, {0x1F17E, 0x1F17F},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1E5}, {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F}, {0x1F249, 0x1F3FA}, {0x1F400, 0x1F53D},
    {0x1F546, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F}, {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F},
    {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF}, {0x1FC00, 0x1FFFD},
};

constexpr uint32_t ZERO_WIDTH_JOINER = 0x200D;
constexpr uint32_t COMBINING_ENCLOSING_KEYCAP = 0x20E3;

bool extendsEmojiCluster(const uint32_t cp) {
  return cp == 0xFE0E || cp == 0xFE0F || utf8IsEmojiModifier(cp) || (cp >= 0xE0020 && cp <= 0xE007F) ||
         cp == COMBINING_ENCLOSING_KEYCAP;
}
}  // namespace

bool utf8IsDefaultIgnorable(const uint32_t cp) {
  if (cp < 0x00AD) return false;  // fast path for ASCII and most Latin-1
  return inRanges(cp, kDefaultIgnorable, sizeof(kDefaultIgnorable) / sizeof(kDefaultIgnorable[0]));
}

bool utf8IsEmojiBase(const uint32_t cp) {
  if (cp < 0x00A9) return false;
  if (utf8IsRegionalIndicator(cp)) return true;
  return inRanges(cp, kExtendedPictographic, sizeof(kExtendedPictographic) / sizeof(kExtendedPictographic[0]));
}

void utf8SkipEmojiClusterTail(const unsigned char** string, const uint32_t base) {
  if (!string || !*string || !utf8IsEmojiBase(base)) return;
  bool expectRegionalPair = utf8IsRegionalIndicator(base);
  while (**string) {
    const unsigned char* const beforeNext = *string;
    const uint32_t next = utf8NextCodepoint(string);
    if (expectRegionalPair) {
      expectRegionalPair = false;
      if (utf8IsRegionalIndicator(next)) continue;
    }
    if (extendsEmojiCluster(next)) continue;
    if (next == ZERO_WIDTH_JOINER) {
      const unsigned char* const afterJoiner = *string;
      if (**string && utf8IsEmojiBase(utf8NextCodepoint(string))) continue;
      *string = afterJoiner;  // the joiner itself is invisible; the next codepoint stands alone
      return;
    }
    *string = beforeNext;
    return;
  }
}
