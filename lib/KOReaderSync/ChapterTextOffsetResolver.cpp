#include "ChapterTextOffsetResolver.h"

#include <Logging.h>
#include <Print.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>
#include <strings.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "Epub/htmlEntities.h"

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

// expat expands the five XML entities and numeric references itself; HTML named
// entities (e.g. &nbsp; under an XHTML DOCTYPE) reach the default handler
// unexpanded. Those count as their decoded text, as in the reader's layout
// (ChapterHtmlSlimParser::defaultHandlerExpand), ProgressMapper and the app's DOM.
const char* htmlEntityText(const XML_Char* s, const int len) {
  if (!s || len < 3 || s[0] != '&' || s[len - 1] != ';') return nullptr;
  return lookupHtmlEntity(s, static_cast<size_t>(len));
}

// ECMAScript \s: the class KOReader-compatible readers (and the companion app) use to
// skip whitespace-only text nodes when numbering text()[N].
bool isTextWhitespace(const uint32_t cp) {
  return cp == ' ' || (cp >= 0x09 && cp <= 0x0D) || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
    XML_SetDefaultHandlerExpand(parser, &ParagraphTextCounter::entityData);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  size_t totalVisibleChars() const { return visibleChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL entityData(void* userData, const XML_Char* s, const int len) {
    if (const char* text = htmlEntityText(s, len)) characterData(userData, text, static_cast<int>(strlen(text)));
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (name == "p" || name == "li") {
      paragraphDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      return;
    }

    if ((name == "p" || name == "li") && paragraphDepth > 0) {
      paragraphDepth--;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || paragraphDepth <= 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  size_t visibleChars = 0;
};

class XPathParagraphResolver final : public Print {
 public:
  explicit XPathParagraphResolver(const int targetParagraph) : targetParagraph(targetParagraph) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathParagraphResolver::startElement, &XPathParagraphResolver::endElement);
  }

  ~XPathParagraphResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    // Count <p> only: the index comes from Section's paragraph LUT, which the layout
    // (ChapterHtmlSlimParser::xpathParagraphIndex) advances on <p> alone. Counting <li>
    // too shifted every paragraph after a list to a later one.
    if (name == "p") {
      paragraphCount++;
      if (paragraphCount == targetParagraph) {
        xpath = buildParagraphXPath(spineIndex, path, 0, 0);
        stopped = true;
        XML_StopParser(parser, XML_FALSE);
      }
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const int targetParagraph;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

class XPathProgressResolver final : public Print {
 public:
  explicit XPathProgressResolver(const size_t targetVisibleChar) : targetVisibleChar(targetVisibleChar) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
    XML_SetDefaultHandlerExpand(parser, &XPathProgressResolver::entityData);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL entityData(void* userData, const XML_Char* s, const int len) {
    if (const char* text = htmlEntityText(s, len)) characterData(userData, text, static_cast<int>(strlen(text)));
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);
    pendingTextNode = true;

    if (name == "p") {
      paragraphDepth++;
    }
    if (name == "li") {
      liDepth++;
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      return;
    }

    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
    }
    if (name == "li" && liDepth > 0) {
      liDepth--;
    }

    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    if (paragraphDepth > 0 || liDepth > 0) {
      pendingTextNode = true;
    }
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || (paragraphDepth <= 0 && liDepth <= 0) || len <= 0 || stopped) {
      return;
    }

    // A text node starts at the first character data after any element boundary (expat
    // may deliver one node in several chunks). Only nodes holding non-whitespace are
    // numbered, matching KOReader's text()[N] and the companion app, which skip the
    // whitespace-only nodes between elements and the empty ones bare anchors create.
    if (pendingTextNode) {
      textNodeStartChars = visibleChars;
      textNodeHasContent = false;
      textNodeSerial++;
      pendingTextNode = false;
    }

    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      const uint32_t cp = utf8NextCodepoint(&ptr);
      const size_t index = visibleChars++;
      if (!textNodeHasContent && !isTextWhitespace(cp)) {
        textNodeHasContent = true;
        if (!textNodeIndexStack.empty()) {
          textNodeIndexStack.back()++;
        }
        if (awaitingContent) {
          // The target fell in whitespace: keep its offset if the node it was in turned out
          // to hold content, otherwise land on the next content character.
          emit(awaitingSerial == textNodeSerial ? targetVisibleChar - textNodeStartChars : index - textNodeStartChars);
          return;
        }
      }
      if (!awaitingContent && visibleChars == targetVisibleChar) {
        if (textNodeHasContent) {
          emit(targetVisibleChar - textNodeStartChars);
          return;
        }
        awaitingContent = true;
        awaitingSerial = textNodeSerial;
      }
    }
  }

  void emit(const size_t charOffset) {
    const int texNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
    xpath = buildParagraphXPath(spineIndex, path, texNode, charOffset);
    stopped = true;
    XML_StopParser(parser, XML_FALSE);
  }

  XML_Parser parser = nullptr;
  const size_t targetVisibleChar;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool pendingTextNode = true;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  bool textNodeHasContent = false;
  uint32_t textNodeSerial = 0;
  bool awaitingContent = false;
  uint32_t awaitingSerial = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};
