# armGBA's core, as its own builds list it. GBA DS drives the machine directly (emu/emu_armgba.c): no libretro
# layer, no Gambatte. ARMGBA_BACKEND picks the recompiler: a32 (the 3DS), x64 (the desktop test build).
# The sources are built from a copy of the submodule's src/ (third_party/armgba, ARMGBA_SRC) with armgba-patches/
# applied, so the submodule stays at its public commit and clean. The includer sets ARMGBA_COPY, a folder in its
# build folder at the same path every build (a PGO profile only fits the paths it was made from), and makes its
# objects and the app files that include armGBA's headers depend on ARMGBA_STAMP. ARMGBA_PATCHES= builds the
# submodule as it is. ARMGBA_LINE_NAMES=1 names every copied file as the submodule's, which a PGO profile needs
# (GCC identifies a static function by its file's name, as the submodule's backend.c includes the three jit-*.h by
# "../"); the includer sets it for PGO=gen|use.
ifeq ($(strip $(ARMGBA_COPY)),)
$(error ARMGBA_COPY is not set)
endif
ARMGBA_SRC ?= $(REPO)/third_party/armgba
ARMGBA_PATCHES ?= $(sort $(wildcard $(REPO)/3ds/emu/armgba-patches/*.patch))
ARMGBA := $(ARMGBA_COPY)
ARMGBA_STAMP := $(ARMGBA_COPY)/.patched
$(ARMGBA_STAMP): $(ARMGBA_PATCHES) $(shell find $(ARMGBA_SRC)/src -type f 2>/dev/null)
	@test -f $(ARMGBA_SRC)/src/gba.c || { echo "armGBA's source is missing: git submodule update --init third_party/armgba"; exit 1; }
	@echo patching armGBA
	@rm -rf $(ARMGBA_COPY) && mkdir -p $(ARMGBA_COPY) && cp -r $(ARMGBA_SRC)/src $(ARMGBA_COPY)/src
	@for p in $(ARMGBA_PATCHES); do \
		echo "  $$(basename $$p)"; \
		patch -p1 -s -N -F0 --no-backup-if-mismatch -d $(ARMGBA_COPY) < $$p || { echo "armGBA patch $$p does not apply"; exit 1; }; \
	done
ifneq ($(ARMGBA_LINE_NAMES),)
	@cd $(ARMGBA_COPY)/src && find . -type f \( -name '*.c' -o -name '*.h' \) | while read f; do \
		n="$(ARMGBA_SRC)/src/$${f#./}"; \
		case $$f in ./cpu/jit-compile.h|./cpu/jit-layout.h|./cpu/jit-tiny.h) n="$(ARMGBA_SRC)/src/cpu/$(ARMGBA_BACKEND)/../$${f##*/}";; esac; \
		sed -i "1i #line 1 \"$$n\"" $$f; \
	done
endif
	@touch $@

ARMGBA_CORE := bios-image.c bus.c cheats.c gba.c memory.c state.c timing.c \
	audio/audio.c audio/audio-thread.c audio/psg.c cart/actionreplay.c cart/fcd.c cart/cart.c cart/ereader.c cart/gpio.c cart/overrides.c cart/savedata.c \
	cpu/arm.c cpu/decode.c cpu/step.c cpu/lean.c io/dma.c io/io.c io/rfu.c io/sio.c io/timer.c \
	video/render-thread.c video/render.c video/video.c cpu/jit.c cpu/$(ARMGBA_BACKEND)/backend.c
ifeq ($(ARMGBA_BACKEND),x64)
ARMGBA_CORE += video/render-x86.c
endif
ARMGBA_CORE_SRC := $(addprefix $(ARMGBA)/src/,$(ARMGBA_CORE))
# An object is named as the submodule's file would be (where a PGO profile looks for its counts): $(call ARMGBA_OBJ,folder).
ARMGBA_OBJ = $(patsubst $(ARMGBA)/%,$(1)/third_party/armgba/%.o,$(ARMGBA_CORE_SRC))
# -fwrapv: armGBA's clocks are 32-bit counters compared by subtracting, which signed overflow must allow.
ARMGBA_CORE_FLAGS := -O3 -fwrapv -Wall -Wextra -Wno-unused-parameter -I$(ARMGBA)/src
