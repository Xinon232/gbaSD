/* Exact-ROM runner on mGBA with the SuperCard SD model (../models/scsd_model.h).
 * Runs ROM until the 32-bit word at WAIT reaches VALUE (or FRAMES frames),
 * then prints the words asked for and the model's counts.
 * Usage: scsd_runner ROM FRAMES WAIT VALUE ADDR:WORDS...   (env SD_IMAGE, SD_*)
 * Prints "WORDS <addr> w0 w1 ..." per ADDR, "FRAMES n", the scsd_report line.
 * gbaSD, GPL-3.0-or-later. Not hardware proof. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>
#include "../models/scsd_model.h"
int main(int argc, char **argv) {
  if (argc < 5) return 2;
  struct mCore *c = mCoreFind(argv[1]);
  if (!c || !c->init(c)) return 3;
  mCoreInitConfig(c, NULL);
  mCoreConfigSetIntValue(&c->config, "idleOptimization", 0);
  mCoreConfigSetIntValue(&c->config, "logLevel", 0);
  mCoreLoadForeignConfig(c, &c->config);
  static color_t px[240 * 160];
  c->setVideoBuffer(c, px, 240);
  struct VFile *v = VFileOpen(argv[1], O_RDONLY);
  if (!v || !c->loadROM(c, v)) return 4;
  c->reset(c);
  model_install(c);
  unsigned frames = strtoul(argv[2], 0, 0), f;
  uint32_t wait = strtoul(argv[3], 0, 0), value = strtoul(argv[4], 0, 0);
  for (f = 0; f < frames; f++) {
    c->runFrame(c);
    if (c->busRead32(c, wait) >= value) break;
  }
  printf("FRAMES %u\n", f);
  for (int i = 5; i < argc; i++) {
    uint32_t a = strtoul(argv[i], 0, 0), n = strtoul(strchr(argv[i], ':') + 1, 0, 0);
    printf("WORDS %08x", a);
    for (uint32_t k = 0; k < n; k++) printf(" %u", c->busRead32(c, a + 4 * k));
    printf("\n");
  }
  scsd_report(stdout);
  c->deinit(c);
  if (image_file) fclose(image_file);
  return 0;
}
