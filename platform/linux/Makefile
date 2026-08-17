# Makefile for Metroid Zero Mission Native Linux Port (x86_64)

CC ?= gcc
TARGET := mzm-linux

ROOT ?= $(abspath $(CURDIR)/../..)
BUILD := $(CURDIR)/build

CFLAGS := -m64 -O1 -g -std=gnu89 -fcommon \
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
	-I$(ROOT)/port \
	-I$(ROOT)/port/generated/shadow \
	-I$(ROOT)/port/generated \
	-I$(ROOT)/include \
	-I$(CURDIR)/source

LDFLAGS := -lrt -lm

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
	$(ROOT)/port/port_gba_symbols.c \
	$(ROOT)/port/port_rom.c \
	$(ROOT)/port/port_gba_mem.c \
	$(ROOT)/port/port_bios.c \
	$(ROOT)/port/port_audio_stubs.c \
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
