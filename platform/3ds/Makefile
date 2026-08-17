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
APP_DESCRIPTION := Native 3DS port of Metroid Zero Mission
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
	$(ROOT)/port/port_gba_symbols.c \
	$(ROOT)/port/port_rom.c \
	$(ROOT)/port/port_gba_mem.c \
	$(ROOT)/port/port_bios.c \
	$(ROOT)/port/port_gba_timing.c \
	$(ROOT)/port/port_debug_log.c \
	$(filter-out %_selftest.c %_test.c, $(wildcard $(ROOT)/port/generated/*.c))

SRCS_ASM := \
	$(ROOT)/asm/audio_internal.s \
	$(ROOT)/asm/soundcode.s \
	$(ROOT)/asm/intr_main.s \
	$(ROOT)/asm/linker_symbols.s

SRCS_3DS := \
	$(CURDIR)/source/main_3ds.c \
	$(CURDIR)/source/platform_3ds_minimal.c

# Generar objetos
OBJS_ROOT := $(patsubst $(ROOT)/%.c, $(BUILD)/root/%.o, $(SRCS_ROOT))
OBJS_ASM  := $(patsubst $(ROOT)/%.s, $(BUILD)/root/%.o, $(SRCS_ASM))
OBJS_3DS  := $(patsubst $(CURDIR)/%.c, $(BUILD)/3ds/%.o, $(SRCS_3DS))
C_OBJS    := $(OBJS_ROOT) $(OBJS_ASM) $(OBJS_3DS)
DEPS      := $(C_OBJS:.o=.d)

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -O2 -mword-relocations -ffunction-sections -fdata-sections \
           -DREGION_EU -DMZM_PORT_VERSION='"0.0-dev"' -D__3DS__ -DTMC_3DS \
           -Wno-implicit-function-declaration -Wno-implicit-int -Wno-builtin-declaration-mismatch \
           -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function \
           -Wno-return-mismatch -Wno-stringop-overflow -Wno-strict-aliasing -Wno-parentheses \
           -Wno-int-conversion -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
           $(ARCH)

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

LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(BUILD)/mzm-3ds.map -Wl,--gc-sections
LIBS     := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib -lcitro2d -lcitro3d -lctru -lm

MAKEROM    ?= $(shell which makerom 2>/dev/null || echo /home/tinaut1986/tools/bin/makerom)
BANNERTOOL ?= $(shell which bannertool 2>/dev/null || echo /home/tinaut1986/tools/bin/bannertool)

.PHONY: all clean cia elf

all: $(TARGET).cia

cia: $(TARGET).cia

elf: $(TARGET).elf

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
	@$(LD) $(LDFLAGS) $(C_OBJS) $(LIBS) -o $@
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