// Where the Nth <p> starts and how much text it holds, counted like XPathProgressResolver
// (text inside <p>/<li> only), so a position inside it can be handed to that resolver.
class ParagraphSpanCounter final : public Print {
 public:
  explicit ParagraphSpanCounter(const int targetParagraph) : targetParagraph(targetParagraph) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphSpanCounter::startElement, &ParagraphSpanCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphSpanCounter::characterData);
    XML_SetDefaultHandlerExpand(parser, &ParagraphSpanCounter::entityData);
  }

  ~ParagraphSpanCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }
  bool found() const { return closed; }
  size_t charsBefore() const { return before; }
  size_t charsInside() const { return inside; }

  bool finish() {
    if (!parser || !parseOk || closed) {
      return parseOk;
    }
    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || closed) {
      return size;
    }
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }
    return size;
  }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<ParagraphSpanCounter*>(userData)->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<ParagraphSpanCounter*>(userData)->onEndElement(name);
  }

  static void XMLCALL entityData(void* userData, const XML_Char* s, const int len) {
    if (const char* text = htmlEntityText(s, len)) characterData(userData, text, static_cast<int>(strlen(text)));
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphSpanCounter*>(userData);
    if (self->insideBody && (self->paragraphDepth > 0 || self->liDepth > 0) && !self->closed) {
      self->visibleChars += countUtf8Codepoints(data, len);
    }
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }
    if (name == "p") {
      paragraphDepth++;
      if (++paragraphCount == targetParagraph) {
        targetDepth = depth;
        before = visibleChars;
      }
    } else if (name == "li") {
      liDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    depth--;
    if (!insideBody) {
      return;
    }
    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      return;
    }
    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
      if (depth == targetDepth) {
        inside = visibleChars - before;
        closed = true;
        XML_StopParser(parser, XML_FALSE);
      }
    } else if (name == "li" && liDepth > 0) {
      liDepth--;
    }
  }

  XML_Parser parser = nullptr;
  const int targetParagraph;
  bool parseOk = true;
  bool insideBody = false;
  bool closed = false;
  int depth = 0;
  int bodyDepth = -1;
  int targetDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  int paragraphCount = 0;
  size_t visibleChars = 0;
  size_t before = 0;
  size_t inside = 0;
};

