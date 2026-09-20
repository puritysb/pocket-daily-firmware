#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstddef>
#include <functional>

#include "pocket_daily/web/LiveStudioService.h"
#include "pocket_daily/web/Profile.h"
#include "pocket_daily/web/UploadStreamServer.h"

namespace PocketDaily::Web {

// Host reach-ins for the Pocket route table, same plain-function-pointer
// contract as Host.h. Everything a handler needs that is not reachable from
// library or pocket headers flows through here.
struct RouteHost {
  void* self = nullptr;
  void (*noteClientActivity)(void* self) = nullptr;
  bool (*httpUploadBusy)(void* self) = nullptr;    // session/end 409 gate
  void (*endDirectSession)(void* self) = nullptr;  // session/end accepted
  void (*requestRepaint)(void* self) = nullptr;    // pack apply + dev render
  String (*normalizeWebPath)(void* self, const String& path) = nullptr;
  bool (*isProtectedItemName)(void* self, const String& name) = nullptr;
  // Directory scan of the .uipack store, adapted from the host's FileInfo
  // scan so this module stays independent of the inherited header.
  void (*scanUiPacks)(void* self,
                      const std::function<void(const char* name, bool isDirectory, size_t size)>& fn) = nullptr;
};

struct RouteDeps {
  Profile profile = Profile::FULL;
  bool apMode = false;
  UploadStreamServer* stream = nullptr;     // commit verification, session/end gate
  LiveStudioService* liveStudio = nullptr;  // pack apply, prefs notify, active flag
  RouteHost host{};
};

// Registers every Pocket-owned route on the host's WebServer: private-AP
// session/end, diagnostics, verified commit, Pocket Sync preferences, the
// Live Studio fetch/pack endpoints, and the dev-only evidence endpoints
// (compiled in only under their build flags). `deps` must outlive the
// registered routes — the host stores it as a member.
void registerPocketRoutes(WebServer& server, const RouteDeps& deps);

}  // namespace PocketDaily::Web
