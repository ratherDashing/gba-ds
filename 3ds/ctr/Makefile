# GBA DS for the New Nintendo 3DS: the app (../../src) on armGBA (../emu) with the 3DS host in this
# folder (sdl_ctr.c stands in for SDL2), packed into gba-ds.3dsx. `make 3ds` in ../ runs this in the
# devkitpro/devkitarm image (devkitARM with libctru, citro3d, citro2d and zlib).
#
#   make             gba-ds.3dsx
#   make TEST=1      gba-ds-test.3dsx, for ../test (the Azahar emulator)
#   make LOG_URL=http://<host>:<port>/log   a diagnosis build: its logs sent there (../tools/logserver.py) when the
#                    app starts (the last run's) and closes. Never a release; rebuild host.c without it after.
#   make SLOWSD=1    TEST ONLY: the SD card freezes on a schedule (host.c's host_slow_sd), to show what waits on it
#   make HOTCOLD=1   links with ld/3dsx.ld, devkitARM's script with the profile's hot code kept together and its cold code
#                    apart (-fprofile-use libmgba, ../mgba/build-libmgba.sh MGBA_PGO=use: GCC marks them .text.hot/.text.unlikely)
#   XDEF, XLDFLAGS, XLIBS: more compiler defines, linker flags and libraries (empty by default; a profiling build's -lgcov)
#   XCORE: more compiler flags for armGBA's files only (the profile build's -fprofile-generate / -fprofile-use)
#   PGO=gen          armGBA's files count what they do (-fprofile-generate, -lgcov): ../emu/pgo-train.py plays ../emu/pgo/training.txt
#                    in Azahar with the build and keeps the counts in ../emu/pgo/. PGO=use builds armGBA's files from them
#                    (a function whose code changed since loses its profile). Both want BUILD=build/gen, emptied first: GCC names a
#                    static function's profile by its object's path, so another BUILD loses those (PGO_QUIET= lists them). The patched
#                    copy of armGBA (../emu/armgba.mk) is named as the submodule's files for it: a profile made before it still fits.
#   make cia-parts   what bannertool makes for the .cia (../Makefile's `cia` then runs makerom: cia.sh)

ifeq ($(strip $(DEVKITARM)),)
$(error "Set DEVKITARM in the environment: export DEVKITARM=<devkitPro>/devkitARM")
endif
include $(DEVKITARM)/3ds_rules
.DEFAULT_GOAL := all

REPO    := $(abspath ../..)
# TEST=1: gba-ds-test.3dsx, with the capture test's profile (src/profiles.c, GBADS_TEST_PROFILE). Never a player's.
comma := ,
TARGET  := gba-ds$(if $(TEST),-test)$(if $(BENCH),-bench-$(BENCH))
TITLE   := GBA DS$(if $(TEST), test)$(if $(BENCH), bench $(BENCH))
# The 3DS port's own version (../VERSION); UPSTREAM is the GBA DS it is.
VERSION := $(shell cat $(REPO)/3ds/VERSION)
UPSTREAM := $(shell cat $(REPO)/VERSION)
BUILD   := build$(if $(TEST),-test)

# CORE=armgba (default): armGBA and its recompiler (../emu). CORE=mgba: src/emu.c on libmgba, as every other
# GBA DS build (../mgba/build-libmgba.sh makes it; MGBA_BUILD is its build folder).
CORE ?= armgba
ifeq ($(CORE),armgba)
ARMGBA_BACKEND := a32
ARMGBA_COPY := $(BUILD)/armgba-src
ARMGBA_LINE_NAMES := $(filter gen use,$(PGO))
include $(REPO)/3ds/emu/armgba.mk
CORE_DEFS := -DGBADS_ARMGBA
CORE_INC  := -I$(ARMGBA)/src
else
MGBA_BUILD ?= $(REPO)/deps/mgba-3ds/build
ARMGBA_CORE_SRC :=
# libmgba's own defines, as the other builds take them: its structs' layout depends on them.
CORE_DEFS := $(shell sed -n 's/^C_DEFINES = //p' $(MGBA_BUILD)/CMakeFiles/mgba.dir/flags.make)
CORE_INC  := -I$(MGBA_BUILD)/../include -I$(MGBA_BUILD)/include -I$(REPO)/third_party/rzstd/include
endif

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
# As armGBA-3DS builds (its Makefile says why): -fwrapv or every game hangs after 128 seconds,
# -flto because the core's hot paths cross its files.
COMMON   := -g -fwrapv -flto -fomit-frame-pointer -ffunction-sections -fdata-sections -mword-relocations $(ARCH) \
            -D__3DS__ -I$(CTRULIB)/include -I$(PORTLIBS)/include