// Streams a spine item to the character at chapter text offset `target` (0-based code point
// index into the body's character data, counted like ChapterHtmlSlimParser::textOffset) and
// builds its XPointer. Text runs are numbered like XPathProgressResolver, but every element
// counts (headings, divs, text directly in <body>), not only <p>/<li>.
class XPathTextOffsetResolver final : public Print {
 public:
  explicit XPathTextOffsetResolver(const uint32_t target) : target(target) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathTextOffsetResolver::startElement, &XPathTextOffsetResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathTextOffsetResolver::characterData);
    XML_SetDefaultHandlerExpand(parser, &XPathTextOffsetResolver::entityData);
  }

  ~XPathTextOffsetResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }
  bool hasMatch() const { return !xpath.empty(); }
  const std::string& getXPath() const { return xpath; }

  bool finish() {
    if (!parser || !parseOk || stopped) return parseOk;
    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) return size;
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }
    return size;
  }

  int spineIndex = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<XPathTextOffsetResolver*>(userData)->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<XPathTextOffsetResolver*>(userData)->onEndElement(name);
  }

  static void XMLCALL entityData(void* userData, const XML_Char* s, const int len) {
    if (const char* text = htmlEntityText(s, len)) characterData(userData, text, static_cast<int>(strlen(text)));
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<XPathTextOffsetResolver*>(userData)->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
        textNodeIndexStack.push_back(0);  // runs directly in <body>
        pendingTextNode = true;
      }
      depth++;
      return;
    }
    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);
    pendingTextNode = true;
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    depth--;
    if (!insideBody) return;
    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      return;
    }
    if (!textNodeIndexStack.empty()) textNodeIndexStack.pop_back();
    if (!path.empty()) path.pop_back();
    if (!parentStates.empty()) parentStates.pop_back();
    pendingTextNode = true;  // the parent's next run starts after this element
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || len <= 0 || stopped) return;
    // A text node starts at the first character data after an element boundary; only runs
    // holding non-whitespace are numbered (KOReader text()[N], the app's resolver).
    if (pendingTextNode) {
      nodeStart = count;
      nodeHasContent = false;
      nodeSerial++;
      pendingTextNode = false;
    }
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      const uint32_t cp = utf8NextCodepoint(&ptr);
      const uint32_t index = count++;
      if (!nodeHasContent && !isTextWhitespace(cp)) {
        nodeHasContent = true;
        if (!textNodeIndexStack.empty()) textNodeIndexStack.back()++;
        if (awaitingContent) {
          // The target sat in whitespace: keep it if its run has content, else take this character.
          emit(awaitingSerial == nodeSerial ? target - nodeStart : index - nodeStart);
          return;
        }
      }
      if (!awaitingContent && index == target) {
        if (nodeHasContent) {
          emit(target - nodeStart);
          return;
        }
        awaitingContent = true;
        awaitingSerial = nodeSerial;
      }
    }
  }

  void emit(const uint32_t charOffset) {
    const int textNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
    xpath = buildParagraphXPath(spineIndex, path, textNode, charOffset);
    stopped = true;
    XML_StopParser(parser, XML_FALSE);
  }

  XML_Parser parser = nullptr;
  const uint32_t target;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool pendingTextNode = true;
  int depth = 0;
  int bodyDepth = -1;
  uint32_t count = 0;
  uint32_t nodeStart = 0;
  bool nodeHasContent = false;
  uint32_t nodeSerial = 0;
  bool awaitingContent = false;
  uint32_t awaitingSerial = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
  std::string xpath;
};

// A parsed XPointer: element steps below <body>, then an optional text()[k].offset.
struct XPointerSteps {
  static constexpr int MAX_STEPS = 24;
  static constexpr size_t MAX_NAME = 32;
  struct Step {
    char name[MAX_NAME];
    int index;
  };
  Step steps[MAX_STEPS];
  int stepCount = 0;
  bool hasText = false;
  int textIndex = 1;
  uint32_t offset = 0;
};

bool parseDigits(const std::string& text, size_t& pos, uint32_t& value) {
  const size_t start = pos;
  uint64_t parsed = 0;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
    parsed = parsed * 10 + static_cast<uint64_t>(text[pos] - '0');
    if (parsed > UINT32_MAX) return false;
    pos++;
  }
  value = static_cast<uint32_t>(parsed);
  return pos > start;
}

