#
# Kbuild for the virtio-video driver
#

# Default module git repository directory to the module source directory path.
MODULE_GIT_REPOSITORY_DIR ?= $(src)

# --tags to take into account non-annotated tags
# --dirty to mark version with uncommitted changes as dirty
GIT_VERSION = $(shell git -C "$(MODULE_GIT_REPOSITORY_DIR)" describe --tags --dirty | sed 's/^v//')

ccflags-y += -DDRIVER_VERSION=\"$(GIT_VERSION)\"

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
