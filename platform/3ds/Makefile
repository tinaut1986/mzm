#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# MZM_ROOT is the root of the mzm decompilation (two levels up from here).
#
# SOURCES is deliberately NOT "all of src/ recursively": src/data/*.c bakes
# copyrighted assets straight from the user's ROM into the binary at build
# time (see ../../docs/3ds-port-rom-loading.md) -- the whole point of the
# runtime-ROM-loading design is that the 3DS binary doesn't ship those, so
# src/data/ is excluded and port/generated/*_rom.c (the ROM-offset shims)
# stands in for it instead. nes_metroid/ (the embedded original-NES-Metroid
# mode) is hand-written GBA Thumb assembly, never decompiled to C -- out of
# scope until there's a plan for running/translating real GBA asm on ARM11.
#---------------------------------------------------------------------------------
MZM_ROOT	:=	$(CURDIR)/../..

TARGET		:=	mzm-3ds
BUILD		:=	build
# All entries absolute (MZM_ROOT already is, via $(CURDIR)): VPATH below
# uses these directly without re-prefixing $(CURDIR) -- doing that on an
# already-absolute path produced a broken "/a//b"-style concatenation that
# silently resolved to nothing, which is why the inner (recursive, cwd=
# build/) pass couldn't find any of these sources despite the outer pass
# computing the right file list.
SOURCES		:=	$(MZM_ROOT)/src \
			$(CURDIR)/source
SOURCES_DISABLED_FOR_DEBUG := \
			$(MZM_ROOT)/src/menus \
			$(MZM_ROOT)/src/sprites_ai \
			$(MZM_ROOT)/src/cutscenes \
			$(MZM_ROOT)/src/sram \
			$(MZM_ROOT)/port \
			$(MZM_ROOT)/port/ppu/src \
			$(MZM_ROOT)/port/generated
DATA		:=	data
INCLUDES	:=	include

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			-DREGION_EU -DMZM_PORT_VERSION='"0.0-dev"' \
			-DTMC_3DS \
			$(ARCH)
# TMC_3DS is an internal-only macro name (never user-facing) kept from the
# files still copied wholesale from zelda-tmc-3ds (port_gba_mem.c,
# old3ds_frame_pacer.c, platform_gpu_3ds.c) to select their "3DS build"
# `#ifdef` branches, rather than touching every conditional in files that
# weren't otherwise modified.

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(SOURCES) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

# Excluded from ./source (platform/3ds/source): still deeply coupled to
# zelda-tmc-3ds's own subsystems (PPU<->GPU bridge, audio backend, second-
# screen UI, save/rando/TTS/reborn) that haven't been adapted for mzm yet --
# see docs/3ds-port-skeleton-import.md. This build proves everything else
# (all gameplay logic, generated ROM data, GBA BIOS reimplementation, ROM
# loading, core 3DS platform init) compiles and links; it reaches agbmain()
# with no video/audio output yet.
# platform_3ds.c itself is also excluded: platform_3ds_minimal.c (this
# repo's own clean-room implementation of the same platform_3ds.h contract)
# replaces it -- see that file's header comment for why.
PLATFORM_3DS_EXCLUDE := port_ppu_3ds.c port_audio_3ds.c port_second_screen_3ds.c \
			port_second_screen_sync_3ds.c bottom_frame_state_3ds.c bottom_idle_3ds.c \
			port_config_3ds.c port_optional_stubs_3ds.c port_rando_3ds.c port_asset_stubs_3ds.c \
			platform_3ds.c

# port_m4a_backend/port_m4a_stubs: copied from zelda-tmc-3ds early on as a
# reference for the NDSP buffer-feeding pattern, but mzm doesn't use M4A/
# Sappy (see docs/3ds-port-rom-loading.md's audio-engine finding) -- not
# part of this build. *_selftest.c / *_test.c files each have their own
# main() for standalone `gcc ... -o /tmp/t` runs (see their header
# comments) and would collide with main_3ds.c's main() if linked in here.
PORT_EXCLUDE := port_m4a_backend.cpp port_m4a_stubs.c port_gba_mem_test.c \
			port_rom_selftest.c port_bios_selftest.c chozodia_escape_data_rom_selftest.c

CFILES		:=	$(filter-out $(PLATFORM_3DS_EXCLUDE) $(PORT_EXCLUDE),$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))))
CPPFILES	:=	$(filter-out $(PORT_EXCLUDE),$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp))))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

# port/generated/shadow goes FIRST: it shadows include/data/*.h (ROM-offset
# shims instead of compiled-in asset data) and include/syscalls.h (routes
# SYSCALL(n) through Port_Bios_Halt() instead of a raw `swi`, since that
# traps into Horizon OS's syscall table on 3DS, not the GBA BIOS -- see
# ../../port/port_bios.c). Every #include "data/*.h" / "syscalls.h" in the
# unmodified src/*.c resolves to these instead purely by search-path order.
export INCLUDE	:=	-I$(MZM_ROOT)/port/generated/shadow \
			-I$(MZM_ROOT) \
			$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			-I$(MZM_ROOT)/include \
			-I$(MZM_ROOT)/port \
			-I$(MZM_ROOT)/port/ppu/include \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		else
			export APP_ICON := $(TOPDIR)/assets/icon-48.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

APP_TITLE := Metroid Zero Mission 3DS
APP_DESCRIPTION := Native 3DS port of the mzm decompilation
APP_AUTHOR := metroidret + community port work

.PHONY: all clean

#---------------------------------------------------------------------------------
all: $(BUILD) $(DEPSDIR)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
