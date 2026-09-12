# ReRfM-MarsMiner — self-contained C/C++ ROM asset extractor.
#
# One binary. Plain C for the ROM prep + asset decoders; the DCS-2 audio decoder
# (vendored ADSP-2105 core, BSD-3) is C++ and linked in. Only external runtime
# dependency: libFLAC (lossless audio). PNG is the vendored stb_image_write.
#
#   make            build ./marsminer
#   make test       build + run the unit tests (skip themselves without ROMs)
#   make golden     re-extract real ROMs and diff the output against known hashes (~1.5 min)
#   make clean

CC       ?= gcc
CXX      ?= g++
CSTD     := -std=c11
CXXSTD   := -std=c++20
OPT      := -O2
WARN     := -Wall -Wextra -Wno-unused-parameter
INC      := -Isrc -Ivendor -Ivendor/dcs
# -MMD -MP: emit a .d per object listing the headers it included, so editing marsminer.h
# rebuilds everything that includes it. Without this an incremental build silently mixes
# objects compiled against two different struct layouts.
DEPFLAGS := -MMD -MP

# libFLAC via pkg-config (falls back to -lFLAC)
FLAC_CFLAGS := $(shell pkg-config --cflags flac 2>/dev/null)
FLAC_LIBS   := $(shell pkg-config --libs flac 2>/dev/null || echo -lFLAC)

BUILD := build
BIN   := marsminer

# --- sources -----------------------------------------------------------------
C_SRC := src/util.c src/addrspace.c src/symbols.c src/prep.c \
         src/assets_common.c src/png.c src/anims.c src/movies.c src/scan.c \
         src/fonts.c src/messages.c src/adjustments.c src/tables.c src/used_ids.c \
         src/spectral.c src/zip.c src/main.c

CXX_SRC := src/sounds.cpp src/music.cpp \
           vendor/dcs/adsp2105.cpp vendor/dcs/2100dasm.cpp \
           vendor/dcs/dcs2_p2k.cpp vendor/dcs/flac_encode.cpp

# the two decode drivers; rename each main() so we call them as functions.
EXPORT_SRC := vendor/dcs/dcs2_export.cpp
EXTRACT_SRC := vendor/dcs/dcs2_extract.cpp

C_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC))
CXX_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(CXX_SRC))
EXPORT_OBJ := $(BUILD)/vendor/dcs/dcs2_export.o
EXTRACT_OBJ := $(BUILD)/vendor/dcs/dcs2_extract.o

OBJ := $(C_OBJ) $(CXX_OBJ) $(EXPORT_OBJ) $(EXTRACT_OBJ)

# --- rules -------------------------------------------------------------------
.PHONY: all clean test golden
all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(OPT) $(OBJ) -o $@ $(FLAC_LIBS) -lm
	@echo "built ./$(BIN)"

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(OPT) $(WARN) $(DEPFLAGS) $(INC) -c $< -o $@

# Our own C++ builds with warnings on, like the C does.
$(BUILD)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) $(DEPFLAGS) $(INC) $(FLAC_CFLAGS) -c $< -o $@

# The vendored MAME core does not build warning-clean under our flags and is not ours to
# fix, so -w applies there and only there.
$(BUILD)/vendor/%.o: vendor/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) -w $(DEPFLAGS) $(INC) $(FLAC_CFLAGS) -c $< -o $@

# the decode drivers: rename main -> dcs2_{export,extract}_main so we can invoke them.
$(EXPORT_OBJ): $(EXPORT_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) -w $(DEPFLAGS) -Dmain=dcs2_export_main $(INC) $(FLAC_CFLAGS) -c $< -o $@

$(EXTRACT_OBJ): $(EXTRACT_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) -w $(DEPFLAGS) -Dmain=dcs2_extract_main $(INC) $(FLAC_CFLAGS) -c $< -o $@

# --- tests -------------------------------------------------------------------
# The vendored ADSP-2105 core is the one piece of MarsMiner that is emulation rather than
# decoding, so it is the one piece worth proving on its own. core_test executes a hand-decoded
# instruction and reads the register back; dcs2_test boots the real DCS-2 board from the ROMs and
# checks the DSP converges on its documented post-boot state (PC in the resident-engine idle band,
# engine paged into program SRAM). used_ids_test pins the DCS play-set walk — the one decision that
# silently changes WHICH sounds get extracted — against tests/baseline/rfm_dcs_used_ids.txt, whose
# header names the exact game and ROM version it came from (the test checks those md5s first).
# Both ROM-reading tests skip themselves when the ROMs are absent, so this target still passes in a
# ROM-free checkout.
#
# ROMS = where your dumps live, in the layout README documents (chips/, update_0180/). Default:
# ./roms if you have one, else ../roms — so this works both standalone and inside the ReRfM tree,
# with no edit either way. Override on the command line: make test ROMS=/path/to/roms
ROMS ?= $(firstword $(wildcard roms ../roms) roms)

test: $(BUILD)/core_test $(BUILD)/dcs2_test $(BUILD)/used_ids_test
	$(BUILD)/core_test
	$(BUILD)/dcs2_test "$(abspath $(ROMS))"
	$(BUILD)/used_ids_test "$(abspath $(ROMS))"
	@echo "[marsminer] tests passed"

$(BUILD)/core_test: tests/core_test.cpp vendor/dcs/adsp2105.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) -w $(INC) $^ -o $@

$(BUILD)/dcs2_test: tests/dcs2_test.cpp vendor/dcs/adsp2105.cpp vendor/dcs/2100dasm.cpp vendor/dcs/dcs2_p2k.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) -w $(INC) $^ -o $@

# links the real extractor objects (C, built by the pattern rule above) — the test must exercise
# the shipped walk, not a copy of it.
$(BUILD)/used_ids_test: tests/used_ids_test.cpp $(BUILD)/src/used_ids.o $(BUILD)/src/symbols.o $(BUILD)/src/util.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) $(INC) $^ -o $@

# The decoders' regression net: the unit tests cover the DSP core and the play-set walk, not the
# image/font/table decoders — those were validated against a Python reference that does not ship
# with this repository. Separate from `test` because it needs ROMs and takes ~1.5 minutes.
golden: $(BIN)
	tests/golden.sh "$(ROMS)"

clean:
	rm -rf $(BUILD) $(BIN)

# the generated header dependencies (see DEPFLAGS); absent on a first build, hence the dash
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
