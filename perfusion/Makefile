TARGETS= obs

.PHONY: all clean

CC = $(ANDROID_BUILD_TOP)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin/aarch64-rockchip1020-linux-gnu-gcc

# 添加 -I 选项，以指定 cutils/sockets.h 头文件的路径
CFLAGS = -I$(ANDROID_BUILD_TOP)/system/core/libcutils/include_outside_system


all: $(TARGETS)

obs: v4l2_perfusion.c
	$(CC) $(CFLAGS) -static -o obs v4l2_perfusion.c

clean:
	-rm $(TARGETS)
