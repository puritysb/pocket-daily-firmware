#pragma once

namespace FsHelpers {

// pocket-daily holds the firmware's Home cards, their staging area and the
// learning pack; the library and file lists keep it out of the shelf.
inline constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache", "pocket-daily"};

}  // namespace FsHelpers
