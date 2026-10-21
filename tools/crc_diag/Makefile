# crc_diag: SuperCard SD read CRC16 diagnostics ROM (see README.md).
# Needs devkitARM and libgba (devkitPro). Builds crc_diag.gba (count only)
# and crc_diag_check.gba (check and retry, as the apps).
#   make            (DEVKITPRO defaults to /opt/devkitpro)
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
PREFIX    := $(DEVKITARM)/bin/arm-none-eabi-
CC        := $(PREFIX)gcc
OBJCOPY   := $(PREFIX)objcopy
GBAFIX    := $(DEVKITPRO)/tools/bin/gbafix
GBASD     := ../..
SRC_C     := main.c $(wildcard $(GBASD)/src/*.c)
SRC_S     := $(wildcard $(GBASD)/src/*.s)
CFLAGS    := -O2 -mthumb -mthumb-interwork -mcpu=arm7tdmi -std=gnu11 -Wall -Wextra \
             -ffunction-sections -fdata-sections -I$(GBASD)/include -I$(DEVKITPRO)/libgba/include
LDFLAGS   := -specs=gba.specs -mthumb-interwork -Wl,--gc-sections -L$(DEVKITPRO)/libgba/lib -lgba

all: crc_diag.gba crc_diag_check.gba

crc_diag.elf: CRC := -DGBASD_SC_READ_CRC=1
crc_diag_check.elf: CRC := -DGBASD_SC_READ_CRC=2

%.elf: $(SRC_C) $(SRC_S) $(wildcard $(GBASD)/include/*.h) Makefile
	@mkdir -p build/$*
	for f in $(SRC_C); do $(CC) $(CFLAGS) $(CRC) -c $$f -o build/$*/$$(basename $$f).o || exit 1; done
	for f in $(SRC_S); do $(CC) $(CFLAGS) $(CRC) -x assembler-with-cpp -c $$f -o build/$*/$$(basename $$f).o || exit 1; done
	$(CC) build/$*/*.o $(LDFLAGS) -Wl,-Map,build/$*.map -o $@

%.gba: %.elf
	$(OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -tCRCDIAG -cCRCD -m00 >/dev/null

clean:
	rm -rf build *.elf *.gba

.PHONY: all clean
