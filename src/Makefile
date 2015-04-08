TOOLS	:= /home/kmtaylor/rpi/tools
GCC_VER := arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian
HOST	:= arm-linux-gnueabihf
RPATH	:= /home/pi

SYSROOT	:= /opt/bcm-rootfs
TOOLCHAIN := $(TOOLS)/$(GCC_VER)/bin/$(HOST)

CFLAGS += `pkg-config --cflags sdl`

LDFLAGS	:= -L /opt/bcm-rootfs/opt/vc/lib -L ../omxplayer -Wl,-rpath=$(RPATH)
LIBS	:= -lomxplayer -lWFC -lGLESv2 -lEGL -lbcm_host \
	   -lopenmaxil -lvchiq_arm -lvcos -lasound -lSDL

game: game.o
	$(TOOLCHAIN)-gcc -Wall --sysroot=$(SYSROOT) $(LDFLAGS) $(LIBS) $^ -o $@

%.o: %.c
	$(TOOLCHAIN)-gcc -Wall --sysroot=$(SYSROOT) $(CFLAGS) -c $<

clean:
	rm -f *.o
	
