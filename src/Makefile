TOOLS	:= /home/kmtaylor/rpi/tools
GCC_VER := arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian
HOST	:= arm-linux-gnueabihf
RPATH	:= /home/pi

SYSROOT	:= /opt/bcm-rootfs
TOOLCHAIN := $(TOOLS)/$(GCC_VER)/bin/$(HOST)

CFLAGS += `pkg-config --cflags sdl` -I /opt/bcm-rootfs/usr/local/include
CFLAGS += -I ../../omxplayer

CFLAGS += -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DTARGET_POSIX \
	  -DTARGET_LINUX -fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE \
	  -D_FILE_OFFSET_BITS=64 -DHAVE_CMAKE_CONFIG -D__VIDEOCORE4__ \
	  -U_FORTIFY_SOURCE -Wall -DHAVE_OMXLIB -DUSE_EXTERNAL_FFMPEG  \
	  -DHAVE_LIBAVCODEC_AVCODEC_H -DHAVE_LIBAVUTIL_OPT_H \
	  -DHAVE_LIBAVUTIL_MEM_H -DHAVE_LIBAVUTIL_AVUTIL_H \
	  -DHAVE_LIBAVFORMAT_AVFORMAT_H -DHAVE_LIBAVFILTER_AVFILTER_H \
	  -DHAVE_LIBSWRESAMPLE_SWRESAMPLE_H -DOMX -DOMX_SKIP64BIT \
	  -ftree-vectorize -DUSE_EXTERNAL_OMX -DTARGET_RASPBERRY_PI \
	  -DUSE_EXTERNAL_LIBBCM_HOST -Wno-deprecated-declarations

LDFLAGS	:= -L /opt/bcm-rootfs/opt/vc/lib -L ../../omxplayer -Wl,-rpath=$(RPATH)
LIBS	:= -lomxplayer -lWFC -lGLESv2 -lEGL -lbcm_host \
	   -lopenmaxil -lvchiq_arm -lvcos -lasound -lSDL

game: game.o
	$(TOOLCHAIN)-g++ -Wall --sysroot=$(SYSROOT) $(LDFLAGS) $(LIBS) $^ -o $@

%.o: %.cpp
	$(TOOLCHAIN)-g++ -Wall --sysroot=$(SYSROOT) $(CFLAGS) -c $<

clean:
	rm -f *.o
	