// "[n]" at pos (optional; absent means 1).
bool parseIndexSuffix(const std::string& text, size_t& pos, int& index) {
  index = 1;
  if (pos >= text.size() || text[pos] != '[') return true;
  pos++;
  uint32_t value = 0;
  if (!parseDigits(text, pos, value) || value == 0 || value > INT32_MAX || pos >= text.size() || text[pos] != ']') {
    return false;
  }
  pos++;
  index = static_cast<int>(value);
  return true;
}

// /body/DocFragment[N]/body[/name[i]...][/text()[k][.offset]] or an element path with ".offset".
bool parseXPointer(const std::string& xpath, XPointerSteps& out) {
  static constexpr char PREFIX[] = "/body/DocFragment[";
  static constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1;
  if (xpath.compare(0, PREFIX_LEN, PREFIX) != 0) return false;
  size_t pos = xpath.find(']', PREFIX_LEN);
  if (pos == std::string::npos) return false;
  pos++;
  if (pos == xpath.size()) return true;  // the chapter itself
  if (xpath.compare(pos, 5, "/body") != 0) return false;
  pos += 5;
  while (pos < xpath.size()) {
    if (xpath[pos] == '.') {  // an element pointer's offset: lands on the element
      pos++;
      uint32_t ignored = 0;
      return parseDigits(xpath, pos, ignored) && pos == xpath.size();
    }
    if (xpath[pos] != '/') return false;
    pos++;
    if (xpath.compare(pos, 6, "text()") == 0) {
      pos += 6;
      out.hasText = true;
      if (!parseIndexSuffix(xpath, pos, out.textIndex)) return false;
      if (pos < xpath.size()) {
        if (xpath[pos] != '.') return false;
        pos++;
        if (!parseDigits(xpath, pos, out.offset)) return false;
      }
      return pos == xpath.size();
    }
    if (out.stepCount >= XPointerSteps::MAX_STEPS) return false;
    XPointerSteps::Step& step = out.steps[out.stepCount];
    const size_t nameStart = pos;
    while (pos < xpath.size() && xpath[pos] != '[' && xpath[pos] != '/' && xpath[pos] != '.') pos++;
    const size_t nameLen = pos - nameStart;
    if (nameLen == 0 || nameLen >= XPointerSteps::MAX_NAME) return false;
    memcpy(step.name, xpath.data() + nameStart, nameLen);
    step.name[nameLen] = '\0';
    if (!parseIndexSuffix(xpath, pos, step.index)) return false;
    out.stepCount++;
  }
  return true;
}

