#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int main(void) {
  assert(setenv("HC_GAMEDIR", "/tmp/horizon-game-root///", 1) == 0);
  assert(strcmp(hc_game_dir(), "/tmp/horizon-game-root") == 0);

  char path[128];
  assert(hc_game_path(path, sizeof path, "userdata/shared_prefs.bin") == 0);
  assert(strcmp(path,
                "/tmp/horizon-game-root/userdata/shared_prefs.bin") == 0);
  assert(hc_game_path(path, sizeof path, "/UnityDataAssetPack.apk") == 0);
  assert(strcmp(path,
                "/tmp/horizon-game-root/UnityDataAssetPack.apk") == 0);

  char too_small[8];
  assert(hc_game_path(too_small, sizeof too_small, "bin/Data") == -1);
  assert(too_small[0] == '\0');
  return 0;
}
