#include <PocketUIHost.h>
int main(void) {
  pdui_context* context = NULL;
  if (pdui_abi_version() != 1) return 1;
  if (pdui_create(800, 480, 0, NULL, 0, &context) != PDUI_INVALID_ARGUMENT || context) return 2;
  pdui_destroy(context);
  return 0;
}
