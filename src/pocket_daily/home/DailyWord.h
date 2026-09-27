#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "pocket_daily/learning_pack.h"
#include "pocket_daily/models.h"

// The firmware daily word shown on Home and in the Daily Brief, shared by
// PocketDailyActivity and the Sync screen presenter so both pick and compose
// the same card for the same day.
namespace PocketDaily::Home {

// Best wall-clock estimate for the daily word: `now` when the clock is set,
// else the glance's compose time (a lower bound on the real date), else 0.
uint32_t dailyWordEpoch(time_t now, uint32_t glanceSavedEpoch);

// Day number the word turns over on: the companion's local midnight when a
// glance carried its UTC offset, else UTC midnight. 0 without a clock.
uint32_t dailyWordDay(uint32_t epoch, int16_t utcOffsetMinutes);

// Composes `card` from the device-owned 16-word list; returns the list index.
size_t builtInDailyWord(uint32_t day, uint16_t offset, PocketDaily::Card& card);

// Composes `card` from one SD learning-pack record.
void packDailyWord(const LearningPack::Record& lesson, uint32_t contentVersion, PocketDaily::Card& card);

// Module, action class and the Again/Next/Known choices of the word card.
void finishDailyWord(PocketDaily::Card& card);

enum class WordSource : uint8_t { Pack, BuiltIn, OutOfMemory };

// Single-paint variant for memory-tight callers: one open of the installed
// pack, its header shape and checksum (not the whole-pack SHA-256), one record
// for `day` at offset 0. The built-in list only when the pack is missing or
// unreadable. The 1316 B record scratch is heap-allocated and freed before
// this returns; OutOfMemory leaves `card` cleared.
WordSource lightDailyWord(uint32_t day, PocketDaily::Card& card);

}  // namespace PocketDaily::Home
