#pragma once

// Umbrella for the Pocket-owned web service modules (SEAM.md: the inherited
// CrossPointWebServer includes exactly this one pocket header and reaches all
// pocket behavior through it). Includes are added here as modules land.
#include "pocket_daily/web/Host.h"
#include "pocket_daily/web/LiveStudioService.h"
#include "pocket_daily/web/PocketStatus.h"
#include "pocket_daily/web/Profile.h"
#include "pocket_daily/web/UploadStreamServer.h"
