#---------------------------------------------------------------------------------
# FireTV Remote for 3DS - based on the devkitPro 3DS template
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET          :=  firetv-remote
BUILD           :=  build
SOURCES         :=  source
DATA            :=
INCLUDES        :=  source

APP_TITLE       :=  FireTV Remote
APP_DESCRIPTION :=  Control a Fire TV over Wi-Fi (ADB)
APP_AUTHOR      :=  Kai
ICON            :=  icon.png

#---------------------------------------------------------------------------------
ARCH     :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   :=  -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS   +=  $(INCLUDE) -D__3DS__
ASFLAGS  :=  -g $(ARCH)
LDFLAGS   =  -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS     :=  -lcitro2d -lcitro3d -lmbedtls -lmbedx509 -lmbedcrypto -lctru -lm
LIBDIRS  :=  $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)
export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES          :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD       :=  $(CC)
export OFILES   :=  $(CFILES:.c=.o)
export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)
export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

.PHONY: all clean cia

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Needs makerom + bannertool on PATH (see README)
cia: all
	@cia/make_cia.sh $(TARGET).elf $(TARGET).cia

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).cia

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf  : $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
