#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pocket_daily/home/HomeRenderer.h"

class GfxRenderer;

namespace PocketUIHost {
// Mirrors PDUI_SAMPLE_*. Provider (4) and usage (32) came from the retired
// AgentDeck daemon; they stay valid in the mask and draw nothing.
enum SampleFlag : uint32_t {
  SampleBook = 1u,
  SampleStudy = 2u,
  SampleWeather = 8u,
  SampleEvents = 16u,
};

struct Sample {
  PocketDaily::Glance glance{};
  PocketDaily::Card card{};  // sample study card (SampleStudy)
  PocketDaily::Card word{};  // the firmware daily word, always present
  PocketDaily::Home::Reading reading;
};

// The companion's own cards (pdui_set_cards), owned by the context. They take
// the place of the sample study card.
struct UserCard {
  PocketDaily::Card card{};
  std::string summary;         // Home body: text, then context when it fits
  std::vector<uint8_t> image;  // PBM bytes; empty when the card has none
};
using UserCards = std::vector<UserCard>;

void registerUiFonts(GfxRenderer& renderer);
PocketDaily::Home::Env hostEnv(GfxRenderer& renderer, const UserCards& cards);
Sample buildSample(uint32_t samples);
int buildRows(const PocketDaily::DailyProfile::Profile& profile, const Sample& sample, uint32_t samples,
              const UserCards& cards, PocketDaily::Home::Row* rows, int cap);
}  // namespace PocketUIHost
