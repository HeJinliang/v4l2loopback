# 判断.gitversion是否存在，根据git 仓库的状态生成一个版本字符串（TAG），并将这个版本信息作为一个宏定义添加到编译过程中
ifneq ($(wildcard .gitversion),)
# building a snapshot version
V4L2LOOPBACK_SNAPSHOT_VERSION=$(patsubst v%,%,$(shell git describe --always --dirty 2>/dev/null || shell git describe --always 2>/dev/null || echo snapshot))
override KCPPFLAGS += -DSNAPSHOT_VERSION='"$(V4L2LOOPBACK_SNAPSHOT_VERSION)"'
endif

$(info KCPPFLAGS=$(KCPPFLAGS))

# Kbuild 里 只有 obj-m := v4l2loopback.o  表示要编译的模块是v4l2loopback.o
include Kbuild
# 判断变量 KBUILD_MODULES 是否被设置，如果为空，则当前构建不是专门构建内核模块的，即执行里面的代码
ifeq ($(KBUILD_MODULES),)

# 设置目标架构是 ARM 64
ARCH=arm64
# 设置交叉编译目标的内核源码路径
KERNEL_DIR=$(ANDROID_BUILD_TOP)/kernel-5.10
# KERNEL_DIR=$(ANDROID_BUILD_TOP)/kernel/oneplus/sm8250

# 设置交叉编译链的路径 (gcc)
# CROSS_COMPILE=aarch64-linux-android-
# 设置交叉编译链的路径 (clang)
CROSS_COMPILE=aarch64-linux-gnu- LLVM=1 LLVM_IAS=1

# 加载 clang 编译链 环境变量， ANDROID_BUILD_TOP 在执行 lunch 时已经确定
export PATH := $(ANDROID_BUILD_TOP)/prebuilts/clang/host/linux-x86/clang-r416183b/bin:$(PATH)
# export PATH := $(ANDROID_BUILD_TOP)/prebuilts/clang/host/linux-x86/clang-r450784d/bin:$(PATH)
$(info PATH=$(PATH))

PWD		:= $(shell pwd)

PREFIX ?= /usr/local
BINDIR  = $(PREFIX)/bin
INCLUDEDIR = $(PREFIX)/include
MANDIR  = $(PREFIX)/share/man
MAN1DIR = $(MANDIR)/man1
INSTALL = install
INSTALL_PROGRAM = $(INSTALL) -p -m 755
INSTALL_DIR     = $(INSTALL) -p -m 755 -d
INSTALL_DATA    = $(INSTALL) -m 644

MODULE_OPTIONS = devices=2

##########################################
# note on build targets
#
# module-assistant makes some assumptions about targets, namely
#  <modulename>: must be present and build the module <modulename>
#                <modulename>.ko is not enough
# install: must be present (and should only install the module)
#
# we therefore make <modulename> a .PHONY alias to <modulename>.ko
# and remove utils-installation from 'install'
# call 'make install-all' if you want to install everything
##########################################

# 使用 .PHONY 来声明一些伪目标， 这些目标不对应实际的文件，而是一些命令的集合
.PHONY: all install clean distclean
.PHONY: install-all install-extra install-utils install-man install-headers
.PHONY: modprobe v4l2loopback

# we don't control the .ko file dependencies, as it is done by kernel
# makefiles. therefore v4l2loopback.ko is a phony target actually
.PHONY: v4l2loopback.ko utils

# 在 all 目标中 （手动执行 make 时，会触发 'all' 目标的构建），首先依赖 'v4l2loopback.ko' 和 'utils'
all: v4l2loopback.ko utils

# v4l2loopback.ko 会调用内核的Makefile来编译模块
v4l2loopback: v4l2loopback.ko
v4l2loopback.ko:
	@echo "Building v4l2-loopback driver..."
	$(MAKE) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) -C $(KERNEL_DIR) M=$(PWD) KCPPFLAGS="$(KCPPFLAGS)" CC=clang CXX=clang++ modules

