# Makefile for Metroid Zero Mission Native Linux Port (x86, 32-bit)
#
# Deliberately -m32, not -m64: several structs (e.g. struct SoundEntry in
# include/structs/audio.h) are overlaid directly on raw GBA ROM bytes and
# contain pointer-typed fields sized for a 32-bit target (GBA/3DS ARM are
# both 32-bit). On a 64-bit build those fields double in size and desync
# the whole struct layout from the ROM, corrupting every field after the
# first pointer (confirmed via a QueueSound segfault reading garbage
# pHeader from sSoundDataEntries at boot). Building 32-bit keeps pointer
# width consistent with the ROM layout, matching the 3DS (ARM32) build,
# without having to retrofit every such struct.

CC ?= gcc
TARGET := mzm-linux

ROOT ?= $(abspath $(CURDIR)/../..)
BUILD := $(CURDIR)/build

CFLAGS := -m32 -O1 -g -std=gnu89 -fcommon \
	-Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
	-Wno-format-overflow -Wno-array-bounds -Wno-address-of-packed-member \
	-Wno-comment -Wno-strict-aliasing -Wno-implicit-function-declaration \
	-Wno-implicit-int -Wno-builtin-declaration-mismatch -Wno-discarded-qualifiers

CPPFLAGS := \
	-DREGION_EU \
	-DPORT_NATIVE \
	-DNON_MATCHING=1 \
	-DPLATFORM_LINUX \
	-DTMC_3DS \
	-DPORT_NATIVE_AUDIO_STUBS \
	-I$(ROOT) \
	-I$(ROOT)/port \
	-I$(ROOT)/port/generated/shadow \
	-I$(ROOT)/port/generated \
	-I$(ROOT)/include \
	-I$(CURDIR)/source

# Extra EWRAM-offset symbols (see ewram_symbols.ld) injected additively via
# --defsym instead of -T, since a bare -T script (no SECTIONS/INSERT) replaces
# the default Linux ELF script wholesale and breaks PT_PHDR/PIE layout.
EWRAM_DEFSYMS := $(shell sed -n 's/^\([A-Za-z0-9_]*\) = \(.*\);$$/-Wl,--defsym,\1=\2/p' $(CURDIR)/ewram_symbols.ld | tr -d ' ')

LDFLAGS := -no-pie $(EWRAM_DEFSYMS) -lrt -lm -lpthread

# Fuentes C
SRCS_ROOT := \
	$(wildcard $(ROOT)/src/*.c) \
	$(wildcard $(ROOT)/src/cutscenes/*.c) \
	$(wildcard $(ROOT)/src/menus/*.c) \
	$(wildcard $(ROOT)/src/sprites_ai/*.c) \
	$(wildcard $(ROOT)/src/sram/*.c) \
	$(ROOT)/src/data/shortcut_pointers.c \
	$(ROOT)/src/data/music_track_data.c \
	$(ROOT)/src/data/audio.c \
	$(ROOT)/src/data/color_fading_data.c \
	$(ROOT)/src/data/cutscenes/cutscenes_data.c \
	$(ROOT)/src/data/in_game_cutscene_data.c \
	$(ROOT)/src/data/empty_datatypes.c \
	$(ROOT)/port/port_gba_symbols.c \
	$(ROOT)/port/port_rom.c \
	$(ROOT)/port/port_gba_mem.c \
	$(ROOT)/port/port_bios.c \
	$(ROOT)/port/port_audio_stubs.c \
	$(ROOT)/port/port_debug_log.c \
	$(ROOT)/port/port_constructor_init.c \
	$(filter-out %_selftest.c %_test.c, $(wildcard $(ROOT)/port/generated/*.c))

SRCS_LINUX := \
	$(CURDIR)/source/main_linux.c \
	$(CURDIR)/source/platform_linux.c

OBJS_ROOT  := $(patsubst $(ROOT)/%.c, $(BUILD)/root/%.o, $(SRCS_ROOT))
OBJS_LINUX := $(patsubst $(CURDIR)/%.c, $(BUILD)/linux/%.o, $(SRCS_LINUX))
OBJS := $(OBJS_ROOT) $(OBJS_LINUX)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "Linking ELF (Linux): $@"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successful: $(CURDIR)/$@"

$(BUILD)/root/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD)/linux/%.o: $(CURDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

test: $(TARGET)
	@echo "Running headless automated test..."
	./$(TARGET) --test 300

clean:
	@rm -rf $(BUILD) $(TARGET)
