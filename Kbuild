#
# Kbuild for the virtio-video driver
#

# make $(src) as absolute path if it isn't already, by prefixing $(srctree)
src := $(if $(patsubst /%,,$(src)),$(srctree)/$(src),$(src))

# Default module git repository directory to the module source directory path.
MODULE_GIT_REPOSITORY_DIR ?= $(src)

# --tags to take into account non-annotated tags
# --dirty to mark version with uncommitted changes as dirty
GIT_VERSION = $(shell git -C "$(MODULE_GIT_REPOSITORY_DIR)" describe --tags --dirty | sed 's/^v//')

ccflags-y := -I"$(src)/include/uapi"
ccflags-y += -DDRIVER_VERSION=\"$(GIT_VERSION)\"

# In case of external kernel module build within Android's kernel/build system
# the previous line doesn't work. Hence, specify headers location in other way
ccflags-y += -I$(VIRTIO_VIDEO_ROOT)/include/uapi

# During development, to turn-off treating warning as error, pass
# `CFLAGS_MODULE=-Wno-error` to make as an argument or environment variable.
ccflags-y += -Werror

virtio_video-y := \
	virtio_video_driver.o \
	virtio_video_vq.o \
	virtio_video_device.o \
	virtio_video_dec.o \
	virtio_video_enc.o \
	virtio_video_caps.o \
	virtio_video_helpers.o

obj-m += virtio_video.o
