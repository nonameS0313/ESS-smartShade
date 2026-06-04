# smartshade_mqtt — Raspberry Pi 4 cross-build / static link
#
# Native build (on Pi):
#   make
#
# Cross-build from PC (WSL/Linux), 64-bit Pi OS (default):
#   make CROSS_COMPILE=aarch64-linux-gnu- STATIC=1
#
# 32-bit Pi OS:
#   make PI_ARCH=armhf CROSS_COMPILE=arm-linux-gnueabihf- STATIC=1
#
# Sysroot (optional, when libs are under e.g. ~/pi-sysroot):
#   make CROSS_COMPILE=aarch64-linux-gnu- SYSROOT=~/pi-sysroot STATIC=1
#
# Host deps (Ubuntu/WSL example, 64-bit):
#   sudo apt install gcc-aarch64-linux-gnu
#   # static libmosquitto for target — copy from Pi after:
#   #   sudo apt install libmosquitto-dev
#   #   dpkg -L libmosquitto-dev | grep '\.a$'
#   # or build mosquitto with -DBUILD_SHARED=OFF for aarch64

CROSS_COMPILE ?=
CC            := $(CROSS_COMPILE)gcc
STRIP         := $(CROSS_COMPILE)strip

PI_ARCH       ?= arm64

TARGET        = smartshade_mqtt
SRCS          = smartshade_mqtt.c

CFLAGS        += -Wall -Wextra -O2 -Idriver -I../pi2_sensor -pthread
LDFLAGS       += -pthread

# Architecture tuning
ifeq ($(PI_ARCH),arm64)
  CFLAGS += -march=armv8-a
else ifeq ($(PI_ARCH),armhf)
  CFLAGS += -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
endif

# Optional sysroot for cross lib/headers
ifneq ($(SYSROOT),)
  CFLAGS  += --sysroot=$(SYSROOT)
  LDFLAGS += --sysroot=$(SYSROOT)
  ifneq ($(wildcard $(SYSROOT)/usr/lib/aarch64-linux-gnu),)
    LDFLAGS += -L$(SYSROOT)/usr/lib/aarch64-linux-gnu
  endif
  ifneq ($(wildcard $(SYSROOT)/usr/lib/arm-linux-gnueabihf),)
    LDFLAGS += -L$(SYSROOT)/usr/lib/arm-linux-gnueabihf
  endif
  LDFLAGS += -L$(SYSROOT)/usr/lib
endif

ifeq ($(STATIC),1)
  LDFLAGS += -static
  # libmosquitto.a often pulls OpenSSL; drop -lssl -lcrypto if built with WITH_TLS=no
  LDLIBS   = -lmosquitto -lssl -lcrypto -ldl -lm
else
  LDLIBS   = -lmosquitto
endif

.PHONY: all clean strip driver rpi4 rpi4-static rpi4-32-static

all: $(TARGET)

driver:
	$(MAKE) -C driver

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

strip: $(TARGET)
	$(STRIP) $(TARGET)

# Presets for Pi 4
rpi4:
	$(MAKE) CROSS_COMPILE=aarch64-linux-gnu- PI_ARCH=arm64

rpi4-static:
	$(MAKE) CROSS_COMPILE=aarch64-linux-gnu- PI_ARCH=arm64 STATIC=1

rpi4-32-static:
	$(MAKE) CROSS_COMPILE=arm-linux-gnueabihf- PI_ARCH=armhf STATIC=1

clean:
	rm -f $(TARGET)
