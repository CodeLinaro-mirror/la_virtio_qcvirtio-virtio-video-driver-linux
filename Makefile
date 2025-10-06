#
# Makefile for virtio_video kernel module.
#
KDIR ?= $(KERNEL_SRC)
ifeq ($(KDIR),)
$(error "KDIR must be specified.")
endif
KBUILD_OPTIONS ?= MODNAME=virtio_video
# The variable "M" is used to point to the location of this module, and it is
# passed to kbuild to build this module.
#
# Allow to specify variable "M" from outside. It is needed to set it to the
# relative path to this module. It must be relative to the kernel source
# directory.
#
# If kernel is built with "O" option then setting "M" to the relative path
# empowers the build system to put output/object files (.o, .ko.) into a
# directory different from the module source directory.
M ?= $$PWD
# Some build systems may rsync module sources out of git repository to have
# sources directory untouched during build. Then, they should specify Git
# repository directory separately for 'git describe' to work properly.
MODULE_GIT_REPOSITORY_DIR ?= $(M)
default:
	$(MAKE) -C $(KDIR) M=$(M) modules $(KBUILD_OPTIONS) MODULE_GIT_REPOSITORY_DIR=$(MODULE_GIT_REPOSITORY_DIR)
modules_install:
	$(MAKE) -C $(KDIR) M=$(M) $@
%:
	$(MAKE) -C $(KDIR) M=$(M) $@ $(KBUILD_OPTIONS) MODULE_GIT_REPOSITORY_DIR=$(MODULE_GIT_REPOSITORY_DIR)
clean:
	$(MAKE) -C $(KDIR) M=$(M) $@
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions
