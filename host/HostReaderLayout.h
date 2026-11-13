#pragma once
#include "HostHome.h"
#include "pocket_daily/preview/LayoutInputs.h"
namespace PocketUIHost {
struct RecentCovers {
  const uint8_t* bytes[4]{};
  size_t sizes[4]{};
};
bool renderReaderLayout(GfxRenderer& renderer, const PocketDaily::Preview::LayoutInputs& inputs,
                        const PocketDaily::DailyProfile::Profile& profile, const UserCards& cards, bool brief,
                        bool sleepCover, bool actual = false, bool wakeIndicator = false,
                        const uint8_t* cover = nullptr, size_t coverSize = 0, bool fullScreenCover = false,
                        const RecentCovers* recent = nullptr, uint32_t selected = 0, uint32_t date = 0);
}  // namespace PocketUIHost
