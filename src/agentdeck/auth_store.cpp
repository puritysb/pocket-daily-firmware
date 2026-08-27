#include "auth_store.h"

#include <Preferences.h>

namespace AgentDeck {
namespace AuthStore {

namespace {
constexpr const char* kNamespace = "adwifi";
constexpr const char* kTokenKey = "bridge_token";
}  // namespace

bool load(char* token, size_t tokenCap) {
  if (!token || tokenCap == 0) return false;
  token[0] = '\0';
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;
  prefs.getString(kTokenKey, token, tokenCap);
  prefs.end();
  token[tokenCap - 1] = '\0';
  return token[0] != '\0';
}

bool save(const char* token) {
  // Empty means "no new credential", never "erase the working credential".
  if (!token || token[0] == '\0') return false;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const size_t written = prefs.putString(kTokenKey, token);
  prefs.end();
  return written > 0;
}

}  // namespace AuthStore
}  // namespace AgentDeck