CFLAGS   := $(COMMON) -std=gnu11
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=3dsx.specs -g -O3 -fwrapv -flto=auto -mword-relocations $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map -Wl,--gc-sections -Wl,--wrap=stat -Wl,--wrap=threadCreate$(if $(SLOWSD), -Wl$(comma)--wrap=_write_r -Wl$(comma)--wrap=_read_r -Wl$(comma)--wrap=_open_r)$(if $(TEST), -Wl$(comma)--wrap=malloc -Wl$(comma)--wrap=calloc) $(if $(HOTCOLD),-B$(CURDIR)/ld/) $(XLDFLAGS)
LIBS     := $(if $(filter armgba,$(CORE)),,$(MGBA_BUILD)/libmgba.a) -L$(CTRULIB)/lib -L$(PORTLIBS)/lib -lcitro2d -lcitro3d -lctru -lz -lm $(XLIBS)

PGO_DIR := $(REPO)/3ds/emu/pgo
# (PGO_QUIET= shows which functions' profiles no longer fit the code)
PGO_QUIET ?= -Wno-coverage-mismatch -Wno-missing-profile
ifneq ($(filter gen use,$(PGO)),)
ifneq ($(BUILD),build/gen)
$(error PGO=$(PGO) needs BUILD=build/gen: a profile only fits the objects it was made from by their path)
endif
endif
ifeq ($(PGO),gen)
XCORE += -fprofile-generate -fprofile-update=single -fprofile-dir=/gcov -fprofile-prefix-path=$(CURDIR)/$(BUILD)
XDEF  += -DGBADS_PGO_GEN
LIBS  += -lgcov
else ifeq ($(PGO),use)
XCORE += -fprofile-use=$(PGO_DIR) -fprofile-prefix-path=$(CURDIR)/$(BUILD) -fprofile-partial-training -fprofile-correction $(PGO_QUIET)
endif

D3 := $(REPO)/3ds
S  := $(REPO)/src
APP_SRC := $(addprefix $(S)/,main.c bench.c config.c i18n.c i18n_data.c font.c ui.c library.c mapper.c hotkeys.c folderpick.c game.c paths.c image.c brand_data.c \
	bezel.c bezel_logo.c logo.c cheats.c textfont.c pixfont.c ra.c sha256.c patch.c mapcap.c profiles.c logo_masks.c) \
	$(D3)/ctr/stubs.c $(D3)/ctr/fonts_ctr.c