// Streams a spine item to the element an XPointer names and turns its text()[k].offset into
// a chapter text offset (the rules of ProgressMapper::locateInSpine, counted like the layout).
class TextOffsetLocator final : public Print {
 public:
  explicit TextOffsetLocator(const XPointerSteps& pointer) : pointer(pointer) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &TextOffsetLocator::startElement, &TextOffsetLocator::endElement);
    XML_SetCharacterDataHandler(parser, &TextOffsetLocator::characterData);
    XML_SetDefaultHandlerExpand(parser, &TextOffsetLocator::entityData);
  }

  ~TextOffsetLocator() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }
  bool found() const { return located; }
  uint32_t textOffset() const { return result; }

  bool finish() {
    if (!parser || !parseOk || stopped) return parseOk;
    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) return size;
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }
    return size;
  }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<TextOffsetLocator*>(userData)->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char*) {
    static_cast<TextOffsetLocator*>(userData)->onEndElement();
  }

  static void XMLCALL entityData(void* userData, const XML_Char* s, const int len) {
    if (const char* text = htmlEntityText(s, len)) characterData(userData, text, static_cast<int>(strlen(text)));
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<TextOffsetLocator*>(userData)->onCharacterData(data, len);
  }

  static const char* localName(const XML_Char* name) {
    const char* local = std::strrchr(name, ':');
    return local ? local + 1 : name;
  }

  void onStartElement(const XML_Char* rawName) {
    const char* name = localName(rawName);
    depth++;
    if (!insideBody) {
      if (strcmp(name, "body") == 0) {
        insideBody = true;
        bodyDepth = depth;
        relDepth = 0;
        if (pointer.stepCount == 0) enterTarget();
      }
      return;
    }
    if (inTarget && relDepth == targetRel) {
      endRun();  // a child element ends the target's current direct run
      if (stopped) return;
    }
    relDepth++;
    // Element steps: step i is a direct child of the element step i-1 matched (<body> for 0).
    if (!inTarget && matched < pointer.stepCount && matched == relDepth - 1) {
      const XPointerSteps::Step& step = pointer.steps[matched];
      if (strcasecmp(name, step.name) == 0 && ++siblingCounters[matched] == step.index) {
        matched++;
        if (matched == pointer.stepCount) enterTarget();
      }
    }
  }

  void onEndElement() {
    const int closingDepth = depth--;
    if (!insideBody) return;
    if (closingDepth == bodyDepth) {
      if (inTarget) finishTarget();
      insideBody = false;
      return;
    }
    if (inTarget && relDepth == targetRel) {
      finishTarget();
      return;
    }
    if (matched > 0 && relDepth == matched) {
      // A matched ancestor closed before the target was reached; its children's counts restart.
      matched = relDepth - 1;
      for (int i = relDepth; i < XPointerSteps::MAX_STEPS; i++) siblingCounters[i] = 0;
    }
    relDepth--;
    if (inTarget && relDepth == targetRel) beginRun();  // a direct child closed: a new run
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || stopped || len <= 0) return;
    const bool directText = inTarget && relDepth == targetRel;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (directText && !targetKnown && !runHasContent && !isTextWhitespace(cp)) {
        runHasContent = true;
        if (++contentRuns == pointer.textIndex) {
          targetKnown = true;
          target = runStart + pointer.offset;
        }
      }
      count++;
      if (targetKnown && count > target) {
        land(target);
        return;
      }
    }
  }

  void enterTarget() {
    inTarget = true;
    targetRel = relDepth;
    elementStart = count;
    if (!pointer.hasText) {
      land(elementStart);
      return;
    }
    beginRun();
  }

  void beginRun() {
    runStart = count;
    runHasContent = false;
  }

  // The current run ended: an offset past its end clamps to the end.
  void endRun() {
    if (targetKnown) land(std::min(target, count));
  }

  void finishTarget() {
    endRun();
    if (!stopped) land(elementStart);  // the numbered run does not exist
  }

  void land(const uint32_t offset) {
    result = offset;
    located = true;
    stopped = true;
    XML_StopParser(parser, XML_FALSE);
  }

  XML_Parser parser = nullptr;
  const XPointerSteps& pointer;
  bool parseOk = true;
  bool stopped = false;
  bool located = false;
  uint32_t result = 0;
  bool insideBody = false;
  int depth = 0;
  int bodyDepth = -1;
  int relDepth = 0;  // depth below <body>: its children are 1
  int matched = 0;   // steps matched along the open ancestor chain
  int siblingCounters[XPointerSteps::MAX_STEPS] = {};
  uint32_t count = 0;
  bool inTarget = false;
  int targetRel = 0;
  uint32_t elementStart = 0;
  uint32_t runStart = 0;
  bool runHasContent = false;
  int contentRuns = 0;
  bool targetKnown = false;
  uint32_t target = 0;
};

std::string resolveVisibleChar(const std::shared_ptr<Epub>& epub, const int spineIndex,
                               const size_t targetVisibleChar) {
  XPathProgressResolver resolver(targetVisibleChar);
  if (!resolver.ok()) {
    return "";
  }
  resolver.spineIndex = spineIndex;
  if (!epub->readSpineItemToStream(spineIndex, resolver, 1024) || !resolver.finish()) {
    return "";
  }
  return resolver.hasMatch() ? resolver.getXPath() : "";
}
}  // namespace

