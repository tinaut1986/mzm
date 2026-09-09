#---------------------------------------------------------------------------------
# Standalone .3dsx: drive port_affine_subtile.h (the shared affine-OBJ subtile
# geometry) on real hardware / Azahar, with no game and no scene recorder.
# See README.md.
#
# Flat, non-recursive -- same style as platform/3ds/Makefile, NOT the
# devkitPro 3ds_rules template (its recursive $(MAKE) breaks under the
# git-bash + Cygwin-make mix on Windows).
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
NM        := $(DEVKITARM)/bin/arm-none-eabi-nm
SMDHTOOL  := $(DEVKITPRO)/tools/bin/smdhtool
3DSXTOOL  := $(DEVKITPRO)/tools/bin/3dsxtool

BUILD      := $(CURDIR)/build

# devkitPro's Cygwin make does not pass TMP/TEMP to recipes; devkitARM's gcc
# is a native Windows binary and falls back to C:\WINDOWS, which is not
# writable. Point them at the build dir (harmless on Linux/macOS). Same fix
# as platform/3ds/Makefile.
ifeq ($(strip $(TMP)),)
  export TMP  := $(BUILD)/tmp
  export TEMP := $(BUILD)/tmp
  $(shell mkdir -p $(BUILD)/tmp)
endif

TARGET     := $(CURDIR)/affine_probe
PORT_SRC   := $(CURDIR)/../../source

APP_TITLE       := Affine subtile probe
APP_DESCRIPTION := Debug port_affine_subtile.h seams
APP_AUTHOR      := mzm-3ds
APP_ICON        := $(CTRULIB)/default_icon.png

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -O2 -fomit-frame-pointer -Wall -Wextra -mword-relocations \
           -ffunction-sections -fdata-sections -std=gnu11 -D__3DS__ -D_3DS $(ARCH)
INCLUDES := -I$(CURDIR)/source -I$(PORT_SRC) -I$(CTRULIB)/include -I$(PORTLIBS)/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(BUILD)/affine_probe.map
LIBS    := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib -lcitro2d -lcitro3d -lctru -lm

C_SRCS := $(wildcard $(CURDIR)/source/*.c)
C_OBJS := $(patsubst $(CURDIR)/source/%.c,$(BUILD)/%.o,$(C_SRCS))

.PHONY: all clean
all: $(TARGET).3dsx

$(BUILD)/%.o: $(CURDIR)/source/%.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) -MMD -MP -MF $(@:.o=.d) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET).elf: $(C_OBJS)
	@echo "Linking $(notdir $@)"
	@$(CC) $(LDFLAGS) $(C_OBJS) $(LIBS) -o $@
	@$(NM) -CSn $@ > $(BUILD)/affine_probe.lst

$(TARGET).smdh: $(APP_ICON)
	@echo "Creating SMDH"
	@$(SMDHTOOL) --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh
	@echo "Creating 3DSX: $(notdir $@)"
	@$(3DSXTOOL) $(TARGET).elf $@ --smdh=$(TARGET).smdh
	@echo "Built $@"

clean:
	@rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

-include $(wildcard $(BUILD)/*.d)
