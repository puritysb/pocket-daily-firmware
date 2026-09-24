#include <gtest/gtest.h>

#include "pocket_daily/ContentSessionInput.h"

using PocketDaily::Content::ContentSessionInput;
using Action = ContentSessionInput::Action;

TEST(ContentSessionInput, BackDuringPaintKeepsServerRunningThenDismissesWithoutAnotherEdge) {
  ContentSessionInput input;
  EXPECT_EQ(input.update(true, true, false, true, false, false), Action::None);
  for (int i = 0; i < 10; ++i) EXPECT_EQ(input.update(true, true, false, false, false, false), Action::None);
  EXPECT_EQ(input.update(true, false, true, false, false, false), Action::DismissView);
  EXPECT_EQ(input.update(false, false, true, false, false, false), Action::None);
  EXPECT_EQ(input.update(false, false, true, true, false, false), Action::ExitSession);
}

TEST(ContentSessionInput, ReadyViewBackIsConsumedBeforeNavigationAndNeverExitsSession) {
  ContentSessionInput input;
  EXPECT_EQ(input.update(true, false, true, true, true, true), Action::DismissView);
  EXPECT_EQ(input.update(false, false, true, false, false, false), Action::None);
}

TEST(ContentSessionInput, NavigationRequiresVisibleReadyViewAndWriterAdmission) {
  ContentSessionInput input;
  EXPECT_EQ(input.update(false, false, true, false, true, false), Action::None);
  EXPECT_EQ(input.update(true, true, true, false, true, false), Action::None);
  EXPECT_EQ(input.update(true, false, false, false, false, true), Action::None);
  EXPECT_EQ(input.update(true, false, true, false, true, false), Action::Next);
  EXPECT_EQ(input.update(true, false, true, false, false, true), Action::Previous);
  EXPECT_EQ(input.update(true, false, true, false, true, true), Action::Next);
}

TEST(ContentSessionInput, RepeatedBackDuringPaintCannotBecomeAnExitAfterDismissal) {
  ContentSessionInput input;
  EXPECT_EQ(input.update(true, true, false, true, false, false), Action::None);
  EXPECT_EQ(input.update(true, true, false, true, false, false), Action::None);
  EXPECT_EQ(input.update(true, false, false, true, false, false), Action::DismissView);
  EXPECT_EQ(input.update(false, false, false, false, false, false), Action::None);
}
