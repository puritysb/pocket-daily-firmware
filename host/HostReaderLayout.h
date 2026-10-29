#pragma once
#include "HostHome.h"
#include "pocket_daily/preview/LayoutInputs.h"
namespace PocketUIHost {
bool renderReaderLayout(GfxRenderer& renderer, const PocketDaily::Preview::LayoutInputs& inputs,
                        const PocketDaily::DailyProfile::Profile& profile, const UserCards& cards, bool brief,
                        bool sleepCover, bool actual = false, bool wakeIndicator = false,
                        const uint8_t* cover = nullptr, size_t coverSize = 0);
}
