# gbaSD

SD card access for Game Boy Advance homebrew on three flashcarts, with one
ROM for all of them:

| Cart | Status |
|---|---|
| SuperCard SD | Working (hardware-tested with the apps below) |
| EZ-Flash Omega | Working (hardware-tested) |
| EZ-Flash Omega Definitive Edition | Expected to work, not tested |
| EverDrive GBA Mini | Expected to work, emulator-tested only |
| EverDrive GBA X5 / X3 | Expected to work, not tested |

The cart is detected once at start-up (EverDrive, then Omega, then
SuperCard). Holding **L+R** while the app starts skips the EverDrive and
Omega checks and uses the SuperCard driver.

Used by [gbareader](https://github.com/Xinon232/gbareader),
[gbawriter](https://github.com/Xinon232/gbawriter),
[gbavocab](https://github.com/Xinon232/gbavocab) and
[gbamp3](https://github.com/Xinon232/gbamp3).

## Contents

| Path | What |
|---|---|
| `include/cart_io.h`, `src/cart_io.c` | Cart detection and one read/write interface for all carts |
| `src/supercard_*`, `src/crc.c` | SuperCard SD driver (from SuperFW by David Guillen Fandos) |
| `src/ezfo_driver.c` | EZ-Flash Omega driver |
| `src/edgba_driver.c` | EverDrive GBA driver (port of krikzz's edio-gba) |
| `include/gbasd_config.h` | Build settings: timeouts, write retries, code placement |
| `tests/models/` | Register-level models of the three carts for exact-ROM tests on mGBA |
| `tests/check_ram_code.py` | Checks a linked ELF: code that runs while the Omega kernel is mapped must not reach the game ROM |
| `tests/build_check.sh` | Compiles the library in the default and the gbamp3 configuration |
| `docs/` | How each driver works, its limits and a hardware test checklist |

## Using it

Plain C, no engine dependency. Add the repository as a submodule, compile
everything in `src/` and put `include/` on the include path. Then:

```c
#include "cart_io.h"

if (cart_sd_init() == 0) {              /* detects the cart, starts the card */
    cart_read(buffer, lba, count);      /* 512-byte sectors */
    cart_write(buffer, lba, count);
}
```

`cart_kind` tells which cart was found (`CART_SUPERCARD`, `CART_EZFO`,
`CART_EDGBA`, or one of the `*_NO_CARD` values). A FatFs `diskio.c` only
needs to forward to `cart_read` / `cart_write`.

### Settings

Defaults suit apps that may wait for a slow card. See `gbasd_config.h`:

- `GBASD_SC_IDLE_TIMEOUT`, `GBASD_SC_DATA_TIMEOUT`: SuperCard waits
- `GBASD_SC_WRITE_RETRIES` (default 2), `GBASD_SC_MAX_WRITE_BLOCKS` (default no limit)
- `GBASD_SC_FAST_FN`, `GBASD_SC_CRC7_FN`, `GBASD_SC_CRC16_FN`,
  `GBASD_SC_CMD_IN_IWRAM`, `GBASD_SC_WRITE_IN_EWRAM`: where the SuperCard code lives
- `EZFO_SECTION`, `EZFO_ISA`, `EZFO_OUTER_SECTION`, `EZFO_PROBE_SECTION`,
  `EZFO_PROBE_ISA`: where the Omega code lives (see `ezfo_driver.c`). The
  Omega maps its kernel over the game ROM during SD access, so this code must
  run from RAM; `tests/check_ram_code.py` proves it.

gbamp3 (a music player with full IWRAM) uses short timeouts, no write
retry, and puts the code in its own overlay sections; see
`tests/build_check.sh` for the exact flags.

## Testing

- `tests/build_check.sh`: compiles every file in both configurations with
  `-Wall -Wextra -Werror` (needs devkitARM). Runs in CI.
- The apps run their exact ROMs on mGBA against `tests/models/*.h`: book
  import, editing, saving and music playback on each cart model, with failing,
  slow, write-protected and missing cards. The models are stricter than the
  hardware (the Omega model stops the run if the game ROM is read while the
  kernel is mapped; the EverDrive model checks every command and data CRC).
  They are models, not hardware proof: please report results on real carts.

## Licence

GPL-3.0-or-later, see `LICENSE`. Parts by others keep their notices; see
`CREDITS.md`.
