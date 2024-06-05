TARGETS=test yuv4mpeg_to_v4l2 ondemandcam yuv420_infiniteloop

.PHONY: all clean

CC = $(ANDROID_BUILD_TOP)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin/aarch64-rockchip1020-linux-gnu-gcc


all: $(TARGETS)

ondemandcam: ondemandcam.c
	$(CC) -o ondemandcam ondemandcam.c -lrt -lpthread

test: test.c
	$(CC) -static -o test test.c

yuv420_infiniteloop: yuv420_infiniteloop.c
	$(CC) -static -o yuv420_infiniteloop yuv420_infiniteloop.c

clean:
	-rm $(TARGETS)
