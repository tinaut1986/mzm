#---------------------------------------------------------------------------------
# Metroid Zero Mission - 3DS Port Makefile
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
  DEVKITPRO := /opt/devkitpro
endif
ifeq ($(strip $(DEVKITARM)),)
  DEVKITARM := $(DEVKITPRO)/devkitARM
endif

CTRULIB   := $(DEVKITPRO)/libctru
PORTLIBS  := $(DEVKITPRO)/portlibs/3ds

CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
CXX       := $(DEVKITARM)/bin/arm-none-eabi-g++
LD        := $(DEVKITARM)/bin/arm-none-eabi-gcc
NM        := $(DEVKITARM)/bin/arm-none-eabi-nm
SMDHTOOL  := $(DEVKITPRO)/tools/bin/smdhtool
3DSXTOOL  := $(DEVKITPRO)/tools/bin/3dsxtool

ROOT      := $(abspath $(CURDIR)/../..)
BUILD     := $(CURDIR)/build
TARGET    := $(CURDIR)/mzm-3ds

APP_TITLE       := Metroid Zero Mission 3DS
# Recursively expanded (=) so it's resolved at recipe time, once VERSION is known.
# SMDH text fields have a hard 64-character limit, so the version is truncated
# if "Native 3DS port of Metroid Zero Mission <VERSION>" would overflow it.
APP_DESCRIPTION  = $(shell printf '%s %s' "Native 3DS port of Metroid Zero Mission" "$(VERSION)" | cut -c1-64)
APP_AUTHOR      := metroidret + community
APP_ICON        := $(CURDIR)/assets/icon-48.png
ifeq ($(wildcard $(APP_ICON)),)
  APP_ICON := $(CTRULIB)/default_icon.png
endif

