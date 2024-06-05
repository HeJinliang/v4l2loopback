CPPFLAGS += -I..

TARGETS= v4l2loopback-ctl

.PHONY: all clean

CC = $(ANDROID_BUILD_TOP)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin/aarch64-rockchip1020-linux-gnu-gcc

all: $(TARGETS)

v4l2loopback-ctl: v4l2loopback-ctl.c
	$(CC) $(CPPFLAGS) -static -o v4l2loopback-ctl v4l2loopback-ctl.c

clean:
	-rm $(TARGETS)
