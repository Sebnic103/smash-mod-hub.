#---------------------------------------------------------------------------------
# Smash Mod Hub - Makefile para devkitPro (libnx)
#   Requiere: devkitA64, libnx y los paquetes de portlibs:
#   dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_image switch-sdl2_ttf \
#                 switch-curl switch-libarchive switch-mbedtls switch-zlib \
#                 switch-bzip2 switch-liblzma switch-zstd switch-libpng switch-libjpeg-turbo \
#                 switch-libwebp switch-freetype
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Define DEVKITPRO en tu entorno: export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET      := smash-mod-hub
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
ROMFS       := romfs

APP_TITLE   := Smash Mod Hub
APP_AUTHOR  := Smash Mod Hub
APP_VERSION := 1.0.0
ICON        := icon.jpg

#---------------------------------------------------------------------------------
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS   += $(INCLUDE) -D__SWITCH__ -DHAVE_LIBARCHIVE -DAPP_VERSION_STR=\"$(APP_VERSION)\"
CXXFLAGS := $(CFLAGS) -fno-rtti -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# Las dependencias exactas de enlazado las da pkg-config (así no hay que adivinar
# qué necesita libarchive/curl/SDL2 en tu versión de portlibs).
PKGCONF  ?= $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
PC_LIBS  := $(shell $(PKGCONF) --libs --static libarchive libcurl sdl2 SDL2_image SDL2_ttf 2>/dev/null)
ifeq ($(strip $(PC_LIBS)),)
# Plan B si pkg-config no está disponible: lista manual (puede variar según la versión de portlibs).
PC_LIBS  := -lSDL2_ttf -lSDL2_image -lSDL2 -lcurl -larchive -lmbedtls -lmbedx509 -lmbedcrypto \
            -lfreetype -lpng -ljpeg -lwebp -llzma -lbz2 -lz -lzstd -lEGL -lglapi -ldrm_nouveau
endif
LIBS     := $(PC_LIBS) -lnx

LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH  := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                 $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(CURDIR)/$(ICON)
endif
ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif
ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all check-romfs

all: check-romfs $(BUILD)

# Sin cacert.pem la app funciona pero NO verifica los certificados HTTPS.
check-romfs:
	@test -f $(ROMFS)/cacert.pem || echo "AVISO: falta $(ROMFS)/cacert.pem (descárgalo de https://curl.se/ca/cacert.pem); sin él las conexiones HTTPS no se verifican"

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo limpiando ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all : $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