# Fuentes C a compilar (relativas a ROOT)
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
	$(ROOT)/port/port_gba_timing.c \
	$(ROOT)/port/port_debug_log.c \
	$(ROOT)/port/port_constructor_init.c \
	$(ROOT)/port/port_mzm_audio_glue.c \
	$(ROOT)/port/port_psg_synth.c \
	$(ROOT)/port/ppu/src/virtuappu.c \
	$(ROOT)/port/ppu/src/mode1.c \
	$(filter-out %_selftest.c %_test.c, $(wildcard $(ROOT)/port/generated/*.c))



# Motor de audio real de mzm: UpdateAudio/InitTrack/UpdateTrack viven en
# asm/audio_internal.s (ARM Thumb sin descompilar) y el mezclador PCM en
# asm/soundcode.s. Ya incluyen el fix de port_resolve_addr() en InitTrack.
# Se enlazan en el build 3DS para que el sonido use el engine propio de mzm;
# el PCM se consume desde gMusicInfo.soundRawData por el pegamento NDSP
# (port_mzm_audio_3ds.c).
SRCS_ASM := \
	$(ROOT)/asm/intr_main.s \
	$(ROOT)/asm/linker_symbols.s \
	$(ROOT)/asm/audio_internal.s \
	$(ROOT)/asm/soundcode.s

SRCS_3DS := \
	$(CURDIR)/source/main_3ds.c \
	$(CURDIR)/source/platform_3ds_minimal.c \
	$(CURDIR)/source/platform_gpu_3ds.c \
	$(CURDIR)/source/port_bottom_ui_3ds.c \
	$(CURDIR)/source/port_ppu_mzm.c \
	$(CURDIR)/source/port_gpu_renderer.c \
	$(CURDIR)/source/port_mzm_audio_3ds.c \
	$(CURDIR)/source/port_retroachievements_3ds.c \
	$(CURDIR)/source/port_ra_badges_data.c

# Generar objetos
OBJS_ROOT := $(patsubst $(ROOT)/%.c, $(BUILD)/root/%.o, $(SRCS_ROOT))
OBJS_ASM  := $(patsubst $(ROOT)/%.s, $(BUILD)/root/%.o, $(SRCS_ASM))
OBJS_3DS  := $(patsubst $(CURDIR)/%.c, $(BUILD)/3ds/%.o, $(SRCS_3DS))
C_OBJS    := $(OBJS_ROOT) $(OBJS_ASM) $(OBJS_3DS)
DEPS      := $(C_OBJS:.o=.d)

GIT_BRANCH  := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)
EXACT_TAG   := $(shell git describe --tags --exact-match 2>/dev/null)
# Rama release/* más reciente que contiene HEAD (local u origin): permite
# que ramas laterales (p.ej. PRs basados en release) hereden su versión
REL_MERGED := $(shell git for-each-ref --format='%(refname:short)' --merged HEAD refs/heads/'release/*' refs/remotes/'origin/release/*' 2>/dev/null | sort -V | tail -1)
REL_BASE    := $(shell git rev-parse --verify -q refs/remotes/origin/main >/dev/null && echo refs/remotes/origin/main || echo main)

ifneq ($(strip $(EXACT_TAG)),)
  GIT_VERSION := $(EXACT_TAG)
else
ifeq ($(filter release/%,$(GIT_BRANCH)),)
  REL_NAME    := $(subst origin/,,$(REL_MERGED))
else
  REL_NAME    := $(GIT_BRANCH)
endif
ifneq ($(filter release/%,$(REL_NAME)),)
  REL_TARGET  := $(shell git rev-parse --verify -q refs/heads/$(REL_NAME) >/dev/null && echo refs/heads/$(REL_NAME) || echo refs/remotes/origin/$(REL_NAME))
  REL_CNT     := $(shell git rev-list --count $(REL_BASE)..$(REL_TARGET) 2>/dev/null || echo 1)
  DEV_CNT     := $(shell git rev-list --count $(REL_TARGET)..HEAD 2>/dev/null || echo 0)
  REL_HASH    := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
  ifeq ($(DEV_CNT),0)
    GIT_VERSION := $(patsubst release/%,%,$(REL_NAME))-dev.$(REL_CNT)+$(REL_HASH)
  else
    GIT_VERSION := $(patsubst release/%,%,$(REL_NAME))-dev.$(REL_CNT).$(DEV_CNT)+$(REL_HASH)
  endif
else
  GIT_VERSION := $(shell git describe --tags --always 2>/dev/null || echo "0.0-dev")
endif
endif
VERSION ?= $(GIT_VERSION)

# Which retail region's code paths / ROM offsets to compile in. The port
# still detects the loaded ROM's region at runtime (gRomRegion), but the
# decomp's `#if REGION_*` structure differences are a compile-time choice,
# so one binary targets one region. eu = historical default.
REGION  ?= eu
ifeq ($(REGION),eu)
  REGION_DEFINE := -DREGION_EU
else ifeq ($(REGION),us)
  REGION_DEFINE := -DREGION_US
else ifeq ($(REGION),jp)
  REGION_DEFINE := -DREGION_JP
else
  $(error REGION must be one of: eu us jp)
endif

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -O2 -fomit-frame-pointer -Wall -mword-relocations -ffunction-sections -fdata-sections \
           $(REGION_DEFINE) -DMZM_PORT_VERSION=\"$(VERSION)\" -D__3DS__ -DMZM_3DS \
           -DNON_MATCHING=1 \
           -Wno-implicit-function-declaration -Wno-implicit-int -Wno-builtin-declaration-mismatch \
           -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
           -Wno-return-mismatch -Wno-stringop-overflow -Wno-strict-aliasing -Wno-parentheses \
           -Wno-int-conversion -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
           $(ARCH)


# Which top-screen renderer to build: gpu (default) offloads mode-0 tile/
# sprite compositing to the PICA200 (port_gpu_renderer.c), falling back to
# the CPU scanline renderer per-frame for anything it doesn't support yet
# (affine BG, blend, windows, mosaic, affine OBJ -- see
# Port_GpuRenderer_CanRenderFrame). cpu forces the plain CPU scanline path
# (mode1.c) unconditionally, e.g. to isolate whether a bug is renderer-
# specific:
#   make clean && make RENDERER=cpu
# The clean matters: changing flags alone does not invalidate existing
# objects, which is exactly the stale-.o trap documented in
# docs/3ds-port-status-2026-08-17.md section 6l.
RENDERER ?= gpu
ifeq ($(RENDERER),gpu)
  CFLAGS += -DPORT_GPU_TILE_RENDERER
else ifneq ($(RENDERER),cpu)
  $(error Unknown RENDERER '$(RENDERER)' -- expected 'gpu' or 'cpu')
endif

# Force the Old3DS performance profile even when actually running on New3DS
# hardware: no CPU/L2 clock speedup (osSetSpeedupEnable is skipped, so the
# CPU stays at Old3DS's base clock instead of jumping to New3DS's 804MHz),
# no extra Core1 worker thread (APT_SetAppCpuTimeLimit is skipped), and the
# PPU/GPU renderers pick their Old3DS render profile (adaptive frameskip
# enabled, virtuappu_mode1's old3ds mode1 profile) -- see the sIsNew3DS
# override in Platform3DS_Init (platform_3ds_minimal.c). Lets you benchmark
# how the game would actually run on an Old3DS/2DS by building and
# installing this on a New3DS you already own, no separate console needed:
#   make clean && make FORCE_OLD3DS=1
# The clean matters: changing flags alone does not invalidate existing
# objects, which is exactly the stale-.o trap documented in section 6l.
FORCE_OLD3DS ?= 0
ifeq ($(FORCE_OLD3DS),1)
  CFLAGS += -DPORT_FORCE_OLD3DS_PROFILE
else ifneq ($(FORCE_OLD3DS),0)
  $(error Unknown FORCE_OLD3DS '$(FORCE_OLD3DS)' -- expected '0' or '1')
endif

# Compiles in the L+R+<button> debug combos (issue #17's investigation
# tools: instant-kill, scene recorder, one-shot fixture replay, live atlas
# dump, one-shot screen/state dump, USER MARK log line -- see
# docs/3ds-debug-tools.md) -- source-level, not just runtime-gated, so a
# production build has NO WAY to trigger any of them from a controller, not
# even by accident. This is the "simple debug" build: on its own it does NOT
# turn on any of the verbose per-frame *_DIAG_LOG tracing below (those cost
# real frame rate / SD I/O and are opt-in separately), just the on-demand
# combos:
#   make clean && make DEBUG_TOOLS=1
# Any of the *_DIAG_LOG flags below also turn this on automatically (see
# PORT_DEBUG_TOOLS_ACTIVE in source/port_debug_tools.h) -- a session that
# only asked for GPU or audio tracing still gets the combos for free, no
# need to also remember this flag.
# The clean matters: changing flags alone does not invalidate existing
# objects, which is exactly the stale-.o trap documented in section 6l.
DEBUG_TOOLS ?= 0
ifeq ($(DEBUG_TOOLS),1)
  CFLAGS += -DPORT_DEBUG_TOOLS
else ifneq ($(DEBUG_TOOLS),0)
  $(error Unknown DEBUG_TOOLS '$(DEBUG_TOOLS)' -- expected '0' or '1')
endif

# Extra flags from the command line, e.g. to turn on the audio diagnostics that
# are off by default (they do SD I/O on the audio path and cost real frame rate,
# see docs/3ds-port-status-2026-08-17.md section 6s):
#   make clean && make EXTRA_CFLAGS=-DPORT_AUDIO_DIAG_LOG
# The clean matters: changing flags alone does not invalidate existing objects,
# which is exactly the stale-.o trap documented in section 6l.
CFLAGS  += $(EXTRA_CFLAGS)

INCLUDES := \
	-I$(ROOT)/port/generated/shadow \
	-I$(ROOT) \
	-I$(ROOT)/include \
	-I$(ROOT)/port \
	-I$(ROOT)/port/generated \
	-I$(ROOT)/port/ppu/include \
	-I$(CURDIR)/source \
	-I$(CTRULIB)/include \
	-I$(PORTLIBS)/include

LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(BUILD)/mzm-3ds.map -Wl,--gc-sections -T $(CURDIR)/ewram_symbols.ld
LIBS     := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib -lcitro2d -lcitro3d -lctru -lm


MAKEROM    ?= $(shell which makerom 2>/dev/null || echo $(DEVKITPRO)/tools/bin/makerom)
BANNERTOOL ?= $(shell which bannertool 2>/dev/null || echo $(DEVKITPRO)/tools/bin/bannertool)

.PHONY: all clean cia elf 3dsx print-version ftp

all: $(TARGET).cia

cia: $(TARGET).cia

3dsx: $(TARGET).3dsx

elf: $(TARGET).elf

print-version:
	@echo $(VERSION)

ftp: $(TARGET).cia
	@echo "Uploading $(TARGET).cia as mzm-3ds-$(VERSION).cia to FTP..."
	curl -T $(TARGET).cia "ftp://$(FTP_HOST):$(FTP_PORT)/cias/mzm-3ds-$(VERSION).cia"

$(TARGET).cia: $(TARGET).elf $(BUILD)/mzm-3ds.smdh $(BUILD)/banner.bnr
	@echo "Creating CIA: $(notdir $@)"
	@$(MAKEROM) -f cia -o $@ -elf $(TARGET).elf -rsf $(CURDIR)/cia/mzm3ds.rsf \
		-icon $(BUILD)/mzm-3ds.smdh -banner $(BUILD)/banner.bnr -DAPP_ROMFS=$(CURDIR)/romfs
	@echo "CIA build successful: $@"

$(BUILD)/banner.bnr: $(CURDIR)/assets/banner.png $(CURDIR)/assets/banner.wav
	@mkdir -p $(dir $@)
	@echo "Creating Banner: $(notdir $@)"
	@$(BANNERTOOL) makebanner -i $(CURDIR)/assets/banner.png -a $(CURDIR)/assets/banner.wav -o $@

$(BUILD)/mzm-3ds.smdh: $(CURDIR)/assets/icon-48.png
	@mkdir -p $(dir $@)
	@echo "Creating SMDH: $(notdir $@)"
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $< -o $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	@echo "Creating 3DSX: $(notdir $@)"
	@$(3DSXTOOL) $(TARGET).elf $@ --smdh=$(TARGET).smdh

$(TARGET).smdh: $(APP_ICON)
	@echo "Creating SMDH: $(notdir $@)"
	@$(SMDHTOOL) --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

$(TARGET).elf: $(C_OBJS)
	@echo "Linking ELF: $(notdir $@)"
	@for f in $(C_OBJS); do cygpath -m "$$f" 2>/dev/null || echo "$$f"; done > $(BUILD)/objects.rsp
	@$(LD) $(LDFLAGS) @$(BUILD)/objects.rsp $(LIBS) -o $@
	@$(NM) -CSn $@ > $(BUILD)/mzm-3ds.lst
	@echo "Build successful: $(TARGET).elf"

CFLAGS_GEN := $(patsubst -O2,-O1,$(CFLAGS))

# Compilación de archivos autogenerados (ROM shims) con -O1 para velocidad
$(BUILD)/root/port/generated/%.o: $(ROOT)/port/generated/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) -MMD -MP -MF $(@:.o=.d) $(CFLAGS_GEN) $(INCLUDES) -c $< -o $@

# Compilación de archivos C bajo ROOT
$(BUILD)/root/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) -MMD -MP -MF $(@:.o=.d) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compilación de archivos ensamblador bajo asm/
$(BUILD)/root/asm/%.o: $(ROOT)/asm/%.s
	@mkdir -p $(dir $@)
	@echo "AS $<"
	@$(CC) -x assembler-with-cpp -Wa,-I$(ROOT) -march=armv6k -mfloat-abi=hard -mtp=soft -I$(ROOT) -c $< -o $@

# Compilación de archivos C bajo platform/3ds
$(BUILD)/3ds/%.o: $(CURDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) -MMD -MP -MF $(@:.o=.d) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

-include $(DEPS)
