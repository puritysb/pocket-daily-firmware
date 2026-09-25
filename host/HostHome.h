#pragma once

#include <cstdint>

#include "pocket_daily/home/HomeRenderer.h"

class GfxRenderer;

namespace PocketUIHost {
enum SampleFlag : uint32_t {
  SampleBook = 1u,
  SampleStudy = 2u,
  SampleProvider = 4u,
  SampleWeather = 8u,
  SampleEvents = 16u,
  SampleUsage = 32u,
};

struct Sample {
  PocketDaily::Glance glance{};
  PocketDaily::Card card{};
  PocketDaily::Home::Reading reading;
};

void registerUiFonts(GfxRenderer& renderer);
PocketDaily::Home::Env hostEnv(GfxRenderer& renderer);
Sample buildSample(uint32_t samples);
int buildRows(const PocketDaily::DailyProfile::Profile& profile, const Sample& sample, uint32_t samples,
              PocketDaily::Home::Row* rows, int cap);
}  // namespace PocketUIHost