# 安装模块，'install' 目标用于安装编译好的模块，使用内核的Makefile执行模块安装。
install-all: install install-extra
install:
# 安装外部模块
	$(MAKE) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) -C $(KERNEL_DIR) M=$(PWD) CC=clang CXX=clang++ modules_install
	@echo ""
	@echo "SUCCESS (if you got 'SSL errors' above, you can safely ignore them)"
	@echo ""

install-extra: install-utils install-man install-headers
install-utils: utils/v4l2loopback-ctl
	$(INSTALL_DIR) "$(DESTDIR)$(BINDIR)"
	$(INSTALL_PROGRAM) $< "$(DESTDIR)$(BINDIR)"

install-man: man/v4l2loopback-ctl.1
	$(INSTALL_DIR) "$(DESTDIR)$(MAN1DIR)"
	$(INSTALL_DATA) $< "$(DESTDIR)$(MAN1DIR)"

install-headers: v4l2loopback.h
	$(INSTALL_DIR) "$(DESTDIR)$(INCLUDEDIR)/linux"
	$(INSTALL_DATA) $< "$(DESTDIR)$(INCLUDEDIR)/linux"

# 清理构建，'clean'目标用于清理构建过程中产生的临时文件。
clean:
	rm -f *~
	rm -f Module.symvers Module.markers modules.order
# 仅删除模块目录中生成的所有文件
	$(MAKE) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) -C $(KERNEL_DIR) M=$(PWD) clean
	$(MAKE) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) -C utils clean

distclean: clean
	rm -f man/v4l2loopback-ctl.1

modprobe: v4l2loopback.ko
	chmod a+r v4l2loopback.ko
	sudo modprobe videodev
	-sudo rmmod v4l2loopback
	sudo insmod ./v4l2loopback.ko $(MODULE_OPTIONS)

man/v4l2loopback-ctl.1: utils/v4l2loopback-ctl
	help2man -N --name "control v4l2 loopback devices" \
		--no-discard-stderr --help-option=-h --version-option=-v \
		$^ > $@

utils: utils/v4l2loopback-ctl
utils/v4l2loopback-ctl: utils/v4l2loopback-ctl.c v4l2loopback.h
	$(MAKE) CROSS_COMPILE=$(CROSS_COMPILE) ARCH=$(ARCH) -C utils V4L2LOOPBACK_SNAPSHOT_VERSION=$(V4L2LOOPBACK_SNAPSHOT_VERSION)

.clang-format:
	curl "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/.clang-format" > $@

.PHONY: clang-format
clang-format: .clang-format
	clang-format -i *.c *.h utils/*.c

.PHONY: sign
# try to read the default certificate/key from the dkms config
dkms_framework=/etc/dkms/framework.conf
-include $(dkms_framework)
KBUILD_SIGN_KEY=$(mok_signing_key)
KBUILD_SIGN_CERT=$(mok_certificate)

ifeq ($(KBUILD_SIGN_PIN),)
define usage_kbuildsignpin
$(info )
$(info ++++++ If your certificate requires a password, pass it via the KBUILD_SIGN_PIN env-var!)
$(info ++++++ E.g. using 'export KBUILD_SIGN_PIN; read -s -p "Passphrase for signing key $(KBUILD_SIGN_KEY): " KBUILD_SIGN_PIN; sudo --preserve-env=KBUILD_SIGN_PIN make sign')
$(info )
endef
endif

define usage_kbuildsign
sign: v4l2loopback.ko
	$(info )
	$(info ++++++ To sign the $< module, you must set KBUILD_SIGN_KEY/KBUILD_SIGN_CERT to point to the signing key/certificate!)
	$(info ++++++ For your convenience, we try to read these variables as 'mok_signing_key' resp. 'mok_certificate' from $(dkms_framework))
	$(call usage_kbuildsignpin)
endef

ifeq ($(wildcard $(KBUILD_SIGN_KEY)),)
$(call usage_kbuildsign)
else ifeq ($(wildcard $(KBUILD_SIGN_CERT)),)
$(call usage_kbuildsign)
else
sign: v4l2loopback.ko
	$(call usage_kbuildsignpin)
	"$(KERNEL_DIR)"/scripts/sign-file sha256 $(KBUILD_SIGN_KEY) $(KBUILD_SIGN_CERT) $<
endif

endif # !kbuild