std::string ChapterTextOffsetResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                             const uint16_t paragraphIndex) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  XPathParagraphResolver resolver(paragraphIndex);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  if (!epub->readSpineItemToStream(spineIndex, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (resolver.hasMatch()) {
    LOG_DBG("KOX", "Resolved paragraph %u in spine %d -> %s", paragraphIndex, spineIndex, resolver.getXPath().c_str());
    return resolver.getXPath();
  }

  LOG_DBG("KOX", "Paragraph %u not found in spine %d", paragraphIndex, spineIndex);
  return "";
}

std::string ChapterTextOffsetResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                            const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  }

  ParagraphTextCounter counter;
  if (!counter.ok() || !epub->readSpineItemToStream(spineIndex, counter, 1024) || !counter.finish()) {
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  std::string xpath = resolveVisibleChar(epub, spineIndex, targetVisibleChar);
  if (!xpath.empty()) {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex, xpath.c_str());
    return xpath;
  }

  LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  return "";
}

std::string ChapterTextOffsetResolver::findXPathForParagraphProgress(const std::shared_ptr<Epub>& epub,
                                                                     const int spineIndex,
                                                                     const uint16_t paragraphIndex,
                                                                     const float fraction) {
  if (!epub || paragraphIndex == 0 || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }
  if (!(fraction > 0.0f)) {
    return findXPathForParagraph(epub, spineIndex, paragraphIndex);
  }

  ParagraphSpanCounter counter(paragraphIndex);
  if (!counter.ok() || !epub->readSpineItemToStream(spineIndex, counter, 1024) || !counter.finish() ||
      !counter.found() || counter.charsInside() == 0) {
    return findXPathForParagraph(epub, spineIndex, paragraphIndex);
  }

  const size_t inside = counter.charsInside();
  const size_t offset =
      std::min(inside - 1, static_cast<size_t>(std::min(1.0f, fraction) * static_cast<float>(inside)));
  if (offset == 0) {
    return findXPathForParagraph(epub, spineIndex, paragraphIndex);
  }
  // XPathProgressResolver points just past its Nth counted character.
  std::string xpath = resolveVisibleChar(epub, spineIndex, counter.charsBefore() + offset);
  LOG_DBG("KOX", "Paragraph %u + %.3f (%zu/%zu) in spine %d -> %s", paragraphIndex, fraction, offset, inside,
          spineIndex, xpath.c_str());
  return !xpath.empty() ? xpath : findXPathForParagraph(epub, spineIndex, paragraphIndex);
}

std::string ChapterTextOffsetResolver::findXPathForTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                              const uint32_t textOffset) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }
  XPathTextOffsetResolver resolver(textOffset);
  if (!resolver.ok()) {
    return "";
  }
  resolver.spineIndex = spineIndex;
  if (!epub->readSpineItemToStream(spineIndex, resolver, 1024) || !resolver.finish()) {
    return "";
  }
  if (!resolver.hasMatch()) {
    LOG_DBG("KOX", "Text offset %lu is past the text of spine %d", static_cast<unsigned long>(textOffset), spineIndex);
    return "";
  }
  LOG_DBG("KOX", "Text offset %lu in spine %d -> %s", static_cast<unsigned long>(textOffset), spineIndex,
          resolver.getXPath().c_str());
  return resolver.getXPath();
}

bool ChapterTextOffsetResolver::findTextOffsetForXPath(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const std::string& xpath, uint32_t& textOffset) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return false;
  }
  // Parsed on the heap: the step table is ~900 B, too much for the caller's stack.
  auto pointer = std::unique_ptr<XPointerSteps>(new (std::nothrow) XPointerSteps());
  if (!pointer) {
    LOG_ERR("KOX", "No memory to parse an XPointer");
    return false;
  }
  if (!parseXPointer(xpath, *pointer)) {
    LOG_DBG("KOX", "Unsupported XPointer %s", xpath.c_str());
    return false;
  }
  TextOffsetLocator locator(*pointer);
  if (!locator.ok() || !epub->readSpineItemToStream(spineIndex, locator, 1024) || !locator.finish() ||
      !locator.found()) {
    return false;
  }
  textOffset = locator.textOffset();
  LOG_DBG("KOX", "%s -> text offset %lu", xpath.c_str(), static_cast<unsigned long>(textOffset));
  return true;
}