# WebP logos: libwebp's decoder, vendored as for the other builds.
WEBP_SRC := $(wildcard $(REPO)/third_party/libwebp/src/dec/*.c $(REPO)/third_party/libwebp/src/dsp/*.c $(REPO)/third_party/libwebp/src/utils/*.c)
HOST_SRC := $(D3)/ctr/host.c $(D3)/ctr/sdl_ctr.c
ifeq ($(CORE),armgba)
APP_SRC  += $(D3)/emu/emu_armgba.c
HOST_SRC += $(D3)/ctr/jitmem.c $(ARMGBA_SRC)/platform/3ds/source/svc.s
else
APP_SRC  += $(S)/emu.c $(D3)/ctr/emu_ctr.c
# (emu.c reads RetroArch's zstd save states with it, as the other builds do)
RZSTD_SRC := $(wildcard $(REPO)/third_party/rzstd/src/*.c)
endif

obj = $(patsubst $(REPO)/%,$(BUILD)/%.o,$(1))
APP_OBJ  := $(call obj,$(APP_SRC))
HOST_OBJ := $(call obj,$(HOST_SRC))
CORE_OBJ := $(call ARMGBA_OBJ,$(BUILD))
WEBP_OBJ := $(call obj,$(WEBP_SRC))
RZSTD_OBJ := $(call obj,$(RZSTD_SRC))
OBJ := $(APP_OBJ) $(HOST_OBJ) $(CORE_OBJ) $(WEBP_OBJ) $(RZSTD_OBJ)

# sdl/ stands where SDL2's headers do. The core's headers before src/: armGBA's cheats.h is the one
# emu_armgba.c means. The app's threads are made by host.c (the core they run on), and its main is called from
# host.c's.
APP_INC := -I$(D3)/ctr/sdl -I$(D3)/ctr/compat -I$(D3)/ctr $(CORE_INC) -I$(S) -I$(REPO)/third_party/libwebp
$(APP_OBJ):  EXTRA := -O2 -Wall -Wno-unused-parameter $(APP_INC) $(CORE_DEFS) -DGBADS_VERSION='"$(VERSION)"' \
                      -Dpthread_create=host_pthread_create -Dmain=gbads_main -D_GNU_SOURCE $(if $(TEST),-DGBADS_TEST_PROFILE) $(if $(SLOWSD),-DGBADS_SLOW_SD) $(XDEF)
# BENCH=TAG: a test bench build (its jobs from the LOG_URL machine, results back there), gba-ds-bench-TAG.3dsx
ifneq ($(BENCH),)
BENCH_DEF := -DGBADS_BENCH_TAG='"$(BENCH)"'
endif
$(HOST_OBJ): EXTRA := -O2 -Wall -Wextra $(APP_INC) $(CORE_DEFS) $(BENCH_DEF) $(if $(SLOWSD),-DGBADS_SLOW_SD) -DGBADS_VERSION='"$(VERSION)"' -DGBADS_UPSTREAM='"$(UPSTREAM)"' $(if $(LOG_URL),-DGBADS_LOG_URL='"$(LOG_URL)"') $(if $(EMU_HEAP_KIB),-DGBADS_EMU_HEAP_KIB=$(EMU_HEAP_KIB))
$(CORE_OBJ): EXTRA := $(ARMGBA_CORE_FLAGS) $(XCORE)
$(WEBP_OBJ): EXTRA := -O2 -I$(REPO)/third_party/libwebp
$(RZSTD_OBJ): EXTRA := -O2 -I$(REPO)/third_party/rzstd/include

.PHONY: all cia-parts clean
all: $(TARGET).3dsx

# (a flag changed here rebuilds everything: objects built with other defines see other struct layouts)
$(OBJ): Makefile
ifeq ($(CORE),armgba)
$(APP_OBJ) $(HOST_OBJ) $(CORE_OBJ) $(ARMGBA_CORE_SRC): $(ARMGBA_STAMP)
$(CORE_OBJ): $(BUILD)/third_party/armgba/%.o: $(ARMGBA)/%
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) $(EXTRA) -c $< -o $@
endif

$(BUILD)/%.c.o: $(REPO)/%.c
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) $(EXTRA) -c $< -o $@

$(BUILD)/%.s.o: $(REPO)/%.s
	@mkdir -p $(dir $@)
	@echo $(notdir $<)
	@$(CC) -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJ)
	@echo linking $@
	@$(CC) $(LDFLAGS) $(OBJ) $(LIBS) -o $@

$(BUILD)/$(TARGET).smdh: icon.png
	@smdhtool --create "$(TITLE)" "GBA games with their own map on the second screen" "GBA DS" icon.png $@

$(TARGET).3dsx: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).smdh
	@3dsxtool $< $@ --smdh=$(BUILD)/$(TARGET).smdh
	@echo built $@

# The CIA: makerom packs the ELF with the permissions the recompiler needs: armGBA-3DS's own
# (platform/3ds/armgba.rsf), with GBA DS's title, product code and unique ID in place of armGBA's.
BANNERTOOL ?= bannertool
$(BUILD)/banner.bnr: banner.png banner.wav
	@mkdir -p $(BUILD)
	@$(BANNERTOOL) makebanner -i banner.png -a banner.wav -o $@
$(BUILD)/icon.icn: icon.png
	@$(BANNERTOOL) makesmdh -s "$(TITLE)" -l "GBA games with their own map on the second screen" -p "GBA DS" -i icon.png -o $@
$(BUILD)/gba-ds.rsf: $(REPO)/third_party/armgba/platform/3ds/armgba.rsf
	@sed -e 's/: armGBA$$/: GBA DS/' -e 's/CTR-H-AGBA/CTR-H-GBDS/' -e 's/0xFA6BA/0xFD5D5/' $< > $@
cia-parts: $(BUILD)/$(TARGET).elf $(BUILD)/banner.bnr $(BUILD)/icon.icn $(BUILD)/gba-ds.rsf

clean:
	rm -rf build build-test gba-ds.3dsx gba-ds-test.3dsx gba-ds.cia

-include $(OBJ:.o=.d)
